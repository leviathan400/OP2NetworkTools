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
decode (type, unit selection, every payload field) covering all 55 command opcodes.
-> [`OP2SessionLogger/README.md`](OP2SessionLogger/README.md)

### OP2Spectator

Joins a game as a player and **records the match** to structured files - every player's actions
(build orders, production, research, movement, combat orders, logistics), the chat, and the roster.
Built on the OP2SessionLogger net layer. Outpost 2 is deterministic lockstep, so it sees every
player's *input* but not derived simulation state. -> [`OP2Spectator/README.md`](OP2Spectator/README.md)

### op2finder / op2finder-py

Console LAN session finders - the same discovery protocol as OP2LanScan, without the GUI. `op2finder`
is a single-file C tool (no dependencies); `op2finder-py` is the readable Python reference
implementation (standard library only). Both support a direct-IP query.
-> [`op2finder/README.md`](op2finder/README.md), [`op2finder-py/README.md`](op2finder-py/README.md)

### selftest

A small round-trip check of the discovery query (checksum + field layout) against the spec and
against the built OP2LanScan. -> [`selftest/README.md`](selftest/README.md)

## Reference Documents
- [`PROTOCOL.md`](PROTOCOL.md) - the multiplayer protocol, byte-exact: discover -> join ->
  lobby -> start -> in-game command turns + chat.
- [`NETWORK_PROTOCOL.md`](NETWORK_PROTOCOL.md) - deeper synthesis of the networking functions
  (stack layers, GurManager reliability, lobby state machine, packet formats).
- [`DISCOVERY_PROTOCOL.md`](DISCOVERY_PROTOCOL.md) - the UDP discovery query/reply wire spec.
- [`JOIN_PROTOCOL.md`](JOIN_PROTOCOL.md) - early over-arching join + command-turn spec.
- [`START_CHECKSUM.md`](START_CHECKSUM.md) - the game-start file checksum (why we can skip it).
- [`OP2SessionLogger/NETPLAY.md`](OP2SessionLogger/NETPLAY.md) - lobby/transport deep-dive +
  the OPU **1.4.1** compatibility fix (host-version gate, same-machine bind collision).

Each project folder has its own README with build + usage details.

## Status
Verified live against Outpost 2 host: LAN discovery, full join->play pipeline, two-way
lobby and in-game chat, a stable lockstep match with the external client as a player, a clean
in-game leave (ctQuit, so the host removes the bot without a drop-timeout), and **multi-peer
(3+ player) games** - the bot broadcasts its command turns to every peer and shows all players'
chat. A 3-player match ran past 3400 ticks with the bot as a full player.

Works against **both retail 1.3.6 and OPU 1.4.1** hosts. The clients advertise `TApp::Version`
**1.4.0.1** by default (pass `--ver 1.3.0.4` for a 1.3.6 host) - the host ejects a version mismatch.
Same-machine play needs the game-socket bind to **match the host's style** - a host with **NetHelper on**
binds wildcard `0.0.0.0`, a host with **NetHelper off** (vanilla) binds specific-IP - or Windows lets the
two co-exist on one port and the most-specific socket steals the traffic. The clients **auto-detect**
which (a wildcard-bind probe of the host's port), so `--ver` is only the version gate, not the bind mode.
In game it's confirmed **peer-to-peer** (and the bot keeps playing if the host leaves). See
[`OP2SessionLogger/NETPLAY.md`](OP2SessionLogger/NETPLAY.md).
