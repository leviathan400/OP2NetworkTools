# OP2Session - RE journey, gotchas, and lessons

How we got from "discover a game" to "join the lobby as a named player and chat with the
host," iterating live against a real game host. This is the *narrative* companion
to `PROTOCOL.md` (the byte-exact spec) - the things we learned the hard way.

Method throughout: **iterate against the live host** like we did for discovery. The host's
responses (or silence) are the feedback; the user watching the GAME SETTINGS screen is the
ground truth. The decompile is the source of truth for byte
layouts - but several handlers (notably the join responder thread `LAB_0048C3D0`) were
never promoted to functions, so the **client-side builders** define the contract.

---

## The blockers, in the order we hit them

### 1. The join GUID is the SESSION GUID, not the game-type GUID
First join attempts to `:47777` got **total silence**. The request format was right (it
matches the client builder `FUN_004965A0` exactly), so the host was rejecting on content.
The caller `FUN_00491DE0` passes the GUID from `session+0x2C`, which the discovery parser
fills from reply `[0x1A]` - the **per-session GUID**, *not* the constant game-type GUID at
`[0x0A]`. Sending the session GUID → instant accept.
> Lesson: there are two GUIDs in discovery. Join uses the unique per-session one.

### 2. Same-machine binding - match the host's bind style (NetHelper on vs off)
On one box, our client must not co-exist on the host's own game port (47800), or replies we send to
`hostIp:47800` get delivered to the most-specific socket instead of the host. The trap: **on Windows a
wildcard (`0.0.0.0`) bind and a specific-IP bind to the same UDP port co-exist** (most-specific wins
delivery), so there is **no single bind safe against both host types** - it must be two modes that
*match the host's bind style*. The real determinant is **NetHelper ON vs OFF** (NetHelper's `BindWrapper`
makes the host bind wildcard), *not* the game version - NetHelper can be toggled independently (1.3.6
ships with it too, usually disabled), it just correlates with 1.3.6/1.4.1 in normal use:
- **Host with NetHelper ON** binds wildcard `0.0.0.0:47800` → **we bind wildcard** (wildcard-vs-wildcard
  conflicts, so we skip 47800). A specific bind here co-exists with the host's wildcard and steals its
  acks - the original lobby-drop.
- **Host with NetHelper OFF (vanilla)** binds specific `10.x:47800` → **we bind specific to our local
  IP** (specific-vs-specific conflicts cleanly). A wildcard bind here co-exists with the host's (and a
  3rd player's) specific bind and loses/steals traffic - "host stopped responding" / "Connecting to
  session failed".

**We DETECT the mode by probe, not from `--ver`.** Before the real bind we try a throwaway wildcard bind
to the host's 47800: two wildcard binds *conflict* but a wildcard + a specific bind *co-exist*, so the
probe **fails iff the host already holds 47800 wildcard** (NetHelper ON) and succeeds otherwise (NetHelper
OFF). Decisive same-machine (the only case that matters); cross-machine the port is free → reads OFF,
harmless. `--ver` stays purely for the version gate (which game to join). Plus skip 47800 explicitly when
`localIp == hostIp`. This also fixes same-machine multi-instance: in each mode our bind conflicts cleanly
with the other same-style clients, so everyone lands on a distinct port.
> Lesson: a client must look like the *other* clients on the wire, and NetHelper is what changes what
> those look like. We tried specific-only (ok off / broke on), wildcard-only (ok on / broke off), and a
> `--ver`-guessed dual mode (broke when NetHelper was toggled against the version default) before landing
> on probing the host's actual bind style. Full writeup: `OP2SessionLogger/NETPLAY.md`.

### 3. The join-reply offsets (and host NetID location)
The reply is parsed by `FUN_004965A0`: **result at `[0x0E]`** (1=ok/2=full), GUID echo at
`[0x12]`, **assigned client NetID at `[0x22]`**, and the **host's NetID is the header
source `[0x00]`** - not in the payload. (Our first decode had result/hostNetID in the
wrong spots; the join still "worked" but the parse was wrong.)

