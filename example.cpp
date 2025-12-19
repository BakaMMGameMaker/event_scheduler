// example.cpp
#include "event.hpp"
#include "event_id.hpp"
#include "scheduler.hpp"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <vcruntime_typeinfo.h>

using Scheduler = es::EventScheduler<>;
using EventID = es::EventID;
using TimeMode = es::TimeMode;
using EventType = es::EventType;

static void test1() {
    Scheduler scheduler;
    scheduler.schedule(1000, [] { std::cout << "once @ 1000ms\n"; });
    scheduler.schedule(500, [] { std::cout << "repeat every 500ms\n"; }, TimeMode::Relative, EventType::Repeat, 500);
    for (int i = 0; i < 10; ++i) { scheduler.tick(300); } // repeat500, once1000, 5xrepeat500
}

static void test2() {
    Scheduler scheduler;
    size_t cnt = 0;
    scheduler.schedule(100, [&] { ++cnt; }, TimeMode::Relative, EventType::Repeat, 100);
    scheduler.tick(10'000);
    assert(cnt == 100);
}

static void test3() {
    Scheduler scheduler;
    size_t cnt = 0;
    EventID id = scheduler.schedule(
        100,
        [&] {
            scheduler.cancel(id);
            ++cnt;
        },
        TimeMode::Relative, EventType::Repeat, 100);
    scheduler.tick(1000);
    assert(cnt == 1);
}

static void test4() {
    Scheduler scheduler;
    size_t cnt = 0;
    size_t cnt2 = 0;
    scheduler.schedule(100, [&] {
        ++cnt;
        scheduler.schedule(0, [&] { ++cnt2; });
    });
    scheduler.tick(100);
    assert(cnt == 1);
    assert(cnt2 == 0);
    scheduler.tick(0);
    assert(cnt2 == 1);
}

static void test5() {
    Scheduler scheduler;
    size_t cnt = 0;
    EventID id = scheduler.schedule(1000, [&] { ++cnt; });
    scheduler.cancel(id);
    scheduler.cancel(id);
    scheduler.tick(2000);
    assert(cnt == 0);
    assert(scheduler.size() == 0);
}

static void test6() {
    Scheduler scheduler;
    size_t cnt = 0;
    scheduler.schedule(1000, [&] { ++cnt; });
    scheduler.clear();
    assert(scheduler.now() == 0);
    scheduler.tick(2000);
    assert(cnt == 0);
    assert(scheduler.size() == 0);
    assert(scheduler.fire_count() == 0);
    scheduler.schedule(0, [&] { ++cnt; });
    assert(cnt == 0);
    scheduler.tick(0);
    assert(cnt == 1);
}

int main() {
    test1();
    test2();
    test3();
    test4();
    test5();
    test6();
}