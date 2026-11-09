# op2finder - external Outpost 2 LAN session finder

A standalone tool that finds Outpost 2 multiplayer games on your local network
**without running Outpost 2.** It speaks OP2's own UDP discovery protocol:
broadcasts the same query the game's "Find Session → Broadcasting on the local
network" sends, and parses the replies hosts send back.

Works against retail **1.3.6** and **OPU 1.4.1** hosts (identical wire format).

## Use

```
op2finder            live LAN scan, refreshes every 3 seconds
op2finder <ip>       query ONE host directly ("Looking at the IP address")
op2finder -once      scan once, print results, exit
op2finder -raw       also dump raw reply bytes (debugging)
```

`op2finder 10.0.0.5` probes just that host (cross-subnet / VPN / internet, where
broadcast can't reach). Combine with `-once`, e.g. `op2finder 10.0.0.5 -once`.

Output:
```
op2finder - scanning LAN for Outpost 2 sessions (UDP :47776)
broadcasting to: 255.255.255.255 192.168.1.255

[20:14:07] 1 session(s):
  HOST IP           GAME (creator)    PING     SESSION
  ----------------------------------------------------------------------
  192.168.1.50      Leviathan         3ms      A1B2C3D4-1122-3344-55667788AABBCCDD
```

## How to test
1. On one PC, start **Outpost 2 → Multiplayer → TCP/IP → Create Game**, and sit on
   the lobby (the host is now broadcasting/answering on UDP 47776).
2. On the same LAN (can be the same PC), run **`op2finder`**. The hosted game
   should appear with the host's IP + the creator's name.

If nothing shows, see **Firewall** below.

## Firewall
The host's reply is an inbound UDP packet to op2finder's ephemeral port. If Windows
Firewall blocks it, allow `op2finder.exe` (or inbound UDP). The same applies to OP2
itself (ports 47776 discovery, 47800-47807 game data).

## What it does (protocol)
- Builds a **42-byte query**, broadcasts to **255.255.255.255:47776** (and each
  NIC's `/24` directed broadcast).
- OP2 hosts validate the query (checksum + message-type `0x1000` + the OP2 game
  GUID) and **reply (81 bytes)** from their `:47776`.
- We validate each reply (type `0x1001` + checksum + GUID) and read: **host IP**
  (the datagram source), **game-creator name**, **session GUID**, **ping** (echoed
  token round-trip).

### Wire format (reverse-engineered from Outpost2.exe)
Query builder `BroadcastForGames @0x4917E0`; reply parser
`NetworkWaitForBroadcastReply @0x491A50`; host responder `@0x48C2C0`.

**QUERY (42 bytes), to 255.255.255.255:47776**
| Off | Size | Field |
|---|---|---|
| 0x00 | 4 | checksum = `(Σ 9 LE dwords[0x04..0x27] + u16[0x28]) XOR 0xFDE24ACB` |
| 0x04 | 2 | message type = **0x1000** |
| 0x06 | 16 | game GUID = `11 CF 55 5A 41 B8 CE 11 92 10 00 AA 00 6C 49 72` |
| 0x16 | 2 | **reply port** - host replies to `<source-ip>:<this port>`; put your own bound port here |
| 0x1A | 4 | ping token (host echoes it at reply+0x06) |
| rest | - | zero (host ignores unless a game-name filter is set) |

> Note: GUID is at **+0x06** (not +0x0A) and Data1 is little-endian (`11 CF 55 5A`).
> The host replies to the port named at **+0x16**, *not* the UDP source port - this is
> mandatory, and is why same-machine discovery needs a port the host doesn't own.

**REPLY (81 bytes), from host :47776**
| Off | Size | Field |
|---|---|---|
| 0x00 | 4 | checksum = `(Σ 19 LE dwords[0x04..0x4f] + u8[0x50]) XOR 0xFDE24ACB` |
| 0x04 | 2 | message type = **0x1001** |
| 0x06 | 4 | echoed query token (ping = now − token) |
| 0x0A | 16 | game GUID (must match) |
| 0x1A | 16 | session GUID (unique per advertised game) |
| 0x2A | 16 | host identity GUID |
| 0x2A | 16 | **StartupFlags** game-settings (bit-packed: maxPlayers=bits6-8, missionType=bits9-13) |
| 0x3A | 4 | **maxPlayers** (PICK SESSION "Max Players") |
| 0x42 | 14 | **gameCreatorName** (ASCII, NUL-padded) |
| host IP | - | from the UDP source address (not in packet body) |

`missionType = (u32@0x2A >> 9) & 0x1F` (consecutive codes, all verified live by hosting
each): **24 = Last One Standing (0x3080), 25 = Midas (0x3280), 26 = Resource Race
(0x3480), 27 = Space Race (0x3680), 28 = Land Rush (0x3880)**. Unknown shows as
`Type N`. The `+0x3E` dword is 0 for every mode - *not* the type.
**Password** and **current/joined player count** are *not* in the UDP reply (TCP join
only). Discovery gives host, creator, max players, scenario type, session id and ping.

## Files
- `op2finder.c` - the tool (Winsock, standalone console exe)
- `op2finder.exe` - built (no dependencies)
- `build.bat` - MSVC build (`cl /O2 op2finder.c ws2_32.lib`)

A Python reference implementation of the same protocol lives in `..\op2finder-py\`.

## Ideas / next steps
- Add a follow-up TCP probe (port 47800-47807) to fetch current/max players per
  session (discovery reply doesn't include it).
- "Looking at the IP address" mode: send the query unicast to a specific IP
  (skip broadcast) to find games across subnets / over the internet.
- GUI / tray version, or JSON output for a web dashboard.
