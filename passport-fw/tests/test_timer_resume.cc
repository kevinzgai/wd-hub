#include "launcher/timer_resume.h"
#include <cassert>
#include <cstdio>
int main() {
    stopwatch_t watch{}; pomodoro_t clock = POMODORO_INITIALIZER;
    stopwatch_toggle(&watch, 1000000); pomodoro_toggle(&clock, 1000000);
    stopwatch_lap(&watch, 3000000);
    timer_snapshot(watch, clock, 6000000);
    assert(watch.accumulated_us == 5000000 && watch.lap_count == 1);
    timer_rebase(watch, clock, 400000, 2000000);
    assert(stopwatch_elapsed(&watch, 1400000) == 8000000);
    assert(pomodoro_remaining_us(&clock, 400000) == POMODORO_FOCUS_US - 7000000);
    stopwatch_toggle(&watch, 1400000); pomodoro_toggle(&clock, 1400000);
    const auto elapsed = watch.accumulated_us, left = clock.remaining_us;
    timer_snapshot(watch, clock, 2400000); timer_rebase(watch, clock, 100, 9000000);
    assert(stopwatch_elapsed(&watch, 10000000) == elapsed && clock.remaining_us == left);
    clock.status = POMODORO_RUNNING; clock.remaining_us = 1000000;
    timer_rebase(watch, clock, 500, 2000000);
    assert(clock.status == POMODORO_FINISHED && clock.notice_pending && clock.completed == 1);
    assert(!pomodoro_poll(&clock, 1000000) && clock.completed == 1);
    puts("Timer restart: running/paused, elapsed downtime, laps, one overdue completion PASS");
}
