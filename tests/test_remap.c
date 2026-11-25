#include "remap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS(x) ((int64_t)(x) * 1000000LL)

static struct input_event make_ev(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ev;
}

static void feed_key_frame(struct k650_state *st, int64_t now_ns, struct k650_out *out,
                           uint16_t code, int32_t value, int32_t scancode)
{
    if (value != 2) {
        struct input_event msc = make_ev(EV_MSC, MSC_SCAN, scancode);
        k650_process_event(st, &msc, now_ns, out);
    }
    struct input_event kev = make_ev(EV_KEY, code, value);
    k650_process_event(st, &kev, now_ns, out);
    struct input_event syn = make_ev(EV_SYN, SYN_REPORT, value == 2 ? 1 : 0);
    k650_process_event(st, &syn, now_ns, out);
}

#define ASSERT_TRUE(cond)                                                              \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

/*
 * Real K650 hardware timing test:
 *   t=0ms:    LEFTMETA=1
 *   t=8ms:    SPACE=1       -> emits INSERT=1, SYN
 *   t=15ms:   SPACE=0       -> emits INSERT=0, SYN (no leaked MSC_SCAN!)
 *   t=200ms:  real SPACE=1  -> MUST emit SPACE=1 (not INSERT!), while LEFTMETA is still held by K650 firmware
 *   t=280ms:  real SPACE=0  -> MUST emit SPACE=0
 *   t=1208ms: LEFTMETA=0    -> suppressed cleanly with no empty SYN_REPORT
 */
static int test_k650_hardware_1200ms_meta_hold(void)
{
    struct k650_state st;
    struct k650_out out = { .count = 0 };
    k650_state_init(&st);

    feed_key_frame(&st, MS(0), &out, KEY_LEFTMETA, 1, 0x700e3);
    ASSERT_TRUE(out.count == 0);
    ASSERT_TRUE(k650_timeout_ms(&st, MS(0)) == 40);

    feed_key_frame(&st, MS(8), &out, KEY_SPACE, 1, 0x7002c);
    ASSERT_TRUE(out.count == 2);
    ASSERT_TRUE(out.events[0].type == EV_KEY && out.events[0].code == KEY_INSERT && out.events[0].value == 1);
    ASSERT_TRUE(out.events[1].type == EV_SYN && out.events[1].code == SYN_REPORT);
    out.count = 0;

    feed_key_frame(&st, MS(15), &out, KEY_SPACE, 0, 0x7002c);
    ASSERT_TRUE(out.count == 2);
    ASSERT_TRUE(out.events[0].type == EV_KEY && out.events[0].code == KEY_INSERT && out.events[0].value == 0);
    ASSERT_TRUE(out.events[1].type == EV_SYN && out.events[1].code == SYN_REPORT);
    out.count = 0;

    /* Normal Spacebar typing (80ms > 20ms) during the 1.2s K650 firmware Meta hold stays KEY_SPACE. */
    feed_key_frame(&st, MS(200), &out, KEY_SPACE, 1, 0x7002c);
    ASSERT_TRUE(out.count == 0);
    ASSERT_TRUE(k650_timeout_ms(&st, MS(200)) == 20);
    k650_expire_combo(&st, MS(220), &out);
    ASSERT_TRUE(out.count == 3);
    ASSERT_TRUE(out.events[1].type == EV_KEY && out.events[1].code == KEY_SPACE && out.events[1].value == 1);
    out.count = 0;

    feed_key_frame(&st, MS(280), &out, KEY_SPACE, 0, 0x7002c);
    ASSERT_TRUE(out.count == 3);
    ASSERT_TRUE(out.events[1].type == EV_KEY && out.events[1].code == KEY_SPACE && out.events[1].value == 0);
    out.count = 0;

    /* A second 7ms firmware 'ins' pulse at +500ms..+507ms (without new LEFTMETA=1) emits Insert tap. */
    feed_key_frame(&st, MS(500), &out, KEY_SPACE, 1, 0x7002c);
    ASSERT_TRUE(out.count == 0);
    feed_key_frame(&st, MS(507), &out, KEY_SPACE, 0, 0x7002c);
    ASSERT_TRUE(out.count == 4);
    ASSERT_TRUE(out.events[0].type == EV_KEY && out.events[0].code == KEY_INSERT && out.events[0].value == 1);
    ASSERT_TRUE(out.events[2].type == EV_KEY && out.events[2].code == KEY_INSERT && out.events[2].value == 0);
    out.count = 0;

    /* Delayed K650 firmware LEFTMETA=0 at +1208ms is suppressed with zero output events. */
    feed_key_frame(&st, MS(1208), &out, KEY_LEFTMETA, 0, 0x700e3);
    ASSERT_TRUE(out.count == 0);
    ASSERT_TRUE(!st.insert_down && !st.meta_suppressed && st.wait_mode == K650_WAIT_NONE);
    return 0;
}

