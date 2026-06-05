# OP2LanScan

A Windows desktop app (C# / WinForms, .NET Framework 4.8) that finds **Outpost 2**
multiplayer games on your local network **without running Outpost 2**. It speaks OP2's own
UDP discovery protocol: it broadcasts the same query the game's *Find Session ->
"Broadcasting on the local network"* sends, and lists every host that answers.

![Screenshot](https://images.outpostuniverse.org/OP2LanScan.png)

## Build
Open `OP2LanScan.slnx` in Visual Studio 2022/18 and press F5, **or** from a command line:
```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" ^
    OP2LanScan\OP2LanScan.csproj /t:Build /p:Configuration=Release /p:Platform=AnyCPU
```
Output: `OP2LanScan\bin\Release\OP2LanScan.exe` - .NET Framework 4.8, a single self-contained
exe (no extra DLLs). The window/exe icon is `Plymouth.ico` (embedded resource).

## Use
- **Scan LAN** - broadcast once and refresh the list.
- **Auto-refresh (3s)** - keep scanning; sessions that go away drop off after ~12s.
- **Find at IP/host** - type an IP or hostname and **Query** to probe one machine directly
  (the same as OP2's *"Looking at the IP address"* option). Works across subnets / over the
  internet / through a VPN, where broadcast can't reach. The IP is **pinned**: every refresh
  keeps querying it, and the status bar reports `<ip>: GAME FOUND` or `: no game hosted`.
  Clear the box + **Query** to unpin.

Columns (like OP2's PICK SESSION): **Host IP**, **Game (creator)**, **Max Players**,
**Scenario Type**, **Ping**, **Session ID**.

### Test it for real
1. On any PC, start **Outpost 2 -> Multiplayer -> TCP/IP -> Create Game** and sit on the
   lobby (the host now answers discovery on UDP 47776).
2. Run **OP2LanScan.exe** on the same LAN (can be the same PC). The hosted game appears with
   the host's IP and the creator's name.
3. Nothing showing? Allow `OP2LanScan.exe` (or inbound UDP) through Windows Firewall - the
   host's reply is an inbound datagram.

## How it works
- Builds a 42-byte query (message type `0x1000`, the OP2 game GUID, a ping token, a
  sum-of-dwords checksum) and broadcasts it to `255.255.255.255:47776` plus each NIC's `/24`
  directed broadcast.
- OP2 hosts validate it and reply (81 bytes, type `0x1001`) from their `:47776`.
- We read the host IP (datagram source), the game-creator name, the session GUID, the max
  players / scenario type, and the ping (echoed-token round-trip).

Byte-exact spec: [`..\DISCOVERY_PROTOCOL.md`](../DISCOVERY_PROTOCOL.md). Design notes:
[`..\NOTES.md`](../NOTES.md).

## Files
```
OP2LanScan.slnx               <- solution
OP2LanScan\
  Op2Discovery.cs             <- the protocol (build query / parse reply / scan)
  fMain.cs / fMain.Designer.cs<- form logic + UI
  Program.cs                  <- entry point
  Plymouth.ico                <- app/window icon
  OP2LanScan.csproj
```

## Status
- Protocol reverse-engineered from `Outpost2.exe`, verified by a round-trip self-test
  (`..\selftest\`).
- **Confirmed live** (retail 1.3.6): a game hosted as *"Leviathan"* was discovered by both
  broadcast and direct-IP query, with the correct host IP, creator name, session GUID, ping.

## Roadmap
- Current/joined player count + password-locked flag (not in the UDP reply - needs a
  follow-up TCP probe to the host's game port; max players & scenario type are done).
- Double-click a row to copy the host IP / a "join" helper.
- Tray mode + toast when a new game appears.
