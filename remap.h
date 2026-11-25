#ifndef K650_REMAP_H
#define K650_REMAP_H

#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * K650 hardware timing (verified via evdev capture):
 *   - Initial 'ins' press: KEY_LEFTMETA=1 followed ~7-8 ms later by KEY_SPACE=1,
 *     then ~7-8 ms later by KEY_SPACE=0, while KEY_LEFTMETA stays 1 for ~1.1-1.4 s.
 *   - Subsequent 'ins' press within that ~1.2 s window: firmware emits another
 *     7-8 ms KEY_SPACE=1 -> KEY_SPACE=0 pulse without re-sending KEY_LEFTMETA=1.
 *   - Human Spacebar taps on K650 last ~80-210 ms.
 *
 * K650_COMBO_TIMEOUT_NS (40 ms) captures the initial LEFTMETA=1 -> SPACE=1 gap.
 * K650_PULSE_TIMEOUT_NS (20 ms) distinguishes a <=20 ms firmware 'ins' repeat
 * pulse (-> KEY_INSERT) from a >20 ms human Spacebar press (-> KEY_SPACE) while
 * meta_suppressed is active.
 */
#define K650_COMBO_TIMEOUT_NS (40LL * 1000000LL)
#define K650_PULSE_TIMEOUT_NS (20LL * 1000000LL)
#define K650_PENDING_CAP      16
#define K650_OUT_CAP          128
#define K650_KEY_BYTES        ((KEY_CNT + 7) / 8)

enum k650_wait_mode {
    K650_WAIT_NONE = 0,
    K650_WAIT_SPACE_DOWN, /* buffered LEFTMETA=1; waiting <=40 ms for SPACE=1 */
    K650_WAIT_SPACE_UP,   /* meta_suppressed active, buffered SPACE=1; waiting <=20 ms for SPACE=0 */
};

struct k650_out {
    struct input_event events[K650_OUT_CAP];
    size_t count;
};

struct k650_state {
    enum k650_wait_mode wait_mode;
    bool combo_frame_suppressed;
    int64_t wait_deadline_ns;
    struct input_event pending[K650_PENDING_CAP];
    size_t pending_len;

    bool insert_down;
    bool meta_suppressed;

    bool has_pending_msc;
    struct input_event pending_msc;

    bool frame_emitted;
    bool frame_suppressed;

    uint8_t virt_down[K650_KEY_BYTES];
};

void k650_state_init(struct k650_state *st);

/* Returns remaining milliseconds until active wait timeout, or -1 if none. */
int k650_timeout_ms(const struct k650_state *st, int64_t now_ns);

/* Flushes buffered events if now_ns >= wait_deadline_ns. */
void k650_expire_combo(struct k650_state *st, int64_t now_ns, struct k650_out *out);

/* Processes a single input_event (other than SYN_DROPPED) at monotonic timestamp now_ns. */
void k650_process_event(struct k650_state *st, const struct input_event *in_ev, int64_t now_ns, struct k650_out *out);

/* Recovers state after EV_SYN / SYN_DROPPED using the physical EVIOCGKEY bitmap (may be NULL). */
void k650_handle_syn_dropped(struct k650_state *st, const uint8_t *phys_keys, size_t phys_keys_len, struct k650_out *out);

/* Releases any keys currently held down on the virtual keyboard (e.g. on USB disconnect). */
void k650_release_all(struct k650_state *st, struct k650_out *out);

#endif /* K650_REMAP_H */