### 4. You MUST ack the join reply to the reply's *source port*
After accept, the host reliably re-sends the join reply from an **ANY-bound ephemeral**
send socket (e.g. 47783). The client (`FUN_004965A0` line 109-110) replies with a
**cmd-6 status(2) to that reply's source port**. That single packet is what advances the
host from the join responder into the lobby/GUR phase. We regressed here once (dropped the
reply-source ack during a refactor) and the host got stuck re-sending the join reply
forever, never reaching the lobby - a one-line fix.
> Lesson: the post-join confirmation goes to the *reply's source port*, then everything
> else goes to 47800.

### 5. Send game traffic to 47800; the host receives there even though it sends elsewhere
The host's receive thread (`FUN_004974A0`) reads only its bound game socket `47800`
(`Network+0x14`). It *sends* from an ANY-bound ephemeral (60278). So replying to the
packet source doesn't reach the receive thread - **everything game-level must go to 47800.**

### 6. Raise status, or your type-0 packets are dropped at the gate
The recv gate requires `roster[slot].netID == src && status > 1` for `type==0` packets.
`type==1` (cmd-6) bypasses the gate and is always processed, so sending **cmd-6 status(2)
to 47800** raises `roster[ourSlot].status` to Normal - *then* our GUR packets are accepted.
We proved acceptance by sending a sequenced packet and watching the host's **ackA of us
flip to 1** (`HOST-ACKS-US(A)=1`) - the moment we knew the gate was open.

### 7. The GUR ack is what stops the resend-and-drop
The host sends reliable keepalives (channel A, incrementing seq, ~1/800 ms) and **drops a
peer whose reliable buffer exceeds its retry budget without an ack** (`FUN_0042E800`,
surfaced by the lobby timer `FUN_004601F0`). Sending a **pure-ack** (`flags 0x08`,
`ackA/ackB = latest seqs received`) every 200 ms releases those buffers *and* refreshes the
host's `timeOfLastReceived`. Once we did this the host went from "spam-then-drop" to a
steady, stable exchange.

### 8. cmd-3 ShowJoinGame = the *lobby* join (transport ≠ lobby)
Being transport-alive (host acks us) is **not** the same as being in the GAME SETTINGS UI.
The visible-slot join is the **cmd-3 ShowJoinGame** packet: a channel-A reliable carrying
our **player name** + **TApp::Version `0x01030004`** ("1.3.0.4", parsed from `DAT_004E973C`
by `FUN_00488200`). The moment the host acked our cmd-3, **"OPU" appeared in slot 2**
(Yellow, with an EJECT button). Wrong version or a malformed packet → the host rejects and
disconnects us instead of adding us.

### 9. Chat is the next sequenced packet
Lobby chat is **engine cmd 6** (`FUN_00461380`): channel-A reliable, `payload+5` = our
NetID, `payload+9` = the text, `payloadSize = msgLen + 10`. It's simply **our next
channel-A seq after cmd-3** (cmd-3 = seq 1, chat = seq 2). Sent "Good Luck! Have Fun!" → it showed as
**"OPU: Good Luck! Have Fun!"** and the host typed back **"Leviathan: hey"**. Full bidirectional
lobby chat. 🎉

