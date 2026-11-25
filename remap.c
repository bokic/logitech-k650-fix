#include "remap.h"

#include <string.h>

static bool bit_is_set(const uint8_t *bits, size_t len, unsigned int code)
{
    size_t byte_idx = code / 8U;
    if (!bits || byte_idx >= len) {
        return false;
    }
    return ((bits[byte_idx] >> (code % 8U)) & 1U) != 0U;
}

static void set_bit_u8(uint8_t *bits, unsigned int code, bool val)
{
    size_t byte_idx = code / 8U;
    uint8_t mask = (uint8_t)(1U << (code % 8U));
    if (byte_idx >= K650_KEY_BYTES) {
        return;
    }
    if (val) {
        bits[byte_idx] |= mask;
    } else {
        bits[byte_idx] &= (uint8_t)~mask;
    }
}

static void emit_event(struct k650_state *st, struct k650_out *out, const struct input_event *ev)
{
    if (out->count < K650_OUT_CAP) {
        out->events[out->count++] = *ev;
    }

    if (ev->type == EV_KEY && (unsigned int)ev->code < KEY_CNT) {
        if (ev->value == 1) {
            set_bit_u8(st->virt_down, (unsigned int)ev->code, true);
        } else if (ev->value == 0) {
            set_bit_u8(st->virt_down, (unsigned int)ev->code, false);
        }
    }

    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        st->frame_emitted = false;
        st->frame_suppressed = false;
    } else {
        st->frame_emitted = true;
    }
}

static void emit_key(struct k650_state *st, struct k650_out *out, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    emit_event(st, out, &ev);
}

static void emit_syn(struct k650_state *st, struct k650_out *out)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    emit_event(st, out, &ev);
}

static void flush_pending(struct k650_state *st, struct k650_out *out)
{
    for (size_t i = 0; i < st->pending_len; i++) {
        emit_event(st, out, &st->pending[i]);
    }
    st->pending_len = 0;
    st->wait_mode = K650_WAIT_NONE;
    st->combo_frame_suppressed = false;
}

static void begin_wait(struct k650_state *st, enum k650_wait_mode mode, int64_t deadline_ns, const struct input_event *ev)
{
    st->wait_mode = mode;
    st->combo_frame_suppressed = false;
    st->wait_deadline_ns = deadline_ns;
    st->pending_len = 0;
    if (st->has_pending_msc) {
        st->pending[st->pending_len++] = st->pending_msc;
        st->has_pending_msc = false;
    }
    st->pending[st->pending_len++] = *ev;
}

void k650_state_init(struct k650_state *st)
{
    memset(st, 0, sizeof(*st));
}

int k650_timeout_ms(const struct k650_state *st, int64_t now_ns)
{
    if (st->wait_mode == K650_WAIT_NONE) {
        return -1;
    }
    if (now_ns >= st->wait_deadline_ns) {
        return 0;
    }
    int64_t rem_ns = st->wait_deadline_ns - now_ns;
    /* Round up to the next millisecond so poll() does not wake up slightly early. */
    int64_t rem_ms = (rem_ns + 999999LL) / 1000000LL;
    if (rem_ms > 1000LL) {
        rem_ms = 1000LL;
    }
    return (int)rem_ms;
}

void k650_expire_combo(struct k650_state *st, int64_t now_ns, struct k650_out *out)
{
    if (st->wait_mode != K650_WAIT_NONE && now_ns >= st->wait_deadline_ns) {
        flush_pending(st, out);
    }
}

