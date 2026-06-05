# Outpost 2: Divided Destiny - multiplayer protocol, discover to in-game (byte-exact)

Everything `op2session` needs to join an OP2 multiplayer session, appear in the lobby as a
named player, chat, ready up, survive the START handshake, and keep the running game's
lockstep simulation advancing - reverse-engineered from the retail 1.3.6 decompile and
**verified live** against a real host (a bot played as Player 2; the sim ran past 2000 ticks
with 500+ command turns, no desync). Image base `0x00400000`. All wire offsets are from the
start of the UDP datagram unless noted.

Companion docs: `JOIN_PROTOCOL.md` (early spec), `START_CHECKSUM.md`
(the game-start file checksum), `FINDINGS.md` (the debugging journey + gotchas).

---

## 1. Ports & sockets
| Port | Purpose | Bind |
|---|---|---|
| 47776 (0xBAA0) | discovery (find games) | host, bound to its **specific IP** |
| 47777 (0xBAA1) | **join handshake** | host, specific IP |
| 47800-47807 (0xBAB8-F) | **game data / lobby / gameplay** | host binds 47800 (specific IP); client binds first free, **to the local IP** |
| ephemeral (ANY) | the host's *send* sockets (e.g. 60278) | `0.0.0.0` - they appear as the source of host→client packets and *can* receive |

The host's game **receive** thread reads only its bound game socket (47800,
`Network+0x14`). **All client→host game traffic must go to 47800**, even though the host
*sends* from an ANY-bound ephemeral port.

> **Same-machine gotcha:** bind the client game socket to the **resolved local IP**
> (`gethostname`→`gethostbyname`), *not* `INADDR_ANY`. The host binds 47800 to its
> specific IP; an `INADDR_ANY` bind lets the host's more-specific socket steal our
> replies. Binding the local IP also makes the 47800-port scan correctly skip the host.

---

## 2. Wire packet - 14-byte header + payload
Recv/validate `FUN_004974A0`; checksum `FUN_00490F10`.

| Off | Sz | Field |
|----:|--|--|
| 0x00 | 4 | `sourcePlayerNetID` - **slot = NetID & 7** |
| 0x04 | 4 | `destPlayerNetID` - `== ourNetID` or `0` (broadcast) |
| 0x08 | 1 | `payloadSize` (bytes after the 14-byte header) |
| 0x09 | 1 | `type` - **1 = ProtocolControl**, **0 = game / GUR** |
| 0x0A | 4 | `checksum` (excludes itself) |
| 0x0E | N | payload |

### Checksum `FUN_00490F10` (constant `0xFDE24ACB`)
```
acc  = (u32[0x00] + u32[0x04] + u16[0x08]) ^ 0xFDE24ACB      # u16[0x08] = size | (type<<8)
psum = Σ (payloadSize>>2) LE u32 from 0x0E
if (payloadSize & 3):  psum = (psum + trailingBytes) ^ 0xFDE24ACB
checksum = acc + psum
```

### Receive gate (`FUN_004974A0`) - when the host accepts a client packet
1. `len > 0x11`, `payloadSize + 0x0E <= len`, checksum valid.
2. `type == 1` → handled by `FUN_00497240` **always** (no roster check).
3. `type == 0` (game): accepted onto the GUR queue **only if**
   `dest == hostNetID || dest == 0`, **and** `slot = src&7`,
   `roster[slot].netID == src`, `roster[slot].status > 1`.

Roster entry (12 B at `Network+0x8264 + slot*0xC`): `+0` IP, `+4` port,
`+6` status (u16: 0 Empty,1 Joining,2 Normal,3 Replicated,4 Fail), `+8` netID.

---

## 3. JOIN handshake (port 47777) - `FUN_004965A0`
**Request (50 B), type 1, to host:47777** - *uses the per-session GUID, not the game-type GUID:*

