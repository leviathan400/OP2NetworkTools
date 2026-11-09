# OP2 LAN - dev notes

## What this is
A C# WinForms desktop app (.NET Framework 4.8) that finds Outpost 2 LAN games by
speaking OP2's UDP discovery protocol directly - no game required. It grew out of the
C/console proof of concept + Python reference now kept alongside it in `op2finder\` and
`op2finder-py\`; this is the proper GUI version.

## Architecture
- **`Op2Discovery.cs`** - all the protocol. Pure managed `System.Net.Sockets`.
  - `BuildQuery(token)` / `ParseReply(...)` / `Checksum(...)` - byte-exact to the
    RE'd spec (see `DISCOVERY_PROTOCOL.md`).
  - `Scan(durationMs, unicastIp, ct, rawLog)` - **synchronous/blocking**; bind an
    ephemeral UDP socket with `SO_BROADCAST`, send the query ~6× over ~1.2 s, poll
    for replies, dedupe by host IP, return `List<Op2Session>`. Pass `unicastIp` to
    probe a single host instead of broadcasting.
  - `BroadcastTargets()` - `255.255.255.255` + each local NIC's `/24` directed
    broadcast (helps on segmented LANs / multi-NIC boxes).
- **`fMain.cs`** - UI logic.
  - Grid bound to a `BindingList<Op2Session>`; a `Dictionary<string,Op2Session>`
    keyed by IP backs the merge.
  - `ScanAsync` runs `Op2Discovery.Scan` on a background thread via `Task.Run`
    (keeps the UI responsive), then merges on the UI thread.
  - `Merge` updates/adds rows and **ages out** sessions not seen in 12 s.
  - A `System.Windows.Forms.Timer` (3 s) drives auto-refresh; a re-entrancy guard
    (`_scanning`) prevents overlapping scans.

## Decisions / gotchas
- **`Timer` ambiguity** - `System.Threading.Timer` vs `System.Windows.Forms.Timer`
  collide when both namespaces are imported. Fully qualified the WinForms one.
- **Old-style csproj** - .NET Framework projects don't auto-include `.cs` files;
  `Op2Discovery.cs` had to be added as an explicit `<Compile Include=...>`.
- **Blocking scan on a worker thread** - chose a simple synchronous socket loop
  (`Poll` + `ReceiveFrom`) over async socket APIs; easier to reason about and the
  scan is short-lived. The UI never blocks because it's wrapped in `Task.Run`.
- **Ping with wrap** - ping = `(uint)Environment.TickCount - echoedToken`. Fine for
  real queries (token is set at send time); `-1` sentinel only when the echo
  doesn't match the token we sent.
- **Host IP comes from the datagram source**, never from packet bytes.
- **DataGridView binds to PROPERTIES, not fields.** `Op2Session` first exposed
  `Creator`/`SessionId` as public *fields*; the grid left those columns blank while
  `HostText`/`PingText` (real properties) worked. Reflection in the test harness reads
  fields, so it masked the bug - only a screenshot of the running GUI caught it. All of
  `Op2Session` is now auto-properties. (Test/Probe reflection switched `GetField` →
  `GetProperty`.)

## Direct-IP ("Looking at the IP address")
Mirrors OP2's own *Find Session → Looking at the IP address* option. `Op2Discovery.Scan`
takes `(durationMs, broadcast, unicastIp, …)`: broadcast sweeps the LAN, `unicastIp`
queries one host directly (cross-subnet / VPN / internet, where broadcast can't reach);
both can run in one pass. In the GUI, typing an IP/hostname in **Find at IP/host** and
clicking **Query** *pins* it - every refresh then also queries that host, so a game on
another subnet stays visible (not a one-shot), and the status bar shows
`<ip>: GAME FOUND` or `: no game hosted`. Clearing the box + Query unpins it. The C
console tool does the same with `op2finder <ip>`. Verified live: `10.0.61.18` (hosting
"Leviathan") is found by direct IP; `10.0.61.250` (no host) returns nothing.

## Verification
- `selftest\Test.cs` (compiled with Roslyn `csc`) reflects into the built assembly
  and round-trips the protocol: builds a query and checks a host would accept it
  (checksum/type/GUID@0x06/port@0x16/token@0x1A); builds a synthetic reply and checks
  the parser extracts creator/host/ping/session; checks bad checksum / wrong type /
  wrong length are rejected. **All pass.**
- App builds (Debug + Release) and launches without startup exceptions.
- **Confirmed against a LIVE host (2026-06-04).** `D:\OP2-136\Outpost2.exe` hosting a
  game *"Leviathan"* on `10.0.61.18`. `selftest\Probe.exe` (a thin console harness over
  `Op2Discovery.Scan`) and the GUI both discover it via broadcast and direct IP.

## The bug that mattered (first live test returned nothing)
The first build found 0 games even with OP2 hosting on the same box. The reverse-
engineered query layout had **three errors**, all caught by reading the host responder
(`FUN_0048C2C0`) and builder (`FUN_004917E0`) in the 1.3.6 decompile:
1. **GUID offset** - it sits at query `+0x06`, not `+0x0A` (I'd shifted it by 4).
2. **Reply-port field at `+0x16`** - the host replies to the port named *here*, not to
   the UDP source port. We now write our own bound socket port into it. Without this
   the reply is misrouted (and same-machine discovery is impossible).
3. **GUID bytes** - Data1 is little-endian: `11 CF 55 5A ...`, not `5A 55 CF 11 ...`.
   The wrong value failed the host's GUID match outright.
The original "self-test" passed because it checked our builder against our own parser
- both wrong the same way. Lesson: validate against the *real* host (or its decompiled
validator), not just internal round-trips.

## Max Players + Scenario Type (matches OP2's PICK SESSION)
The 81-byte reply carries these. **Two wrong guesses before the right answer - log them
so we don't repeat:**
1. First thought `+0x3A`=maxPlayers (right, =2) and `+0x3E`=scenarioType (WRONG -
   `+0x3E` is 0 for every mode).
2. Hosting Land Rush revealed the type lives in the **StartupFlags bitfield at
   `reply+0x2A`**, not a clean dword. First tried `missionType = (cfg>>11)&7` (gave
   LOS=6, LandRush=7) - but hosting **Midas** broke it: Midas is `0x3280`, and
   `(0x3280>>11)&7 = 6` collides with LOS (`0x3080`). The mode actually sits at **bit 9**.
3. Correct: `maxPlayers = +0x3A` (== `(cfg>>6)&7`); `missionType = (cfg>>9)&0x1F`.
   Codes are **consecutive** (`0x2B` byte = `30,32,34,36,38`). **All five verified live
   by hosting each:** 24 = Last One Standing (`0x3080`), 25 = Midas (`0x3280`),
   26 = Resource Race (`0x3480`), 27 = Space Race (`0x3680`), 28 = Land Rush (`0x3880`).

Gotchas that cost time:
- Captures are **async snapshots of a live lobby**; the `0x200` bit I first dismissed as
  a "stray option" was actually the Midas mode bit (bit 9). Don't diff snapshots across a
  lobby change - correlate each capture with what the host screen shows at that instant.
- The host instance changed mid-session (PID 9724 "Outpost2" → PID 18664 "OP2 Dev Panel
  (retail)"); whichever instance owns UDP 47776 is the one answering discovery.

**All five modes captured and verified live** - nothing left to confirm. Unknown codes
still fall back to `Type N`. The five mode strings are at `Outpost2.exe` `.data`
`0x4E5518`-`0x4E5550`, but the code→name map is NOT a direct index into that table - pin
codes by capture, not assumption. (Password / current-player count are still TCP-join only.)

Behaviour: **Scan LAN** and **Query** now clear the list first (a deliberate refresh
starts clean); auto-refresh still updates in place + ages out after 12 s.

## Roadmap / ideas
1. **Current/joined player count + password flag** - not in discovery. Add a follow-up
   TCP probe to the host's game port (47800-47807). Needs RE of the TCP join handshake
   (start from `NETWORK_PROTOCOL.md`).
2. **Confirm scenarioType codes 1-4** by hosting Space Race / Resource Race / Land Rush
   / Midas and reading the value op2finder reports.
3. **Join helper** - double-click a row → copy host IP, or write it where OP2's
   "join by IP" expects it; possibly launch OP2.
4. **Quality-of-life** - sortable columns, "new game" toast, tray mode, remember
   window size, JSON/CLI export.
5. **More tools under the same app** - the name `OP2LanScan` is deliberately
   generic; could host a "scan a remote IP", a packet inspector, or a presence/chat
   tab later.

## Build/run quick reference
All paths relative to the repo root (`op2network\`):
- C# app - MSBuild: `...\MSBuild.exe OP2LanScan\OP2LanScan\OP2LanScan.csproj /p:Configuration=Release`
  → `OP2LanScan\OP2LanScan\bin\Release\OP2LanScan.exe`
- C console tool - run `op2finder\build.bat` (cd's to its own dir) → `op2finder\op2finder.exe`
- Self-test: compile `selftest\Test.cs` with `csc.exe`, run `Test.exe` (it loads the
  app's Debug build via the hard-coded path near the top - adjust if you relocate again).

## Related work (same OP2 RE effort)
- `op2finder\` + `op2finder-py\` - C console + Python versions of the same finder (in this repo).
- `D:\Oupost2Decompile\Review\findings\NETWORK_PROTOCOL.md` - full OP2 net stack RE (external).
