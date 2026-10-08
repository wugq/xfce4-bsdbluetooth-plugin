# xfce4-bsdbluetooth-plugin: a Bluetooth manager for FreeBSD

Goal: what blueman-applet does on Linux, for FreeBSD's own Bluetooth
stack -- an XFCE panel icon to switch Bluetooth on and off, find devices,
pair them and connect mice, keyboards and (later) headphones.

## Why not wrap the command-line tools

FreeBSD has no BlueZ/D-Bus service; Bluetooth is a set of daemons and
tools (`hccontrol`, `hcsecd`, `bthidd`, `bthidcontrol`, `sdpcontrol`,
`bluetooth-config`) around the netgraph stack.  Running those and parsing
their text output is fragile, has no events, and pairing prompts cannot
reach a GUI.  The stack's interfaces can be used directly instead:

| Interface | What it gives us |
|---|---|
| HCI raw socket (`PF_BLUETOOTH`, `BLUETOOTH_PROTO_HCI`), `libbluetooth` (`bt_devopen`, `bt_devreq`, `bt_devinquiry`, `bt_devenum`) | send HCI commands, receive HCI events as they happen |
| `SIOC_HCI_RAW_NODE_*` ioctls (`<netgraph/bluetooth/include/ng_btsocket.h>`) | node state, BD_ADDR, connection list, neighbor cache |
| `libsdp` (`sdp_open`, `sdp_search`) | ask a device for its services and its HID descriptor |
| L2CAP sockets | what bthidd uses for HID; later audio |

### Privileges (checked in `ng_btsocket_hci_raw.c`)

- An unprivileged HCI raw socket may send a whitelist of commands:
  inquiry, remote name request, remote features/version, and most
  "read" commands.  So the GUI can scan and show state as the user.
- Link-key events (`Return_Link_Keys`, `Link_Key_Notification`), PIN and
  link-key replies, ACL data and configuration commands need
  `PRIV_NETBLUETOOTH_RAW` (root).

## Architecture

```
  panel plugin (user) <-- UNIX socket -->  btagentd (daemon, root)
        |                                       |
        | HCI raw socket (read-only commands)   | HCI raw socket (privileged)
        v                                       v
                 netgraph Bluetooth stack (ubt0hci ...)
                                                |
                                     bthidd (base system, HID input)
```

- **btagentd** (C, root, rc.d service): the security agent in place of
  `hcsecd` -- answers PIN code and link key requests, stores link keys,
  and adds Secure Simple Pairing (IO capability, user confirmation),
  which `hcsecd` does not implement.  It asks the GUI for a PIN or a
  confirmation over its socket.  It reads a device's HID descriptor with
  libsdp, writes `bthidd.conf` and reloads `bthidd`; brings the adapter
  up and down.  The socket is restricted to a group (e.g. `operator`).
- **xfce4-bsdbluetooth-plugin** (XFCE panel plugin, C, GTK3, user): icon with state, device list (paired /
  connected / nearby), scan, pair, connect, disconnect, forget,
  Bluetooth on/off.  Scans and reads state directly where the kernel
  allows it; everything else goes through btagentd.
- **HID input stays with `bthidd`** (base system): it turns HID reports
  into keyboard/mouse events.  btagentd only manages its configuration.

## Limits

- Bluetooth Low Energy HID (most new mice) is not supported by the
  FreeBSD stack; only Bluetooth Classic devices.
- Headphones need audio through `virtual_oss` and, on the RTL8822BE,
  full Wi-Fi/Bluetooth coexistence in the Wi-Fi driver (rtwb).  Phase 3.

## Milestones

| Step | Content | Done when |
|---|---|---|
| B0 | a command-line tool: enumerate adapters, state, BD_ADDR, connections; inquiry with names -- direct HCI/ioctl calls, no root | lists the adapter and nearby devices |
| B1 | `btagentd`: security agent (PIN + link keys, compatible key store), socket protocol, CLI client | a mouse pairs and reconnects after reboot without hcsecd |
| B2 | HID setup through libsdp + bthidd config; connect/disconnect/forget | a Bluetooth Classic mouse works end to end from the CLI |
| B3 | XFCE panel plugin on top of the same socket protocol | the same from the panel icon |
| B4 | Secure Simple Pairing (just works / numeric comparison) | devices that refuse legacy PIN pairing pair |
| B5 | Audio (virtual_oss), after rtwb coexistence | headphones play |

## Names

Split like blueman (`blueman-applet`, `blueman-manager`,
`blueman-mechanism`):

- `xfce4-bsdbluetooth-plugin` -- the project and the panel plugin.
  Xfce plugins are named `xfce4-<name>-plugin`; `bsd` marks it as
  FreeBSD-specific (as `xfce4-bsdcpufreq-plugin` does) and avoids the
  BlueZ-based `xfce4-bluetooth-plugin`.
- `btagentd` -- the privileged daemon, named like the base system's
  Bluetooth daemons (`bthidd`, `hcsecd`, `sdpd`); it is the pairing agent.

## FreeBSD conventions

- `btagentd`: `-d` runs in the foreground, otherwise `daemon(3)`;
  `pidfile(3)` at `/var/run/btagentd.pid`; `syslog(3)`; an rc.d script
  with `btagentd_enable`; config `/usr/local/etc/btagentd.conf`, link keys
  under `/var/db/btagentd/`, socket `/var/run/btagentd.sock`; man pages
  `btagentd(8)` and `btagentd.conf(5)`; style(9), BSD-2-Clause.
  It replaces `hcsecd` (`hcsecd_enable="NO"`) and can import
  `/var/db/hcsecd.keys`.
- Build: one portable Makefile (FreeBSD and GNU make), `PREFIX` and
  `DESTDIR`, `pkg-config` for `libxfce4panel-2.0` and `gtk+-3.0`.
- Port: `comms/xfce4-bsdbluetooth-plugin` in this repository; packages
  attached to GitHub releases by CI.  Not submitted to the ports tree.

## Open questions

- Socket access control: a group, or polkit's "active local session"
  rule as used by other desktop helpers.
- Test hardware: needs a Bluetooth Classic mouse; test on the A475
  (RTL8822BE Bluetooth, `ubt0`).  Light traffic works with Wi-Fi up;
  headphones with 2.4 GHz Wi-Fi may need dynamic coexistence in the
  Wi-Fi driver.
