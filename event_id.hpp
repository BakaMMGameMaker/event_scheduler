// event_id.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace es {

struct EventID {
    using ValueType = uint64_t;
    static constexpr ValueType InvalidID = std::numeric_limits<ValueType>::max();
    ValueType value;
    static EventID invalid() noexcept { return EventID{InvalidID}; }
    bool is_valid() const noexcept { return value != InvalidID; }
    bool operator==(const EventID &rhs) const noexcept { return value == rhs.value; }
    bool operator!=(const EventID &rhs) const noexcept { return !(*this == rhs); }
    operator size_t() const noexcept { return static_cast<size_t>(value); }
};

} // namespace es