### 10. EJECT is silence, not a packet - and your keepalive can undo it
Engine cmd `0x0B` is the quit/eject (`FUN_0045FAC0` case 0xB: from the host's NetID →
"The host ejected you"). But the host's **EJECT button just removes the peer and stops
sending it traffic** - it doesn't reliably push the `0x0B` to us. First eject test failed
because our **keepalive (status + cmd-3) re-joined us faster than the kick landed**, so the
host kept streaming us packets and never sent a kick. Fixes: **stop re-announcing once
joined** (acks only - status is persistent), and detect the eject as **>4 s of host
silence** after being joined. Then it worked: kick → host goes quiet → we report
*"'OPU' was kicked from the lobby"* and exit. (`HOST-ACKS-US` flipped to `0xEF` at the
kick - the host's reset sentinel.)
> Lesson: in a removal-by-silence protocol, "be a good citizen" - don't spam re-joins, and
> treat host-silence as the disconnect signal.

---

## Diagnostic tricks that worked
- **`HOST-ACKS-US(A)`**: read the host's `ackA` of *us* (received packet `payload+2`). When
  it flips from 0→1 after we send a sequenced packet, the recv gate is open and our packets
  reach the GUR layer. This single number ended a long "are our packets even accepted?"
  rabbit hole.
- **Latching on the ack**: detect `LOBBY JOIN ACKED` / `CHAT DELIVERED` when
  `hostAckA >= ourSeq` (the ack value oscillates across packet types, so latch on the first
  qualifying read).
- **De-dup the dump**: the host re-sends a lot; collapsing identical consecutive packets
  made the real state transitions visible (GameStartInfo spammed → sent once = our ack
  landed).
- **netstat during a join**: revealed the host's two ANY-bound ephemeral send sockets vs
  the specific-IP 47776/47777/47800 - key to understanding the socket topology.

---

## Status: the whole pipeline is solved (everything below was once "open")
Every item that used to live here is now done and documented as a numbered finding:
- **GameStartInfo 191-byte field map** - fully decoded (color bits 5-7, race/resources/READY
  bits, map DLL, netIDs, names). See `PROTOCOL.md` and the live change-detection milestone.
- **Ready-up** - cmd-2 status update (field 4 = Ready). See milestone "READY-UP".
- **Start handshake** - roster (status 3) + cmd-9 "loaded"; the file checksum is a
  client-side self-check we skip. See findings #11 and #12.
- **In-game command turns** - cmd-0x0C, echo the host's blocks verbatim (non-zero `unk`
  required). See findings #13 and #14: the lockstep sim runs.

### Genuinely next (beyond an idle player)
- Issue real gameplay commands (build/move) by filling command blocks at type != 0 with a
  non-zero `unk`, instead of echoing the host's empty turns.
- **DONE** - clean in-game leave via `ctQuit` (command block type `0x31`): implemented in
  OP2DummyPlayer and verified live (host shows "OPU has quit the game"). See finding #16.
- **DONE** - multi-peer (3+ players): the bot broadcasts command turns to every peer and reads
  all players' chat/leaves. Verified live in a 3-player match. See finding #17.
