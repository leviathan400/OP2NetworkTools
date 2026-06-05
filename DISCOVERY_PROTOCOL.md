# Outpost 2: Divided Destiny - LAN discovery protocol (UDP)

Reverse-engineered from `Outpost2.exe`.
This is the protocol behind *Find Session → "Broadcasting on the local
network."* Implemented in `OP2LanScan\OP2LanScan\Op2Discovery.cs`.

## Transport
- UDP. Discovery port **47776** (`0xBAA0`).
- Client broadcasts a query to `255.255.255.255:47776` (and per-NIC `/24` directed
  broadcasts). A host with an open session replies **unicast** from its `:47776`.
- The **host IP is the datagram source address** - it is *not* a field in the
  packet body.
- Related ports (not used by discovery): game data **47800-47807**.

## Checksum  (Outpost2.exe `FUN_00490EB0`)
```
checksum(buf, start, ndwords, tail) =
    ( Σ  ndwords little-endian u32 starting at buf[start]
      +  (tail==2 ? u16 at next offset : tail==1 ? u8 at next offset : 0) )
    XOR 0xFDE24ACB        (kept to 32 bits)
```
The checksum field itself (bytes `[0x00..0x03]`) is excluded from the sum.

## Game-type GUID
`{5A55CF11-B841-11CE-9210-00AA006C4972}`, stored/serialized in `Outpost2.exe` with
**Data1 little-endian** (standard GUID struct layout). The 16 raw bytes are:
```
11 CF 55 5A 41 B8 CE 11 92 10 00 AA 00 6C 49 72
```
Both sides must match this exactly, or the packet is ignored. (Note the first four
bytes are byte-reversed vs the textual GUID - `11 CF 55 5A`, not `5A 55 CF 11`. The
host reads it from query `+0x06` and from reply `+0x0A`.)

## QUERY - 42 bytes (0x2A)
Built by `BroadcastForGames @0x4917E0`; validated by the host responder `@0x48C2C0`.

| Off  | Size | Field                                                            |
|------|------|------------------------------------------------------------------|
| 0x00 | 4    | checksum = `checksum(buf, 0x04, 9, tail=2)` - 9 dwords + u16     |
| 0x04 | 2    | message type = **0x1000**                                        |
| 0x06 | 16   | game-type GUID (above) - host matches against this              |
| 0x16 | 2    | **reply port** - host sends its reply to `<source-ip>:<this>`    |
| 0x18 | 2    | (zero; high half of the port dword the host reads)              |
| 0x1A | 4    | ping token - host echoes it at reply `+0x06`                     |
| 0x1E | 12   | (zero / game-name slot, see below)                              |

Host accepts only if: `len >= 42`, checksum valid, type == `0x1000`, GUID matches.

**The `[0x16]` reply-port field is essential.** The host does *not* reply to the
UDP source port - it replies to the port the client writes here (real OP2 uses its
own listening port, `47776`). A finder must put **its own bound socket port** here,
otherwise the reply is sent to the wrong port and never arrives. (This is also why a
naïve scanner can't see a game hosted on the *same machine*: it must reply to a port
the host doesn't already own.)

`[0x1E]` is a game-name/filter slot. The host only compares it (11 bytes,
case-insensitive) when it has a name set (`DAT_00574450+0x54 != 0`); for an ordinary
open game that pointer is null and the field is ignored, so leaving it zero works.

## REPLY - 81 bytes (0x51)
Built by the host `@0x48C2C0`; parsed by `NetworkWaitForBroadcastReply @0x491A50`.

| Off  | Size | Field                                                            |
|------|------|------------------------------------------------------------------|
| 0x00 | 4    | checksum = `checksum(buf, 0x04, 19, tail=1)` - 19 dwords + u8    |
| 0x04 | 2    | message type = **0x1001**                                        |
| 0x06 | 4    | echoed query token  → ping = now − token                        |
| 0x0A | 16   | game-type GUID (must match)                                      |
| 0x1A | 16   | **session GUID** (unique per advertised game)                   |
| 0x2A | 16   | **StartupFlags** game-settings struct (bit-packed, see below)    |
| 0x3A | 4    | **maxPlayers** ("Max Players"; == StartupFlags bits 6-8)          |
| 0x3E | 4    | 0 in all captures - *not* the type (my earlier guess was wrong)   |
| 0x42 | 14   | **gameCreatorName** - ASCII, NUL-padded                          |
| 0x50 | 1    | checksum tail byte                                              |

We accept only if: `len == 81`, type == `0x1001`, checksum valid, GUID matches.

OP2's own reply parser (`@0x491A50`) copies `+0x2A`..`+0x39` and `+0x3A` into its
session-list object next to the GUIDs - that's where the PICK SESSION dialog reads
**Max Players** and **Scenario Type**.

### StartupFlags at `+0x2A` (the `+0x3E` dword is NOT the type)
Game settings are bit-packed in the little-endian dword at `+0x2A`:

| bits | field | notes |
|---|---|---|
| 6-8 | maxPlayers   | matches the plain dword at `+0x3A` |
| 9-13 | **missionType** | the "Scenario Type" |

`missionType = (u32@0x2A >> 9) & 0x1F`. **Verified by hosting each mode live:**

| code | config `0x2A` | name |
|---|---|---|
| 24 | `0x3080` | Last One Standing *(verified)* |
| 25 | `0x3280` | Midas *(verified)* |
| 26 | `0x3480` | Resource Race *(verified)* |
| 27 | `0x3680` | Space Race *(verified)* |
| 28 | `0x3880` | Land Rush *(verified)* |

All five verified live by hosting each mode. The codes are **consecutive** (the `0x2B`
byte steps `30,32,34,36,38` per mode). The
mode lives at **bit 9**, not bit 11 - Last One Standing (`0x3080`) and Midas (`0x3280`)
differ only there, so a `>>11` extraction wrongly collapses them. Unknown codes display
as `Type N`. The five mode-name strings live in `Outpost2.exe` `.data`
(`0x4E5518`-`0x4E5550`), but the code→name map is **not** a direct index into that
table - codes are pinned by live capture, not assumed.

## What discovery does NOT carry
**Password** and **current/joined player count** are *not* in the UDP reply - those
are negotiated later over the **TCP join** to the host's game port. Discovery gives
host, creator, max players, scenario type, session id and ping.

## Reference functions (Outpost2.exe VAs)
| Address     | Role                                             |
|-------------|--------------------------------------------------|
| 0x004917E0  | `BroadcastForGames` - builds + sends the query   |
| 0x00491A50  | `NetworkWaitForBroadcastReply` - parses replies  |
| 0x00491CBE  | builds a reply (host side)                        |
| 0x0048C2C0  | host responder - validates query, sends reply     |
| 0x00490EB0  | checksum                                          |
| 0x004E9B18  | game-type GUID (static data)                      |

A fuller dump of the Outpost 2 network stack is in
`NETWORK_PROTOCOL.md`.
