# OP2Spectator

Joins an Outpost 2 multiplayer game **as a player** and **records the match** to structured files —
every player's actions, the chat, and the roster — without running the game engine. It's the
observation/recording member of the OP2NetworkTools suite, built on the same proven net layer as
[`OP2SessionLogger`](../OP2SessionLogger) but with the RE/census diagnostics replaced by a clean,
analysable match recorder.

## What it captures (and what it can't)

Outpost 2 is **deterministic lockstep**: the only thing on the wire is each player's *input* (command
packets), one batch per tick — every client then simulates the same result locally. So OP2Spectator can
see **everything every player does**, but **not** the derived simulation state.

**Recorded (on the wire, reliable):**

| | Commands |
|---|---|
| Build order (structure, where, when) | `ctMoBuild`, `ctMoBuildWall` |
| Production (unit, factory, weapon) | `ctMoProduce` |
| Research / training | `ctMoResearch`, `ctMoTrainScientists` |
| Unit creation | `ctMoCreate` |
| Movement / scouting (waypoints) | `ctMoMove`/`Dock`/`Patrol`/`StandGround` |
| Combat orders (targets) | `ctMoAttack`, `ctMoGuard` |
| Economy logistics | `ctMoTransfer`, `TransferCargo`, `CargoRoute`, `Recycle`, `Salvage` |
| Diplomacy | `ctAlly` |
| Cheats used | `ctGameOpt` |
| Chat (lobby + in-game) | `ctChat` + lobby chat |
| Joins / readies / leaves / drops | roster diffs + `ctQuit` |
| Lobby metadata | map, players, colors, races, host version, NetHelper on/off |

**NOT recorded (sim-only — would require running OP2):** resources, population, morale, food, power,
unit HP, current army composition. (You see every unit a player *builds*; combat *losses* are computed
locally and never sent, so live composition can't be tracked — only decisions can.)

## Output

On joining a game it opens three files next to the exe, stamped with the match start time:

```
op2spectator_match_<stamp>.log       human-readable master log: lobby header, roster, chat, every action
op2spectator_match_<stamp>.jsonl     one JSON object per in-game action (for tooling / analysis)
op2spectator_match_<stamp>.summary   per-player action counts + build order, written when the match ends
```

Master-log lines carry the OP2 game tick/mark, e.g.:
```
[t1234  M12] slot0 DevHost  ctMoBuild sel=[5012] to=(480,512) footprint=(30,20)-(34,23)
[t1240  M12] slot2 Leviathan  ctMoProduce factory=14 item=144 weapon=0 stub=-1
[t1255  M12] CHAT DevHost: gl hf
```

Actions are **deduped by execution mark** — the lockstep sliding window resends each block many times,
but always pinned to the same mark, so each real action is logged exactly once.

## Running

The CLI flags are inherited from the shared base:

```
OP2Spectator.exe --scan                   # scan the LAN and join the first hosted game
OP2Spectator.exe --scan --ver 1.3.0.4     # advertise a version (for the lobby version gate)
OP2Spectator.exe --debug 5m               # timed: join, record for 5 min, leave cleanly, exit
```

It auto-detects the host's bind style (NetHelper on/off — see `..\OP2SessionLogger\NETPLAY.md`), readies
up so the host can start, then records until the match ends or it's disconnected. The GUI shows the
lobby, a unified chat log, in-game stats, and a `● REC` indicator with the match-file name and a live
action count. (`--ver` only sets the advertised lobby version; the bind mode is detected, not inferred.)

> Note: OP2 has no true spectator slot, so OP2Spectator occupies a normal player slot and stays idle
> (it never issues unit commands). Issuing commands is a separate project, deliberately out of scope here.

## Build

```
winbuild.bat
```
(Same toolchain as the rest of the suite — VS18 + bundled CMake/Ninja; the `vswhere.exe` warning is
harmless. Output: `build\OP2Spectator.exe`.)

## Relationship to the suite

- **OP2SessionLogger** — the RE/dev base: decodes command packets to learn the protocol.
- **OP2Spectator** (this) — the recorder: joins, watches, and writes a structured match log.
- **OP2DummyPlayer** — the clean user-facing client.
- **OP2Session** — single-file console research tool.

All four share the same from-scratch net layer (discover → join → lobby → in-game). The wire/transport
deep-dive lives in [`..\OP2SessionLogger\NETPLAY.md`](../OP2SessionLogger/NETPLAY.md); the command-packet
decode reference is in [`..\OP2SessionLogger\FINDINGS.md`](../OP2SessionLogger/FINDINGS.md).
