// example.cpp
#include "event.hpp"
#include "event_id.hpp"
#include "scheduler.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using Scheduler = es::EventScheduler<>;
using EventID = es::EventID;
using TimeMs = es::TimeMs;
using TimeMode = es::TimeMode;
using EventType = es::EventType;
using ExceptionPolicy = es::ExceptionPolicy;
using EventPriority = es::EventPriority;
using CatchUp = es::CatchUp;

static int g_failed = 0;

#define REQUIRE(expr)                                                                                                  \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  REQUIRE(" #expr ")\n";                         \
            std::terminate();                                                                                          \
        }                                                                                                              \
    } while (0)

#define EXPECT(expr)                                                                                                   \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  EXPECT(" #expr ")\n";                          \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

#define EXPECT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (!(_a == _b)) {                                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << "  EXPECT_EQ(" #a ", " #b ") " << " got (" << _a  \
                      << ") vs (" << _b << ")\n";                                                                      \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

static void print_summary() {
    if (g_failed == 0) std::cout << "[OK] all tests passed\n";
    else std::cout << "[WARN] failed checks: " << g_failed << "\n";
}

// -----------------------------
// helpers
// -----------------------------
struct Trace {
    std::vector<std::string> log;
    void push(std::string s) { log.emplace_back(std::move(s)); }
};

