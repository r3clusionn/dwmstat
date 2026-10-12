# dwmstat

A status bar program for dwm in one C file: the power plan, brightness, volume, Bluetooth, Wi-Fi, battery with time left, and the clock, written to the root window name that dwm shows. Unlike status programs that redraw on a timer, it sleeps until something changes: the kernel's uevents and network link messages, a signal from the scripts that change volume or brightness, or the next minute for the clock. It is made for one laptop (HP OmniBook 7, Ryzen AI 9 HX 475, Gentoo with dwm, PipeWire and Bluetooth started on demand). It never starts PipeWire or bluetoothd, it only looks at whether they are running.

**Status:** working, in daily use since October 2026 on the target laptop.

![The right end of the dwm bar at 2x scale, on a dark background: "⚙ auto:bal  ☀ 100%  ♪ 30%  Ƀ off  ⇅ on  ⚡87% 11:34  Mon 12 Oct 10:58", meaning the balanced power plan chosen automatically, backlight 100%, volume 30%, Bluetooth off, Wi-Fi connected, battery at 87% with 11 hours 34 minutes left, and the date and time](docs/images/bar.png)

## Features

- **Power plan** from the `powerprofile` script's state file (`/run/powerprofile`): `⚙ bal` for a plan picked by hand, `⚙ auto:bal` when it follows the charger (bat, bal, perf, game). Shown only if the file exists.
- **Brightness** from `/sys/class/backlight`.
- **Volume** from `wpctl` (PipeWire's default sink), with "mute"; "off" when PipeWire isn't running. A stale socket from a killed daemon counts as off.
- **Bluetooth:** off, on, or the number of connected devices, read from the HCI device and its links in sysfs. No bluez libraries needed.
- **Wi-Fi:** on when a wireless interface's link is up.
- **Battery:** percentage, ⚡…+ while charging, and the time left while discharging, from the battery's own energy and power readings.
- **Clock:** day, date and time, updated on the minute.
- **No polling.** Between events it is blocked in `poll()`: about 5 wakeups a minute on an idle laptop.
- **Exits with X:** when the X server goes away, so it never outlives a session.

## How to install

Requires a C compiler, make and libX11.

```sh
git clone https://github.com/r3clusionn/dwmstat
cd dwmstat
make install            # to ~/.local/bin; PREFIX=/usr/local for system-wide
```

## How to use

Start it before dwm in `~/.xinitrc`:

```sh
dwmstat &
exec dwm
```

Anything that changes what the bar shows sends it `SIGUSR1` so the bar updates at once. These are the scripts on the target laptop:

```sh
pkill -USR1 -x dwmstat
```

| Change | Who signals |
|---|---|
| Volume, mute | `vol` (a `wpctl` wrapper bound to the volume keys) |
| Brightness | `bl` (bound to the brightness keys) |
| Audio on/off | `audio toggle` (F2), `audio ensure` from the media players |
| Bluetooth power | `bt-toggle` |
| Power plan | `powerprofile` |

Battery, power source, Wi-Fi and Bluetooth connections need no signal: they come from the kernel.

## How it works

- **One `poll()` over four descriptors:**
  - a `signalfd` for SIGUSR1 (update) and SIGTERM/SIGINT/SIGHUP (exit);
  - a `NETLINK_KOBJECT_UEVENT` socket, filtered to the `power_supply`, `rfkill` and `bluetooth` subsystems (plugging in, battery status, radio switches, Bluetooth links);
  - a `NETLINK_ROUTE` socket for link changes (Wi-Fi connecting or dropping);
  - the X connection, readable when the server shuts down.
- **The timeout** is computed to land 20 ms after the next minute boundary, so the clock changes on time with one wakeup a minute.
- **Volume** is the only value that needs another program (`wpctl`), so it is read only on SIGUSR1 and once a minute, never on other events. A hung `wpctl` is killed after one second.
- **The root window name** is only set when the text changed, so dwm doesn't redraw its bar for nothing.

## Measurements

On the target laptop, idle, over 60 seconds:

| | |
|---|---|
| Wakeups | 5 in 60 s |
| CPU | 20 ms in 24 minutes of running |
| Memory | 434 kB PSS (244 kB private) |
| Binary | 26 kB (11 kB of code) |

For comparison, slstatus redraws every second by default (60 wakeups a minute), and its volume module starts a program on each of them.

## Verification

- `bl +5` then `bl -5` (the brightness script, which sends SIGUSR1): the root window name read back with `XFetchName` showed ☀ 75% and then ☀ 70% within 0.2 s of each.
- Wakeups and CPU measured from `/proc/PID/status` and `/proc/PID/stat` over 60 s (see Measurements).
- Charger, Wi-Fi and Bluetooth changes arrive as kernel uevents and link messages; they are in daily use but weren't re-tested for this README.

## Limits

- Linux only (netlink, signalfd, sysfs). It reads the first backlight and the first system battery it finds.
- The format and the order of the parts are in the source; change `snprintf` in `main` to rearrange them.
- Volume needs PipeWire's `wpctl`; Bluetooth reads `hci0` only.

## License

MIT (see `LICENSE`).
