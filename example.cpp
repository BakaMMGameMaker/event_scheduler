// example.cpp
#include "event_id.hpp"
#include "scheduler.hpp"
#include <iostream>

int main() {

    es::EventScheduler scheduler;

    es::EventID first = scheduler.schedule({es::EventType::Once, 1000, 0, [] { std::cout << "once @ 1000ms\n"; }});

    es::EventID second =
        scheduler.schedule({es::EventType::Repeat, 500, 500, [] { std::cout << "repeat every 500ms\n"; }});

    (void)first;
    (void)second;

    // for (int i = 0; i < 10; ++i) { scheduler.tick(300); } // repeat500, once1000, 5xrepeat500

    // scheduler.tick(1400); // repeat500, once1000, repeat500

    for (int i = 0; i < 10; ++i) {
        if (i == 5) scheduler.cancel(second);
        scheduler.tick(300);
    } // repeat500, once1000, 2xrepeat500
}