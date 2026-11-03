#include "apps/pomodoro/pomodoro_core.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void finish(pomodoro_t *s, uint64_t *now) {
    assert(s->status == POMODORO_RUNNING);
    *now = s->deadline_us;
    assert(pomodoro_poll(s, *now));
    assert(!pomodoro_poll(s, *now + 1000000));
    assert(s->notice_pending && s->sound_pending && s->status == POMODORO_FINISHED);
}

int main(void) {
    pomodoro_t s = POMODORO_INITIALIZER;
    assert(sizeof(s) <= 32);
    assert(pomodoro_remaining_seconds(&s, 0) == 1500);
    assert(!pomodoro_poll(&s, 9000000000ULL));
    uint64_t now = 1000000;
    pomodoro_toggle(&s, now);
    assert(pomodoro_remaining_seconds(&s, now + 1) == 1500);
    assert(pomodoro_remaining_seconds(&s, now + 1000000) == 1499);
    assert(!pomodoro_select(&s, 1));
    now += 10000001;
    pomodoro_toggle(&s, now);
    const uint64_t paused = s.remaining_us;
    assert(s.status == POMODORO_PAUSED && !pomodoro_select(&s, -1));
    now += 3600000000ULL;
    assert(pomodoro_remaining_us(&s, now) == paused && !pomodoro_poll(&s, now));
    pomodoro_toggle(&s, now);
    assert(s.deadline_us == now + paused);
    /* Switching screens and light sleep do not participate in the state model. */
    now = s.deadline_us + 7200000000ULL;
    assert(pomodoro_poll(&s, now));
    assert(s.completed == 1 && !pomodoro_poll(&s, now + 86400000000ULL));
    assert(pomodoro_next_phase(&s) == POMODORO_SHORT_BREAK);
    assert(pomodoro_remaining_seconds(&s, now) == 0);
    pomodoro_toggle(&s, now);
    assert(s.phase == POMODORO_SHORT_BREAK && !s.notice_pending && !s.sound_pending);
    assert(pomodoro_remaining_seconds(&s, now) == 300);
    finish(&s, &now);
    assert(s.completed == 1 && pomodoro_next_phase(&s) == POMODORO_FOCUS);
    for (unsigned round = 2; round <= 4; ++round) {
        pomodoro_toggle(&s, now);
        assert(s.phase == POMODORO_FOCUS);
        finish(&s, &now);
        assert(s.completed == round);
        assert(pomodoro_next_phase(&s) == (round == 4 ? POMODORO_LONG_BREAK : POMODORO_SHORT_BREAK));
        pomodoro_toggle(&s, now);
        assert(pomodoro_remaining_seconds(&s, now) == (round == 4 ? 900 : 300));
        finish(&s, &now);
    }
    assert(s.completed == 4);
    pomodoro_reset(&s);
    assert(s.status == POMODORO_READY && !s.notice_pending && !s.sound_pending && s.completed == 4);
    assert(pomodoro_select(&s, -1) && s.phase == POMODORO_SHORT_BREAK);
    assert(pomodoro_select(&s, -1) && s.phase == POMODORO_FOCUS);
    assert(pomodoro_select(&s, -1) && s.phase == POMODORO_LONG_BREAK);
    assert(!pomodoro_select(&s, 0));
    s.phase = POMODORO_FOCUS; s.completed = UINT32_MAX;
    pomodoro_reset(&s); pomodoro_toggle(&s, now); finish(&s, &now);
    assert(s.completed == UINT32_MAX);
    printf("Pomodoro: %zu-byte state, monotonic timing, background/sleep, pause, exact completion, manual phases, four-round long break and reset PASS\n", sizeof(s));
}
