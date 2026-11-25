# logitech-k650-fix [![Ask DeepWiki](https://img.shields.io/badge/Ask-DeepWiki-blue)](https://deepwiki.com/bokic/logitech-k650-fix)

User-space `evdev`/`uinput` daemon for the **Logitech Signature K650** keyboard on Linux.

In Windows/Linux mode, pressing the K650's dual-labeled `ins` / language-switch key emits `Left Super + Space` instead of `Insert`. `logitech-k650-fix` grabs the Logi Bolt USB receiver's keyboard interface and translates that firmware sequence into a proper `KEY_INSERT` press/release while passing normal typing and `Super` shortcuts through untouched.

References:
- https://www.reddit.com/r/logitech/comments/10gc42d/k650_insert_button_with_language_switch_double/
- https://support.logi.com/hc/en-in/articles/5124495316631-Getting-Started-SIGNATURE-K650

## Installation

### Arch Linux (AUR)

AUR package: https://aur.archlinux.org/packages/logitech-k650-fix

```sh
yay -S logitech-k650-fix
sudo systemctl enable --now logitech-k650-fix
```

### Build & Install from Source

Dependencies: CMake (>= 3.15), a C11 compiler, `pkg-config`, `libudev` headers, and Python 3 (for tests).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now logitech-k650-fix
```

## Usage & Device Selection

```sh
sudo logitech-k650-fix /dev/input/by-id/usb-Logitech_USB_Receiver-event-kbd
```

### Scope & Troubleshooting
- **Logi Bolt USB receiver vs. Bluetooth:** This daemon matches the Logi Bolt USB receiver (`046d:c548`). Bluetooth connections use a different bus/product ID and are not currently targeted.
- **Multiple Logitech USB receivers:** Because Logi Bolt receivers do not expose unique USB serial numbers, two receivers plugged in at once compete for `/dev/input/by-id/usb-Logitech_USB_Receiver-event-kbd`. If `logitech-k650-fix` exits reporting the wrong device, point it at the port-specific `/dev/input/by-path/...-event-kbd` symlink instead (check `ls -l /dev/input/by-path/` or `/proc/bus/input/devices`).

## License

MIT
