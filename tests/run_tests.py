#!/usr/bin/env python3
"""Black-box integration tests for logitech-k650-fix.

The real binary is run with tests/shim.c preloaded. The shim swaps the evdev
device for a pipe (fed with scripted input_event frames by this script) and
/dev/uinput for a capture file, so no hardware or root access is needed.

Every state-machine case must satisfy strict evdev protocol invariants:
  * events are committed by SYN_REPORT; nothing remains uncommitted at exit;
  * no empty SYN_REPORT or orphaned MSC_SCAN-only frames;
  * no duplicate key presses, no releases of keys that are not down, and no
    forwarded autorepeat (value=2) or SYN_DROPPED events;
  * synthesized KEY_INSERT carries no leaked Spacebar MSC_SCAN;
  * no keys remain held at the end, and the process exits cleanly with code 0.

Usage:
  run_tests.py --binary build/logitech-k650-fix --shim build/tests/k650_test_shim.so
               [--group state|process|all] [-k SUBSTRING] [-v]
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

EV_SYN, EV_KEY, EV_MSC = 0x00, 0x01, 0x04
SYN_REPORT, SYN_DROPPED = 0, 3
MSC_SCAN = 0x04

KEY_A = 30
KEY_LEFTSHIFT = 42
KEY_LEFTCTRL = 29
KEY_SPACE = 57
KEY_INSERT = 110
KEY_LEFTMETA = 125
BTN_LEFT = 0x110
KEY_POWER = 116

KEY_NAMES = {
    KEY_A: "A",
    KEY_LEFTSHIFT: "LEFTSHIFT",
    KEY_LEFTCTRL: "LEFTCTRL",
    KEY_SPACE: "SPACE",
    KEY_INSERT: "INSERT",
    KEY_LEFTMETA: "LEFTMETA",
}
HID_SCANCODES = {
    KEY_A: 0x70004,
    KEY_LEFTSHIFT: 0x700E1,
    KEY_LEFTCTRL: 0x700E0,
    KEY_SPACE: 0x7002C,
    KEY_LEFTMETA: 0x700E3,
}

# struct input_event on LP64 / non-time64 ABIs: struct timeval + u16 + u16 + s32.
EVENT_FMT = "@llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FMT)

A, SHIFT, CTRL, SPACE, INSERT, META = (
    KEY_A,
    KEY_LEFTSHIFT,
    KEY_LEFTCTRL,
    KEY_SPACE,
    KEY_INSERT,
    KEY_LEFTMETA,
)


# --------------------------------------------------------------------------
# Script building helpers
# --------------------------------------------------------------------------

def _event(ev_type, code, value):
    return struct.pack(EVENT_FMT, 0, 0, ev_type, code, value)


def frame(*keys):
    """One HID report: (code, value) pairs followed by SYN_REPORT.

    Like the kernel, a press or release (value 0/1) is preceded by an
    MSC_SCAN event; autorepeat events (value 2) are not, and kernel repeat
    frames use SYN_REPORT value=1.
    """
    data = b""
    all_repeats = bool(keys) and all(v == 2 for _, v in keys)
    for code, value in keys:
        if value != 2:
            data += _event(EV_MSC, MSC_SCAN, HID_SCANCODES.get(code, 0x70000 | code))
        data += _event(EV_KEY, code, value)
    return data + _event(EV_SYN, SYN_REPORT, 1 if all_repeats else 0)


def syn_dropped():
    return _event(EV_SYN, SYN_DROPPED, 0)


class Sleep:
    def __init__(self, seconds):
        self.seconds = seconds


def tap(code):
    return [frame((code, 1)), frame((code, 0))]


def build_preload_env(shim, extra_env=None):
    env = dict(os.environ)
    existing = env.get("LD_PRELOAD", "").strip()
    env["LD_PRELOAD"] = f"{existing}:{shim}" if existing else shim
    asan_opts = env.get("ASAN_OPTIONS", "").strip()
    if "verify_asan_link_order" not in asan_opts:
        env["ASAN_OPTIONS"] = f"{asan_opts}:verify_asan_link_order=0" if asan_opts else "verify_asan_link_order=0"
    if extra_env:
        env.update(extra_env)
    return env


# --------------------------------------------------------------------------
# Running the binary and analysing its output
# --------------------------------------------------------------------------

def run_fake(binary, shim, script, extra_env=None, timeout=10.0):
    read_fd, write_fd = os.pipe()
    with tempfile.TemporaryDirectory(prefix="k650-test-") as tmp:
        out_path = os.path.join(tmp, "uinput.bin")
        ready_path = os.path.join(tmp, "uinput.ready")
        caps_path = os.path.join(tmp, "uinput.caps")
        env = build_preload_env(
            shim,
            {
                "K650_IN_FD": str(read_fd),
                "K650_OUT": out_path,
                "K650_READY_FILE": ready_path,
                "K650_CAPS_FILE": caps_path,
                **(extra_env or {}),
            },
        )
        proc = subprocess.Popen(
            [binary, "K650_FAKE_INPUT"],
            env=env,
            pass_fds=(read_fd,),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        os.close(read_fd)

        # Wait until UI_DEV_CREATE completes (or process exits early) so timed
        # scripts never race process startup.
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if os.path.exists(ready_path) or proc.poll() is not None:
                break
            time.sleep(0.005)

        broken_pipe = False
        try:
            for item in script:
                if isinstance(item, Sleep):
                    time.sleep(item.seconds)
                else:
                    os.write(write_fd, item)
            # Let any pending combo window expire before signalling EOF.
            time.sleep(0.08)
        except BrokenPipeError:
            broken_pipe = True
        finally:
            os.close(write_fd)

        try:
            log, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            log, _ = proc.communicate()
            return None, -1, b"", log.decode(errors="replace") + "\n<timed out>", broken_pipe

        data = b""
        if os.path.exists(out_path):
            with open(out_path, "rb") as f:
                data = f.read()
        caps = b""
        if os.path.exists(caps_path):
            with open(caps_path, "rb") as f:
                caps = f.read()

    events = [
        struct.unpack(EVENT_FMT, data[i:i + EVENT_SIZE])[2:]
        for i in range(0, len(data) - EVENT_SIZE + 1, EVENT_SIZE)
    ]
    return events, proc.returncode, caps, log.decode(errors="replace"), broken_pipe


def analyze(events):
    """Validate strict evdev stream invariants and return (frames, violations, down)."""
    violations = []
    frames = []
    current_keys = []
    pending_msc = None
    down = set()

    for idx, (ev_type, code, value) in enumerate(events):
        if ev_type == EV_MSC and code == MSC_SCAN:
            if pending_msc is not None:
                violations.append(f"event #{idx}: consecutive MSC_SCAN (0x{pending_msc:x} not followed by EV_KEY)")
            pending_msc = value
        elif ev_type == EV_KEY:
            kname = KEY_NAMES.get(code, str(code))
            if code == KEY_INSERT and pending_msc is not None:
                violations.append(f"event #{idx}: synthesized INSERT carried leaked MSC_SCAN 0x{pending_msc:x}")
            pending_msc = None

            if value == 2:
                violations.append(f"event #{idx}: forwarded autorepeat {kname}=2")
                current_keys.append((code, 2))
            elif value == 1:
                if code in down:
                    violations.append(f"event #{idx}: duplicate press {kname}=1 while already down")
                down.add(code)
                current_keys.append((code, 1))
            elif value == 0:
                if code not in down:
                    violations.append(f"event #{idx}: spurious release {kname}=0 while not down")
                down.discard(code)
                current_keys.append((code, 0))
            else:
                violations.append(f"event #{idx}: invalid EV_KEY value {kname}={value}")
        elif ev_type == EV_SYN and code == SYN_REPORT:
            if pending_msc is not None:
                violations.append(f"event #{idx}: orphaned MSC_SCAN 0x{pending_msc:x} before SYN_REPORT")
                pending_msc = None
            if not current_keys:
                violations.append(f"event #{idx}: empty SYN_REPORT frame")
            else:
                frames.append(current_keys)
                current_keys = []
        else:
            violations.append(f"event #{idx}: unexpected event (type={ev_type}, code={code}, value={value})")

    if pending_msc is not None:
        violations.append(f"uncommitted MSC_SCAN 0x{pending_msc:x} after last SYN_REPORT")
    if current_keys:
        violations.append("uncommitted events after last SYN_REPORT: " + fmt_events(current_keys))

    return frames, violations, down


def fmt_frames(frames):
    return " | ".join(" ".join(f"{KEY_NAMES.get(c, c)}={v}" for c, v in f) for f in frames) or "<none>"


def fmt_events(events):
    return " ".join(f"{KEY_NAMES.get(c, c)}={v}" for c, v in events)


# --------------------------------------------------------------------------
# Test cases
# --------------------------------------------------------------------------

def F(*pairs):
    return list(pairs)


STATE_CASES = [
    # name, input script, expected committed frames, xfail reason (or None)
    ("passthrough_typing",
     tap(A),
     [F((A, 1)), F((A, 0))], None),

    ("normal_key_autorepeat_dropped",
     [frame((A, 1)), frame((A, 2)), frame((A, 2)), frame((A, 0))],
     [F((A, 1)), F((A, 0))], None),

    ("super_tap_is_forwarded",
     tap(META),
     [F((META, 1)), F((META, 0))], None),

    ("super_plus_other_key_is_forwarded",
     [frame((META, 1)), frame((A, 1)), frame((A, 0)), frame((META, 0))],
     [F((META, 1)), F((A, 1)), F((A, 0)), F((META, 0))], None),

    ("slow_super_space_is_forwarded",
     [frame((META, 1)), Sleep(0.08), frame((SPACE, 1)), frame((SPACE, 0)), frame((META, 0))],
     [F((META, 1)), F((SPACE, 1)), F((SPACE, 0)), F((META, 0))], None),

    ("insert_press_same_frame_release_space_then_meta_frames",
     [frame((META, 1), (SPACE, 1)), frame((SPACE, 0)), frame((META, 0))],
     [F((INSERT, 1)), F((INSERT, 0))], None),

    ("insert_press_separate_frames_release_separate_frames",
     [frame((META, 1)), frame((SPACE, 1)), frame((SPACE, 0)), frame((META, 0))],
     [F((INSERT, 1)), F((INSERT, 0))], None),

    ("k650_rapid_double_tap_ins_within_meta_hold",
     [frame((META, 1)), Sleep(0.008), frame((SPACE, 1)), Sleep(0.007), frame((SPACE, 0)),
      Sleep(0.04), frame((SPACE, 1)), Sleep(0.007), frame((SPACE, 0)), frame((META, 0))],
     [F((INSERT, 1)), F((INSERT, 0)), F((INSERT, 1)), F((INSERT, 0))], None),

    ("k650_firmware_delayed_meta_does_not_hijack_spacebar",
     [frame((META, 1)), Sleep(0.008), frame((SPACE, 1)), Sleep(0.007), frame((SPACE, 0)),
      Sleep(0.02), frame((SPACE, 1)), Sleep(0.045), frame((SPACE, 0)), frame((META, 0))],
     [F((INSERT, 1)), F((INSERT, 0)), F((SPACE, 1)), F((SPACE, 0))], None),

    ("insert_twice",
     [frame((META, 1), (SPACE, 1)), frame((SPACE, 0)), frame((META, 0))] * 2,
     [F((INSERT, 1)), F((INSERT, 0)), F((INSERT, 1)), F((INSERT, 0))], None),

    ("state_resets_after_insert",
     [frame((META, 1), (SPACE, 1)), frame((SPACE, 0)), frame((META, 0))] + tap(SPACE) + tap(A),
     [F((INSERT, 1)), F((INSERT, 0)), F((SPACE, 1)), F((SPACE, 0)), F((A, 1)), F((A, 0))], None),

    ("insert_release_same_frame_meta_first",
     [frame((META, 1), (SPACE, 1)), frame((META, 0), (SPACE, 0))],
     [F((INSERT, 1)), F((INSERT, 0))], None),

    ("insert_release_same_frame_space_first",
     [frame((META, 1), (SPACE, 1)), frame((SPACE, 0), (META, 0))],
     [F((INSERT, 1)), F((INSERT, 0))], None),

    ("insert_release_meta_frame_before_space_frame",
     [frame((META, 1), (SPACE, 1)), frame((META, 0)), frame((SPACE, 0))],
     [F((INSERT, 1)), F((INSERT, 0))], None),

    ("shift_insert_with_repeat_and_same_frame_release",
     [frame((SHIFT, 1)), frame((META, 1)), frame((SHIFT, 2)), frame((SPACE, 1)),
      frame((SPACE, 0)), frame((SHIFT, 0), (META, 0))],
     [F((SHIFT, 1)), F((INSERT, 1)), F((INSERT, 0)), F((SHIFT, 0))], None),

    ("ctrl_insert_same_frame_release",
     [frame((CTRL, 1)), frame((META, 1)), frame((SPACE, 1)),
      frame((SPACE, 0)), frame((CTRL, 0), (META, 0))],
     [F((CTRL, 1)), F((INSERT, 1)), F((INSERT, 0)), F((CTRL, 0))], None),

    ("insert_held_no_forwarded_autorepeat",
     [frame((META, 1), (SPACE, 1)), frame((SPACE, 2)), frame((SPACE, 2)),
      frame((SPACE, 0)), frame((META, 0))],
     [F((INSERT, 1)), F((INSERT, 0))], None),

    ("syn_dropped_recovers_mid_insert_combo",
     [frame((META, 1), (SPACE, 1)), syn_dropped()] + tap(SPACE),
     [F((INSERT, 1)), F((INSERT, 0)), F((SPACE, 1)), F((SPACE, 0))], None),
]


def run_state_case(args, name, script, expected):
    events, code, _, log, broken_pipe = run_fake(args.binary, args.shim, script)
    problems = []
    if events is None:
        return ["process timed out"], log
    if broken_pipe:
        problems.append("child closed input pipe early (BrokenPipeError)")
    frames, violations, down = analyze(events)
    if frames != expected:
        problems.append(f"frames: expected [{fmt_frames(expected)}], got [{fmt_frames(frames)}]")
    problems.extend(violations)
    if down:
        problems.append("keys still held at end: " + ", ".join(KEY_NAMES.get(c, str(c)) for c in sorted(down)))
    if code != 0:
        problems.append(f"exit code {code}")
    return problems, log


def case_usage_without_args(args):
    proc = subprocess.run([args.binary], capture_output=True, timeout=5)
    if proc.returncode == 0 or b"Usage" not in proc.stderr:
        return [f"expected non-zero exit with usage text, got {proc.returncode}"], proc.stderr.decode()
    return [], ""


def case_persistent_error_exits(args):
    """A device that can never work must make the process exit with failure
    (so systemd's Restart=on-failure takes over), not spin at 100% CPU."""
    proc = subprocess.Popen([args.binary, "/dev/null"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=1.5)
        timed_out = False
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        timed_out = True
    lines = out.count(b"\n")
    problems = []
    if timed_out:
        problems.append(f"still running after 1.5s, {lines} log lines emitted")
    elif proc.returncode == 0:
        problems.append("exited with status 0")
    if lines > 20:
        problems.append(f"log flood: {lines} lines")
    return problems, out[:2000].decode(errors="replace")


def _check_caps_bit(caps, bit):
    idx = bit // 8
    return idx < len(caps) and bool((caps[idx] >> (bit % 8)) & 1)


def case_uinput_capabilities_filtered(args):
    """Virtual device must copy keyboard keys (+ INSERT), not BTN_LEFT / KEY_POWER (H2)."""
    if not args.shim:
        return ["shim required"], ""
    events, code, caps, log, _ = run_fake(args.binary, args.shim, tap(A))
    problems = []
    if events is None or code != 0:
        problems.append(f"unexpected exit {code}")
    if not _check_caps_bit(caps, KEY_INSERT) or not _check_caps_bit(caps, KEY_A):
        problems.append("missing KEY_INSERT or KEY_A in uinput keybits")
    if _check_caps_bit(caps, BTN_LEFT):
        problems.append("BTN_LEFT unexpectedly enabled on uinput keyboard")
    if _check_caps_bit(caps, KEY_POWER):
        problems.append("KEY_POWER unexpectedly enabled despite not being on source")
    return problems, log


def case_rejects_mouse_and_virtual_devices(args):
    """Mouse-only receiver interfaces and virtual uinput devices must exit non-zero (M2/H1)."""
    if not args.shim:
        return ["shim required"], ""
    problems = []
    logs = []
    for label, extra in [
        ("mouse_only", {"K650_FAKE_MOUSE_ONLY": "1"}),
        ("virtual_no_phys", {"K650_FAKE_NO_PHYS": "1"}),
        ("grab_ebusy", {"K650_FAKE_EBUSY": "1"}),
    ]:
        _, code, _, log, _ = run_fake(args.binary, args.shim, [], extra_env=extra, timeout=3.0)
        logs.append(f"[{label}] rc={code}: {log.strip()}")
        if code == 0 or code == -1:
            problems.append(f"{label}: expected immediate non-zero exit, got {code}")
    return problems, "\n".join(logs)


PROCESS_CASES = [
    ("usage_without_args", case_usage_without_args, None),
    ("persistent_error_exits_instead_of_spinning", case_persistent_error_exits, None),
    ("uinput_capabilities_filtered", case_uinput_capabilities_filtered, None),
    ("rejects_mouse_virtual_and_busy_devices", case_rejects_mouse_and_virtual_devices, None),
]


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True, help="path to the logitech-k650-fix executable")
    parser.add_argument("--shim", help="path to k650_test_shim.so")
    parser.add_argument("--group", choices=["state", "process", "all"], default="all")
    parser.add_argument("-k", dest="filter", default="", help="only run cases whose name contains this")
    parser.add_argument("-v", "--verbose", action="store_true", help="print program output for failures")
    args = parser.parse_args()

    if EVENT_SIZE != 24:
        print(f"SKIP: unsupported struct input_event layout ({EVENT_SIZE} bytes)")
        return 0

    args.binary = os.path.abspath(args.binary)
    if args.shim:
        args.shim = os.path.abspath(args.shim)

    results = []  # (status, name, detail, log)

    def record(name, xfail, problems, log):
        if problems and xfail:
            results.append(("XFAIL", name, f"{xfail}\n      observed: " + "; ".join(problems), log))
        elif problems:
            results.append(("FAIL", name, "; ".join(problems), log))
        elif xfail:
            results.append(("XPASS", name, f"{xfail} -- now passes, remove the xfail marker", log))
        else:
            results.append(("PASS", name, "", log))

    if args.group in ("state", "all"):
        if not args.shim:
            parser.error("--shim is required for the state group")
        for name, script, expected, xfail in STATE_CASES:
            if args.filter in name:
                record(name, xfail, *run_state_case(args, name, script, expected))

    if args.group in ("process", "all"):
        for name, func, xfail in PROCESS_CASES:
            if args.filter in name:
                record(name, xfail, *func(args))

    for status, name, detail, log in results:
        line = f"{status:5} {name}"
        if detail:
            line += f"\n      {detail}"
        print(line)
        if args.verbose and status in ("FAIL", "XPASS") and log.strip():
            print("      --- program output ---")
            print("\n".join("      " + l for l in log.strip().splitlines()[:30]))

    counts = {s: sum(1 for r in results if r[0] == s) for s in ("PASS", "FAIL", "XFAIL", "XPASS")}
    print("\n" + ", ".join(f"{v} {k.lower()}" for k, v in counts.items()))
    if not results:
        print("no test cases selected")
        return 1
    return 1 if counts["FAIL"] or counts["XPASS"] else 0


if __name__ == "__main__":
    sys.exit(main())