static std::string join(const std::vector<std::string> &v, const char *sep = ",") {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

static void expect_seq(const std::vector<std::string> &got, const std::vector<std::string> &want) {
    if (got != want) {
        std::cerr << "[FAIL] sequence mismatch\n";
        std::cerr << "  got : [" << join(got, ", ") << "]\n";
        std::cerr << "  want: [" << join(want, ", ") << "]\n";
        ++g_failed;
    }
}

// -----------------------------
// tests
// -----------------------------

// 1) 基础：relative once + repeat，同时刻 tie-break（index）
static void test_basic_order_and_tie_break() {
    Scheduler s;
    Trace t;

    // index 0
    s.schedule(1000, [&] { t.push("once@1000"); });

    // index 1, repeat 500ms
    s.schedule(500, [&] { t.push("repeat"); }, TimeMode::Relative, EventType::Repeat, 500);

    for (int i = 0; i < 10; ++i) s.tick(300); // current = 3000

    // repeat fires at 500,1000,1500,2000,2500,3000 => 6 times
    // once fires at 1000, and should happen BEFORE repeat at 1000 due to tie-break by smaller index.
    std::vector<std::string> want = {
        "repeat",    // 500
        "once@1000", // 1000 (index smaller)
        "repeat",    // 1000
        "repeat",    // 1500
        "repeat",    // 2000
        "repeat",    // 2500
        "repeat"     // 3000
    };
    expect_seq(t.log, want);
    EXPECT_EQ(s.now(), TimeMs(3000));
}

// 2) Absolute vs Relative
static void test_absolute_time() {
    Scheduler s;
    Trace t;

    s.schedule(100, [&] { t.push("rel+100"); }, TimeMode::Relative, EventType::Once);
    s.schedule(250, [&] { t.push("abs@250"); }, TimeMode::Absolute, EventType::Once);

    s.tick(99);
    EXPECT(t.log.empty());

    s.tick(1); // current=100
    EXPECT_EQ(t.log.size(), size_t(1));
    EXPECT_EQ(t.log[0], "rel+100");

    s.tick(149); // current=249
    EXPECT_EQ(t.log.size(), size_t(1));

    s.tick(1); // current=250
    EXPECT_EQ(t.log.size(), size_t(2));
    EXPECT_EQ(t.log[1], "abs@250");

    EXPECT_EQ(s.size(), size_t(0));
}

// 3) Priority：同一 next_fire，pri 小的先（System(0) > User(1) > Debug(2) 的相反顺序）
static void test_priority_order() {
    Scheduler s;
    Trace t;

    s.schedule(
        100, [&] { t.push("user"); }, TimeMode::Relative, EventType::Once, 0, ExceptionPolicy::Swallow,
        EventPriority::User);
    s.schedule(
        100, [&] { t.push("system"); }, TimeMode::Relative, EventType::Once, 0, ExceptionPolicy::Swallow,
        EventPriority::System);
    s.schedule(
        100, [&] { t.push("debug"); }, TimeMode::Relative, EventType::Once, 0, ExceptionPolicy::Swallow,
        EventPriority::Debug);

    s.tick(100);

    // System first, then User, then Debug
    std::vector<std::string> want = {"system", "user", "debug"};
    expect_seq(t.log, want);
}

// 4) tick(0) 语义：tick 内 schedule(0) 不会在同一次 tick 中触发
static void test_tick0_semantics_and_schedule_during_tick() {
    Scheduler s;
    Trace t;

    s.schedule(100, [&] {
        t.push("A");
        s.schedule(0, [&] { t.push("B"); }); // should NOT run in this tick(100)
    });

    s.tick(100);
    expect_seq(t.log, {"A"});

    s.tick(0);
    expect_seq(t.log, {"A", "B"});
}

// 5) callback 内 cancel 自己 (Repeat)：应该只触发一次
static void test_cancel_self_in_callback_repeat() {
    Scheduler s;
    size_t cnt = 0;
    EventID id = EventID::invalid();

    id = s.schedule(
        100,
        [&] {
            s.cancel(id);
            ++cnt;
        },
        TimeMode::Relative, EventType::Repeat, 100);

    s.tick(1000);
    EXPECT_EQ(cnt, size_t(1));
    EXPECT_EQ(s.size(), size_t(0));
}

// 6) ExceptionPolicy：Swallow + CancelEvent（Rethrow 见下方演示）
static void test_exception_policy_swallow_and_cancel_event() {
    // Swallow: repeat 仍会继续 reschedule
    {
        Scheduler s;
        size_t fired = 0;
        s.schedule(
            10,
            [&] {
                ++fired;
                throw std::runtime_error("boom");
            },
            TimeMode::Relative, EventType::Repeat, 10, ExceptionPolicy::Swallow, EventPriority::User);

        s.tick(100);
        // 10,20,...,100 => 10 times
        EXPECT_EQ(fired, size_t(10));
        EXPECT_EQ(s.size(), size_t(1)); // repeat still alive
    }

    // CancelEvent: repeat 第一次抛异常就会 cancel，然后不再继续
    {
        Scheduler s;
        size_t fired = 0;
        s.schedule(
            10,
            [&] {
                ++fired;
                throw std::runtime_error("boom");
            },
            TimeMode::Relative, EventType::Repeat, 10, ExceptionPolicy::CancelEvent, EventPriority::User);

        s.tick(100);
        EXPECT_EQ(fired, size_t(1));
        EXPECT_EQ(s.size(), size_t(0));
    }
}

// 7) pause/resume：暂停期间 tick 只累计时间，不触发；resume 一次性补上
static void test_pause_resume() {
    Scheduler s;
    size_t cnt = 0;

    s.schedule(100, [&] { ++cnt; }, TimeMode::Relative, EventType::Repeat, 100);

    s.tick(250); // fires at 100,200 => 2
    EXPECT_EQ(cnt, size_t(2));
    EXPECT_EQ(s.now(), TimeMs(250));

    s.pause();
    s.tick(450); // paused: no fire, current not advanced
    EXPECT_EQ(cnt, size_t(2));
    EXPECT_EQ(s.now(), TimeMs(250));

    s.resume(); // apply paused_time=450: current becomes 700, fires at 300,400,500,600,700 => 5 more
    EXPECT_EQ(s.now(), TimeMs(700));
    EXPECT_EQ(cnt, size_t(7));
}

// 8) cancelled > alive 触发 rebuild_pq，验证 free-list 复用 + gen 防旧ID
static void test_rebuild_and_generation_safety() {
    Scheduler s;

    // schedule 10 once events far in future
    std::vector<EventID> ids;
    ids.reserve(10);
    for (int i = 0; i < 10; ++i) {
        ids.push_back(s.schedule(10'000 + i, [] {}));
    }
    EXPECT_EQ(s.size(), size_t(10));

    // cancel 9 of them, leaving 1 alive => cancelled(9) > alive(1) triggers rebuild_pq inside cancel
    for (size_t i = 0; i < 9; ++i) s.cancel(ids[i]);
    EXPECT_EQ(s.size(), size_t(1));

    // Now schedule 9 new events, they should reuse the cancelled slots (indices among those 9)
    std::set<uint32_t> cancelled_indices;
    for (size_t i = 0; i < 9; ++i) cancelled_indices.insert(ids[i].index);

    std::set<uint32_t> reused_indices;
    std::vector<EventID> new_ids;
    new_ids.reserve(9);
    for (int i = 0; i < 9; ++i) {
        EventID nid = s.schedule(1 + i, [] {});
        new_ids.push_back(nid);
        reused_indices.insert(nid.index);
    }

    EXPECT_EQ(reused_indices.size(), cancelled_indices.size());
    EXPECT(reused_indices == cancelled_indices);

    // Old IDs should be stale (gen mismatch), cancelling them should NOT cancel the new ones
    for (size_t i = 0; i < 9; ++i) s.cancel(ids[i]);

    // Advance time enough to fire the 9 new ones
    s.tick(100);
    EXPECT_EQ(s.size(), size_t(1)); // the original alive future event still there

    // And the original alive one still triggers at 10000+
    size_t marker = 0;
    EventID far = ids[9];
    s.cancel(far); // make it quiet and end
    EXPECT_EQ(s.size(), size_t(0));
    (void)marker;
}

// 9) clear 重置行为 + schedule(0) 不立即触发
static void test_clear_resets() {
    Scheduler s;
    size_t cnt = 0;

    s.schedule(1000, [&] { ++cnt; });
    s.clear();

    EXPECT_EQ(s.now(), TimeMs(0));
    EXPECT_EQ(s.size(), size_t(0));
    EXPECT_EQ(s._fire_count(), size_t(0));

    s.tick(2000);
    EXPECT_EQ(cnt, size_t(0));

    s.schedule(0, [&] { ++cnt; });
    EXPECT_EQ(cnt, size_t(0));
    s.tick(0);
    EXPECT_EQ(cnt, size_t(1));
}

// 10) 轻 fuzz：只用 Once，随机 schedule/cancel/tick，最后推进到 horizon 结束应全部清空
static void test_fuzz_once_only() {
    Scheduler s;
    std::mt19937 rng(123456);
    std::uniform_int_distribution<int> op_dist(0, 2); // 0 schedule, 1 cancel, 2 tick
    std::uniform_int_distribution<int> delay_dist(0, 200);
    std::uniform_int_distribution<int> tick_dist(0, 50);

    struct Item {
        EventID id;
        bool alive;
        TimeMs fire_at;
    };
    std::vector<Item> items;
    items.reserve(2000);

    for (int step = 0; step < 500; ++step) {
        int op = op_dist(rng);

        if (op == 0) {
            TimeMs d = static_cast<TimeMs>(delay_dist(rng));
            TimeMs scheduled_now = s.now();
            EventID id = s.schedule(d, [] {});
            items.push_back(Item{id, true, scheduled_now + d});
        } else if (op == 1) {
            if (items.empty()) continue;
            std::uniform_int_distribution<size_t> pick(0, items.size() - 1);
            size_t i = pick(rng);
            if (items[i].id.is_valid() && s.is_alive(items[i].id)) s.cancel(items[i].id);
            items[i].alive = false;
        } else {
            s.tick(static_cast<TimeMs>(
                tick_dist(rng))); // 家人们，运气不好可是能 tick 到 2500 的，horizon = 1000 是想干什么
        }

        // 快速一致性检查：标记 alive 的 id 必须 is_alive==true（有误差：已经触发过的会变 false，所以只检查 fire_at >
        // now 的）
        TimeMs now = s.now();
        for (auto &it : items) {
            if (it.alive && it.fire_at > now) { EXPECT(s.is_alive(it.id)); }
        }
    }

    // 推进到 max_delay，确保所有 Once 都触发/或被 cancel
    s.tick(s.now() + 200);
    // 再给一次 tick(0) 把 flush_delay_ops 后 schedule(0) 可能留下的触发掉
    s.tick(0);
    EXPECT_EQ(s.size(), size_t(0));
}

// -----------------------------
// Known sharp edges / demos (disabled)
// -----------------------------

static void test_rethrow() {
    Scheduler s;
    s.schedule(
        10, [] { throw std::runtime_error("rethrow"); }, TimeMode::Relative, EventType::Once, 0,
        ExceptionPolicy::Rethrow, EventPriority::User);

    try {
        s.tick(10);
        REQUIRE(false && "should throw");
    } catch (...) {
        // Now scheduler might still think it's ticking
        size_t cnt = 0;
        s.schedule(0, [&] { ++cnt; });
        s.tick(0);
        s.tick(0);
        EXPECT_EQ(cnt, size_t(1));
    }
}

static void test_clear_then_schedule_in_same_tick() {
    Scheduler s;
    Trace t;

    EventID id_before = EventID::invalid();
    EventID id_after1 = EventID::invalid();
    EventID id_after2 = EventID::invalid();

    // 10ms 时触发，回调里做：
    // schedule(before-clear), clear, schedule(after-clear)
    s.schedule(10, [&] {
        t.push("A");

        id_before = s.schedule(0, [&] { t.push("BEFORE"); }); // should be invalidated by clear
        s.clear();                                            // delayed op (in tick)

        id_after1 = s.schedule(0, [&] { t.push("AFTER1"); }); // should survive
        id_after2 = s.schedule(0, [&] { t.push("AFTER2"); }); // should survive
    });

    s.tick(10);
    // tick 内 schedule(0) 不会在同一次 tick 触发，所以这里只看到 A
    expect_seq(t.log, {"A"});

    // flush 完后：before-clear 的 eid 应该 stale；after-clear 的 eid 应该 alive
    EXPECT(!s.is_alive(id_before));
    EXPECT(s.is_alive(id_after1));
    EXPECT(s.is_alive(id_after2));

    // 下一次 tick(0) 才会触发 AFTER1/AFTER2
    s.tick(0);

    // 顺序按 index / priority tie-break。我们不强行要求 AFTER1/AFTER2 的先后，只要它们都出现且 BEFORE 没出现。
    bool saw_before = false, saw_after1 = false, saw_after2 = false;
    for (auto &x : t.log) {
        if (x == "BEFORE") saw_before = true;
        if (x == "AFTER1") saw_after1 = true;
        if (x == "AFTER2") saw_after2 = true;
    }

    EXPECT(!saw_before);
    EXPECT(saw_after1);
    EXPECT(saw_after2);
    EXPECT_EQ(s.size(), size_t(0));
}

static void test_double_clear_then_schedule_in_same_tick() {
    Scheduler s;
    Trace t;

    EventID id_after = EventID::invalid();

    s.schedule(10, [&] {
        t.push("A");
        s.clear();
        s.clear();
        id_after = s.schedule(0, [&] { t.push("AFTER"); });
    });

    s.tick(10);
    expect_seq(t.log, {"A"});

    EXPECT(s.is_alive(id_after));
    s.tick(0);
    // AFTER 应该触发一次
    bool saw_after = false;
    for (auto &x : t.log)
        if (x == "AFTER") saw_after = true;
    EXPECT(saw_after);
    EXPECT_EQ(s.size(), size_t(0));
}

static void demo_assert_bug_repeat_latest_not_due() {
    Scheduler s;
    Trace t;

    // 未来 100ms 触发 repeat，Latest
    s.schedule_after(
        100, [&] { t.push("R"); }, EventType::Repeat, 50, ExceptionPolicy::Swallow, EventPriority::User,
        CatchUp::Latest);

    // 还没到 100ms，这里不应该做任何事，更不应该 assert 炸掉
    s.tick(10);
    EXPECT(t.log.empty());
}

int main() {
    test_basic_order_and_tie_break();
    test_absolute_time();
    test_priority_order();
    test_tick0_semantics_and_schedule_during_tick();
    test_cancel_self_in_callback_repeat();
    test_exception_policy_swallow_and_cancel_event();
    test_pause_resume();
    test_rebuild_and_generation_safety();
    test_clear_resets();
    test_fuzz_once_only();
    test_rethrow();
    test_clear_then_schedule_in_same_tick(); // <- 已经发现这里有段错误
    test_double_clear_then_schedule_in_same_tick();
    demo_assert_bug_repeat_latest_not_due();

    print_summary();

    // nonzero exit if there were EXPECT failures (REQUIRE terminates immediately)
    return g_failed == 0 ? 0 : 1;
}
