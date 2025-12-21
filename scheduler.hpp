// scheduler.hpp
#pragma once
#include "event.hpp"
#include "event_id.hpp"
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <queue>
#include <sstream>
#include <type_traits>
#include <vector>

namespace es {

enum class EventStatus : uint8_t { Alive, Cancelled };
enum class OpType : uint8_t { Schedule, Clear, Delay };

template <typename Callback = DefaultCallback> class EventScheduler {

    using Desc = EventDesc<Callback>;

    struct Event {
        Desc desc{};
        EventStatus status = EventStatus::Cancelled;
        TimeMs next_fire = TimeMs{};
    };

    using Events = std::deque<Event>; // 防止扩容 Callback 搬家

    struct EventCompare {
        const Events &events;
        explicit EventCompare(const Events &es) : events(es) {}
        bool tie_break(const EventID &lhs, const EventID &rhs) const noexcept {
            const Event &le = events[lhs.index];
            const Event &re = events[rhs.index];
            if (le.desc.pri == re.desc.pri) return lhs.index > rhs.index;
            return le.desc.pri > re.desc.pri;
        }
        bool operator()(const EventID &lhs, const EventID &rhs) const noexcept {
            // 小根堆
            const Event &le = events[lhs.index];
            const Event &re = events[rhs.index];
            if (le.next_fire == re.next_fire) return tie_break(lhs, rhs);
            return le.next_fire > re.next_fire;
        }
        friend void swap(EventCompare &, EventCompare &) noexcept {}
    };

    struct Op {
        OpType op_type;
        // for schedule || set next fire
        TimeMs next_fire;
        Desc desc;
        EventID eid;
    };

    struct TickGuard {
        EventScheduler *es;
        explicit TickGuard(EventScheduler *_es) : es(_es) {
            assert(!es->ticking);
            es->ticking = true;
            assert(es->delay_ops.empty());
            assert(es->pending_clear == 0);
        }
        ~TickGuard() {
            es->flush_delay_ops();
            es->pending_clear = 0;
            es->ticking = false;
        }
    };

    using PQ = std::priority_queue<EventID, std::vector<EventID>, EventCompare>;
    using FL = std::vector<uint32_t>;
    using Idxs = std::vector<uint32_t>;
    using Gens = std::vector<uint32_t>;
    using Ops = std::vector<Op>;

private:
    void set_event(TimeMs next_fire, Desc &&d, EventID eid) {
        // 更新调度器
        Event &e = events[eid.index];
        e.desc = std::move(d);
        e.status = EventStatus::Alive;
        e.next_fire = next_fire;
        pq.push(eid);
        ++alive;
    }

    template <typename F> void call(F &&f, EventID eid) {
        Event &e = events[eid.index];
        ExceptionPolicy ep = e.desc.ep;
        try {
            if constexpr (std::is_invocable_r_v<void, F &>) f();
            else if (std::is_invocable_r_v<void, F &, EventID>) f(eid);
        } catch (...) {
            if (ep == ExceptionPolicy::Cancel) cancel(eid);
            else if (ep == ExceptionPolicy::Rethrow) throw;
        }
    }

    EventID append() {
        uint32_t id = static_cast<uint32_t>(events.size());
        EventID eid{id, 0};
        events.emplace_back();
        gens.emplace_back(0);
        return eid;
    }

    void reuse(EventID eid) noexcept {
        Event &e = events[eid.index];
        if (e.status == EventStatus::Alive) --alive;
        e.status = EventStatus::Cancelled;
        fl.push_back(eid.index);
        ++gens[eid.index];
    }

    // pop 不增加 gen
    EventID pop_fl() noexcept {
        EventID eid{fl.back(), gens[fl.back()]};
        fl.pop_back();
        return eid;
    }

    bool try_pop_cancelled() {
        EventID top = pq.top();
        const Event &e = events[top.index];
        if (e.status != EventStatus::Cancelled) return false;
        pq.pop();
        fl.push_back(top.index);
        ++gens[top.index];
        --cancelled;
        return true;
    }

    // 尝试重用 cancel 节点
    bool try_reuse(EventID eid) {
        const Event &e = events[eid.index];
        if (e.status != EventStatus::Cancelled) return false;
        fl.push_back(eid.index);
        ++gens[eid.index];
        return true;
    }

    bool try_update_pause(TimeMs delta_ms) {
        if (!paused) return false;
        paused_time_ += delta_ms;
        return true;
    }

    void add_schedule(TimeMs next_fire, Desc &&d, EventID eid) {
        Op op;
        op.op_type = OpType::Schedule;
        op.next_fire = next_fire;
        op.desc = std::move(d);
        op.eid = eid;
        delay_ops.emplace_back(std::move(op));
    }

    void add_clear() {
        ++pending_clear;
        delay_ops.clear(); // 最后一次 clear 之前的操作都是没有意义的
        Op op;
        op.op_type = OpType::Clear;
        delay_ops.emplace_back(std::move(op));
    }

    void add_delay(EventID eid, TimeMs next_fire) {
        Op op;
        op.op_type = OpType::Delay;
        op.eid = eid;
        op.next_fire = next_fire;
        delay_ops.emplace_back(std::move(op));
    }

    void handle_clear_op(const Ops &ops, size_t i) {
        // Clear 操作应该在整个 delay_ops 中的首个位置
        assert(i == 0);

        // 收集 resevered 槽位
        Idxs reserved_indices;
        for (size_t j = 1; j < ops.size(); ++j) {
            const Op &op = ops[j];
            assert(op.op_type == OpType::Schedule);
            reserved_indices.push_back(op.eid.index);
        }
        clear_in_tick(reserved_indices);
    }

    void flush_delay_ops() {
        Ops ops;
        ops.swap(delay_ops); // 清空 delay_ops 同时仍能继续遍历
        for (size_t i = 0; i < ops.size(); ++i) {
            Op &op = ops[i];
            if (op.op_type == OpType::Schedule) set_event(op.next_fire, std::move(op.desc), op.eid);
            else if (op.op_type == OpType::Clear) handle_clear_op(ops, i);
            else default_set_next_fire(op.eid, op.next_fire);
        }
    }

    // 一个 tick 内只会执行一次
    void clear_in_tick(const Idxs &reserved_indices) noexcept {
        // 清空 pq
        PQ tmp{EventCompare(events)};
        pq.swap(tmp);

        // 标记预定槽位
        std::vector<uint8_t> reserved(events.size(), 0);
        for (uint32_t idx : reserved_indices) {
            assert(idx < reserved.size());
            reserved[idx] = 1;
        }

        fl.clear();
        fl.reserve(events.size());

        for (uint32_t i = 0; i < events.size(); ++i) {
            Event &e = events[i];
            e.status = EventStatus::Cancelled;
            gens[i] += pending_clear; // 一次性加够

            // 只有未被预定的槽位可以进入 free list
            if (!reserved[i]) fl.push_back(i);
        }

        alive = 0;
        cancelled = 0;
        fire_count = 0;
    }

    bool try_skip_repeat() {
        EventID top = pq.top();
        Event &e = events[top.index];
        const Desc &d = e.desc;
        if (d.type != EventType::Repeat) return false;
        if (d.cu != CatchUp::Latest) return false;

        // 把 Repeat 类的事件的 next_fire 更新到最后一次触发时刻
        TimeMs delta = current - e.next_fire;
        if (delta <= 0) return false;
        int64_t ts = static_cast<int64_t>(delta / d.interval_ms); // 周期
        if (ts == 0) return false;                                // 确保会被触发

        pq.pop();
        e.next_fire += ts * d.interval_ms;
        pq.push(top);
        return true;
    }

    bool try_skip_old() {
        EventID eid = pq.top();
        if (eid.gen == gens[eid.index]) return false;
        pq.pop();
        // 这里没有回收逻辑，因为可能 event 已经被更新为新版本
        return true;
    }

    void reschedule(EventID eid) noexcept {
        _assert_eid(eid);
        Event &e = events[eid.index];
        assert(e.desc.type == EventType::Repeat);
        e.next_fire += e.desc.interval_ms;
        pq.push(eid);
    }

    void default_clear() {
        events.clear();
        PQ tmp{EventCompare(events)};
        pq.swap(tmp);
        fl.clear();
        gens.clear();
        assert(delay_ops.empty());
        alive = 0;
        cancelled = 0;
        fire_count = 0;
        assert(!ticking);
    }

    void default_set_next_fire(EventID eid, TimeMs next_fire) {
        Event &e = events[eid.index];
        EventID new_id;
        new_id.index = eid.index;
        new_id.gen = eid.gen + 1;
        ++gens[eid.index]; // 把堆中原有事件标记为旧事件
        pq.push(new_id);   // 添加新的事件
        e.next_fire = next_fire;
    }

    template <typename F>
    static constexpr bool is_valid_callback_t =
        std::is_constructible_v<Callback, F> &&
        (std::is_invocable_r_v<void, F &> || std::is_invocable_r_v<void, F &, EventID>);

public:
    EventScheduler() : events(), pq(EventCompare(events)) {}
    ~EventScheduler() {}

    EventScheduler(const EventScheduler &) = delete;
    EventScheduler &operator=(const EventScheduler &) = delete;
    EventScheduler(EventScheduler &&) = delete;
    EventScheduler &operator=(EventScheduler &&) = delete;

    template <typename F>
    EventID schedule_after(TimeMs time_ms, F &&f, EventType type = EventType::Once, TimeMs interval_ms = TimeMs{},
                           ExceptionPolicy ep = ExceptionPolicy::Swallow, EventPriority pri = EventPriority::User,
                           CatchUp cu = CatchUp::All) {
        static_assert(is_valid_callback_t<F>,
                      "callback must be invocable with signature void() / void(EventID)，而且能用于构造 Callback 对象");
        assert(!(type == EventType::Repeat && interval_ms <= 0)); // 防止同一 tick 重复触发某一 Repeat 事件

        // 获取事件最终的 eid
        EventID eid;
        if (fl.empty()) eid = append();
        else eid = pop_fl();

        Desc d;
        d.type = type;
        d.interval_ms = interval_ms;
        d.callback = std::move(Callback(std::forward<F>(f)));
        d.ep = ep;
        d.pri = pri;
        d.cu = cu;

        TimeMs next_fire = current + time_ms;

        // 处理 ticking clear 带来的 gen 偏移
        assert(!(ticking == false && pending_clear != 0));
        assert(eid.gen == gens[eid.index]);
        eid.gen += pending_clear;
        // 不给 gens 加偏移，因为 flush 的时候会加上

        if (ticking) add_schedule(next_fire, std::move(d), eid);
        else set_event(next_fire, std::move(d), eid);
        return eid;
    }

    template <typename F>
    EventID schedule_at(TimeMs time_ms, F &&f, EventType type = EventType::Once, TimeMs interval_ms = TimeMs{},
                        ExceptionPolicy ep = ExceptionPolicy::Swallow, EventPriority pri = EventPriority::User,
                        CatchUp cu = CatchUp::All) {
        return schedule_after(time_ms - current, std::forward<F>(f), type, interval_ms, ep, pri, cu);
    }

    template <typename F>
    EventID schedule(TimeMs time_ms, F &&f, TimeMode mode = TimeMode::Relative, EventType type = EventType::Once,
                     TimeMs interval_ms = TimeMs{}, ExceptionPolicy ep = ExceptionPolicy::Swallow,
                     EventPriority pri = EventPriority::User, CatchUp cu = CatchUp::All) {
        if (mode == TimeMode::Relative)
            return schedule_after(time_ms, std::forward<F>(f), type, interval_ms, ep, pri, cu);
        return schedule_at(time_ms, std::forward<F>(f), type, interval_ms, ep, pri, cu);
    }

    void fire_top() {
        ++fire_count;
        EventID top = pq.top();
        pq.pop();
        Event &e = events[top.index];
        Desc &d = e.desc;

        if (e.status != EventStatus::Alive) return;

        try {
            // call 后事件不一定仍为 Alive
            call(d.callback, top);
        } catch (...) {
            // 若在此处捕获，说明 Policy 为 rethrow
            if (d.type == EventType::Repeat && e.status != EventStatus::Cancelled) reschedule(top);
            else reuse(top);
            throw;
        }
        if (d.type == EventType::Repeat && e.status != EventStatus::Cancelled) reschedule(top);
        else reuse(top);
    }

    // 取消事件，若已经非活跃，返回 false
    bool cancel(EventID eid) noexcept {
        if (!eid.is_valid() || !is_alive(eid)) return false;
        Event &e = events[eid.index];
        if (e.status == EventStatus::Cancelled) return false;
        events[eid.index].status = EventStatus::Cancelled;
        // 不要在这里回收，如更新 fl 和 gen 等
        --alive;
        ++cancelled;
        if (cancelled > alive) rebuild_pq();
        return true;
    }

    void rebuild_pq() {
        size_t old_pq_size = pq.size();
        size_t old_fl_size = fl.size();
        PQ tmp{EventCompare(events)};
        while (!pq.empty()) {
            EventID eid = pq.top();
            pq.pop();
            if (!try_reuse(eid)) tmp.push(eid);
        }
        pq.swap(tmp);
        assert(old_pq_size - pq.size() == fl.size() - old_fl_size);
        cancelled = 0;
    }

    // 事件是否活跃，是否非旧事件
    bool is_alive(EventID eid) const noexcept {
        if (static_cast<size_t>(eid.index) >= events.size()) return false;
        if (eid.gen != gens[eid.index]) return false;
        const Event &e = events[eid.index];
        return e.status == EventStatus::Alive;
    }

    // 推进时间
    void tick(TimeMs delta_ms) {
        assert(!ticking);
        if (try_update_pause(delta_ms)) return;
        TickGuard tg(this); // RAII guard
        current += delta_ms;
        while (!pq.empty()) {
            if (try_pop_cancelled()) continue; // 处理 Cancelled 堆顶
            if (try_skip_old()) continue;      // 跳过旧事件，注意顺序
            if (try_skip_repeat()) continue;   // CatchUp = Latest 时，跳过重复 repeat 事件

            EventID top = pq.top();
            const Event &e = events[top.index];

            if (current < e.next_fire) break;
            fire_top();
        }
    }

    void tick_until(TimeMs end_time) {
        if (end_time <= current) return;
        TimeMs delta_ms = end_time - current;
        tick(delta_ms);
    }

    // 获取最近事件的 id 和触发时间
    auto peek() const noexcept -> std::optional<std::pair<EventID, TimeMs>> {
        if (pq.empty()) return std::nullopt;
        EventID eid = pq.top();
        const Event &e = events[eid.index];
        return {eid, e.next_fire};
    }

    void run() {
        assert(!ticking);
        if (paused) return;
        TickGuard tg(this);
        while (!pq.empty()) {
            if (try_pop_cancelled()) continue;
            if (try_skip_repeat()) continue;
            if (try_skip_old()) continue;

            EventID top = pq.top();
            const Event &e = events[top.index];

            current = e.next_fire;
            fire_top();
        }
    }

    TimeMs now() const noexcept { return current; }
    TimeMs paused_time() const noexcept { return paused_time_; }
    size_t size() const noexcept { return alive; }
    size_t num_cancelled() const noexcept { return cancelled; }
    size_t num_pending_clear() const noexcept { return pending_clear; }

    // 清空所有事件
    void clear() noexcept {
        // 处理 ticking 情况
        if (ticking) add_clear();
        else default_clear();
    }

    // 暂停/恢复
    void pause() noexcept { paused = true; }
    void resume() {
        paused = false;
        tick(paused_time_); // 这里应该还会保持 ALL / LATEST 属性
        paused_time_ = 0;
    }

    size_t _fire_count() const noexcept { return fire_count; }
    size_t _fl_size() const noexcept { return fl.size(); }
    size_t _pq_size() const noexcept { return pq.size(); }
    void _assert_eid(EventID eid) {
        assert(eid.is_valid());
        assert(is_alive(eid));
    }
    std::ostringstream _top_info() const noexcept {
        EventID top = pq.top();
        const Event &e = events[top.index];
        std::ostringstream oss;
        oss << "top event idx:       " << top.index << std::endl;
        oss << "top event status:    " << (e.status == EventStatus::Alive ? "Alive" : "Cancelled") << std::endl;
        oss << "top event next fire: " << e.next_fire << std::endl;
        return oss;
    }
    const Events &_events() const noexcept { return events; }
    const Event &_event_of(EventID eid) const noexcept {
        _assert_eid(eid);
        return events[eid.index];
    }

    void set_interval(EventID eid, TimeMs new_interval) noexcept {
        _assert_eid(eid);
        Desc &d = events[eid.index].desc;
        assert(d.type == EventType::Repeat);
        assert(new_interval > 0);
        d.interval_ms = new_interval;
    }

    void set_type(EventID eid, EventType new_type) noexcept {
        _assert_eid(eid);
        events[eid.index].desc.type = new_type;
    }

    void set_exp_policy(EventID eid, ExceptionPolicy new_policy) noexcept {
        _assert_eid(eid);
        events[eid.index].desc.ep = new_policy;
    }

    void set_priority(EventID eid, EventPriority new_pri) noexcept {
        _assert_eid(eid);
        events[eid.index].desc.pri = new_pri;
    }

    void set_catchup(EventID eid, CatchUp new_cu) noexcept {
        _assert_eid(eid);
        events[eid.index].desc.cu = new_cu;
    }

    // === 以下接口会修改事件顺序，通过更新 gen 把原来的事件“标记”为旧事件

    // 可以传入负数
    void delay(EventID eid, TimeMs ms) noexcept {
        Event &e = events[eid.index];
        set_next_fire(eid, e.next_fire + ms);
    }

    void set_next_fire(EventID eid, TimeMs next_fire) noexcept {
        _assert_eid(eid);
        Event &e = events[eid.index];
        if (e.next_fire == next_fire) return;
        // 只有 ticking 且提早事件发生时间到 current 之前的操作才有必要进入 delay ops
        if (ticking && next_fire <= current) add_delay(eid, next_fire);
        else default_set_next_fire();
    }

private:
    Events events;
    PQ pq;
    FL fl;
    Gens gens;
    Ops delay_ops;
    TimeMs current{};
    TimeMs paused_time_{};
    size_t alive{};
    size_t cancelled{};
    size_t fire_count{};
    uint32_t pending_clear{}; // delay ops 中未执行的 clear，用于防止 gen 漂移
    bool paused = false;
    bool ticking = false;
};

} // namespace es