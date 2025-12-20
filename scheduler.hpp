// scheduler.hpp
#pragma once
#include "event.hpp"
#include "event_id.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <queue>
#include <type_traits>
#include <vector>

namespace es {

enum class EventStatus : uint8_t { Alive, Cancelled };
enum class OpType : uint8_t { Add, Clear };

template <typename Callback = DefaultCallback> class EventScheduler {

    using Desc = EventDesc<Callback>;

    struct Event {
        Desc desc;
        EventStatus status = EventStatus::Cancelled;
        TimeMs next_fire = TimeMs{};
    };

    using Events = std::vector<Event>;

    struct EventCompare {
        const Events &events;
        explicit EventCompare(const Events &es) : events(es) {}
        bool tie_break(const EventID &lhs, const EventID &rhs) const noexcept {
            const Event &le = events[lhs];
            const Event &re = events[rhs];
            if (le.desc.pri == re.desc.pri) return lhs.index > rhs.index;
            return le.desc.pri > re.desc.pri;
        }
        bool operator()(const EventID &lhs, const EventID &rhs) const noexcept {
            // 小根堆
            const Event &le = events[lhs];
            const Event &re = events[rhs];
            if (le.next_fire == re.next_fire) return tie_break(lhs, rhs);
            return le.next_fire > re.next_fire;
        }
        friend void swap(EventCompare &, EventCompare &) noexcept {}
    };

    struct Op {
        OpType op_type;

        // for schedule in ticking
        TimeMs time_ms;
        Callback f;
        TimeMode mode = TimeMode::Relative;
        EventType event_type = EventType::Once;
        TimeMs interval_ms = TimeMs{};
        ExceptionPolicy ep = ExceptionPolicy::Swallow;
        EventPriority pri = EventPriority::User;

        EventID eid;
    };

    struct TickGuard {
        EventScheduler *es;
        bool flush = true;
        explicit TickGuard(EventScheduler *_es, bool _flush) : es(_es), flush(_flush) { es->ticking = true; }
        ~TickGuard() noexcept(false) {
            es->ticking = false;
            if (flush) es->flush_delay_ops();
        }
    };

    using PQ = std::priority_queue<EventID, std::vector<EventID>, EventCompare>;
    using FL = std::vector<uint32_t>;
    using Gens = std::vector<uint32_t>;
    using Ops = std::vector<Op>;

private:
    void reschedule(EventID eid, Event &e) noexcept {
        e.next_fire += e.desc.interval_ms;
        pq.push(eid);
    }

    void fire_top() {
        ++fire_count_;
        EventID top = pq.top();
        pq.pop();
        Event &e = events[top];
        Desc &desc = e.desc;

        if (e.status != EventStatus::Alive) return;

        try {
            call(desc.callback, desc.ep, top); // 这里 call 之后事件不一定仍为 Alive 状态
        } catch (...) {
            // 若在此处捕获，说明 Policy 为 rethrow
            // rethrow 只是重新抛出，不代表事件会被自动取消
            if (e.status == EventStatus::Cancelled) throw;
            if (desc.type == EventType::Repeat) reschedule(top, e);
            else reuse_once(top);
            throw;
        }

        if (e.status == EventStatus::Cancelled) return;
        if (desc.type == EventType::Repeat) reschedule(top, e);
        else reuse_once(top);
    }

    void print_top(TimeMs delta_ms) const noexcept {
        EventID top = pq.top();
        const Event &e = events[top];
        std::cout << "tick:                " << delta_ms << std::endl;
        std::cout << "top event id:        " << top << std::endl;
        std::cout << "top event status:    " << (e.status == EventStatus::Alive ? "Alive" : "Cancelled") << std::endl;
        std::cout << "top event next fire: " << e.next_fire << std::endl;
    }

    template <typename F>
    void set_event(TimeMs time_ms, TimeMode mode, EventType type, TimeMs interval_ms, F &&f, ExceptionPolicy ep,
                   EventPriority pri, EventID eid) {
        // 计算绝对触发时间（这里是第一次触发的时间）
        TimeMs next_fire;
        if (mode == TimeMode::Absolute) next_fire = time_ms;
        else next_fire = current + time_ms;

        // 更新调度器
        Desc desc{type, interval_ms, Callback(std::forward<F>(f)), ep, pri};
        events[eid] = Event{desc, EventStatus::Alive, next_fire};
        pq.push(eid);
        ++alive;
    }

    template <typename F> void call(F &&f, ExceptionPolicy ep, EventID eid) {
        try {
            f();
        } catch (...) {
            if (ep == ExceptionPolicy::CancelEvent) cancel(eid);
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

    // 回收 Once 事件
    void reuse_once(EventID eid) noexcept {
        Event &e = events[eid];
        assert(e.status == EventStatus::Alive);
        e.status = EventStatus::Cancelled; // 不再 Alive
        fl.push_back(eid.index);
        ++gens[eid];
        --alive;
    }

    // pop 不增加 gen
    EventID pop_fl() noexcept {
        EventID eid{fl.back(), gens[fl.back()]};
        fl.pop_back();
        return eid;
    }

    bool try_pop_cancelled() {
        EventID top = pq.top();
        const Event &e = events[top];
        if (e.status != EventStatus::Cancelled) return false;
        pq.pop();
        fl.push_back(top.index);
        ++gens[top];
        --cancelled;
        return true;
    }

    // 尝试重用 cancel 节点
    bool try_reuse(EventID eid) {
        const Event &e = events[eid];
        if (e.status != EventStatus::Cancelled) return false;
        fl.push_back(eid.index);
        ++gens[eid];
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

    bool try_update_pause(TimeMs delta_ms) {
        if (!paused) return false;
        paused_time += delta_ms;
        return true;
    }

    template <typename F>
    bool try_add_ops(
        OpType op_type, TimeMs time_ms = 0, F &&f = [] {}, TimeMode mode = TimeMode::Relative,
        EventType event_type = EventType::Once, TimeMs interval_ms = TimeMs{},
        ExceptionPolicy ep = ExceptionPolicy::Swallow, EventPriority pri = EventPriority::User,
        EventID eid = EventID::invalid()) {
        if (!ticking) return false;
        Op op{op_type, time_ms, Callback(std::forward<F>(f)), mode, event_type, interval_ms, ep, pri, eid};
        delay_ops.emplace_back(std::move(op));
        return true;
    }

    void flush_delay_ops() {
        Ops ops;
        ops.swap(delay_ops); // 清空 delay_ops 同时仍能继续遍历
        for (Op &op : ops) {
            if (op.op_type == OpType::Add) // 这里 move op.f 没事，因为本来 op 就是临时对象
                set_event(op.time_ms, op.mode, op.event_type, op.interval_ms, std::move(op.f), op.ep, op.pri, op.eid);
            else if (op.op_type == OpType::Clear) clear_in_tick(); // 不 return，确保后面的 schedule 被处理
        }
    }

    void clear_in_tick() noexcept {
        PQ tmp{EventCompare(events)};
        pq.swap(tmp);

        // pq 已经为空，不用担心 try pop cancel 带来的问题
        fl.clear();
        fl.reserve(events.size());
        for (uint32_t i = 0; i < events.size(); ++i) {
            ++gens[i];
            events[i].status = EventStatus::Cancelled;
            fl.push_back(i);
        }

        current = TimeMs{};
        paused_time = TimeMs{};
        alive = 0;
        cancelled = 0;
        fire_count_ = 0;
    }

public:
    EventScheduler() : events(), pq(EventCompare(events)) {}
    ~EventScheduler() {}

    EventScheduler(const EventScheduler &) = delete;
    EventScheduler &operator=(const EventScheduler &) = delete;
    EventScheduler(EventScheduler &&) = delete;
    EventScheduler &operator=(EventScheduler &&) = delete;

    // 添加事件
    template <typename F>
    EventID schedule(TimeMs time_ms, F &&f, TimeMode mode = TimeMode::Relative, EventType type = EventType::Once,
                     TimeMs interval_ms = TimeMs{}, ExceptionPolicy ep = ExceptionPolicy::Swallow,
                     EventPriority pri = EventPriority::User) {
        static_assert(std::is_invocable_r_v<void, F &>, "callback must be invocable with signature void()");

        assert(!(type == EventType::Repeat && interval_ms == 0)); // 防止同一 tick 重复触发某一 Repeat 事件

        // 获取事件最终的 eid
        EventID eid;
        if (fl.empty()) eid = append();
        else eid = pop_fl();
        // 获取 eid 时，其 gen 应该等于列表中记录的 gen
        assert(eid.gen == gens[eid]);

        // 处理正在 ticking 的情况
        if (try_add_ops(OpType::Add, time_ms, std::forward<F>(f), mode, type, interval_ms, ep, pri, eid)) return eid;

        set_event(time_ms, mode, type, interval_ms, std::forward<F>(f), ep, pri, eid);
        return eid;
    }

    // 取消事件
    void cancel(EventID eid) noexcept {
        if (!eid.is_valid() || !is_alive(eid)) return;
        Event &e = events[eid];
        if (e.status == EventStatus::Cancelled) return;
        events[eid].status = EventStatus::Cancelled;
        // 不要在这里回收，如更新 fl 和 gen，因为可能还会碰这个事件
        --alive;
        ++cancelled;
        if (cancelled > alive) rebuild_pq();
    }

    // 事件是否活跃
    bool is_alive(EventID eid) {
        if (eid >= events.size()) return false;
        if (eid.gen != gens[eid]) return false;
        const Event &e = events[eid];
        return e.status == EventStatus::Alive;
    }

    // 推进时间
    void tick(TimeMs delta_ms) {
        // tick 0 应该触发未被执行且调用 tick 前已经 schedule 的事件
        // 但不触发在 tick 中 delay = 0 的事件

        // tick 中 cancel 同一时刻且未被触发的事件，则被 cancel 的事件不会被触发

        if (try_update_pause(delta_ms)) return;
        TickGuard tg(this, true); // RAII guard
        current += delta_ms;
        while (!pq.empty()) {
            // 提前防止 Cancelled 堆积
            if (try_pop_cancelled()) continue;

            EventID top = pq.top();
            const Event &e = events[top];
            if (current < e.next_fire) break;
            fire_top();
        }
        // 出去之后，ticking = false, delay ops 都完成
    }

    void tick_until(TimeMs end_time) {
        if (end_time <= current) return;
        TimeMs delta_ms = end_time - current;
        tick(delta_ms);
    }

    // 获取最近事件的 id 和触发时间
    auto peek() const noexcept -> std::pair<EventID, TimeMs> {
        EventID eid = pq.top();
        const Event &e = events[eid];
        return {eid, e.next_fire};
    }

    void run() {
        if (paused) return;
        while (!pq.empty()) {
            if (try_pop_cancelled()) continue;

            EventID top = pq.top();
            const Event &e = events[top];
            current = e.next_fire;
            fire_top();
        }
    }

    // 当前调度器时间
    TimeMs now() const noexcept { return current; }

    // 事件数量
    size_t size() const noexcept { return alive; }

    // 事件触发次数
    size_t fire_count() const noexcept { return fire_count_; }

    // 清空所有事件
    void clear() noexcept {
        // 处理 ticking 情况
        if (try_add_ops<Callback>(OpType::Clear)) return;

        events.clear();
        PQ tmp{EventCompare(events)};
        pq.swap(tmp);
        fl.clear();
        gens.clear();
        assert(delay_ops.empty());
        current = TimeMs{};
        paused_time = TimeMs{};
        alive = 0;
        cancelled = 0;
        fire_count_ = 0;
        paused = false;
        assert(!ticking);
    }

    // 暂停/恢复
    void pause() noexcept { paused = true; }

    void resume() {
        paused = false;
        tick(paused_time);
        paused_time = 0;
    }

    size_t _fl_size() const noexcept { return fl.size(); }
    size_t _pq_size() const noexcept { return pq.size(); }

private:
    Events events;
    PQ pq;
    FL fl;
    Gens gens;
    Ops delay_ops;
    TimeMs current{};
    TimeMs paused_time{};
    size_t alive{};
    size_t cancelled{};
    size_t fire_count_{};
    bool paused = false;
    bool ticking = false;
};

} // namespace es