// event.hpp
#pragma once
#include <cstdint>
#include <functional>

namespace es {

using TimeMs = uint64_t;
using Callback = std::function<void()>;

enum class EventType : uint8_t { Once, Repeat };

struct EventDesc {
    EventType type;
    TimeMs delay_ms;
    TimeMs interval_ms; // 仅限 Repeat 使用
    Callback callback;
};

} // namespace es