| Off | Field |
|----:|--|
| 0x08 | payloadSize = 0x24 |
| 0x09 | type = 1 |
| 0x0A | checksum |
| 0x0E | opcode = 0 |
| 0x12 | **session GUID** (16 B) - from the discovery reply's `[0x1A]` field |
| 0x22 | our game-socket port (u32) - host replies *here* (the port, not the request's source) |
| 0x26 | password (11 B, uppercased; 0 if none) |

Resend ≤16× until a reply arrives.

**Reply (38 B), type 1, payloadSize 0x18:**

| Off | Field |
|----:|--|
| 0x00 | **host NetID** (the header source) |
| 0x0E | **result** - 1 = accepted, 2 = session full |
| 0x12 | session GUID echo (must match) |
| 0x22 | **assigned client NetID** (`slot = &7`) |

The reply comes from one of the host's ANY-bound ephemeral send sockets (e.g. 47783) -
**remember that source address**, you must ack to it next.

---

## 4. The post-join handshake (the order that matters)
1. **Ack the join reply** → send a **cmd-6 status(Normal=2)** packet to the **reply's
   source port** (the ephemeral). This is what advances the host from the join responder
   into the lobby/GUR phase. *Skip this and the host just re-sends the join reply forever.*
2. **Raise our status** → send **cmd-6 status(2)** to the game socket **47800**, so our
   `type==1` packet hits `FUN_00497240` and sets `roster[ourSlot].status = 2`. Now our
   `type==0` packets pass the recv gate.
3. From here, talk GUR on 47800 (§5-§7).

### cmd-6 SetPlayerStatus (type 1, 20 B) - `FUN_00497240` case 6
```
0x00 ourNetID | 0x04 hostNetID | 0x08 size=6 | 0x09 type=1 | 0x0A cksum
0x0E commandType = 6 (u32) | 0x12 status (u16)
```
Effect: `roster[src&7].status = max(current, status)`.

---

## 5. GUR reliable layer (the lobby/gameplay transport)
Send `FUN_0042DBD0`/`FUN_0042DDA0`, recv/ack `FUN_0042DE60`, resend/drop `FUN_0042E4D0`.
37 buffers, **800 ms** ack timeout, **200 ms** retry. A GUR packet is `type == 0` with a
sub-header inside the payload (wire `0x0E`…):

| payload off | wire | field |
|---|---|---|
| +0 | 0x0E | **flags** |
| +1 | 0x0F | **seq** (this packet's sequence, per channel) |
| +2 | 0x10 | **ackA** (highest channel-A seq I've received from you) |
| +3 | 0x11 | **ackB** (highest channel-B seq) |
| +4 | 0x12 | **engine command** |
| +5 | 0x13 | data |

**flags bits:** `0x02` channel A (singlecast/ordered), `0x04` channel B
(broadcast/ordered), `0x08` carries ack info (ackA/ackB valid), `0x20` special (set
expected seq from payload+4), `0x01`/`0x10` "needs processing". A typical reliable packet
is **`0x0A` = channel A + ack**; a pure keepalive-ack is **`0x08`**.

**Send sequence numbering (`FUN_0042DBD0`):**
- singlecast (dest NetID != 0) → channel A, per-peer counter; **seq starts at 1**, wraps
  1..255 (0→1).
- broadcast (dest == 0) → channel B, one global counter.

**Receive/ack (`FUN_0042DE60`):** updates `timeOfLastReceived` *first* (so any packet
from a known peer is a keepalive); `flags&8` → release reliable buffers up to `ackA/ackB`;
`flags&2/4` → consume seq, rejecting anything `!= expected`.

**Drop (`FUN_004601F0`→`FUN_0042E800`):** a peer is dropped when a reliable buffer to them
exceeds its retry budget without an ack (and/or no packet received in 800 ms). → the chat
line *"Player <name> has stopped responding and has been dropped."*

### Staying alive
Every ~200 ms send to 47800: a **status(2)** (keeps the join progressing) and a **pure ack
(`flags 0x08`, ackA/ackB = latest seqs received)**. That refreshes the host's
`timeOfLastReceived` *and* releases its keepalive buffers, so it never drops us.

---

## 6. Engine commands (GUR, payload+4)
| code | meaning | builder |
|---|---|---|
| 1 | GameStartInfo (settings poll, 191 B) | `FUN_0045F840` |
| 3 | **ShowJoinGame** (player name join) | `FUN_0045F2F0` |
| 6 | **chat** | `FUN_00461380` |
| 7 / 8 / 9 / 10 | start: prepare / load&go / go / abort | `FUN_00461700` |
| 0x0B | quit | `FUN_004615D0` |
| 0x0C | in-game per-tick commands | `FUN_00420F30` |

### cmd-3 ShowJoinGame - the lobby join (`FUN_0045F2F0`)
Channel-A reliable singlecast to the host. This is what makes the GAME SETTINGS window add
us to a visible slot **with a name**.
```
flags 0x0A | seq = our channel-A seq (1, the FIRST one) | ackA | ackB
payload+4  = 3                         (engine cmd)
payload+5  = 0x01030004                (TApp::Version "1.3.0.4", u32 LE)
payload+9  = player name (NUL-terminated)
payloadSize = nameLen + 0x3E
```
The host acks it (its next packet carries `ackA >= 1`). **TApp::Version** (`FUN_00488200`)
parses the string at `DAT_004E973C` ("1.3.0.4") → `v0<<24|v1<<16|v2<<8|v3` = `0x01030004`.

### cmd-2 player-status update - READY / race (`FUN_0045FAC0` case 2)
A channel-A reliable packet that sets a bit in **our** slot's status dword. The host
applies it and re-broadcasts the GameStartInfo (so the change-detector sees it come back).
```
flags 0x0A | seq = next channel-A (after join seq1 + chat seq2 → seq 3) | ackA | ackB
payload+4 = 2                  (engine cmd 2)
payload+5 = field selector     (dword: 4 = Ready, 2 = Race/Eden)
payload+9 = value              (dword: 1 = on, 0 = off)
payloadSize = 13
```
Verified live: sending field=4 value=1 → host applies it → re-broadcast shows
`OPU Ready: no → YES`, and the host's START button enables. (Color/Resources aren't
handled by case 2 - they likely use a different selector/path; TBD.)

### Quit / EJECT - engine cmd `0x0B` (`FUN_004615D0` sends, `FUN_0045FAC0` case 0xB handles)
A client receiving a sequenced packet whose **engine cmd = `0x0B`** with **source ==
host NetID** is being told *"The host ejected you from the game."* (message 0x282). From a
*player's* NetID the same command means *"Player <name> has quit."*

**The host's EJECT button just removes the peer and goes silent** - it does not push the
`0x0B`. But the host's **CANCEL / close-game broadcasts cmd `0x0B`** explicitly (verified
live → the client reports "HOST ENDED THE LOBBY" and exits). So watch for **both**.

### All lobby-end / disconnect events (`op2session` handles each)
| event | wire signal | report |
|---|---|---|
| host CANCEL / closes game / leaves | cmd `0x0B` from host NetID | HOST ENDED THE LOBBY |
| host start aborted (a player quit during start) | cmd `10` from host | GAME CANCELED |
| host clicks START | cmd `7` / `8` | HOST IS STARTING THE GAME |
| host crash / network loss / silent eject | no host packet >5 s | HOST STOPPED RESPONDING |

(The matching OP2 strings: "The host ejected you from the game.", "The host has quit
before starting the game.", "Game is canceled.", "Session Closed/Canceled".)

> **Gotcha:** a client that keeps re-announcing (status / cmd-3) after joining will
> *undo the eject* - it re-joins faster than the kick lands. **Stop re-announcing once
> joined** (acks/keepalive only - status is persistent) so the removal sticks.

### cmd-6 chat (`FUN_00461380`)
Channel-A reliable singlecast to the host; **the next channel-A seq after cmd-3** (seq 2).
```
flags 0x0A | seq = 2 (then 3, …) | ackA | ackB
payload+4 = 6              (engine cmd)
payload+5 = our NetID      (u32 - the sender)
payload+9 = message text   (NUL-terminated)
payloadSize = msgLen + 10
```
The host shows it as "<name>: <message>" and relays to other players.

---

## 7. Lobby state - GameStartInfo (191 B / 0xBF) - FULLY DECODED
The settings blob, broadcast as **engine cmd 1** during the lobby (and cmd-7/8 at start).
It's a copy of the lobby-window object from `window+0xC8`, so **blob offset N = window
offset 0xC8 + N**, and on the wire **the blob starts at `0x13`** (after the 5-byte GUR
sub-header). The 186-byte blob + 5-byte sub-header = 191 (0xBF).

| blob | wire | size | field |
|---|---|---|---|
| 0x00 | 0x13 | 4 | **startupFlags** (options + maxPlayers + missionType + initVehicles, see below) |
| 0x04 | 0x17 | 4 | game version (`TApp::Version`) |
| 0x0C | 0x1F | 4 | player-count field (`& 7` = numPlayers) |
| 0x10 | 0x23 | 32 | **map DLL name** (NUL-padded ASCII, e.g. `ml2_08.dll`) |
| 0x30 | 0x43 | 6×4 | **per-slot status** (color/resources/race/ready bits, see below) |
| 0x48 | 0x5B | 6×4 | **per-slot NetID** (slot occupied iff `!= 0`) |
| 0x60 | 0x73 | 6×13 | **per-slot player name** (13-byte records, NUL-padded) |
| 0xAE | 0xC1 | 2 | game speed × 4 (`value >> 2` = 1..10) |
| 0xB2 | 0xC5 | 4 | random seed |
| 0xB6 | 0xC9 | 4 | **start file-checksum master** (0 in the lobby poll; set at start cmd-7/8) |

**startupFlags bits** (verified vs `TethysGame` accessors @0x477F40-80):
| bits | field | |
|---|---|---|
| 0 | Disasters | |
| 1 | Day & Night | |
| 2 | Morale Steady | (set ⇒ steady/fixed) |
| 5 | Allow Cheats | |
| 6-8 | **maxPlayers** | `(flags>>6)&7` |
| 9-13 | missionType | `(flags>>9)&0x1F` (often 0 here - the **map DLL** drives the scenario type; discovery's config dword carries the live missionType: 24 LoS, 25 Midas, 26 Resource Race, 27 Space Race, 28 Land Rush) |
| 17-20 | **Initial Vehicles** | `(flags>>17)&0xF` (0-12) |

**Per-slot status dword** (`FUN_00460800`/`FUN_0045FAC0` case 2):
| bits | mask | field | values |
|---|---|---|---|
| 5-7 | 0xE0 | **Color** | 0 Blue · 1 Red · 2 Green · 3 Yellow · 4 Cyan · 5 Magenta (default `(slot+1)%6`) |
| 3-4 | 0x18 | **Resources** | 0 Low · 1 Med · 2 High |
| 2 | 0x04 | **Race** | 1 = Eden, 0 = Plymouth |
| 1 | 0x02 | **Ready** | 1 = ready |
| 0 | 0x01 | host/locked flag | |

`op2session` decodes all of this (`decode_gamestartinfo`), e.g.:
```
Scenario : 2P, Last One Standing, "ml2_08.dll"
Options  : Disasters=ON  Day/Night=ON  MoraleSteady=off  InitVehicles=0  Speed=8
  [0] Leviathan  Red    Plymouth  Med  YES   netID=21CA4B50
  [1] OPU     Cyan   Plymouth  Med  no    netID=2396FE59
```
The colour-index→name order is *probably* `Blue/Red/Green/Yellow/Cyan/Magenta` (the numeric
mask is certain; the exact string order comes from the colour-combo resources - confirm if
exactness matters). Player records (`FUN_0045F2F0` name, `FUN_0045FAC0` color/race/ready)
update live as the lobby changes. See `START_CHECKSUM.md` for the master at 0xB6.

Key VAs: `FUN_0045F840` (send GameStartInfo), `FUN_00461F50` (receive into window+0xC8),
`FUN_00460800` (UI render - the decode reference), `FUN_0045FAC0` (per-player updates).

---

## 8. Key function VAs
| VA | role | VA | role |
|--|--|--|--|
| 0x490F10 | wire checksum | 0x4965A0 | client JoinHost (req/reply) |
| 0x4974A0 | recv thread + accept gate | 0x496440 | client join setup (binds local IP) |
| 0x497240 | ProcessProtocolControl (cmd 1/4/5/6) | 0x48C2C0 | host discovery responder |
| 0x497190 | cmd-6 status sender | 0x48C080 | host: bind 47776/47777 + threads |
| 0x42DBD0 | GUR send (assigns seq) | 0x495ED0 | HostGame (bind 47800) |
| 0x42DDA0 | GUR network send (sets ack/flags) | 0x45F2F0 | **ShowJoinGame (cmd-3 name)** |
| 0x42DE60 | GUR recv/ack | 0x45F840 | DoNetExchange (cmd-1 GameStartInfo) |
| 0x42E4D0 | GUR resend/drop clock | 0x461380 | **chat (cmd-6)** |
| 0x4601F0 | lobby drop timer (FUN_0042E800) | 0x461700 | host StartGame (cmd 7/8/9/10) |
| 0x488200 | TApp::Version ("1.3.0.4"→0x01030004) | 0x462050 | client StartGame (checksum compare) |
| 0x40C050 | file-checksum kernel | 0x44FFE0 | builds the 14-dword checksum array |
| 0x496950 | ReplicatePlayersList (cmd-4/5) | 0x496DC0 | host wait-for-status loop (16x/500ms) |
| 0x420F30 | cmd-0x0C send | 0x420BA0 | cmd-0x0C body builder |
| 0x420E00 | cmd-0x0C receive (unk!=0 gate) | 0x40E300 | command executor (type enum 0..0x34; case 0x30 = chat display) |
| 0x4116ED | in-game chat issuer (type 0x30) | 0x490C80 | sender name (player+0x1d8, XOR 0xE3) |
| 0x439070 | on-screen message display | 0x478740 | TethysGame::AddMessage |

---

## 9. START handshake - roster replication then load/go

When the host clicks START it first runs **ReplicatePlayersList** (`FUN_00496950`,
UI "Sending players list to all players...") then the load/go sequence (`FUN_00461700`).

### Roster (type-1 control, bypasses the GUR gate)
| host sends | we reply | why |
|--|--|--|
| `cmd-4` SetPlayerList (80 B roster: numPlayers, then per-player [ip(4) port(2) flags(2) netID(4)]) | **cmd-6 status(3)** to :47800 | host bumps occupied slots 2->3 and `FUN_00496dc0` waits (16x/500ms) for every client to report **status 3**, else it falls back to cmd-5 and the start FAILS |
| `cmd-5` finalize | **cmd-6 status(4)** | finalize (only seen if cmd-4 timed out) |

Reply target = first roster entry (the host) = `host:47800`.

### Load / go (GUR engine commands)
| host sends | meaning | we do |
|--|--|--|
| `cmd-7` (`+0x24`=0xBF) | prepare (GameStartInfo refresh) | (process as a GameStartInfo) |
| `cmd-8` (`'\b'`, 191 B GameStartInfo) | load & go | run checksum + load... **we SKIP it** and send our marker |
| - | our reply | **cmd-9 "loaded"**: channel-A reliable, payloadSize=5, engine cmd `9` at 0x12, no data |
| `cmd-9` (`'\t'`, broadcast) | GO - all players loaded | the mission launches; we are a live player |

Markers (`FUN_00462050`/`FUN_00461700`, byte at wire 0x12): `'\t'`(9)=loaded/go,
`'\n'`(10)=abort, `'\v'`(11)=quit-while-starting. The host counts each client whose marker
is 9 (starting its count at 1 for itself); when the count == numPlayers it broadcasts GO.
**The file checksum (`FUN_0044FFE0`, master at GameStartInfo wire 0xC9) is a pure
client-side self-check the host never inspects - an external client skips it entirely.**

## 10. In-game command turns - engine cmd `0x0C`

The running game is lockstep: each "mark" every player must submit a command packet or the
host's simulation halts. Sender `FUN_00420F30`/`FUN_00420BA0`, receiver `FUN_00420E00`.

```
wire:  ...GUR sub-header(0x0E..0x11)... | 0x12: cmd 0x0C | 0x13: playerNum(u8)
       | 0x14: executionTick(i32) | command blocks...
block: { u8 type, u8 len, u32 unk, u8 data[len] }   (empty turn = a 6-byte block)
```

Receiver rules (the gotchas):
- A block whose **`unk` (block+2, u32) is zero is DISCARDED** - an all-zero ctNop is silently
  dropped and the host never registers your mark (it acks at the GUR layer but the sim never
  advances).
- One block is stored per **consecutive mark** (`tick += CPPI` each block), so a single
  multi-block packet commits several marks at once.
- `playerNum` (wire 0x13) selects **which player's ring** to write
  (`DAT_0056F3E4 + playerNum*0xC24 + idx*0x71`); it MUST be our own index (`netID & 7`).

`op2session` strategy: **mirror** - for each host `cmd-0x0C`, reply our own with the same
`executionTick` and the host's block region copied **verbatim** (non-zero `unk` tokens
intact, which also makes any state-checksum comparison pass), under our player index,
channel-A singlecast to the host. Result: the sim advances continuously.

Command-type enum (executor `FUN_0040E300`): `0`=ctNop, `0x31`=ctQuit,
`0x34`=ctMachineSettings, ... (full table in `FINDINGS.md`). A non-idle bot
fills real blocks here (still with a non-zero `unk`).

In-game globals: CPPI `0x56EB08`, lgCPPI `0x56EB0C`, commandTurnDelay `0x56EB18`,
tick `0x56EB1C`; per-player command ring base `DAT_0056F3E4` (stride `0xC24`, slot `0x71`).

## 11. Full client sequence (what op2session does, end to end)
```
DISCOVER  broadcast :47776 -> reply gives hostIP, sessionGUID, maxPlayers
BIND      game socket to localIP, first free 47800-47807
JOIN      send 50B req to :47777 (sessionGUID, ourPort) -> 38B reply (assignedNetID, result)
ACK-JOIN  cmd-6 status(2) -> reply source port   (advances host to lobby)
STATUS    cmd-6 status(2) -> :47800              (raises our status -> gate passes)
NAME      cmd-3 ShowJoinGame (seq 1, "OPU", v1.3.0.4) -> :47800, resend until ackA>=1
          *** host shows us in a lobby slot, named ***
CHAT      cmd-6 chat (seq 2, "Hello!") -> :47800, resend until ackA>=2
READY     cmd-2 status update (field 4 = Ready, seq 3) -> host enables START
KEEPALIVE every 200ms while in lobby: pure-ack(flags 0x08, latest acks) -> :47800
ROSTER    on host cmd-4 -> cmd-6 status(3); on cmd-5 -> status(4)
START     on host cmd-8 -> cmd-9 "loaded" (seq 4, checksum skipped); detect cmd-9 GO
INGAME    on each host cmd-0x0C -> reply our cmd-0x0C (a ctNop per mark, keeping each mark's
          non-zero unk), injecting our chat block (type 0x30) at a fresh mark when desired
          *** the lockstep simulation runs with us as a live player; chat works both ways ***
```

## 12. In-game CHAT - command block type `0x30`

In-game chat is NOT the lobby cmd-6 (that handler is lobby-window-only). It is a **command
block inside the cmd-0x0C stream** - issued by `FUN_004116ed` (the in-game "Chat:" edit box),
displayed by `FUN_0040E300` case `0x30` (the only case that prints `"%s: %s"` via
`FUN_00439070`; sender name from the processed player object, `FUN_00490C80`).

Wire block (inside a command turn, one block = one mark):
```
30 | len | unk(4) | <data: sender(1) recipientMask(1) text... NUL>
```
- `len` = `textlen + 3` (sender + mask + text + NUL).
- `unk` = the mark's token (any non-zero; the receiver discards unk==0).
- `sender` = the sending player's index (host=0; an external client uses its own `NetID & 7`).
- `recipientMask` = bitmask of players who should see it; `0xFF` = everyone. Display test in
  case 0x30: `(1 << localPlayer) & recipientMask`.

**To send it you must pin the chat to a mark you have NOT already committed.** Our turn echoes
a ctNop for every mark in each sliding window, so a mark near the frontier has usually been
ctNop'd already - and the receiver (`FUN_00420E00`) refuses to overwrite a stored mark
(`newTick <= storedTick` -> skip). Pin instead to `maxSentMark + CPPI` (the first never-sent
mark) and re-send the block in each overlapping window until the mark passes - exactly how the
host does it (we observed its chat block at a fixed mark, sliding from block index 4->0 across
5 windows). Result: the message displays exactly once. Verified live both directions:
`OPU: Good Luck! Have Fun!` shown on the host; `Leviathan: ...` decoded by the client.
