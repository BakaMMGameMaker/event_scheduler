// scheduler.hpp
#pragma once
#include "event.hpp"
#include "event_id.hpp"
#include <cstddef>
#include <queue>
#include <vector>

namespace es {
class EventScheduler {

    struct Entry {
        EventDesc desc;
        TimeMs offset{};
    };

    using EventPool = std::vector<Entry>;
    struct EventCompare {
        const EventPool &pool_ref;
        explicit EventCompare(const EventPool &pool) : pool_ref(pool) {}
        bool operator()(const EventID &lhs, const EventID &rhs) const {
            // min heap
            return pool_ref[lhs].desc.delay_ms > pool_ref[rhs].desc.delay_ms;
        }
        friend void swap(EventCompare &, EventCompare &) noexcept {}
    };
    using PQType = std::priority_queue<EventID, std::vector<EventID>, EventCompare>;

private:
    void reschedule_top() noexcept;
    void prepare(Entry &e, TimeMs d) const noexcept;

public:
    EventScheduler() : entries(), pq(EventCompare(entries)), current() {}
    ~EventScheduler() {}

    EventScheduler(const EventScheduler &) = delete;
    EventScheduler &operator=(const EventScheduler &) = delete;

    // 添加事件
    EventID schedule(const EventDesc &desc);

    // 推进时间
    void tick(TimeMs delta_ms);

    // 当前调度器时间
    TimeMs now() const noexcept;

    // 事件数量
    size_t size() const noexcept;

    // 清空所有事件
    void clear() noexcept;

private:
    EventPool entries;
    PQType pq;
    TimeMs current{};
};

} // namespace es