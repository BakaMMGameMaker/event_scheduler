// scheduler.hpp
#pragma once
#include "event.hpp"
#include "event_id.hpp"
#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

namespace es {

enum class EventStatus : uint8_t { Alive, Cancelled };

class EventScheduler {
    struct Event {
        EventDesc desc;
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
            return events[lhs].desc.delay_ms > events[rhs].desc.delay_ms;
        }
        friend void swap(EventCompare &, EventCompare &) noexcept {}
    };
    using PQType = std::priority_queue<EventID, std::vector<EventID>, EventCompare>;

private:
    void reschedule(EventID id, Event &e) noexcept;
    void fire_top();
    void print_top(TimeMs delta_ms) const noexcept;

public:
    EventScheduler() : events(), pq(EventCompare(events)), current(), alive() {}
    ~EventScheduler() {}

    EventScheduler(const EventScheduler &) = delete;
    EventScheduler &operator=(const EventScheduler &) = delete;

    // 添加事件
    EventID schedule(const EventDesc &desc);

    // 取消事件
    void cancel(EventID id) noexcept;

    // 推进时间
    void tick(TimeMs delta_ms);

    // 当前调度器时间
    TimeMs now() const noexcept;

    // 事件数量
    size_t size() const noexcept;

    // 清空所有事件
    void clear() noexcept;

private:
    Events events;
    PQType pq;
    TimeMs current{};
    size_t alive;
};

} // namespace es