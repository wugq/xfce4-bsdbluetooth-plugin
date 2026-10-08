# xfce4-bsdbluetooth-plugin

A Bluetooth manager for **FreeBSD**: an XFCE panel icon to switch Bluetooth
on and off, find devices, pair them and use Bluetooth Classic mice and
keyboards, plus `bsdbt`, the same from the command line. Think of
blueman-applet, for FreeBSD's own Bluetooth stack.

FreeBSD has no BlueZ. Its Bluetooth stack works, and so do its daemons:
`hcsecd` answers PIN and link key requests and keeps link keys, `bthidd`
turns keyboard and mouse reports into input. What it lacks is a way to
manage them without editing `hcsecd.conf` and `bthidd.conf` as root and
restarting services, by hand or with the `bluetooth-config` wizard. This
project is that layer; it keeps using the base system's daemons.

**Status:** early. Tested on one machine, a ThinkPad A475 (Realtek
RTL8822BE Bluetooth) with FreeBSD 15.1 and XFCE 4.20: pairing with a Linux
laptop, and a Logitech M535 mouse (pairing, use, reconnecting after it
sleeps). Not yet tested with a keyboard. Reports from other machines are
welcome.

On cards where Wi-Fi and Bluetooth share the antenna, the Wi-Fi driver has
to let Bluetooth listen for connections while Wi-Fi is up, or devices that
reconnect by themselves (mice, keyboards) cannot get back in. On the
RTL8822BE this needs [rtwb](https://github.com/wugq/rtwb) 0.2.2 or later.

## What it does

- Panel icon: Bluetooth on or off, connected devices in the tooltip.
- Its menu, like the PulseAudio plugin's and the power manager's: an
  on/off switch; the devices set up before, connected or not, each with
  Remove (and Disconnect, except for mice and keyboards, which connect
  again as soon as they are used) in a submenu; "Search for devices", which lists the devices in
  range, and choosing one pairs with it.
- Pairing a keyboard shows a PIN to type on it; other devices get 0000, and
  if one refuses that, a dialog asks for the PIN from its manual. Results
  come as desktop notifications.
- Input devices are handed to `bthidd`, which connects to them; after a
  reboot they reconnect by themselves.

```
$ bsdbt scan
Scanning on ubt0hci for 10 seconds...
00:11:22:33:44:55  mouse           Bluetooth Mouse
$ bsdbt pair 00:11:22:33:44:55
Pairing with Bluetooth Mouse (00:11:22:33:44:55)...
Paired.
Added as an input device; bthidd connects to it.
```

## How it works

| Part | Runs as | Does |
|---|---|---|
| `bt.c` (in `bsdbt` and the plugin) | user | adapters, connections, search and names through an HCI socket and `libbluetooth`; the kernel allows these to ordinary users |
| `bsdbt-helper` | root, through `pkexec` | edits the `hcsecd` and `bthidd` configuration one device at a time, pairs (HCI connection + authentication, answered by `hcsecd`), reads HID descriptors over SDP with `libsdp`, starts and stops services with rc.d |

It calls the libraries the base system's command-line tools use instead of
running those tools, and does not use `bluetooth-config`. The polkit action
`org.bsdbt.helper` lets the user at the console run the helper without a
password, and nobody else. See [PLAN.md](PLAN.md) for the design.

## Limits

- Bluetooth Classic only: the FreeBSD stack has no Bluetooth Low Energy
  input devices, and many new mice are BLE only.
- `hcsecd` has no Secure Simple Pairing. Without it the adapter falls back
  to legacy PIN pairing, which most Classic mice and keyboards accept; a
  device that insists on SSP cannot be paired.
- No audio devices yet.
- It uses the default `/etc/bluetooth/hcsecd.conf` and `bthidd.conf`, not
  other paths set in `rc.conf`.

## Requirements

- FreeBSD 14 or later, with a Bluetooth adapter the kernel supports
  (`ng_ubt`) and its firmware loaded (for example the `rtlbt-firmware`,
  `iwmbt-firmware` packages)
- Packages `xfce4-panel`, `libnotify`, `polkit`, `consolekit2` (so that
  polkit knows the active session), and a notification daemon
  (`xfce4-notifyd`)
- `pkexec` must work from the desktop. On FreeBSD it can fail with
  "Last login" messages from `pam_lastlog` in `/usr/local/etc/pam.d/polkit-1`;
  see [xfce4-bsdthinkpad-plugin](https://github.com/wugq/xfce4-bsdthinkpad-plugin)
  for the fix.

## Install

### Binary package from a release

Each [release](https://github.com/wugq/xfce4-bsdbluetooth-plugin/releases)
has packages for FreeBSD 14 and 15 (amd64), built by GitHub Actions from the
tag (`.github/workflows/release.yml`). As root:

```
pkg install ./xfce4-bsdbluetooth-plugin-0.1.0-FreeBSD-15-amd64.pkg
```

pkg installs the dependencies from the FreeBSD package repositories.

### As a package (port)

The port is in this repository, not in the ports tree. With a ports tree
in `/usr/ports`, as root:

```
git clone https://github.com/wugq/xfce4-bsdbluetooth-plugin.git
cd xfce4-bsdbluetooth-plugin/port/comms/xfce4-bsdbluetooth-plugin
make install clean
```

### From source

```
make
make install          # as root; PREFIX=/usr/local by default
```

### Then

Restart the panel (`xfce4-panel -r`, or log out and in again) so that it
sees the new plugin, and add "Bluetooth (FreeBSD)" in the panel's "Add New
Items" dialog.

`pair` enables `hcsecd` (and `bthidd` for input devices) in `rc.conf` and
starts them.

## Files

| File | |
|---|---|
| `/usr/local/bin/bsdbt` | command line, `bsdbt(1)` |
| `/usr/local/libexec/bsdbt-helper` | root helper, `bsdbt-helper(8)` |
| `/usr/local/lib/xfce4/panel/plugins/libbsdbluetooth-plugin.so` | panel plugin |
| `/usr/local/share/polkit-1/actions/org.bsdbt.helper.policy` | polkit action |
| `/var/db/bsdbt.devices` | the devices set up, readable by everybody |

## License

BSD-2-Clause, see [LICENSE](LICENSE).
