// event.hpp
#pragma once
#include <cstdint>
#include <functional>

namespace es {

using TimeMs = int64_t;

enum class EventType : uint8_t { Once, Repeat };
enum class TimeMode : uint8_t { Relative, Absolute };
enum class ExceptionPolicy : uint8_t { Swallow, Cancel, Rethrow };
enum class EventPriority : uint8_t { System, User, Debug };
enum class CatchUp : uint8_t {
    All,   // 触发中间经过的全部事件
    Latest // 只触发最后一次事件
};

using DefaultCallback = std::function<void()>;

template <typename Callback = DefaultCallback> struct EventDesc {
    EventType type = EventType::Once;
    TimeMs interval_ms = TimeMs{}; // 仅限 Repeat 使用
    Callback callback{};
    ExceptionPolicy ep = ExceptionPolicy::Swallow;
    EventPriority pri = EventPriority::User;
    CatchUp cu = CatchUp::All;
};

} // namespace es