static int test_shift_repeat_mid_combo_and_same_frame_release(void)
{
    struct k650_state st;
    struct k650_out out = { .count = 0 };
    k650_state_init(&st);

    feed_key_frame(&st, MS(0), &out, KEY_LEFTSHIFT, 1, 0x700e1);
    ASSERT_TRUE(out.count == 3);
    out.count = 0;

    feed_key_frame(&st, MS(290), &out, KEY_LEFTMETA, 1, 0x700e3);
    /* Shift autorepeat lands right inside the 8ms gap between LEFTMETA=1 and SPACE=1! */
    feed_key_frame(&st, MS(294), &out, KEY_LEFTSHIFT, 2, 0);
    feed_key_frame(&st, MS(298), &out, KEY_SPACE, 1, 0x7002c);
    ASSERT_TRUE(out.count == 2);
    ASSERT_TRUE(out.events[0].code == KEY_INSERT && out.events[0].value == 1);
    out.count = 0;

    feed_key_frame(&st, MS(305), &out, KEY_SPACE, 0, 0x7002c);
    ASSERT_TRUE(out.count == 2);
    ASSERT_TRUE(out.events[0].code == KEY_INSERT && out.events[0].value == 0);
    out.count = 0;

    /* Same-frame release: hid-input emits LEFTSHIFT=0 (0xE1) before LEFTMETA=0 (0xE3). */
    struct input_event evs[] = {
        make_ev(EV_MSC, MSC_SCAN, 0x700e1),
        make_ev(EV_KEY, KEY_LEFTSHIFT, 0),
        make_ev(EV_MSC, MSC_SCAN, 0x700e3),
        make_ev(EV_KEY, KEY_LEFTMETA, 0),
        make_ev(EV_SYN, SYN_REPORT, 0),
    };
    for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
        k650_process_event(&st, &evs[i], MS(500), &out);
    }
    /* LEFTSHIFT=0 + MSC_SCAN + SYN_REPORT must be committed; LEFTMETA=0 stripped. */
    ASSERT_TRUE(out.count == 3);
    ASSERT_TRUE(out.events[0].type == EV_MSC && out.events[0].value == 0x700e1);
    ASSERT_TRUE(out.events[1].type == EV_KEY && out.events[1].code == KEY_LEFTSHIFT && out.events[1].value == 0);
    ASSERT_TRUE(out.events[2].type == EV_SYN && out.events[2].code == SYN_REPORT);
    return 0;
}

static int test_syn_dropped_and_disconnect_release(void)
{
    struct k650_state st;
    struct k650_out out = { .count = 0 };
    k650_state_init(&st);

    feed_key_frame(&st, MS(0), &out, KEY_A, 1, 0x70004);
    feed_key_frame(&st, MS(10), &out, KEY_LEFTMETA, 1, 0x700e3);
    feed_key_frame(&st, MS(18), &out, KEY_SPACE, 1, 0x7002c);
    out.count = 0;

    /* Simulate SYN_DROPPED where KEY_A remained physically held, while Space/Meta released. */
    uint8_t phys[K650_KEY_BYTES];
    memset(phys, 0, sizeof(phys));
    phys[KEY_A / 8] |= (uint8_t)(1U << (KEY_A % 8));

    k650_handle_syn_dropped(&st, phys, sizeof(phys), &out);
    ASSERT_TRUE(out.count == 2);
    ASSERT_TRUE(out.events[0].type == EV_KEY && out.events[0].code == KEY_INSERT && out.events[0].value == 0);
    ASSERT_TRUE(out.events[1].type == EV_SYN && out.events[1].code == SYN_REPORT);
    ASSERT_TRUE(!st.insert_down && !st.meta_suppressed);
    out.count = 0;

    /* Disconnect releases still-held KEY_A on persistent virtual device. */
    k650_release_all(&st, &out);
    ASSERT_TRUE(out.count == 2);
    ASSERT_TRUE(out.events[0].type == EV_KEY && out.events[0].code == KEY_A && out.events[0].value == 0);
    ASSERT_TRUE(out.events[1].type == EV_SYN && out.events[1].code == SYN_REPORT);
    return 0;
}

int main(void)
{
    if (test_k650_hardware_1200ms_meta_hold() != 0) {
        return EXIT_FAILURE;
    }
    if (test_shift_repeat_mid_combo_and_same_frame_release() != 0) {
        return EXIT_FAILURE;
    }
    if (test_syn_dropped_and_disconnect_release() != 0) {
        return EXIT_FAILURE;
    }
    printf("All remap unit tests passed.\n");
    return EXIT_SUCCESS;
}
