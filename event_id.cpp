// event_id.cpp
#include "event_id.hpp"

namespace es {

EventID EventID::invalid() noexcept { return EventID{InvalidID}; }

bool EventID::is_valid() const noexcept { return value != InvalidID; }

bool EventID::operator==(const EventID &rhs) const noexcept { return value == rhs.value; }

bool EventID::operator!=(const EventID &rhs) const noexcept { return !(*this == rhs); }

} // namespace es