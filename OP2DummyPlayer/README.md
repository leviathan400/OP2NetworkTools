# OP2DummyPlayer

A cross-platform (Windows + Linux) GUI client that joins a live **Outpost 2** LAN game
**without running Outpost 2**, using the reverse-engineered network protocol. It scans for or
connects to a host, appears in the lobby as a named player, readies up, follows the start
handshake, keeps the in-game lockstep alive, chats both ways, and leaves cleanly.

It is the GUI sibling of the console research tool `..\op2session`; the protocol logic is the
same, verified live against a retail Outpost 2 host.

## Architecture - net and GUI are cleanly separated
```
src/
  net/                 <- the net-client library (op2net). NO GUI dependency.
    socket_compat.h    <- Winsock <-> POSIX UDP shim
    op2proto.h/.cpp    <- pure wire helpers: constants, checksums, GameStartInfo decode
    op2client.h/.cpp   <- threaded client: discover -> join -> lobby -> start -> in-game,
                          exposing a thread-safe ClientState snapshot + simple commands
  gui/
    main.cpp           <- Dear ImGui (GLFW + OpenGL3). Reads snapshots, calls commands.
CMakeLists.txt         <- builds op2net (standalone) + the GUI; fetches ImGui + GLFW
winbuild.bat           <- one-click Windows build via the VS-bundled CMake + Ninja
```
The GUI never touches a socket or a wire byte. `op2net` builds and links on its own, so it can
also back a CLI, automated tests, or a different UI toolkit later.

### The boundary (what the GUI uses)
```cpp
op2::Client client;
client.setPlayerName("OPU");
client.connectScan();              // or client.connectIp("10.0.61.18");
client.sendChat("hello");          // routed: lobby chat or in-game chat by phase
client.disconnect();               // graceful: leave the lobby / say goodbye, then stop
op2::ClientState st = client.snapshot();   // immutable copy, taken under a mutex
```

## What the GUI shows
- Always: **Player Name** (read-only, fixed identity), **Our IP**, connection status.
- Joining: **Scan LAN & Join**, or enter an **IP** and **Join IP**.
- In the lobby: **Host IP**, **Game name**, **Map DLL**, **Our Color**, and a table of **all
  players** (name / color / race / ready).
- In game: **Game Duration**, **Game Tick**, **Game Mark** (= tick / 100, matches OP2's own
  "Current Mark"), and **command-packet counts** (sent / received).
- A unified, timestamped **chat log** (`[hh:mm] name: text`) with a send box - lobby and
  in-game chat, both directions, including the host's and other players' messages.
- A clear **DISCONNECTED** banner with the reason, plus an inline system line in the chat:
  `*** GAME STARTED ***`, `*** GAME DISCONNECTED ***`, or `*** EJECTED FROM LOBBY ***`.

## Behaviors
- **Auto-ready** on join, so the host can START.
- **Auto-greetings**: `Hello from OP2DummyPlayer!` on entering the lobby, and
  `Good Luck! Have Fun!` about 100 ticks into the game.
- **Clean leaving** (Disconnect button or closing the window):
  - From the **lobby** -> sends a real **cmd-0x0B quit** so the host frees our slot (no chat).
  - From an **in-game** match -> sends `I am leaving the game.` in chat, delivers it, then exits.
- **Disconnect handling**:
  - A user Disconnect / rejoin clears the chat for a fresh session.
  - A **host-initiated** end (game over, host quit, eject, timeout) does **not** clear the chat -
    it appends a system line so the history stays readable.
  - **Eject** in the lobby is detected (targeted cmd-0x0B, or the host going silent) and shown
    as `*** EJECTED FROM LOBBY ***`.
- **Crash-safe reconnect**: a finished worker thread is reaped before a new connection starts.
- **Debug log**: `op2dummyplayer_debug.txt` is written next to the exe (append-only, flushed
  per line so it survives a crash) with timestamped connect/discover/join/phase/disconnect
  events.

## Build

### Prerequisites
- CMake >= 3.16 and a C++17 compiler.
- Internet on first configure (CMake fetches ImGui v1.91.5 + GLFW 3.4).
- **Linux only:** GLFW build deps, e.g. on Debian/Ubuntu:
  `sudo apt install build-essential cmake xorg-dev libgl1-mesa-dev`

### Windows
One-click with the Visual Studio toolchain (uses the VS-bundled CMake + Ninja):
```
winbuild.bat
```
-> `build\OP2DummyPlayer.exe` (windowed app, no console).

Or the generic CMake path (e.g. the VS generator):
```
cmake -S . -B build
cmake --build build --config Release
```

### Linux
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
-> `build/OP2DummyPlayer`.

## Use
1. Start an OP2 **Multiplayer -> TCP/IP -> Create Game** on the LAN and sit on GAME SETTINGS.
2. Launch OP2DummyPlayer, click **Scan LAN & Join** (or type the host IP -> **Join IP**). The
   bot appears in the lobby, auto-readies, and greets the lobby.
3. Click START on the host. The bot follows the handshake; in-game stats and chat start updating.
4. Click **Disconnect** (or close the window) to leave cleanly.

## Notes & limitations
- Verified against **retail 1.3.6** on a LAN; OPU 1.4.1 shares the wire format.
- Firewall: allow the app (or inbound UDP on 47776-47807), same as Outpost 2 itself.
- The bot is an **idle (passive) player**: it keeps the lockstep alive with empty command
  turns and chats, but issues no gameplay commands. The file checksum is skipped (it is a
  client-side self-check the host never inspects).
- Chat is sent reliably enough for a LAN (a few redundant sends); a hardened build would track
  per-message acks. The lobby-silence eject detector also fires on a host crash/quit, since a
  passive client can't tell those apart from an eject on the wire.
- `winbuild.bat` hard-codes a VS install path; adjust it for your machine, or use the generic
  CMake commands above.
- Protocol reference: `..\op2session\PROTOCOL.md` (byte-exact) and `..\op2session\FINDINGS.md`
  (the reverse-engineering journey, including the in-game chat fresh-mark gotcha).
- Parked future ideas (in-game actions, hosting games) are noted in `IDEAS.md`.
