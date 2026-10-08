# xfce4-bsdbluetooth-plugin: a Bluetooth manager for FreeBSD

Goal: what blueman-applet does on Linux, for FreeBSD's own Bluetooth
stack -- an XFCE panel icon to switch Bluetooth on and off, find devices,
pair them and connect mice and keyboards.

## What is missing on FreeBSD

FreeBSD has no BlueZ/D-Bus service; Bluetooth is a set of daemons and
tools around the netgraph stack.  The daemons work: `hcsecd` answers PIN
and link key requests and stores link keys, `bthidd` turns HID reports
into keyboard and mouse input.  What is missing is management: pairing a
device means editing `hcsecd.conf` and `bthidd.conf` as root, querying
the HID descriptor, and restarting services, either by hand or with the
`bluetooth-config` shell wizard.  PINs must be in the config file before
pairing, so a keyboard cannot be shown a PIN to type, and nothing shows
which devices are connected.

This project adds that management layer and a GUI on top of the base
system daemons; it does not replace them.

## How it talks to the system

- Command-line tools (`hccontrol`, `bthidcontrol`, `sdpcontrol`) are thin
  wrappers around libraries.  They are not run; the libraries they use
  are called directly, so there is no text output to parse.
- `bluetooth-config` is not used.
- The daemons `hcsecd` and `bthidd` are used as they are, through their
  configuration files and SIGHUP (both reload their configuration on it).
- Services are started and stopped through rc.d (`service`), the
  system's standard way, which also honours `rc.conf`.

| Interface | What it gives us |
|---|---|
| HCI raw socket (`PF_BLUETOOTH`, `BLUETOOTH_PROTO_HCI`), `libbluetooth` (`bt_devopen`, `bt_devreq`, `bt_devinquiry`, `bt_devenum`) | send HCI commands, receive HCI events as they happen |
| `SIOC_HCI_RAW_NODE_*` ioctls (`<netgraph/bluetooth/include/ng_btsocket.h>`) | node state, BD_ADDR, connection list, neighbor cache |
| `libsdp` (`sdp_open`, `sdp_search`) | ask a device for its services and its HID descriptor |
| `hcsecd.conf`, `bthidd.conf` + SIGHUP | pairing (PIN, link keys) and HID input by the base daemons |
| rc.d (`service`) | start and stop `bluetooth`, `hcsecd`, `bthidd` |

### Privileges (checked in `ng_btsocket_hci_raw.c`)

- An unprivileged HCI raw socket may send a whitelist of commands:
  inquiry, remote name request, remote features/version, and most
  "read" commands.  So the GUI can scan and show state as the user.
- Link-key events, PIN and link-key replies, ACL data and configuration
  commands (including disconnect) need `PRIV_NETBLUETOOTH_RAW` (root).
- Editing the daemons' configuration and managing services need root.

## Architecture

```
  panel plugin (user) -- pkexec --> root helper (libexec, one-shot)
        |                               |  edit hcsecd.conf / bthidd.conf,
        |                               |  SIGHUP, service, HCI disconnect
        | HCI raw socket (read-only),   |
        | libsdp                        v
        v                     hcsecd (pairing)   bthidd (HID input)
              netgraph Bluetooth stack (ubt0hci ...)
```

- **xfce4-bsdbluetooth-plugin** (XFCE panel plugin, C, GTK3, user): icon
  with state, device list (paired / connected / nearby), scan, pair,
  disconnect, forget, Bluetooth on/off.  Scans, reads state and queries
  SDP directly; everything that needs root goes through the helper.
- **Root helper** (C, installed in `libexec`, run with `pkexec`; a
  polkit rule allows the active local session without a password, as
  `org.xfce.power.backlight-helper` does).  Small, validates its
  arguments, does one thing per call.
- Pairing a keyboard: the plugin generates a random PIN, the helper
  writes it to `hcsecd.conf`, and the plugin shows it for the user to
  type on the keyboard.
- Adapters are found with `bt_devenum()`; no node name is hard-coded.
  The code only uses standard HCI commands, so it works with any
  adapter the kernel supports (`ng_ubt`); firmware loading (`rtlbtfw`,
  `iwmbtfw`, `bcmfw`, `ath3kfw`) is left to devd.

## Limits

- Bluetooth Low Energy HID (most new mice) is not supported by the
  FreeBSD stack; only Bluetooth Classic devices.
- `hcsecd` has no Secure Simple Pairing.  Without it the controller
  falls back to legacy PIN pairing, which most Classic mice and
  keyboards accept; devices that insist on SSP will not pair.  A
  daemon replacing `hcsecd` with SSP support may come later.
- Headphones (audio through `virtual_oss`) are out of scope for now.

## Milestones

| Step | Content | Done when |
|---|---|---|
| B0 | a command-line tool: enumerate adapters, state, BD_ADDR, connections; inquiry with names -- direct HCI/ioctl calls, no root | lists the adapter and nearby devices |
| B1 | root helper + polkit: add/remove devices in `hcsecd.conf` and `bthidd.conf` (HID descriptor through libsdp), reload daemons, disconnect | a mouse pairs from the command line and reconnects after reboot |
| B2 | XFCE panel plugin | the same from the panel icon |
| later | Secure Simple Pairing (a daemon replacing `hcsecd`) | devices that refuse legacy PIN pairing pair |

## Names

`xfce4-bsdbluetooth-plugin` -- Xfce plugins are named
`xfce4-<name>-plugin`; `bsd` marks it as FreeBSD-specific (as
`xfce4-bsdcpufreq-plugin` does) and avoids the BlueZ-based
`xfce4-bluetooth-plugin`.

## FreeBSD conventions

- Build: one portable Makefile (FreeBSD and GNU make), `PREFIX` and
  `DESTDIR`, `pkg-config` for `libxfce4panel-2.0` and `gtk+-3.0`.
- C code follows style(9), BSD-2-Clause.
- Port: `comms/xfce4-bsdbluetooth-plugin` in this repository; packages
  attached to GitHub releases by CI.  Not submitted to the ports tree.

## Open questions

- Names of the B0 tool and the root helper.
- Test hardware: needs a Bluetooth Classic mouse; test on the A475
  (RTL8822BE Bluetooth, `ubt0`).  Light traffic works with Wi-Fi up.
