# OP2Session - external Outpost 2 multiplayer client

A standalone C++/Winsock console tool that joins a live **Outpost 2**
multiplayer session **without running the game** - speaking OP2's own
discovery/join/lobby/GUR/in-game protocol from scratch. It discovers a hosted game,
completes the join handshake, runs the reliable transport, appears in the GAME SETTINGS
lobby as a named player, chats, readies up, survives the start handshake, and then keeps the
running game's lockstep simulation advancing by exchanging in-game command turns.

> Status: **complete pipeline, verified live against a retail 1.3.6 host.** Believed to be
> the first independent (non-engine) client to join and play an OP2 multiplayer game. A bot
> ("OPU") joined the lobby, chatted both ways, readied up, drove the host through START,
> and ran as Player 2 in a live match - the simulation advanced past 2000 ticks with 500+
> command turns sent and acked, no desync.

This is the research front-end of the `op2network` project; the discovery layer it builds
on is the finished `..\OP2LanScan` / `..\op2finder` work.

## What it does (full flow)
```
discover (:47776)
  -> join handshake (:47777)            assigned NetID + slot
  -> GUR reliable transport (:47800)    bidirectional, host acks us
  -> cmd-3 ShowJoinGame                 we appear in the lobby as "OPU"
  -> cmd-6 chat                         we say "Hello!" in the lobby (host can reply)
  -> cmd-2 ready-up                     we set our READY bit
  -> cmd-4/5 roster handshake           reply cmd-6 status(3)/status(4)
  -> cmd-8 load&go -> cmd-9 "loaded"    skip the file checksum (client-side self-check)
  -> host cmd-9 GO                       the mission launches, we are a live player
  -> cmd-0x0C command turns             reply a ctNop per mark so the sim keeps ticking
  -> in-game chat (cmd-0x0C type 0x30)  receive host messages; send "Good Luck! Have Fun!"
                                        at tick 100 (pinned to a fresh mark)
```
Heavily diagnostic: it dumps every host packet (de-duped), decodes the wire + GUR headers,
decodes and diffs the GameStartInfo (so it reports exactly which lobby setting the host
changed), and prints periodic state (`joined=YES`, `live=YES`, `HOST-ACKS-US`, sequence/ack
counters, `cmd0c=` turn count).

## Build & run
```
build.bat                 (MSVC: cl /O2 op2session.cpp ws2_32.lib  -> op2session.exe)
op2session.exe            (auto-discovers the host)
op2session.exe 10.0.61.18 (target a host by IP)
```
- .NET not involved; a single self-contained exe (Winsock only).
- **Logging:** `op2session.ini` (next to the exe) controls the per-packet dumps:
  `[log] verbose=0` = clean milestone-only output (good for screenshots), `verbose=1` = full
  hex + decoded dump of every packet (debug). Missing ini defaults to verbose on.
- The player name ("OPU") and chat text ("Good Luck! Have Fun!") are constants near the top of the
  post-join section in `op2session.cpp` - easy to change.
- It runs up to ~10 minutes; once in a game it stays a live player until the host leaves
  (then it reports the disconnect and exits cleanly).

### To try it
1. Host an OP2 **Multiplayer -> TCP/IP -> Create Game** and sit on GAME SETTINGS.
2. Run `op2session.exe`. A second player slot fills with the name and the chat box shows its
   "Good Luck! Have Fun!". The bot readies up on its own.
3. Click **START**. The bot answers the roster + start handshake, the mission loads, and the
   simulation runs with the bot as Player 2 (watch the tick/mark advance).

## How it works (one screen)
Every packet is a 14-byte wire header `[srcNetID(4) dstNetID(4) payloadSize(1) type(1)
checksum(4)]` + payload. `type==1` is transport control (join, status, roster); `type==0`
carries the GUR reliable layer whose 4-byte sub-header at wire `0x0E` is
`[flags, seq, ackA, ackB]`, followed by the engine command byte at `0x12`. The checksum is
the `0xFDE24ACB` family (`FUN_00490F10`). The four hard-won keys:

1. **Join** uses the per-session GUID from the discovery reply, not the constant game GUID.
2. **Roster** clears only when we report transport **status 3** (the host bumps occupied
   slots 2->3 and waits for that exact value).
3. **Start** needs only the `cmd-9` "loaded" marker; the file checksum is a client-side
   self-check the host never inspects, so we skip it.
4. **In-game** the command-turn receiver discards any block with a zero `unk` field, so we
   echo the host's command blocks verbatim under our player index.

See `FINDINGS.md` for the full story and `PROTOCOL.md` for the byte-exact spec.

## Files
- `op2session.cpp` - the client (discovery + join + GUR + lobby + chat + ready + start +
  in-game command turns), heavily commented with the decompile function anchors.
- `build.bat` - MSVC build (location-independent).
- **`PROTOCOL.md`** - byte-exact wire protocol (join, GUR, lobby, chat, ready, start,
  command turns) - the reference.
- **`FINDINGS.md`** - the RE journey, gotchas, and lessons (read this to understand *why*).
- `last_run.txt` / `live*.txt` - captured run logs (dev artifacts).

Related docs (repo `docs\`): `JOIN_PROTOCOL.md` (over-arching spec),
`START_CHECKSUM.md` (the game-start file checksum, fully RE'd).

## Where to go next
- **Act, don't idle**: fill real command blocks (build/move units) at command type != 0,
  keeping a non-zero `unk`; the command-type enum is in `FINDINGS.md` / `PROTOCOL.md`.
- **Clean leave**: send `ctQuit` (command type 0x31) instead of relying on silence.
- **Tidy build**: gate the diagnostic dump behind a `-v` flag for a reference-quality client.

## Caveats
- Built and verified against ** 1.3.6** on a LAN. OPU 1.4.1 shares the wire format.
- Firewall: allow `op2session.exe` (or inbound UDP) - same as OP2 itself.
- This is an experimental protocol client, not a polished tool; expect verbose output and
  hard-coded constants. It is an idle (passive) player - it keeps the game alive but issues
  no gameplay commands of its own.
