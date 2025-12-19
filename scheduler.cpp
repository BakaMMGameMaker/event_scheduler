// scheduler.cpp
#include "scheduler.hpp"
#include "event.hpp"
#include "event_id.hpp"
#include <cstddef>
#include <iostream>

namespace es {

void EventScheduler::reschedule(EventID id, Event &e) noexcept {
    e.next_fire += e.desc.interval_ms;
    pq.push(id);
}

void EventScheduler::fire_top() {
    EventID top = pq.top();
    pq.pop();
    Event &e = events[top];
    EventDesc &desc = e.desc;

    if (desc.type == EventType::Repeat) reschedule(top, e);
    if (e.status != EventStatus::Alive) return;

    desc.callback();
}

void EventScheduler::print_top(TimeMs delta_ms) const noexcept {
    EventID top = pq.top();
    const Event &e = events[top];
    std::cout << "tick:                " << delta_ms << std::endl;
    std::cout << "top event id:        " << top << std::endl;
    std::cout << "top event status:    " << (e.status == EventStatus::Alive ? "Alive" : "Cancelled") << std::endl;
    std::cout << "top event next fire: " << e.next_fire << std::endl;
}

EventID EventScheduler::schedule(const EventDesc &desc) {
    size_t id = events.size();
    events.push_back(Event{desc, EventStatus::Alive, desc.delay_ms});
    EventID event_id{static_cast<EventID::ValueType>(id)};
    pq.push(event_id);
    ++alive;
    return event_id;
}

void EventScheduler::cancel(EventID id) noexcept {
    events[id].status = EventStatus::Cancelled;
    --alive;
}

void EventScheduler::tick(TimeMs delta_ms) {
    if (delta_ms == 0) return;
    current += delta_ms;
    while (true) {
        EventID top = pq.top();
        const Event &e = events[top];
        if (current < e.next_fire) break;
        fire_top();
    }
}

TimeMs EventScheduler::now() const noexcept { return current; }

size_t EventScheduler::size() const noexcept { return alive; }

void EventScheduler::clear() noexcept {
    events.clear();
    PQType tmp = PQType(EventCompare(events));
    pq.swap(tmp);
    current = {};
}

} // namespace es