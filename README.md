# lxkey

A simple Linux input device cli that can detect connected input devices inspect what they support monitor their events live intercept (grab) them so other apps don't get the input and inject custom key/button presses into the system.

Works on Linux. Can also work on rooted Android.

## Features

**1. Detect devices (`list`)**
Scans `/dev/input/` and lists every input device on the system like keyboard, mouse, touchpad, power button, etc along with its name, bus type, and vendor/product IDs.

**2. Inspect a device (`info`)**
Shows full capability details for a specific device which keys it can send, which axes (for mouse/joystick/touchpad), LEDs, switches, etc. Useful to figure out which `/dev/input/eventX` node is actually the "real" one for a multi-interface device.

**3. Monitor events (`watch`)**
Reads and decodes raw input events live as they happen - key presses/releases, mouse movement, scroll, etc

**4. Intercept / grab mode (`watch -g`)**
Same as monitor but uses `EVIOCGRAB` to take exclusive control of the device — events go only to this program, not to the rest of the system. basically it intercepts the input so that only this program receives it.....other applications (like your desktop/window manager) don't get it

**5. Inject custom input (`inject`)**
Creates a virtual input device via `uinput` (cloned from a real device's capabilities) and sends a synthetic event so you can simulate key presses, volume buttons, power button, etc. as if a real device sent them.

## Build

```bash
clang lxkey.c -o lxkey
```

## Usage

```bash
sudo ./lxkey list
sudo ./lxkey info /dev/input/eventX
sudo ./lxkey watch /dev/input/eventX
sudo ./lxkey watch -g /dev/input/eventX
sudo ./lxkey inject /dev/input/eventX <type> <code> <value>
```

Type and code can be given as names or numbers, e.g. `EV_KEY KEY_A 1`.

### Examples

Find your devices:
```bash
sudo ./lxkey list
```

Watch a keyboard live:
```bash
sudo ./lxkey watch /dev/input/event3
```

Grab/intercept a device (only this program sees its input):
```bash
sudo ./lxkey watch -g /dev/input/event3
```

Inject a volume-up press (down then up):
```bash
sudo ./lxkey inject /dev/input/event3 EV_KEY KEY_VOLUMEUP 1
sudo ./lxkey inject /dev/input/event3 EV_KEY KEY_VOLUMEUP 0
```

## Notes

- Needs root, since `/dev/input/*` and `/dev/uinput` are root-only by default.
- Not every `/dev/input/eventX` node is "live".... some devices register extra/duplicate event nodes that send nothing. Use `list` + `info`/`watch` to find the correct one.
- Grab (`-g`) only stops other evdev clients (like the compositor) from getting events... it doesn't change how the VT/console keyboard driver works so behaviour can differ slightly on a raw TTY vs a graphical terminal.
- On Android this needs root + access to `/dev/input` and `/dev/uinput` (via Termux/Terminal with root, or `adb shell` as root).
