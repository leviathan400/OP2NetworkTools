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
  CMD 0x02 slot=0 exec=288  len=9   ctMoMove sel=[19] to=(1315,79)
  CMD 0x14 slot=0 exec=208  len=3   ctMoSelfDestruct sel=[17]
  CMD 0x30 slot=0 exec=1020 len=6   ctChat sender=0 mask=ff "bye"
  CMD 0x32 slot=0 exec=1100 len=2   ctAlly target-player=2
  CMD 0x09 slot=0 exec=488  len=19  ctMoProduce unit=14 tail: 01 00 90 04 48 00 21 00 ...
  ```

The decoder (`cmd_name` / `decode_cmd` in `src/net/op2client.cpp`) is derived from the engine command
executor `FUN_0040e300` and its selection helpers `FUN_00490920` / `FUN_00490960`. Command names are
**canonical** - the engine's own CommandType string table from the decompile (all 55 opcodes,
`ctNop`..`WeaponFiring`). Field layouts decoded so far: **ctMoMove/ctMoDock** (`0x02`/`0x03`, with
destination X,Y), **ctMoSelfDestruct** (`0x14`), **ctChat** (`0x30`), **ctQuit** (`0x31`),
**ctAlly** (`0x32`), **ctMachineSettings** (`0x34`); decoded live pending one confirming capture:
**ctMoBuild** (`0x06`, target X,Y + tile-rect footprint + `ff ff`), **ctMoResearch** (`0x16`,
lab + techIndex + scientists), **ctMoTransferCargo** (`0x0a`, unit + bay). The full
command-packet write-up - block structure,
the unit-selection header, the type map, and the decoded layouts - is in [`FINDINGS.md`](FINDINGS.md).
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

## Relationship to OP2DummyPlayer
- **OP2DummyPlayer** = the clean, user-facing client (join, ready, play, chat, leave).
- **OP2SessionLogger** = this dev tool: the same client plus the command-logging diagnostics.

The two currently keep their own copy of the net-client (`src/net/`). When the protocol layer
changes, apply the change to both.