- **DONE** - **OPU 1.4.1 support**: all three clients now join 1.4.1 hosts (full lifecycle:
  join -> lobby -> start -> in-game -> clean leave), verified live. Needed a host-version match
  (advertise 1.4.0.1; `--ver` overrides) and a same-machine bind fix (bind our game socket to
  `INADDR_ANY`/wildcard like the real OPU clients, since NetHelper binds the 1.4.1 host to
  `INADDR_ANY` and a specific-IP bind would otherwise steal the host's port). Also needed an
  in-game P2P fix: substitute the host's real IP for its `0.0.0.0` self-entry in the roster.
  See `OP2SessionLogger/NETPLAY.md`.

### 11. The START handshake - roster replication first, then load/go/checksum
Clicking START on the host does **not** go straight to "go". The host first runs
**ReplicatePlayersList** (`FUN_00496950`→`FUN_00496DC0`) - the UI message *"Sending
players list to all players..."* with an ABORT button. It sends **type-1 control**
packets (these bypass the GUR gate):
- `cmd-4` **SetPlayerList** (80-byte roster payload, `numPlayers`, all NetIDs) - a client
  answers by reaching transport status **Replicated(3)** and replying **cmd-6 status(3)**.
- `cmd-5` **finalize** - client replies **cmd-6 status(4)**.
We were stuck here because the bot never answered cmd-4/5, so the host's roster step never
completed and the host showed ABORT. Fix: in the recv loop, on a **type-1** packet read the
control command at wire `0x0E`; `cc==4 → send_status(gameAddr,3)`, `cc==5 →
send_status(gameAddr,4)`. (Per `FUN_00497240` cases 4/5.)

Only **after** the roster does the host broadcast the GUR start sequence
(`FUN_00461700` host / `FUN_00462050` client):
- engine `cmd-7` prepare, then `cmd-8` (`'\b'`) **load&go** carrying the GameStartInfo.
- Client stores it (`FUN_00461f50`), computes the **file signature** (`FUN_0044FFE0`) and
  compares its master `local_344` to the host's master at GameStartInfo wire `0xC9`
  (`in_ECX+0x17e`). **This is a pure client-side self-check** - the host never inspects our
  checksum, so a bot can skip it entirely.
- On OK, client sends **cmd-9 "loaded"** (`FUN_0042DBD0` channel-A reliable; internal send
  struct sets marker `+0x2e = 9`, len `+0x24 = 5`, NetID `+0x20`). In wire terms this is a
  normal channel-A reliable with **engine cmd `9` at `0x12`** and a small (~5-byte) payload.
- Host collects every client's marker; once all are `'\t'`(9)=loaded it broadcasts the final
  **cmd-9 GO** (`'\t'`) → client `return 1` → the simulation starts.
- Markers: `'\t'`(9)=loaded/go, `'\n'`(10)=abort, `'\v'`(11)=quit-while-starting.
> Next: capture the host's real cmd-7/cmd-8 wire bytes live, then build the cmd-9 reply from
> observed bytes (skip the checksum), then handle in-game command turns (type `0x0C`).

### 12. The START handshake, solved end-to-end - the game LAUNCHES
Three live fixes, in order, drove the host from "Sending players list..." all the way into
the running mission:
1. **Roster status must be 3, not 2.** `FUN_00496950` (ReplicatePlayersList) bumps each
   occupied slot 2→3 in the roster it sends, then `FUN_00496dc0` **waits (16 retries × 500ms)
   for every client to report status 3** via a cmd-6 reply - else it falls back to cmd-5 and
   **fails** (the host stays on ABORT). First attempt replied status 2 → host resent cmd-4
   ~14× then died. Replying **cmd-6 status(3)** to cmd-4 cleared the roster in ONE shot.
   (cmd-5 fallback wants status 4.)
2. **cmd-9 "loaded" marker.** After the roster, the host sends `cmd-7` prepare then **`cmd-8`
   load&go** (the 191-byte GameStartInfo, engine cmd `8` at wire 0x12). The client
   (`FUN_00462050`) replies a tiny **channel-A reliable: payloadSize=5, engine cmd `9` at
   0x12, no data** (just the 4-byte GUR sub-header + the cmd byte). Cross-check: the host's
   cmd-8 builder sets internal `+0x24=0xbf` and the wire shows payloadSize `0xbf`, so the
   internal length field maps **directly** to wire payloadSize - cmd-9's `+0x24=5` ⇒ wire
   payloadSize 5. **We skip the file checksum entirely** (`FUN_0044FFE0` is a pure
   client-side self-check the host never inspects).
3. **The host counts loaded players and GOes.** `FUN_00461700` loops receiving GUR messages,
   incrementing a counter for each client whose marker byte (wire 0x12) == `'\t'`(9); it
   starts at 1 (the host itself). When the count == numPlayers it prints *"All remote players
   have responded"* and **broadcasts the final `cmd-9` GO** → `return 1` → **the mission
   loads and the match is live.** Our bot sends cmd-9, sees `HOST-ACKS-US=04` (our seq
   acked), then catches the GO broadcast = **GAME STARTED**. ✅ Confirmed live: host left the
   lobby and launched into "Waiting for remote players..." → cleared → in-game.

### 13. In-game command turns (cmd 0x0C) - received; lockstep stalls without ours
The instant the game starts the host broadcasts **engine cmd `0x0C` command-turn packets**
(type 0, dst=0 broadcast, ~46-byte payload). Layout: GUR sub-header (0x0E-0x11) + cmd `0x0C`
at 0x12 + `0x13`=00 + a **signed int turn/offset at 0x14** that climbs by +4 each turn
(`-12, -8, -4 …`) + the command/sync body (repeated 6-byte groups, e.g. `c7 8a a9 00 00 00`,
a trailing `24 36 be ba`). The host sent **3 turns then went silent** - classic lockstep: it
runs a few turns ahead, then **blocks waiting for each client's matching `0x0C` turn** before
advancing. Our bot received them and acked, but sends none of its own, so the host's sim
paused and our >5s-silence guard disconnected. **Next phase:** emit our own `0x0C` each turn
(empty command list = `ctNop`, per `JOIN_PROTOCOL.md` §7), echoing the turn counter,
to keep the host advancing - the final piece of "send/receive command packets."

### 14. In-game command turns SENT - the lockstep sim RUNS (the finish line)
Receiving cmd-0x0C was easy; making the host *advance* took one more decompile read. First
attempt: reply our own cmd-0x0C with all-zero ctNop blocks. The host **acked us at the GUR
layer** (`HOST-ACKS-US` climbed) but the **sim stayed frozen at mark 0** - the command layer
silently dropped our turns. The reason is in the receiver `FUN_00420e00`:
```c
puVar1 = ring + playerNum*0xc24 + ringIdx*0x71;
if ((*(int*)(block+2) == 0) || (packetTick <= ring[idx].tick))  // unk==0 -> SKIP/discard
```
**A command block whose `unk` field (block bytes 2-5, u32) is zero is thrown away.** Our
ctNop blocks were zero ⇒ never stored ⇒ the host never saw our mark commitments. Two more
facts from the same function: the receiver stores **one block per consecutive mark**
(`tick += CPPI` each block), so a single 6-block packet commits ~6 marks at once; and
`playerNum` (wire 0x13) selects **which player's ring** to write - it MUST be our own index
(`assignedNetID & 7`), not 0, or we'd clobber the host's ring.
**Fix:** mirror the host's command turn by copying its **block region verbatim** (its
non-zero `unk` tokens intact) under our player index. Instantly the sim took off:
`-12 → -8 → -4 → 0 → 4 → … → 1984` in ~60 s, **500+ command turns sent**, host acking
throughout, no desync abort. Echoing the host's own tokens also means any state-checksum
comparison trivially passes. The bot is now a **fully live (idle) player in a running
retail-1.3.6 multiplayer match.** Command TYPE enum (executor `FUN_0040e300`): 0=ctNop,
0x31=ctQuit, 0x34=ctMachineSettings, … - a future non-idle bot fills real command blocks
here (still with a non-zero unk).
> Tick globals: CPPI `0x56EB08`, lgCPPI `0x56EB0C`, commandTurnDelay `0x56EB18`, tick
> `0x56EB1C`; per-player command ring base `DAT_0056F3E4` (stride `0xC24`, slot `0x71`).

