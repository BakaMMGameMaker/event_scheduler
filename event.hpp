// event.hpp
#pragma once
#include <cstdint>
#include <functional>

namespace es {

using TimeMs = uint64_t;

enum class EventType : uint8_t { Once, Repeat };
enum class TimeMode : uint8_t { Relative, Absolute };
enum class ExceptionPolicy : uint8_t { Swallow, CancelEvent, Rethrow };
enum class EventPriority : uint8_t { System, User, Debug };

using DefaultCallback = std::function<void()>;

template <typename Callback = DefaultCallback> struct EventDesc {
    EventType type = EventType::Once;
    TimeMs interval_ms = TimeMs{}; // 仅限 Repeat 使用
    Callback callback = std::function<void()>{};
    ExceptionPolicy ep = ExceptionPolicy::Swallow;
    EventPriority pri = EventPriority::User;
};

} // namespace es