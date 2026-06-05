# Outpost 2: Divided Destiny - Join / Lobby / Command protocol (implementation spec)

Goal: an **external client** that joins a live OP2 multiplayer session, appears in the
host's lobby, readies up, reaches game start, and exchanges in-game command packets -
**without** reimplementing the full game.

Synthesized from the OP2 1.3.6 decompile + RE corpus
(`D:\Oupost2Decompile\Review\findings\` NETWORK_PROTOCOL / NETPLAY / CMDLOG_REPLAY /
GAME_LOOP / STATE_CHECKSUM / RNG_AND_DETERMINISM). Function addresses are image-base
`0x00400000`. **Confidence is flagged** - several layouts still need a live-capture to
confirm (see §9).

## 0. Feasibility verdict (the important part)
A **passive "ghost" client is feasible without running the deterministic simulation.**
The lockstep gate is **liveness-only**: the game advances a turn once every active
player's command packet for that turn has arrived; a player is dropped only when *both*
"no ack" and "no packet received" exceed **800 ms**. It does **not** inspect your sim
state per turn. An **empty turn** (a `ctNop`, type 0 / length 0 command) is a first-class
"I did nothing" message and doubles as the keepalive.

The **only** thing that forces a full simulation is an **in-game state-checksum
comparison** (`ChecksumGame` @0x40C0B0). It is confirmed to run **at game start**
(techtree/level checksum - matchable by loading the same map/techtree, no live sim), but
whether vanilla 1.3.6 runs a **periodic in-game** state checksum is **not pinned** from
the decompile (the compare-and-kick branch isn't symbolized). That single question
decides whether a ghost client survives long-term → resolve empirically with a capture.

## 1. Wire packet (the UDP datagram) - 14-byte header + payload
Recv/validate `FUN_004974A0`; checksum `FUN_00490F10`. Offsets from packet start:

| Off | Sz | Field | Notes |
|----:|--|--|--|
| 0x00 | 4 | sourcePlayerNetID | sender NetID. **slot = NetID & 7** |
| 0x04 | 4 | destPlayerNetID | `== ourNetID` or `0` (broadcast). (Docs mislabel this "timestamp".) |
| 0x08 | 1 | payloadSize | recv requires `len >= payloadSize + 14` |
| 0x09 | 1 | type | **1 = ProtocolControl** (lobby/roster), else = game data |
| 0x0A | 4 | checksum | `FUN_00490F10`, excludes itself |
| 0x0E | N | payload | |

**Checksum (`FUN_00490F10`)** - same family as discovery, constant `0xFDE24ACB`:
```
acc  = (dword[0x00] + dword[0x04] + u16[0x08]) ^ 0xFDE24ACB     # u16[0x08] = size|(type<<8)
psum = sum of (payloadSize>>2) dwords from 0x0E
if (payloadSize & 3):  psum = (psum + tailBytes) ^ 0xFDE24ACB
checksum = acc + psum                                            # keep payloads dword-aligned -> simple path
```
Accept rules for a game packet (type≠1): checksum ok; `dest == ourNetID || 0`;
`slot = src&7`; `roster[slot].netID == src` AND `roster[slot].status >= 2`.

## 2. Ports / sockets
| Port | Purpose | Bind |
|--|--|--|
| 47776 | discovery (DONE) | host |
| **47777** | **JOIN handshake** | host (responder thread `LAB_0048C3D0`) |
| 47800-47807 | game data + lobby control + gameplay | host binds 47800; client binds first free 47800-47807 |

Client sends control/gameplay from its **unbound** socket, receives on its **bound**
socket. Per-peer roster entry (12 bytes, `Network+0x8264 + slot*0xC`):
`+0 IP(net)`, `+4 port(net u16)`, `+6 status(u16)`, `+8 playerNetID(u32)`.
status: 0 Empty · 1 Joining · 2 Normal · 3 Replicated · 4 ReplicateFail.

## 3. JOIN handshake (port 47777) - client `FUN_004965A0`
**Request (50 bytes = 14 hdr + 36 payload), type 1, to host:47777:**
| wire off | field | value |
|--|--|--|
| 0x08 | payloadSize | 0x24 (36) |
| 0x09 | type | 1 |
| 0x0A | checksum | `FUN_00490F10` |
| 0x0E | opcode | 0 (join request) |
| 0x12 | session GUID | 16 bytes (the game GUID) |
| 0x22 | our UDP port | u32 (our bound game port) |
| 0x26 | password | 11 bytes, uppercased, zero if none |

Loop ≤16×: sendto → select(recv) → recvfrom ≤38.
**Reply (38 bytes), type 1, payloadSize 0x18 (24):**
| payload off | wire off | field |
|--|--|--|
| +0x00 | 0x0E | host playerNetID |
| +0x04 | 0x12 | result: **1=accepted, 2=session full** |
| +0x08 | 0x16 | session GUID (16, must match) |
| +0x14? | 0x22 | **assigned client NetID** *(offset slightly ambiguous - verify)* |

On accept: adopt `assignedNetID`; `localPlayerNum = NetID & 7`; set slot0 = host
(status 2), our slot = us (status 2); immediately send a cmd-6 status packet (§4).

## 4. Lobby control packets (port 47800, type 1) - parser `FUN_00497240`
`commandType = payload dword[0]` (wire 0x0E):

| cmd | Name | Dir | Payload | Effect |
|--:|--|--|--|--|
| 1 | TellHostUpdatedPlayerStatus | C→H | - | host replies via `FUN_00497190` |
| 4 | SetPlayerList (roster) | H→all | +4 numPlayers; +8 = 72-byte roster[6] | copy roster; reply status |
| 5 | host ack / set-replicated | H→C | - | raise statuses to 4 |
| 6 | SetPlayerStatus | both | +4 u16 newStatus | `roster[src&7].status = max(cur,new)` (monotonic) |

**cmd-6 status sender `FUN_00497190` (20-byte packet)** - *the "ready/announce" message*:
```
0x00 ourNetID | 0x04 hostNetID | 0x08 size=6 | 0x09 type=1 | 0x0A cksum
0x0E commandType=6 (u32) | 0x12 status (u16)
```
Host pushes the roster with **ReplicatePlayersList `FUN_00496950`** (cmd 4), retried
≤16× / Sleep(500) by `FUN_00496DC0` until every peer's status matches.

## 5. GUR reliable layer (lobby + gameplay) - `GurManager`
37 buffers, **800 ms** ack timeout, **200 ms** retry, ≤16 tries. A GUR packet adds a
sub-header inside the payload (wire 0x0E…): `+0 flags`, `+1 seq` (per-peer 1..255),
`+2/+3 ack`, `+4 engine command code`, `+5… data`. Senders `FUN_0042DBD0` (ret 3=ok),
recv/ack `FUN_0042DE60`, resend `FUN_0042E4D0`. Engine command codes (wire 0x12):
`1`=GameStartInfo poll, `3`=join name (ShowJoinGame), `7/8/9/10`=start sequence,
`0x0C`=per-tick gameplay commands.

## 6. Lobby state + START - host `FUN_00461700`, client `FUN_0045F2F0`/`FUN_0045F840`
- **GameStartInfo** = **191 bytes (0xBF)** opaque block (all lobby settings: player
  records/name/color/race, victory cond, resource level, `maxPlayers<<6|missionType`,
  seed, CPPI/turn-delay). Broadcast as cmd-1 during lobby; cmd-7→8→9 at start.
  **Internal field offsets (color/race/ready) NOT yet decoded** (§9).
- Client sends **cmd-3** (player name) reliably, expects window result `0x415`.
- **START**: host cmd-7 (prepare) → cmd-8 (load & go) → clients load scenario, reply
  cmd-9 marker `'\t'(9)` (loaded) → host cmd-9 (start now) → lockstep begins.
  Quit/abort during start = marker `'\v'(11)`/`'\n'(10)` → host cmd-10 (abort+reason).

## 7. In-game command turns (type 0x0C) - `FUN_00420F30` / `FUN_00420BA0`
- Tick `TG+0x84`(0x56EB1C); **CPPI** `TG+0x70`(0x56EB08, power of 2, default 4);
  `lgCPPI` `TG+0x74`. **Batch boundary** = `(tick & (CPPI-1))==0`; batch = `tick>>lgCPPI`.
- **commandTurnDelay** `TG+0x78`(0x56EB18) = batches of lookahead (read from GameStartInfo).
  Per-player command **ring = 16 slots**.
- **Gameplay packet**: wire type **0x0C**; payload = NetworkCommandPacket =
  `{u8 playerNum, u32 tick}` + command blocks. Each block = `{u8 type, u8 len, u32 unk, data[len]}`.
- **Empty turn / keepalive** = one block `type=0x00 (ctNop), len=0`. Send one broadcast
  (dest 0) gameplay packet per batch, stamped for `currentBatch + commandTurnDelay`.
- Command-type enum 0x00-0x36 (full table in CMDLOG_REPLAY.md). For a ghost client only
  `0x00 ctNop` (and optionally `0x34 ctMachineSettings` every 8 batches, `0x31 ctQuit` to
  leave) are needed.
- **In-game CHAT = command block type `0x30`** (`{0x30, len=textlen+3, unk, [sender, mask,
  text, NUL]}`, issuer `FUN_004116ED`, display `FUN_0040E300` case 0x30). To send it, pin the
  block to a mark you have NOT already ctNop'd, or the receiver drops it. (All of this is now
  VERIFIED LIVE - see the authoritative `PROTOCOL.md` and `FINDINGS.md`.)

## 8. Minimum viable ghost client - per batch
1. Track tick/CPPI/commandTurnDelay (integer bookkeeping only - no sim).
2. Broadcast one type-0x0C packet = playerNum + tick + a single `ctNop` block.
3. Honor GUR: ack reliable packets you receive; keep sending inside 800 ms.
4. (Optional) cmd-0x34 every 8 batches.
5. Drain inbound packets so your queue doesn't stall.
6. Pass the **game-start techtree checksum** (load same map/techtree). 
Residual risk = a periodic in-game state checksum (see §0) → only then is a full sim needed.

## 9. Open items - ALL RESOLVED by live testing (2026-06-05)
This was the early spec; every open question has since been confirmed against a live retail
1.3.6 host by the working `OP2Session` client. Authoritative byte-exact spec is now
`PROTOCOL.md`; the debugging story is `FINDINGS.md`.
1. Host-side join responder body (47777) - CONFIRMED: reply 38 B, result@0x0E, host NetID =
   header src@0x00, assigned NetID@0x22, GUID echo@0x12. Join uses the **session** GUID.
2. GameStartInfo 191-byte layout - CONFIRMED and fully decoded (color/race/ready/map/netIDs/
   names/checksum-master@0xC9). See PROTOCOL.md section 7.
3. GUR ack-field wire offsets - CONFIRMED: sub-header @0x0E = flags, seq, ackA, ackB.
4. Periodic in-game state checksum - NOT a barrier for an idle client: the in-game command
   turn carries per-block tokens, and **echoing the host's own tokens verbatim makes any
   comparison pass**. A bot played a full match with no desync abort. (A non-idle bot issuing
   real divergent commands would still need to track game state; an idle one does not.)

## Key function VAs
| VA | Role | VA | Role |
|--|--|--|--|
| 0x490F10 | wire checksum | 0x497240 | ProcessProtocolControl (cmd 1/4/5/6) |
| 0x4974A0 | recv thread (accept/reject) | 0x497190 | cmd-6 status sender (20B) |
| 0x495ED0 | HostGame (bind 47800) | 0x496950 | ReplicatePlayersList (cmd 4) |
| 0x496440 | client Join setup | 0x496DC0 | resend-until-status (16×/500ms) |
| 0x4965A0 | client JoinHost (req/reply) | 0x461700 | host StartGame (cmd 7→8→9/10) |
| 0x48C080 | host bind 47776+47777, threads | 0x45F2F0 | ShowJoinGame (cmd-3 name) |
| 0x42DBD0 | GUR send (ret 3) | 0x45F840 | DoNetExchange (cmd-1 GameStartInfo) |
| 0x42DE60 | GUR recv/ack | 0x420F30 | SendPlayerCommands (type 0x0C) |
| 0x42E4D0 | GUR resend / drop | 0x420BA0 | frame per-player turn |
| 0x40C0B0 | ChecksumGame (state) | 0x40E300 | command dispatcher (0x00-0x36) |