### 15. In-game CHAT - a command block (type 0x30), and the fresh-mark trap
Sending an in-game message is a separate subsystem from lobby chat, and the last 5% was a
genuinely subtle bug:
- **Lobby cmd-6 is dead in-game** - `FUN_00461380` is lobby-window-only; sent in-game it is
  accepted by transport but never displayed (no handler).
- The real mechanism is a **command block type `0x30`** inside the cmd-0x0C stream. We found
  it by **capturing a live in-game chat from the host** and reading it off the wire:
  `30 <len> <unk> 00 ff <text> 00`. The issuer `FUN_004116ed` (the in-game "Chat:" box)
  confirmed the layout exactly: `data = [sender(1), recipientMask(1), text, NUL]`,
  `len = textlen + 3`. Display is `FUN_0040E300` case 0x30 (`"%s: %s"`), gated on
  `(1 << localPlayer) & recipientMask`; the name comes from the processed player (`FUN_00490C80`).
- **Receive worked immediately** (scan incoming command blocks for type 0x30).
- **Send did not display** for three iterations even though a byte-dump proved our block was
  *byte-identical in format* to the host's known-good one. The bug was the **mark we pinned
  to**: our turn echoes a ctNop for every mark in each sliding window, so by chat time we had
  *already committed a ctNop* for the target mark - and the receiver `FUN_00420E00` refuses to
  overwrite a stored mark (`newTick <= storedTick` -> skip), silently dropping the chat. Fix:
  pin to `maxSentMark + CPPI` (the first never-sent mark) and re-send across overlapping
  windows until it passes - the host's own pattern. Instantly displayed
  `1: OPU: Good Luck! Have Fun!` on the host. **Full two-way in-game chat.**
> Lesson: in sliding-window lockstep, "where" (which mark) is as load-bearing as "what" (the
> bytes). A format-perfect packet still vanishes if it lands on an already-committed mark.

### 16. Clean in-game leave - ctQuit (command block type 0x31), and why it must be self-sent
Leaving an in-game match by just going silent makes the host wait out its reliable-drop
timeout (~800 ms+) and then show the "We have lost contact with player X - Drop Player"
dialog, stalling the other players. The clean signal is a **ctQuit command block, type
`0x31`**, inside the cmd-0x0C stream. The byte layout is the simplest possible block: `31 00
<unk(4, non-zero)>` with **no data** - the quitting player is identified by the turn's
`playerNum` field at wire `0x13`, so the block carries nothing else. Verified live: the host
printed **"OPU has quit the game"** instead of the lost-contact dialog.

