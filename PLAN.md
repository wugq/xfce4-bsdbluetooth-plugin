# bsdbt: a Bluetooth manager for the FreeBSD desktop

Goal: what blueman-applet does on Linux, for FreeBSD's own Bluetooth
stack -- a tray/panel icon to switch Bluetooth on and off, find devices,
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
  bsdbt (GUI, user)  <-- UNIX socket -->  bsdbtd (daemon, root)
        |                                       |
        | HCI raw socket (read-only commands)   | HCI raw socket (privileged)
        v                                       v
                 netgraph Bluetooth stack (ubt0hci ...)
                                                |
                                     bthidd (base system, HID input)
```

- **bsdbtd** (C, root, rc.d service): the security agent in place of
  `hcsecd` -- answers PIN code and link key requests, stores link keys,
  and adds Secure Simple Pairing (IO capability, user confirmation),
  which `hcsecd` does not implement.  It asks the GUI for a PIN or a
  confirmation over its socket.  It reads a device's HID descriptor with
  libsdp, writes `bthidd.conf` and reloads `bthidd`; brings the adapter
  up and down.  The socket is restricted to a group (e.g. `operator`).
- **bsdbt** (GUI, user): icon with state, device list (paired /
  connected / nearby), scan, pair, connect, disconnect, forget,
  Bluetooth on/off.  Scans and reads state directly where the kernel
  allows it; everything else goes through bsdbtd.
- **HID input stays with `bthidd`** (base system): it turns HID reports
  into keyboard/mouse events.  bsdbtd only manages its configuration.

## Limits

- Bluetooth Low Energy HID (most new mice) is not supported by the
  FreeBSD stack; only Bluetooth Classic devices.
- Headphones need audio through `virtual_oss` and, on the RTL8822BE,
  full Wi-Fi/Bluetooth coexistence in the Wi-Fi driver (rtwb).  Phase 3.

## Milestones

| Step | Content | Done when |
|---|---|---|
| B0 | `bsdbt-cli`: enumerate adapters, state, BD_ADDR, connections; inquiry with names -- direct HCI/ioctl calls, no root | lists the adapter and nearby devices |
| B1 | `bsdbtd`: security agent (PIN + link keys, compatible key store), socket protocol, CLI client | a mouse pairs and reconnects after reboot without hcsecd |
| B2 | HID setup through libsdp + bthidd config; connect/disconnect/forget | a Bluetooth Classic mouse works end to end from the CLI |
| B3 | GUI applet on top of the same socket protocol | the same from the panel icon |
| B4 | Secure Simple Pairing (just works / numeric comparison) | devices that refuse legacy PIN pairing pair |
| B5 | Audio (virtual_oss), after rtwb coexistence | headphones play |

## Open questions

- GUI: an XFCE panel plugin (C, like xfce4-bsdthinkpad-plugin) or a
  standalone tray icon (StatusNotifierItem; GTK) that works on other
  desktops too.
- Test hardware: needs a Bluetooth Classic mouse; test on the A475
  (RTL8822BE Bluetooth, `ubt0`) with Wi-Fi down until rtwb has
  coexistence.
