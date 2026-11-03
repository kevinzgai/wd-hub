#pragma once
#include "apps/stopwatch/stopwatch_core.h"
#include "apps/pomodoro/pomodoro_core.h"

inline void timer_snapshot(stopwatch_t &watch, pomodoro_t &clock, uint64_t now) {
    watch.accumulated_us = stopwatch_elapsed(&watch, now);
    watch.started_us = 0;
    clock.remaining_us = pomodoro_remaining_us(&clock, now);
    clock.deadline_us = 0;
}
inline void timer_rebase(stopwatch_t &watch, pomodoro_t &clock, uint64_t now, uint64_t downtime) {
    if (watch.running) { watch.accumulated_us += downtime; watch.started_us = now; }
    if (clock.status == POMODORO_RUNNING) {
        const uint64_t left = clock.remaining_us > downtime ? clock.remaining_us - downtime : 0;
        clock.deadline_us = now + left;
        clock.remaining_us = left;
        pomodoro_poll(&clock, now);
    }
}
void timers_save_for_restart();
void timers_restore_after_restart();
void stopwatch_export_state(stopwatch_t *out);
void stopwatch_import_state(const stopwatch_t &state);
void pomodoro_export_state(pomodoro_t *out);
void pomodoro_import_state(const pomodoro_t &state);
