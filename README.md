# OP2NetworkTools

A suite of network tools for **Outpost 2**, all speaking the game's own wire
protocols **without running Outpost 2**. The headline result is a complete external
multiplayer **client** - an independent (non-engine) Outpost 2 client: it
joins, readies, starts, and plays a real multiplayer game as Player 2, holding the lockstep
simulation and chatting both ways.

## Projects

### OP2DummyPlayer
![Screenshot](https://images.outpostuniverse.org/OP2DummyPlayer.png)

The multiplayer client as a polished cross-platform (Windows + Linux) **Dear ImGui** GUI,
with the reusable net-client library cleanly separated from the UI. Join, ready, play, and
chat from a window. -> [`OP2DummyPlayer/README.md`](OP2DummyPlayer/README.md)

### OP2LanScan
![Screenshot](https://images.outpostuniverse.org/OP2LanScan.png)

Finds OP2 multiplayer games on your LAN (and across subnets via direct-IP query). C# /
WinForms desktop app. -> [`OP2LanScan/README.md`](OP2LanScan/README.md)

### OP2Session
![Screenshot](https://images.outpostuniverse.org/OP2Session.png)

The same multiplayer client as a single-file Windows console tool - the research front-end
where the protocol was cracked. Heavily diagnostic (toggle the per-packet dumps via its
`op2session.ini`). -> [`OP2Session/README.md`](OP2Session/README.md)

### OP2SessionLogger

A developer tool (same Dear ImGui base as OP2DummyPlayer) that joins a game like a player and
**logs decoded in-game command packets** to a text file - a command-type census plus a per-command
decode (type, unit selection, known fields, undecoded tail) for reverse-engineering the command
set. -> [`OP2SessionLogger/README.md`](OP2SessionLogger/README.md)

## Reference Documents
- [`PROTOCOL.md`](PROTOCOL.md) - the multiplayer protocol, byte-exact: discover -> join ->
  lobby -> start -> in-game command turns + chat.
- [`NETWORK_PROTOCOL.md`](NETWORK_PROTOCOL.md) - deeper synthesis of the networking functions
  (stack layers, GurManager reliability, lobby state machine, packet formats).
- [`DISCOVERY_PROTOCOL.md`](DISCOVERY_PROTOCOL.md) - the UDP discovery query/reply wire spec.
- [`JOIN_PROTOCOL.md`](JOIN_PROTOCOL.md) - early over-arching join + command-turn spec.
- [`START_CHECKSUM.md`](START_CHECKSUM.md) - the game-start file checksum (why we can skip it).

Each project folder has its own README with build + usage details.

## Status
Verified live against Outpost 2 host: LAN discovery, full join->play pipeline, two-way
lobby and in-game chat, a stable lockstep match with the external client as a player, a clean
in-game leave (ctQuit, so the host removes the bot without a drop-timeout), and **multi-peer
(3+ player) games** - the bot broadcasts its command turns to every peer and shows all players'
chat. A 3-player match ran past 3400 ticks with the bot as a full player.
