/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Button and stick hold counts at any frame rate (input_timing.h). Based on b3ndoi's 60 FPS
 * input timing for EU 01.00. */
#ifdef _WIN32
#include <windows.h>
#include "input_timing.h"

typedef struct TlgInputContext {
    void *sensor;
    float dt;
    struct TlgInputContext *previous;
} TlgInputContext;
typedef struct {
    void *sensor;
    unsigned active;
    TlgInputHold buttons[TLG_INPUT_BUTTONS];
    TlgInputHold sticks[TLG_INPUT_STICKS];
} TlgInputHistory;

static TlgInputUpdate original_update;
static __thread TlgInputContext *current_input;
static SRWLOCK history_lock = SRWLOCK_INIT;
static TlgInputHistory histories[128];

void tlg_input_configure(TlgInputUpdate original) {
    original_update = original; /* once, before guest code runs */
}

void TLG_INPUT_ABI tlg_input_update(void *sensor, float dt) {
    /* dt is taken here: the original reuses its saved-XMM stack slot. Nested updates keep their
     * own context; the copy hook only accepts its own sensor, source and destination. */
    TlgInputContext context = {sensor, dt, current_input};
    current_input = &context;
    original_update(sensor, dt);
    current_input = context.previous;
}

void *TLG_INPUT_ABI tlg_input_copy(void *destination, const void *source, size_t size) {
    memcpy(destination, source, size);
    const TlgInputContext *context = current_input;
    if (!context || size != TLG_INPUT_SNAPSHOT_SIZE ||
        destination != (unsigned char *)context->sensor + TLG_INPUT_SNAPSHOT ||
        source != (unsigned char *)context->sensor + TLG_INPUT_LIVE) return destination;

    unsigned char *sensor = context->sensor;
    uint32_t raw[TLG_INPUT_BUTTONS], remap[TLG_INPUT_BUTTONS], sticks[TLG_INPUT_STICKS];
    memcpy(raw, sensor + TLG_INPUT_LIVE_COUNTS, sizeof(raw));
    memcpy(remap, sensor + TLG_INPUT_REMAP_COUNTS, sizeof(remap));
    unsigned active = 0;
    for (unsigned i = 0; i < TLG_INPUT_BUTTONS; ++i) active |= raw[i];
    for (unsigned i = 0; i < TLG_INPUT_STICKS; ++i) {
        memcpy(&sticks[i], sensor + TLG_INPUT_STICK_LIVE +
            i * TLG_INPUT_STICK_STRIDE + TLG_INPUT_STICK_AGE, 4);
        active |= sticks[i];
    }

    /* This runs inside the game's own sensor lock (the memcpy is before its unlock), so the graph
     * never sees a half-corrected snapshot. No allocation or logging on the input path; no host
     * lock is held while guest code runs. */
    AcquireSRWLockExclusive(&history_lock);
    TlgInputHistory *history = NULL, *available = NULL;
    for (unsigned i = 0; i < sizeof(histories) / sizeof(histories[0]); ++i) {
        if (histories[i].sensor == sensor) { history = &histories[i]; break; }
        if (!histories[i].active && !available) available = &histories[i];
    }
    if (!history && active && available) {
        history = available;
        memset(history, 0, sizeof(*history));
        history->sensor = sensor;
    }
    /* No free slot: that sensor keeps the game's own snapshot. */
    if (history) {
        history->active = active;
        for (unsigned i = 0; i < TLG_INPUT_BUTTONS; ++i) {
            int32_t count = tlg_input_hold_step(&history->buttons[i], raw[i], context->dt);
            memcpy(sensor + TLG_INPUT_SNAPSHOT_COUNTS + i * 4, &count, 4);
            /* The remapped counts are copies of the raw ones, zeroed for disabled buttons; any
             * other value is left as the game made it. */
            if (remap[i] == raw[i])
                memcpy(sensor + TLG_INPUT_SNAPSHOT_REMAP + i * 4, &count, 4);
        }
        /* The six stick blocks were copied before this memcpy, under the same lock: only their
         * hold age changes. */
        for (unsigned i = 0; i < TLG_INPUT_STICKS; ++i) {
            int32_t count = tlg_input_hold_step(&history->sticks[i], sticks[i], context->dt);
            memcpy(sensor + TLG_INPUT_STICK_SNAPSHOT +
                i * TLG_INPUT_STICK_STRIDE + TLG_INPUT_STICK_AGE, &count, 4);
        }
    }
    ReleaseSRWLockExclusive(&history_lock);
    return destination;
}
#else
typedef int tlg_input_timing_windows_only;
#endif
