# OP2SessionLogger

A **developer / reverse-engineering tool** for the **Outpost 2** multiplayer protocol. It is built
on the same from-scratch net-client as OP2DummyPlayer - it joins a live game like a real player and
holds the lockstep - but instead of being a polished user tool, its job is to **log and decode the
in-game command packets** to a text file so the command set can be reverse-engineered.

It shares OP2DummyPlayer's look (C++ / Dear ImGui + GLFW) and flow (Scan LAN & Join, lobby, in-game
stats, chat), with the diagnostics that OP2DummyPlayer deliberately omits.

## What it logs (`op2sessionlogger_debug.txt`, next to the exe)
Beyond the usual connect / join / phase / disconnect events, while in a game it writes:

- **Command-type census** (every 30 s and on disconnect): each command type seen, its count, its
  length range, and a byte sample. This is the fast way to learn *which* command types real
  gameplay actually uses.
  ```
  === command-type census (final): 8 types, 3282 blocks (incl. sliding-window resends) ===
    0x00 ctNop               x2784   len 0..0
    0x02 ctMoMove            x78     len 9..13   sample: 03 13 00 12 00 11 00 01 00 ...
    0x09 ctMoProduce         x6      len 19..19  sample: 01 0e 00 01 00 90 04 48 00 ...
    0x30 ctChat              x6      len 6..6    sample: 00 ff 62 79 65 00
  ```
- **Per-command decode** (each distinct command once, deduped by content): the type name, the unit
  **selection** (parsed via the engine's selection helpers), the fields we fully understand, and the
  still-undecoded **tail**.
  ```
  CMD 0x02 slot=0 exec=288  len=9   ctMoMove sel=[19] to=(1863,3685)
  CMD 0x14 slot=0 exec=208  len=3   ctMoSelfDestruct sel=[17]
  CMD 0x06 slot=0 exec=564  len=19  ctMoBuild sel=[33] to=(1488,432) footprint=(43,10)-(46,13)
  CMD 0x30 slot=0 exec=1020 len=6   ctChat sender=0 mask=ff "bye"
  CMD 0x32 slot=0 exec=1100 len=4   ctAlly from=0 to=1
  CMD 0x09 slot=0 exec=488  len=8   ctMoProduce factory=14 item=144 weapon=0 stub=-1
  ```

The decoder (`cmd_name` / `decode_cmd` in `src/net/op2client.cpp`) follows the engine's own
`DataStructure CommandPacket.txt` (the authoritative per-type field spec), cross-checked against the
command executor `FUN_0040e300` and live capture. Command names are the engine's CommandType string
table (all 55 opcodes, `ctNop`..`WeaponFiring`). It renders fields for the full command set -
the move group (`0x02`-`0x04`/`0x26`, packed `pixelX:15|pixelY:14` waypoints), build / build-wall /
remove-wall, produce, transfer-cargo, research, train, repair/reprogram/dismantle, attack/guard,
create, light-toggle, game-options, chat, quit, ally, and machine-settings - leaving only a handful
of map_id enums and a couple of trailing fields as `tail:` to be pinned. The full command-packet
write-up - block structure, the unit-selection header, packed coordinates, the type map, and the
decoded layouts - is in [`FINDINGS.md`](FINDINGS.md).
For the surrounding wire/transport protocol see `..\PROTOCOL.md` / `..\FINDINGS.md`.

## How to capture cleanly
The deterministic lockstep sim puts **nothing** on the wire except player input plus a couple of meta
commands (`ctNop` keepalive, `0x34` sync), so in a 2-player game **every other command is the host's
action**. To decode a specific command:

1. Host a 2-player game; let OP2SessionLogger **Scan LAN & Join**; click START.
2. Do **one action type, repeated** (e.g. move one unit to several far-apart tiles), spaced out.
3. Disconnect and read the `CMD` lines: the bytes that change between repeats are the variable
   fields (coordinates, ids); the ones that stay are the command code.

Run an **idle baseline** first (do nothing) to confirm the background set.

## Build
Same as OP2DummyPlayer:
```
winbuild.bat            (Windows, VS toolchain + bundled CMake/Ninja -> build\OP2SessionLogger.exe)
```
or generic CMake (`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`).
First configure fetches ImGui + GLFW.

## Run
Launch `OP2SessionLogger.exe` and click **Scan LAN & Join**, or auto-connect on startup (handy for
scripted capture runs - no need to drive the GUI):
```
OP2SessionLogger.exe --scan              # scan the LAN and join the first hosted game
OP2SessionLogger.exe --ip 10.0.0.5       # join a specific host directly
OP2SessionLogger.exe --scan --ver 1.3.0.4  # advertise a specific TApp::Version
OP2SessionLogger.exe --debug             # self-test: join, post a "leaving" chat, leave cleanly, exit
OP2SessionLogger.exe --debug 5m          # TIMED run: join, count down on the form, auto-exit after 5 min
```
The GUI still works normally after an auto-connect. The log is written to
`op2sessionlogger_debug.txt` next to the exe.

`--debug` is a headless self-test: it scans + joins (implies `--scan`), posts a "Doing debug run,
Leaving" chat, leaves cleanly (real `cmd-0x0B` quit so the host frees our slot), then closes the app -
so you get a complete clean log and a correctly-shut-down process in one shot. Combine with `--ver`
for a 1.3.6 host: `--debug --ver 1.3.0.4`.

`--debug <dur>` is the **timed** variant: it scans + joins, shows an "auto-exit in M:SS" countdown on
the form, runs for the period, then leaves cleanly (in-game `ctQuit` if a game is live) and closes.
The duration accepts `5m`, `90s`, `2m30s`, or a bare number of seconds. Use it to capture a fixed-length
live session unattended - e.g. start `--debug 5m`, host a 3-player game, and have the master quit
mid-match to capture the host-leave / resync behavior (see `NETPLAY.md`).

**Host version must match.** An OP2 host drops a peer whose advertised `TApp::Version` differs from
its own, and the client must send its version (in `cmd-3`) during the join handshake - before the
host's version is known. So pick it up front with `--ver`:
- default (no flag) = **1.4.0.1** -> joins **OPU 1.4.1** hosts
- `--ver 1.3.0.4` -> joins **retail 1.3.6** hosts

The host's own version is shown in chat and the lobby panel ("Host Ver") once joined, so you can see
which to use. On a mismatch the host ejects us ("Player ... had wrong game version"); the client
detects this and the status reads **"Ejected: wrong game version. Host is X, we sent Y. Pass --ver X."**

Works against **both retail 1.3.6 and OPU 1.4.1** hosts. Same-machine play needs the game-socket bind
to **match the host's style** - a host with **NetHelper on** binds wildcard `0.0.0.0`, a host with
**NetHelper off** (vanilla) binds specific-IP - because Windows lets a wildcard and a specific bind
co-exist on one port and the most-specific socket steals the traffic. The tool **auto-detects** which
(a wildcard-bind probe of the host's port), so `--ver` is only the version gate. In game it's confirmed
**peer-to-peer**, and the tool keeps playing (clock failover) if the host leaves. Full lobby/transport
write-up in [`NETPLAY.md`](NETPLAY.md).

## Relationship to OP2DummyPlayer
- **OP2DummyPlayer** = the clean, user-facing client (join, ready, play, chat, leave).
- **OP2SessionLogger** = this dev tool: the same client plus the command-logging diagnostics.

The two currently keep their own copy of the net-client (`src/net/`). When the protocol layer
changes, apply the change to both.