void k650_process_event(struct k650_state *st, const struct input_event *in_ev, int64_t now_ns, struct k650_out *out)
{
    struct input_event ev = *in_ev;

    if (st->wait_mode != K650_WAIT_NONE && now_ns >= st->wait_deadline_ns) {
        flush_pending(st, out);
    }

    /* Drop physical autorepeat events; uinput's own EV_REP generates repeats. */
    if (ev.type == EV_KEY && ev.value == 2) {
        st->has_pending_msc = false;
        if (st->wait_mode != K650_WAIT_NONE) {
            st->combo_frame_suppressed = true;
        } else {
            st->frame_suppressed = true;
        }
        return;
    }

    if (st->wait_mode != K650_WAIT_NONE) {
        if (ev.type == EV_MSC && ev.code == MSC_SCAN) {
            if (st->has_pending_msc) {
                if (st->pending_len < K650_PENDING_CAP) {
                    st->pending[st->pending_len++] = st->pending_msc;
                } else {
                    flush_pending(st, out);
                    emit_event(st, out, &st->pending_msc);
                }
            }
            st->pending_msc = ev;
            st->has_pending_msc = true;
            return;
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (st->combo_frame_suppressed && !st->has_pending_msc) {
                st->combo_frame_suppressed = false;
                return;
            }
            st->combo_frame_suppressed = false;
            if (st->has_pending_msc) {
                if (st->pending_len + 2U <= K650_PENDING_CAP) {
                    st->pending[st->pending_len++] = st->pending_msc;
                    st->has_pending_msc = false;
                } else {
                    flush_pending(st, out);
                    emit_event(st, out, &st->pending_msc);
                    st->has_pending_msc = false;
                    emit_syn(st, out);
                    return;
                }
            }
            if (st->pending_len < K650_PENDING_CAP) {
                ev.value = 0;
                st->pending[st->pending_len++] = ev;
            } else {
                flush_pending(st, out);
                emit_syn(st, out);
            }
            return;
        }

        if (ev.type == EV_KEY) {
            if (st->wait_mode == K650_WAIT_SPACE_DOWN && ev.code == KEY_SPACE && ev.value == 1) {
                /* Matched initial Super+Space within 40 ms -> emit Insert press. */
                st->pending_len = 0;
                st->wait_mode = K650_WAIT_NONE;
                st->combo_frame_suppressed = false;
                st->has_pending_msc = false;

                emit_key(st, out, KEY_INSERT, 1);
                emit_syn(st, out);

                st->insert_down = true;
                st->meta_suppressed = true;
                st->frame_suppressed = true;
                return;
            }

            if (st->wait_mode == K650_WAIT_SPACE_UP && ev.code == KEY_SPACE && ev.value == 0) {
                /*
                 * Matched <=20 ms firmware SPACE=1 -> SPACE=0 pulse during the
                 * K650's ~1.2 s LEFTMETA hold (rapid double-tap of 'ins' or
                 * 'Shift+ins') -> emit complete Insert tap.
                 */
                st->pending_len = 0;
                st->wait_mode = K650_WAIT_NONE;
                st->combo_frame_suppressed = false;
                st->has_pending_msc = false;

                emit_key(st, out, KEY_INSERT, 1);
                emit_syn(st, out);
                emit_key(st, out, KEY_INSERT, 0);
                emit_syn(st, out);

                st->insert_down = false;
                st->frame_suppressed = true;
                return;
            }

            /* Any other key event cancels the wait: flush buffered events and fall through. */
            flush_pending(st, out);
        } else {
            if (st->pending_len < K650_PENDING_CAP) {
                st->pending[st->pending_len++] = ev;
                return;
            }
            flush_pending(st, out);
        }
    }

    if (ev.type == EV_MSC && ev.code == MSC_SCAN) {
        if (st->has_pending_msc) {
            emit_event(st, out, &st->pending_msc);
        }
        st->pending_msc = ev;
        st->has_pending_msc = true;
        return;
    }

    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        if (st->frame_suppressed && !st->frame_emitted) {
            st->frame_suppressed = false;
            st->has_pending_msc = false;
            return;
        }
        if (st->has_pending_msc) {
            emit_event(st, out, &st->pending_msc);
            st->has_pending_msc = false;
        }
        ev.value = 0;
        emit_event(st, out, &ev);
        return;
    }

    if (ev.type == EV_KEY) {
        if (ev.code == KEY_LEFTMETA) {
            if (st->meta_suppressed) {
                st->has_pending_msc = false;
                st->frame_suppressed = true;
                if (ev.value == 0) {
                    st->meta_suppressed = false;
                }
                return;
            }
            if (!st->insert_down && ev.value == 1) {
                begin_wait(st, K650_WAIT_SPACE_DOWN, now_ns + K650_COMBO_TIMEOUT_NS, &ev);
                return;
            }
        } else if (ev.code == KEY_SPACE) {
            if (st->insert_down) {
                st->has_pending_msc = false;
                ev.code = KEY_INSERT;
                if (ev.value == 0) {
                    st->insert_down = false;
                }
                emit_event(st, out, &ev);
                return;
            }
            if (st->meta_suppressed && ev.value == 1 &&
                !bit_is_set(st->virt_down, sizeof(st->virt_down), KEY_SPACE)) {
                begin_wait(st, K650_WAIT_SPACE_UP, now_ns + K650_PULSE_TIMEOUT_NS, &ev);
                return;
            }
        }
    }

    if (st->has_pending_msc) {
        emit_event(st, out, &st->pending_msc);
        st->has_pending_msc = false;
    }
    emit_event(st, out, &ev);
}

void k650_handle_syn_dropped(struct k650_state *st, const uint8_t *phys_keys, size_t phys_keys_len, struct k650_out *out)
{
    st->wait_mode = K650_WAIT_NONE;
    st->combo_frame_suppressed = false;
    st->pending_len = 0;
    st->has_pending_msc = false;

    if (st->insert_down) {
        emit_key(st, out, KEY_INSERT, 0);
        st->insert_down = false;
    }

    if (st->meta_suppressed && !bit_is_set(phys_keys, phys_keys_len, KEY_LEFTMETA)) {
        st->meta_suppressed = false;
    }

    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (bit_is_set(st->virt_down, sizeof(st->virt_down), code)) {
            if (code == KEY_INSERT || !bit_is_set(phys_keys, phys_keys_len, code)) {
                emit_key(st, out, (uint16_t)code, 0);
            }
        }
    }

    if (st->frame_emitted) {
        emit_syn(st, out);
    }
    st->frame_suppressed = false;
}

void k650_release_all(struct k650_state *st, struct k650_out *out)
{
    st->wait_mode = K650_WAIT_NONE;
    st->combo_frame_suppressed = false;
    st->pending_len = 0;
    st->has_pending_msc = false;
    st->insert_down = false;
    st->meta_suppressed = false;

    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (bit_is_set(st->virt_down, sizeof(st->virt_down), code)) {
            emit_key(st, out, (uint16_t)code, 0);
        }
    }

    if (st->frame_emitted) {
        emit_syn(st, out);
    }
    st->frame_suppressed = false;
}
