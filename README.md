# logitech-k650-fix

## Short
Fix for Logitech K650 keyboard insert key issue.

URLS:
- https://www.reddit.com/r/logitech/comments/10gc42d/k650_insert_button_with_language_switch_double/
- https://support.logi.com/hc/en-in/articles/5124495316631-Getting-Started-SIGNATURE-K650

## Installation
Arch
AUR - https://aur.archlinux.org/packages/logitech-k650-fix
```sh
yay -Syyu logitech-k650-fix
sudo systemctl enable logitech-k650-fix
sudo systemctl start logitech-k650-fix
```

## TODOs

- Comment source code.
- Create Ubuntu PPA packages.
- Update this file.

## Issues
Rarely this fix stops working. It is not related to this service. Unplugging and replugging the receiver helps to solve. See [Issue #5](https://github.com/bokic/logitech-k650-fix/issues/5)

## License

MIT
