// scheduler.cpp
#include "scheduler.hpp"
#include "event.hpp"
#include "event_id.hpp"
#include <cstddef>

namespace es {

void EventScheduler::reschedule_top() noexcept {
    EventID top = pq.top();
    pq.pop();
    Entry &entry = entries[top];
    EventDesc &desc = entry.desc;
    desc.delay_ms += desc.interval_ms - entry.offset;
    entry.offset = 0; // 可能没用，安全起见
    pq.push(top);
}

void EventScheduler::prepare(Entry &e, TimeMs d) const noexcept {
    e.offset = d - e.desc.delay_ms;
    e.desc.delay_ms = 0;
}

EventID EventScheduler::schedule(const EventDesc &desc) {
    size_t id = entries.size();
    entries.push_back(Entry{desc, 0});
    EventID event_id{static_cast<EventID::ValueType>(id)};
    pq.push(event_id);
    return event_id;
}

void EventScheduler::tick(TimeMs delta_ms) {
    current += delta_ms;
    // 全部事件 - delta_ms，这不会破坏堆
    for (Entry &entry : entries) {
        EventDesc &desc = entry.desc;
        if (desc.delay_ms <= delta_ms) prepare(entry, delta_ms);
        else desc.delay_ms -= delta_ms;
    }
    while (true) {
        EventID top = pq.top();
        const Entry &entry = entries[top];
        const EventDesc &desc = entry.desc;
        if (desc.delay_ms > 0) break; // 堆顶事件还没到时间
        desc.callback();
        if (desc.type == EventType::Once) pq.pop();
        else reschedule_top();
    }
}

TimeMs EventScheduler::now() const noexcept { return current; }

size_t EventScheduler::size() const noexcept { return entries.size(); }

void EventScheduler::clear() noexcept {
    entries.clear();
    PQType tmp = PQType(EventCompare(entries));
    pq.swap(tmp);
    current = {};
}

} // namespace es