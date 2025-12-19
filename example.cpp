// example.cpp
#include "event.hpp"
#include "event_id.hpp"
#include "scheduler.hpp"
#include <cassert>
#include <cstddef>
#include <iostream>

using Scheduler = es::EventScheduler<>;
using EventID = es::EventID;
using EventType = es::EventType;

static void test1() {
    Scheduler scheduler;
    scheduler.schedule(EventType::Once, 1000, 0, [] { std::cout << "once @ 1000ms\n"; });
    scheduler.schedule(EventType::Repeat, 500, 500, [] { std::cout << "repeat every 500ms\n"; });
    for (int i = 0; i < 10; ++i) { scheduler.tick(300); } // repeat500, once1000, 5xrepeat500
}

static void test2() {
    Scheduler scheduler;
    size_t cnt = 0;
    scheduler.schedule(EventType::Repeat, 100, 100, [&] { ++cnt; });
    scheduler.tick(10'000);
    assert(cnt == 100);
}

static void test3() {
    Scheduler scheduler;
    size_t cnt = 0;
    EventID id = scheduler.schedule(EventType::Repeat, 100, 100, [&] {
        scheduler.cancel(id);
        ++cnt;
    });
    scheduler.tick(1000);
    assert(cnt == 1);
}

static void test4() {
    Scheduler scheduler;
    size_t cnt = 0;
    size_t cnt2 = 0;
    scheduler.schedule(EventType::Once, 100, 0, [&] {
        ++cnt;
        scheduler.schedule(EventType::Once, 0, 0, [&] { ++cnt2; });
    });
    scheduler.tick(100);
    assert(cnt == 1);
    assert(cnt2 == 1);
}

static void test5() {
    Scheduler scheduler;
    size_t cnt = 0;
    EventID id = scheduler.schedule(EventType::Once, 1000, 0, [&] { ++cnt; });
    scheduler.cancel(id);
    scheduler.tick(2000);
    assert(cnt == 0);
    assert(scheduler.size() == 0);
}

static void test6() {
    Scheduler scheduler;
    size_t cnt = 0;
    scheduler.schedule(EventType::Once, 1000, 0, [&] { ++cnt; });
    scheduler.clear();
    scheduler.tick(2000);
    assert(cnt == 0);
    assert(scheduler.size() == 0);
}

int main() {
    test1();
    test2();
    test3();
    test4();
    test5();
    test6();
}