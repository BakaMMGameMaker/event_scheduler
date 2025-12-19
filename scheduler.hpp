// scheduler.hpp
#pragma once
#include "event.hpp"
#include "event_id.hpp"
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
        EventStatus status;
        TimeMs next_fire;
    };

    using Events = std::vector<Event>;

    struct EventCompare {
        const Events &events;
        explicit EventCompare(const Events &pool) : events(pool) {}
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

        // for schedule
        TimeMs time_ms;
        Callback f;
        TimeMode mode = TimeMode::Relative;
        EventType event_type = EventType::Once;
        TimeMs interval_ms = TimeMs{};
        ExceptionPolicy ep = ExceptionPolicy::Swallow;
        EventPriority pri = EventPriority::User;
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

        call(desc.callback, desc.ep, top);
        if (desc.type == EventType::Repeat) reschedule(top, e);
        else push_fl(top);
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
    void set_event(EventType type, TimeMs interval_ms, F &&f, ExceptionPolicy ep, EventPriority pri, EventID eid,
                   TimeMs next_fire) {
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

    // 离开 pq 增加 gen
    void push_fl(EventID eid) noexcept {
        fl.push_back(eid.index);
        ++gens[eid];
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
        ++gens[top];
        return true;
    }

    void rebuild_pq() {
        PQ tmp = PQ(EventCompare(events));
        while (!pq.empty()) {
            EventID eid = pq.top();
            const Event &e = events[eid];
            if (e.status != EventStatus::Cancelled) tmp.push(eid);
            pq.pop();
        }
        pq.swap(tmp);
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
        ExceptionPolicy ep = ExceptionPolicy::Swallow, EventPriority pri = EventPriority::User) {
        if (!ticking) return false;
        Op op{op_type, time_ms, Callback(std::forward<F>(f)), mode, event_type, interval_ms, ep, pri};
        delay_ops.emplace_back(std::move(op));
        return true;
    }

    template <typename F> void call_delay_ops() {
        for (Op &op : delay_ops) {
            if (op.op_type == OpType::Add)
                schedule(op.time_ms, std::forward<F>(op.f), op.mode, op.event_type, op.interval_ms, op.ep, op.pri);
            else if (op.op_type == OpType::Clear) clear();
        }
    }

public:
    EventScheduler() : events(), pq(EventCompare(events)) {}
    ~EventScheduler() {}

    EventScheduler(const EventScheduler &) = delete;
    EventScheduler &operator=(const EventScheduler &) = delete;

    // 添加事件
    template <typename F>
    EventID schedule(TimeMs time_ms, F &&f, TimeMode mode = TimeMode::Relative, EventType type = EventType::Once,
                     TimeMs interval_ms = TimeMs{}, ExceptionPolicy ep = ExceptionPolicy::Swallow,
                     EventPriority pri = EventPriority::User) {
        static_assert(std::is_invocable_r_v<void, F &>, "callback must be invocable with signature void()");
        EventID eid;
        if (fl.empty()) eid = append();
        else eid = pop_fl();

        // 处理正在 ticking 的情况
        if (try_add_ops(OpType::Add, time_ms, std::forward<F>(f), mode, type, interval_ms, ep, pri)) return eid;

        TimeMs next_fire = (mode == TimeMode::Absolute) ? time_ms : current + time_ms;
        set_event(type, interval_ms, std::forward<F>(f), ep, pri, eid, next_fire);
        return eid;
    }

    // 取消事件
    void cancel(EventID eid) noexcept {
        if (!eid.is_valid() || !is_alive(eid)) return;
        Event &e = events[eid];
        if (e.status == EventStatus::Cancelled) return;
        events[eid].status = EventStatus::Cancelled;
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
        ticking = true;
        current += delta_ms;
        while (!pq.empty()) {
            // 提前防止 Cancelled 堆积
            if (try_pop_cancelled()) continue;

            EventID top = pq.top();
            const Event &e = events[top];
            if (current < e.next_fire) break;
            fire_top();
        }
        ticking = false;
        call_delay_ops<Callback>();
    }

    void tick_until(TimeMs end_time) {
        if (end_time <= current) return;
        TimeMs delta_ms = end_time - current;
        tick(delta_ms);
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
        if (try_add_ops<Callback>(OpType::Clear)) return;
        events.clear();
        PQ tmp = PQ(EventCompare(events));
        pq.swap(tmp);
        fl.clear();
        gens.clear();
        delay_ops.clear();
        current = TimeMs{};
        // paused_time = TimeMs{}; // 不知道要不要处理这玩意儿
        alive = 0;
        cancelled = 0;
        fire_count_ = 0;
        // paused = false;
        ticking = false;
    }

    // 暂停/恢复
    void pause() noexcept { paused = true; }

    void resume() {
        paused = false;
        tick(paused_time);
        paused_time = 0;
    }

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