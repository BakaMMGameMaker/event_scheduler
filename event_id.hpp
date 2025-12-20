// event_id.hpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace es {

struct EventID {
    static constexpr uint32_t u32max = std::numeric_limits<uint32_t>::max();
    uint32_t index = u32max;
    uint32_t gen = u32max;
    static EventID invalid() noexcept { return EventID{u32max, u32max}; }
    bool is_valid() const noexcept { return index != u32max && gen != u32max; }
    bool operator==(const EventID &rhs) const noexcept { return index == rhs.index && gen == rhs.gen; }
    bool operator!=(const EventID &rhs) const noexcept { return !(*this == rhs); }
    operator size_t() const noexcept { return static_cast<size_t>(index); }
};

} // namespace es