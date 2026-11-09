# op2finder.py - Outpost 2 LAN session finder (Python)

The **Python** implementation of the OP2 LAN finder. Same protocol as the C# desktop
app and the C console tool - it speaks Outpost 2's UDP discovery protocol directly, so
it lists multiplayer games on your LAN **without running Outpost 2**.

Works against retail **1.3.6** and **OPU 1.4.1** hosts (identical wire format).

This is the **reference implementation**: the logic is short and readable, so it doubles
as living documentation of the protocol. (For a no-dependencies binary, use
`..\op2finder\op2finder.exe`; for a GUI, use `..\OP2LanScan\`.)

## Requirements
- **Python 3.6+**. Standard library only - nothing to `pip install`.
- Python must be on your machine/PATH (the C and C# versions need nothing).

## Use
```
python op2finder.py                 live scan, refreshes every 3s
python op2finder.py --once          scan once, print results, exit
python op2finder.py --raw           also dump raw reply bytes (debug)
python op2finder.py --interval 5    live scan, custom refresh seconds
```

Sample output:
```
op2finder - scanning LAN for Outpost 2 sessions (UDP :47776)
broadcasting to: 192.168.1.255, 255.255.255.255

[20:14:07] 1 session(s):
  HOST IP          GAME (creator)   MAX  SCENARIO TYPE       PING    SESSION
  ------------------------------------------------------------------------------------
  192.168.1.50     Leviathan        2    Last One Standing   3ms     A1B2C3D4-...
```

Columns mirror OP2's PICK SESSION dialog: **HOST IP**, **GAME (creator)**, **MAX**
(players), **SCENARIO TYPE**, **PING**, **SESSION** (GUID). The host IP is the reply's
source address.

## Scope (vs the other two implementations)
This script is **broadcast-only** - it sweeps the local network. For a **direct-IP**
query ("Looking at the IP address", to reach across subnets / a VPN), use the C tool
(`op2finder <ip>`) or the C# app's *Find at IP/host* box. The wire protocol is identical;
only this front-end omits the unicast option.

## How to test
1. On any PC, start **Outpost 2 → Multiplayer → TCP/IP → Create Game** and sit on the
   lobby (the host now answers discovery on UDP 47776).
2. Run `python op2finder.py` on the same LAN (can be the same PC). The hosted game
   appears with the host's IP, creator name, max players and scenario type.
3. Nothing showing? Allow Python (or inbound UDP) through Windows Firewall - the host's
   reply is an inbound datagram.

## How it works (short version)
- Builds a 42-byte query (message type `0x1000`, the OP2 game GUID at `+0x06`, our reply
  port at `+0x16`, a ping token, a sum-of-dwords checksum) and broadcasts it to
  `255.255.255.255:47776` plus each NIC's `/24` directed broadcast.
- OP2 hosts validate it and reply (81 bytes, type `0x1001`) from their `:47776`.
- We validate each reply and read host IP, creator name, session GUID, ping, **max
  players** (`+0x3A`) and **scenario type** (StartupFlags bits 9-13 at `+0x2A`).

Scenario-type codes (verified live): 24 Last One Standing · 25 Midas · 26 Resource Race ·
27 Space Race · 28 Land Rush. Full byte-level spec in [`..\DISCOVERY_PROTOCOL.md`](../DISCOVERY_PROTOCOL.md).

## Files
- `op2finder.py` - the tool (stdlib only)
- `README.md` - this file