The non-obvious part was *delivery*, not the bytes. Our first attempt injected the `0x31`
block only when a host command turn arrived (to copy its `unk` and pin a fresh mark). That
fails on a one-machine test: the host **stops sending command turns the instant it loses
focus** (when you click the bot to press Disconnect), so no turn ever arrives to ride, and the
bot's own command-turn-silence detector bails first. Fix: **self-send** the leave. Build our
own cmd-0x0C at `maxSentMark + CPPI` (a fresh never-committed mark) carrying a goodbye chat
block (0x30) then the ctQuit (0x31) at the next mark, using a synthetic non-zero `unk` (the
same `0xBABE3624`-style token our filler turns already use, which the host accepts). Resend it
each loop tick (~200 ms) for ~1.2 s so it survives loss and a paused host's socket buffer,
then exit. Also stop mirroring host turns once leaving, or `maxSentMark` advances past the
pinned quit mark and the host rejects it (the same `newTick <= storedTick` rule from #15).
> Lesson: a clean leave does not need an incoming turn to answer - the client can originate its
> own command turn at a fresh mark. That makes the quit robust even when the host has paused.

### 17. Multi-peer (3+ players) - SOLVED, the bot plays in 3-player games
The whole pipeline was first built against a **2-player** game (host + bot), where the only
other peer IS the host, so addressing every command turn to the host happened to be complete.
A **third** player stalled the match within ~6 s of game start. The fix came from a live
3-player capture (temporary on-wire diagnostics), which nailed three facts:

1. **Gameplay is peer-to-peer.** A third player's packets reach us **directly** from its own
   address - not relayed by the host. Each player sends their per-tick command turn to *every*
   other player, and a mark cannot commit until all players' turns are in.
2. **The cmd-4 SetPlayerList roster gives every peer's real game address.** Layout at payload
   `0x12`: `numPlayers(u32)`, then 6 slots of `[IP(4, network order) | port(2, host order) |
   status(2) | netID(4, LE)]`. The port here is the peer's **bound game port** (47800/47801/
   47802, first-free per player) - NOT the ephemeral source port packets happen to arrive from.
   We had been ignoring this roster (using it only to reply status 3).
3. **Command turns are BROADCAST with GUR flags = 0x00.** On the wire, both host and peers send
   their turns with `dst = 0` and `flags = 0x00` - no channel, no seq validation, deduped by
   execution mark. This is the crux: our 2-player turns used channel A (`flags 0x0A`), which a
   peer **rejects** because channel A is a strict per-sender sequence and that peer never saw
   our lobby channel-A stream (it all went only to the host). The host accepted it (its channel
   A was in sync), which is why 2-player worked and 3-player did not.

