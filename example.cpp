#include "scheduler.hpp"
#include <iostream>

int main() {
    es::EventScheduler scheduler;

    scheduler.schedule({es::EventType::Once, 1000, 0, [] { std::cout << "once @ 1000ms\n"; }});

    scheduler.schedule({es::EventType::Repeat, 500, 500, [] { std::cout << "repeat every 500ms\n"; }});

    for (int i = 0; i < 10; ++i) { scheduler.tick(300); }
}