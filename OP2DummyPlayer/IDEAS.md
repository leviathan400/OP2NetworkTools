# OP2DummyPlayer - future ideas (not scheduled)

Parking lot for things we have discussed but are deliberately NOT building right now. Each
entry notes the gist and why it is/ isn't worth it.

## 1. Make the bot actually DO things in-game (issue real gameplay commands)
**Status: out of scope (would mean rebuilding the game engine).**

To issue meaningful commands (build, move, attack) the bot would need to *understand the game
world*, not just the wire protocol. That means:
- Knowing the full **starting state** of the map/scenario (units, buildings, resources, tech) -
  which lives inside the level DLL + the engine's mission init, not on the wire.
- **Tracking every change** by replaying the command-turn stream (and the deterministic sim
  rules) to keep a live model of the world - i.e. running the same lockstep simulation the
  real game runs.

That is effectively reimplementing the Outpost 2 simulation. Way beyond a "dummy player". The
current idle bot (empty `ctNop` turns + chat) is the right altitude: it stays in lockstep and
talks, without needing any game-state model.

## 2. HOST games, not just join them
**Status: feasible, candidate for a future version.**

The app currently only *joins*. It could instead *host* a game: be the authority other clients
(real OP2 or another OP2DummyPlayer) connect to. We already understand both sides of the
protocol from the decompile, so the pieces are known:
- **Discovery responder** on UDP 47776: answer the broadcast query with the 81-byte reply
  (host IP, session GUID, game name, max players, scenario type).
- **Join responder** on 47777: accept join requests, assign NetIDs/slots, reply (result 1/2).
- **Lobby authority**: maintain the roster, broadcast the 191-byte **GameStartInfo** (cmd-1),
  relay chat (cmd-6), apply ready/colour/race updates (cmd-2), handle quit/eject (cmd-0x0B).
- **Start handshake**: ReplicatePlayersList (cmd-4/5), then cmd-7/8 load&go, collect each
  client's cmd-9 "loaded", broadcast the final cmd-9 GO.
- **In-game**: drive the command-turn loop as the authority (broadcast cmd-0x0C each mark,
  enforce lockstep), and relay in-game chat (command block type 0x30).
- A real game still needs a **scenario/map** and the file-checksum master to hand out, so a
  pure external host may only be able to host games that real clients can actually load - worth
  scoping before committing.

Relevant anchors are already documented in `..\PROTOCOL.md` and `..\FINDINGS.md`
(host-side functions: discovery responder `FUN_0048C2C0`, `HostGame` `FUN_00495ED0`,
`ReplicatePlayersList` `FUN_00496950`, host `StartGame` `FUN_00461700`).

## 3. Multi-peer support (play in 3+ player games)
**Status: DONE - verified live in a 3-player match.**

The client now plays in 3+ player games. OP2 gameplay is peer-to-peer, so it parses the cmd-4
SetPlayerList roster into a peer table and **broadcasts** its command turns (dst 0, flags 0x00)
to every peer's game address, and reads in-game chat + leaves from every player's turn. Verified
live: a 3-player match ran past 3400 ticks with the bot as a full player, all players' chat
shown, and a clean "X has left the game" when a player quit (the rest kept playing). The crux
was that command turns must broadcast on flags 0x00 (mark-deduped), not the per-peer channel A a
3rd player would reject. Full write-up in `..\FINDINGS.md` finding #17 and `..\PROTOCOL.md`
section 10.