The implementation: parse the cmd-4 roster into a `peers[]` table (netID + game address, self
excluded); when 2+ peers exist, send each command turn as a `dst=0`, `flags=0x00` broadcast to
**every** peer's roster address; and read in-game chat (block 0x30) and a player leaving (ctQuit
0x31) from **every** player's turn (each rides in that player's own turn, the turn's playerNum
at wire `0x13` = the sender's slot), since the host-only mirror never sees a peer's blocks. The
self-send leave (#16) already broadcasts via the same path, so the bot can ctQuit out of a 3+
player game and the rest play on. Verified live: a 3-player match ran past **3400 ticks** with
the bot as a full player, all three players' chat shown, and a clean `*** X has left the game
***` when a player quit (the remaining two, including the bot, kept playing).
> Lesson: the channel that works for one peer can be wrong for another. Broadcast command data
> on the channel the engine itself uses (flags 0, mark-deduped), not the per-peer ordered one.

> **RESOLVED - in-game is peer-direct P2P (both 1.3.6 and 1.4.1).** The wire capture settles the
> earlier question. Each player's command turns arrive tagged with **that player's own netID**
> (`in-game turn source: slot=2 srcNetID=<peer>`), not a single master's, and when the host (the
> initial "master") leaves, **the sim keeps ticking with ZERO `RESYNC/ctrl` traffic** - our bot and
> the surviving peer keep exchanging turns directly. If steady-state play were master-relayed it would
> stall at the master's departure until a resync election; it doesn't. So **steady-state in-game is
> plain peer-to-peer broadcast**, and the **master-aggregation / resync-master election** machinery the
> KB documents (`OP2KB-FULL/docs/engine/RNG_AND_DETERMINISM.md`, `GAME_LOOP.md`) is the **recovery
> layer** (lag / drop / desync), not the per-tick path. Confirmed identical on retail 1.3.6 and OPU
> 1.4.1 - consistent with the engine code being ~99% shared. Our `dst=0`-broadcast-to-every-peer model
> is protocol-faithful for steady-state; the **host-leave continuation** (`electClock()`) keeps that
> P2P exchange going when the initial master leaves. Full writeup in `OP2SessionLogger/NETPLAY.md`.

## Verified milestones (live, retail 1.3.6 host)
✅ discover → ✅ join (NetID/slot) → ✅ GUR bidirectional (host acks us) → ✅ lobby join as
named player "OPU" (visible slot) → ✅ send lobby chat "Hello!" → ✅ host replied "hey" →
✅ detect EJECT (host kicked OPU; client reported it and exited cleanly) →
✅ decode the full GameStartInfo (scenario/options/all player records) →
✅ live change-detection: diff each GameStartInfo (ignoring seq/ack churn) and report exactly
   what the host edits - confirmed catching color changes ("Leviathan Red→Cyan", "OPU
   Magenta→Red"), name propagation, and works for every option / player field / join / leave →
✅ handle disconnect/end: HOST ENDED THE LOBBY (cmd 0x0B, confirmed via host CANCEL),
   GAME CANCELED (cmd 10), HOST IS STARTING (cmd 7/8), HOST STOPPED RESPONDING (>5s silence -
   covers crash / network loss / silent EJECT-button removal) →
✅ READY-UP (the WRITE side): cmd-2 status update (field=4 Ready, value=1) as our seq-3 channel-A
   packet → host applied it and the re-broadcast came back as "OPU Ready: no → YES", enabling
   the host's START. We can now set our own lobby state, not just read it. →
✅ START HANDSHAKE - THE GAME LAUNCHES: roster reply cmd-6 status(3) to cmd-4 (cleared
   "Sending players list..." in one shot) → on cmd-8 load&go sent our cmd-9 "loaded" marker
   (skipping the file checksum) → host acked it (HOST-ACKS-US=04) → host broadcast the final
   cmd-9 GO → "GAME STARTED". The host left the lobby and loaded the mission with our bot as a
   live player. →
✅ RECEIVE in-game COMMAND PACKETS: the running host broadcast engine cmd 0x0C command-turn
   packets, which our bot received, decoded, and acked. →
✅ SEND in-game COMMAND TURNS - THE SIM RUNS: echo the host's cmd-0x0C blocks verbatim (non-zero
   unk required by receiver FUN_00420e00) under our player index. The host stored our turns and
   the lockstep simulation advanced continuously (-12 → … → ~2000 ticks in ~60s, 500+ turns,
   no desync). **THE BOT IS A LIVE PLAYER IN A RUNNING RETAIL-1.3.6 MULTIPLAYER MATCH.**
   THE ULTIMATE GOAL IS COMPLETE: join → ready → start → send/receive command packets → the
   game runs with our from-scratch reverse-engineered client as the second player. →
✅ IN-GAME CHAT, BOTH WAYS: receive (decode the host's type-0x30 command blocks) and send
   (inject our own type-0x30 block at a fresh, never-committed mark). Confirmed live on the
   host's comms panel: "1: OPU: Good Luck! Have Fun!" alongside "0: Leviathan: testing". →
✅ CLEAN IN-GAME LEAVE: self-send a final command turn carrying a goodbye chat (0x30) and a
   ctQuit (0x31) block at a fresh mark. The host removed the bot cleanly and printed "OPU has
   quit the game" - no lost-contact timeout. (2-player; see finding #16.) →
✅ MULTI-PEER (3+ PLAYERS): parse the cmd-4 roster into a peer table and broadcast our command
   turns (dst 0, flags 0x00) to every peer; read in-game chat + leaves from every player's turn.
   A 3-player match ran past 3400 ticks with the bot as a full player, all players' chat shown,
   and a clean "X has left the game" when a player quit - the rest kept playing. (See #17.)
