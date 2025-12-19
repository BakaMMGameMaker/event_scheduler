#include "scheduler.hpp"
#include "event.hpp"
#include "event_id.hpp"
#include <cstddef>

namespace es {

void EventScheduler::pop_repeat() noexcept {
    EventID top = pq.top();
    pq.pop();
    EventDesc &desc = events[top];
    desc.delay_ms += desc.interval_ms;
    pq.push(top);
}

EventID EventScheduler::schedule(const EventDesc &desc) {
    size_t id = events.size();
    events.push_back(desc);
    EventID event_id{static_cast<EventID::ValueType>(id)};
    pq.push(event_id);
    return event_id;
}

void EventScheduler::tick(TimeMs delta_ms) {
    current += delta_ms;
    // 全部事件 - delta_ms，这不会破坏堆
    for (EventDesc &desc : events) {
        if (desc.delay_ms <= delta_ms) desc.delay_ms = 0;
        else desc.delay_ms -= delta_ms;
    }
    while (true) {
        EventID top = pq.top();
        const EventDesc &desc = events[top];
        if (desc.delay_ms > 0) break; // 堆顶事件还没到时间
        desc.callback();
        if (desc.type == EventType::Once) pq.pop();
        else pop_repeat();
    }
}

TimeMs EventScheduler::now() const noexcept { return current; }

size_t EventScheduler::size() const noexcept { return events.size(); }

void EventScheduler::clear() noexcept {
    events.clear();
    PQType tmp = PQType(EventCompare(events));
    pq.swap(tmp);
    current = {};
}

} // namespace es