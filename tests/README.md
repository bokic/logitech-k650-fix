# Tests

Automated unit and black-box integration tests for `logitech-k650-fix`. Neither root privileges nor physical hardware are required.

- `test_remap.c` (`remap_unit_tests`) exercises `remap.c` directly with deterministic nanosecond timestamps (including the K650's 1.2-second `LEFTMETA` hold, `Shift` autorepeats landing inside the 8 ms combo gap, same-frame `Shift`/`Meta` release, `SYN_DROPPED`, and USB disconnect cleanup).
- `shim.c` (`k650_test_shim.so`) is an `LD_PRELOAD` library that swaps the evdev device for a pipe and `/dev/uinput` for a capture file, emulating `EVIOCGID`, `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGBIT`, `EVIOCGKEY`, `EVIOCGRAB`, and `UI_*` ioctls (plus fault injection for non-keyboard/virtual/busy devices).
- `run_tests.py` feeds scripted HID frames into the real `logitech-k650-fix` binary and checks strict evdev protocol invariants:
  - all events are committed by `SYN_REPORT` with no uncommitted tail;
  - no empty `SYN_REPORT` or orphaned `MSC_SCAN`-only frames;
  - no duplicate presses, spurious releases, forwarded `value=2` repeats, or leaked `MSC_SCAN` on `KEY_INSERT`;
  - no keys left held down and exit status `0`;
  - `uinput` capabilities mirror keyboard keys without `BTN_LEFT`/`KEY_POWER`, and persistent errors exit immediately with non-zero status.

## Running

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure -V
```

Run `run_tests.py` directly:

```sh
tests/run_tests.py --binary build/logitech-k650-fix --shim build/tests/k650_test_shim.so -v
tests/run_tests.py --binary build/logitech-k650-fix --shim build/tests/k650_test_shim.so -k shift_insert
```

To configure without tests, pass `-DBUILD_TESTING=OFF`.
