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
        bool operator()(const EventID &lhs, const EventID &rhs) const {
            // 小根堆
            const Event &le = events[lhs];
            const Event &re = events[rhs];
            // fire time 一致，按照先后顺序
            if (le.next_fire == re.next_fire) return lhs.value > rhs.value;
            return le.next_fire > re.next_fire;
        }
        friend void swap(EventCompare &, EventCompare &) noexcept {}
    };

    using PQ = std::priority_queue<EventID, std::vector<EventID>, EventCompare>;

private:
    EventID add_event(const Desc &desc, TimeMs off) {
        size_t id = events.size();
        events.push_back(Event{desc, EventStatus::Alive, off + desc.delay_or_abs_ms});
        EventID event_id{static_cast<EventID::ValueType>(id)};
        pq.push(event_id);
        ++alive;
        return event_id;
    }

    void reschedule(EventID id, Event &e) noexcept {
        e.next_fire += e.desc.interval_ms;
        pq.push(id);
    }

    void fire_top() {
        ++fire_count_;
        EventID top = pq.top();
        pq.pop();
        Event &e = events[top];
        Desc &desc = e.desc;

        if (e.status != EventStatus::Alive) return;
        if (desc.type == EventType::Repeat) reschedule(top, e);

        desc.callback();
    }

    void print_top(TimeMs delta_ms) const noexcept {
        EventID top = pq.top();
        const Event &e = events[top];
        std::cout << "tick:                " << delta_ms << std::endl;
        std::cout << "top event id:        " << top << std::endl;
        std::cout << "top event status:    " << (e.status == EventStatus::Alive ? "Alive" : "Cancelled") << std::endl;
        std::cout << "top event next fire: " << e.next_fire << std::endl;
    }

public:
    EventScheduler() : events(), pq(EventCompare(events)) {}
    ~EventScheduler() {}

    EventScheduler(const EventScheduler &) = delete;
    EventScheduler &operator=(const EventScheduler &) = delete;

    // 添加事件
    template <typename F> EventID schedule(EventType type, TimeMs delay_or_abs_ms, TimeMs interval_ms, F &&f) {
        static_assert(std::is_invocable_r_v<void, F &>, "callback must be invocable with signature void()");
        Desc desc{type, delay_or_abs_ms, interval_ms, Callback(std::forward<F>(f))};
        return add_event(desc, current);
    }
    template <typename F> EventID schedule_at(EventType type, TimeMs delay_or_abs_ms, TimeMs interval_ms, F &&f) {
        static_assert(std::is_invocable_r_v<void, F &>, "callback must be invocable with signature void()");
        Desc desc{type, delay_or_abs_ms, interval_ms, Callback(std::forward<F>(f))};
        return add_event(desc, 0);
    }

    // 取消事件
    void cancel(EventID id) noexcept {
        if (!id.is_valid() || id >= events.size()) return;
        Event &e = events[id];
        if (e.status == EventStatus::Cancelled) return;
        events[id].status = EventStatus::Cancelled;
        --alive;
    }

    // 推进时间
    void tick(TimeMs delta_ms) {
        if (paused) return;
        if (delta_ms == 0) return;
        current += delta_ms;
        while (!pq.empty()) {
            EventID top = pq.top();
            const Event &e = events[top];
            if (current < e.next_fire) break;
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
        events.clear();
        PQ tmp = PQ(EventCompare(events));
        pq.swap(tmp);
        current = {};
        alive = 0;
        fire_count_ = 0;
    }

    // 暂停/恢复
    void pause() noexcept { paused = true; }
    void resume() noexcept { paused = false; }

private:
    Events events;
    PQ pq;
    TimeMs current{};
    size_t alive{};
    size_t fire_count_{};
    bool paused = false;
};

} // namespace es