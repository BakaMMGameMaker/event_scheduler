// event.hpp
#pragma once
#include <cstdint>
#include <functional>

namespace es {

using TimeMs = uint64_t;

enum class EventType : uint8_t { Once, Repeat };

using DefaultCallback = std::function<void()>;

template <typename Callback = DefaultCallback> struct EventDesc {
    EventType type;
    TimeMs delay_or_abs_ms;
    TimeMs interval_ms; // 仅限 Repeat 使用
    Callback callback;
};

} // namespace es