# OP2SessionLogger - in-game command-packet findings

What we have learned about the **in-game command packets** (the contents of the per-tick command
turns, engine cmd `0x0C`) using OP2SessionLogger plus the retail 1.3.6 decompile. This is the
command-data layer, distinct from the wire/transport protocol in `..\PROTOCOL.md` /
`..\FINDINGS.md`.

## Method
Three sources, cross-checked:
- **Spec doc** (authoritative): the engine's `DataStructure CommandPacket.txt` gives the wire header
  (CommandPacket / NetworkCommandPacket) and the per-type data layouts. Opcode->name comes from the
  CommandType string table (`OP2KB-FULL\data\command_packet_opcodes.tsv`, string VAs
  `0x004e0f10`-`0x004e11fc`). This is now the primary reference; the live captures confirm it.
- **Decompile** (retail 1.3.6): the command **executor** `FUN_0040e300` is one big
  `switch(commandType)` - it defines every command type and what each handler reads. Its
  unit-selection helpers `FUN_00490920` (count) and `FUN_00490960` (id list) define the command
  header.
- **Live capture** (OP2SessionLogger): joins a game, categorises every command it receives, and
  logs a per-command decode + a command-type census to `op2sessionlogger_debug.txt`.

Two capture rules that make decoding tractable:
- **The sim is not on the wire.** OP2 is deterministic lockstep, so morale / population / deaths /
  disasters are computed locally on every machine and never sent. An **idle baseline** (host does
  nothing) shows only `ctNop` (idle keepalive) + `0x34 ctMachineSettings` (the periodic CPU/RAM/
  resolution broadcast). So in a 2-player game, **every other command is the host's deliberate action**.
- **Isolate one action.** Repeat a single action (e.g. self-destruct N units, or move one unit to
  several tiles); the command that appears N times is that action, and the bytes that change between
  repeats are its variable fields.

## Command block structure
Each command in the `0x0C` stream is (this is the engine's **NetworkCommandPacket** per-command
form, confirmed against the official `DataStructure CommandPacket.txt`):
```
+0  u8   type        the executor switch case (messageType)
+1  u8   len         dataLength of data[]
+2  u32  unk         the doc's "Unknown" field. A per-stream constant token (observed 0xBABE3624);
                     MUST be non-zero or the receiver discards the block. Mirror it verbatim.
+6  data[len]        command-specific (see below)
```
The engine's **internal** CommandPacket header is wider (`u32 messageType, u16 dataLength,
u32 timeStamp, u32 Unknown`); the wire form packs type+len into one byte each and shares the same
`Unknown`. We only ever see/emit the wire form.

### The data header is a unit selection
`data` begins with a unit selection, decoded by the executor's helpers:
```
+0  s8   selectionCount   > 0  = N explicitly selected units (doc: numUnits); N u16 ids follow at +1
                          <= 0 = a reference to a saved control-group / select-all
+1  u16 x N   the selected unit ids   (when count > 0)
+...          command-specific fields follow the id list
```
Some commands instead take a **single** `u16` unit id at `+0` (the executor cases that read
`*(short*)data` directly, e.g. produce/transfer-cargo/research). This is why command length varied
in early captures - it was just different numbers of selected units (`sel=[18]` vs `sel=[19,18,17]`).

### Packed map coordinates
Waypoint/destination coordinates are a **single u32**, not two u16s: `pixelX` in the low 15 bits,
`pixelY` in the next 14 (doc: "pixelX:15 [low], pixelY:14 [high]"). At 32 px/tile, `tile = pixel/32`.
An earlier reading split it into two u16s, which **halved every logged Y** and mistook `pixelY`'s
low bit for a bogus "unit-target" flag on X - both now fixed. Tile **rects** (build footprints,
bulldoze/tube regions) are instead four plain u16 tile coords `x1,y1,x2,y2`.

## Command-type map (canonical names)
The opcode -> name map is **canonical**, from the engine's own CommandType string table in the
decompile (`OP2KB-FULL\data\command_packet_opcodes.tsv`, string VAs `0x004e0f10`-`0x004e11fc`).
Header shapes are from the executor switch (`FUN_0040e300`, appendix below); "(no executor
case)" = the name exists in the table but the executor has no case for it (likely internal /
never on the wire). This table corrected several earlier live guesses: `0x09` is
**ctMoProduce** (produce a structure kit - hence the ore deduction), not "build" (`0x06`
ctMoBuild is the build-site command); and the `0x02`-`0x04` "move group" is really
move / dock / dock-EG (all selection + destination).

| type | name | header |
|---|---|---|
| `0x00` | ctNop (per-mark keepalive) | - |
| `0x01` | ctMoDoze | selection |
| `0x02` | ctMoMove | selection + dest X,Y |
| `0x03` | ctMoDock | selection + dest X,Y |
| `0x04` | ctMoDockEG | selection + dest X,Y |
| `0x05` | ctMoStop | selection |
| `0x06` | ctMoBuild | selection |
| `0x07` | ctMoBuildWall | selection |
| `0x08` | ctMoRemoveWall | selection |
| `0x09` | ctMoProduce | single unit |
| `0x0a` | ctMoTransferCargo | single unit |
| `0x0b` | ctMoLoadCargo | single unit |
| `0x0c` | ctMoUnloadCargo | single unit |
| `0x0d` | ctMoRecycle | single unit |
| `0x0e` | ctMoDumpCargo | single unit |
| `0x0f` | ctMoScavenge | (no executor case) |
| `0x10` | ctMoSpecialWait | (no executor case) |
| `0x11` | ctMoSurvey | (no executor case) |
| `0x12` | ctMoIdle | single unit |
| `0x13` | ctMoUnIdle | single unit |
| `0x14` | ctMoSelfDestruct | selection |
| `0x15` | ctMoScatter | selection |
| `0x16` | ctMoResearch | single unit |
| `0x17` | ctMoTrainScientists | single unit |
| `0x18` | ctMoTransfer | selection |
| `0x19` | ctMoLaunch | single unit |
| `0x1a` | ctMoFlyInSpace | (no executor case) |
| `0x1b` | ctMoRepair | (no executor case) |
| `0x1c` | ctMoRepairObj | selection |
| `0x1d` | ctMoReprogram | selection |
| `0x1e` | ctMoDismantle | selection |
| `0x1f` | ctMoSalvage | single unit |
| `0x20` | ctMoCreate (writes map tiles - wall/tube place) | special |
| `0x21` | ctMoDevelop | (no executor case) |
| `0x22` | ctMoUnDevelop | (no executor case) |
| `0x23` | ctMoLightToggle | selection |
| `0x24` | ctMoAttackObj | selection |
| `0x25` | ctMoGuard | selection |
| `0x26` | ctMoStandGround | selection + dest X,Y |
| `0x27` | ctMoCargoRoute (two coord pairs) | selection |
| `0x28` | ctMoPatrol (one coord pair) | selection |
| `0x29` | ctMapChange (handled outside this executor) | - |
| `0x2a` | ctMoPoof (sets the unit destruct flag) | single unit |
| `0x2b` | ctGameOpt | special |
| `0x2c` | ctGodWeapon (no-op in this executor) | - |
| `0x2d` | ctChatText (no-op in this executor) | - |
| `0x2e` | ctChatSFX (no-op in this executor) | - |
| `0x2f` | ctMoDeath | (no executor case) |
| `0x30` | **ctChat** | special |
| `0x31` | **ctQuit** | special |
| `0x32` | **ctAlly** | special |
| `0x33` | ctGoAI | special |
| `0x34` | **ctMachineSettings** (periodic sync, ~5/sec) | special |
| `0x35` | ctInvalidCommand | - |
| `0x36` | WeaponFiring | - |

## Decoded commands (spec layout, cross-checked live)
All layouts below now match the official `DataStructure CommandPacket.txt`; the ones marked
"live" were also observed on the wire and agree byte-for-byte.
- **`0x00` ctNop** - empty turn, `len 0`. Keepalive / lockstep filler.
- **`0x02` ctMoMove / `0x03` ctMoDock / `0x04` ctMoDockEG / `0x26` ctMoStandGround** -
  `[selection]` then `[u16 numWayPoints]` and that many **packed coords** (`pixelX:15 | pixelY:14`,
  see "Packed map coordinates"). Live moves carry `numWayPoints=1`. Correcting the old note: the
  `01 00` we called a "constant" is `numWayPoints`, and there is no "unit-target" flag - that was
  `pixelY`'s low bit leaking out of the broken two-u16 split.
  ```
  ctMoMove sel=[29]  01 00 | 47 87 32 07   dest u32=0x07328747 -> pixel (1863, 3685)  tile ~(58,115)
  ```
- **`0x06` ctMoBuild** (len 19) - `[selection]`(the dozer), `[u16 numWayPoints]`, the packed
  waypoint coord(s), then the build area as four u16 tiles `x1,y1,x2,y2`, then `FF FF` (stored to
  `unit+0x6A`). Confirmed live across structure types: footprints (43,10)-(46,13) (4x4) vs
  (37,13)-(42,17) (6x5) - the rect tracks the structure size, and the waypoint pixel lands on the
  footprint's bottom-right corner (this is what the old "Y anchor TBD" note was chasing - it was the
  halved-Y bug, now fixed).
- **`0x07` ctMoBuildWall** (len 15) - `[selection]`(Earthworker), region rect as four u16 tiles, then
  `[u16 map_id kind]`, then `00 00`. The rect is **TopLeft-inclusive, BottomRight-exclusive** (spec:
  "including tube" / "excluding tube"), so tile span = `(x2-x1) x (y2-y1)`. Ground-truth confirmed:
  a player who laid 4 wall tiles + 2 tube tiles produced exactly three commands - two `kind=0x12`
  rects of 1x2 and one `kind=0x11` rect of 1x2. map_id kinds pinned so far: **`0x11` = tube,
  `0x12` = wall**; microbe-wall and lava-wall ids still to capture.
- **`0x0a` ctMoTransferCargo** (len 6) - `[u16 buildingUnit] [u16 bay] [u16 0000]`. Live: same
  building with bay 1/2/3 across sessions; trailing u16 is a confirmed constant `0000`.
- **`0x14` ctMoSelfDestruct** - `[selection]`, no other data. Live: self-destructing N units emits
  exactly N x `0x14`, one per unit.
- **`0x16` ctMoResearch** (len 6) - `[u16 lab] [u16 techNum] [u16 numScientists]`. Live:
  `1c 00 1a 00 0b 00` (lab 28, tech 0x1a, 11 sci) and `1c 00 1d 00 0a 00` (lab 28, tech 0x1d, 10 sci).
- **`0x17` ctMoTrainScientists** (len 4) - `[u16 university] [u16 numTrained]`. Live: training 10 ->
  `3b 00 0a 00` (university 59, count 10).
- **`0x30` ctChat** - `[srcPlayer(1), destMask(1), text..., NUL]`. Display gated on
  `(1 << localPlayer) & destMask`. e.g. `00 ff 62 79 65 00` = sender 0, all, "bye".
- **`0x31` ctQuit** (len 2) - `[u8 quitMethod, u8 lParam]`. quitMethod `0`=self-destruct,
  `1`=transfer-to-allies, other=go-AI. Live: `02 02` on the host's normal leave (so it actually
  hands off to AI, not a plain disconnect).
- **`0x32` ctAlly** (len 4) - `[u16 allyFromPlayer, u16 allyToPlayer]` (each written as 2 bytes; the
  engine reads 1). Earlier "1 byte target player" was wrong - the target is the **second** field;
  the first is the sender. (Our ally-back already swaps both fields, which the spec confirms.)
- **`0x34` ctMachineSettings** (len 8) - `[u16 cpuSpeed(MHz), u16 physMem(MB), u16 winW, u16 winH]`,
  sent ~5/sec as the desync-check heartbeat. Live `e6 0a 00 08 60 04 00 03` = 2790 MHz, 2048 MB,
  1120x768. Not a player action; a passive client can send `ctNop` instead and the game tolerates it.

## From the spec, not yet seen live (decoder pre-loaded)
The decoder now also renders these straight from the spec, so the next capture self-documents:
- **`0x01` ctMoDoze** - `[selection]` + bulldoze rect (4 u16 tiles).
- **`0x08` ctMoRemoveWall** - `[selection]` + waypoints + tube rect (4 u16 tiles).
- **`0x09` ctMoProduce** (len 8) - `[u16 factory, u16 itemToProduce(map_id), u16 weaponType(map_id),
  s16 scStubIndex(-1=none)]`.
- **`0x19` ctMoLaunch** (len 6) - `[u16 unit, u16 destPixelX, u16 destPixelY]` (note: **plain** u16
  pixels here, not the packed form the move group uses).
- **`0x1c/0x1d/0x1e` ctMoRepairObj/Reprogram/Dismantle** and **`0x24/0x25` ctMoAttackObj/Guard** -
  `[selection] 0000 [u16 target] [u16 FFFF|tileY]`; `FFFF` => target is a unit, else target is a
  ground tile `(tileX, tileY)`.
- **`0x20` ctMoCreate** - `[u16 numUnits]` + `CreateUnitInfo[12]{u32 type, u16 tileX, u16 tileY,
  u32 weaponOrCargo}` (the scenario/cheat unit spawner).
- **`0x23` ctMoLightToggle** - `[selection] [u16 headLightState]` (0 off / 1 on).
- **`0x2b` ctGameOpt** - `[u16 ?, u16 variableId, u16 newValue, (u16 player)]`; variableId 8-32 set
  resources / morale / disasters / game speed, and id `0x22 TickOfLastSetGameOpt != 0` is the engine's
  "CHEATED GAME!" flag.

## Host game version (GameStartInfo blob)
The host's **TApp::Version** is in the GameStartInfo blob (engine cmd 1) at **blob `+0x04`**
(wire offset `0x17`), a u32 right after the flags dword at `+0x00`. Confirmed live: a retail 1.3.6
host broadcasts `04 00 03 01` = `0x01030004` = "1.3.0.4". It is NOT in the discovery reply or the
join-accept (both dumped and checked), and the host does not send a `cmd-3` player-announce back, so
the GSI is the only lobby source. `parse_gsi` exposes it as `LobbyInfo.tappVersion`; the client logs
it and posts it to chat ("Host game version: 1.3.0.4") and the lobby panel shows "Host Ver". Use this
to compare a 1.4.1 host (which drops our 1.3.0.4-advertising client) against a 1.3.6 host.

## Background vs player input
- **Background** (fires even when idle): `0x00 ctNop` (idle keepalive), `0x34 ctMachineSettings` (the periodic CPU/RAM/resolution broadcast that feeds OP2's Network Statistics panel).
- **Player input**: everything else. The engine sim never puts its events on the wire.

## Still to decode / confirm
- **Calibrate the map_id enums against live values**: ctMoBuildWall `kind` (tube=0x11 seen; capture a
  wall / microbe-wall / lava-wall), ctMoProduce `item`/`weapon`, ctMoCreate `type`.
- **ctMoCargoRoute (0x27) / ctMoPatrol (0x28) extra fields**: after the waypoint list the spec lists
  smelter/mine waypoint indices + unit ids (cargo route) and two unknown shorts (patrol). The decoder
  shows the waypoints and dumps these as `tail:` - capture one to label them.
- **ctGameOpt (0x2b) field 0** (the leading u16 the spec marks "unknown") and which variableIds appear
  in normal play vs cheats.
- `0x18` ctMoTransfer - selection header confirmed, trailing fields not in the spec doc; capture live.

Field layouts come from the engine's `DataStructure CommandPacket.txt` (authoritative) plus live
capture. The decoder (`cmd_name` / `decode_cmd` in `src/net/op2client.cpp`) now renders every command
above; extend it as the remaining enums/fields are pinned.

## Appendix: engine internals (from the executor `FUN_0040e300`)
The executor is `FUN_0040e300(param_1, param_2, player, command)`:
- **`player`** (param_3) is the issuing player object: `player[0]` = player number; `player[10]` = the
  dedup-enable flag; `player[0x2f6+]` = the command-dedup ring (below); `player[0x29]`/`[0x2a]` =
  resource amounts used by build/production; `player[0x130]`/`[0x131]` = where `0x34` stores its two
  values; `player[0x307]` = a unit-list head (used by `0x32`).
- **`command`** (param_4) is the **in-memory** command struct, not the raw wire block: the type is at
  `+0`, and the data (starting with the selection count) is at **`+0x0e`**. So the wire block's
  `data` (at wire `+6`) is copied to mem `+0x0e`; the wire `len`/`unk` live in the 0x0e-byte mem
  header. Cases also read `command+0x0f` (selection-count math), `command+0x12` (a 2-byte field:
  chat recipient mask, build target, settings value B), and `command[4]`/`[5]`/`[6]` (per-command
  shorts).
- **Return value**: `1` if handled, `0` for an unknown type (the `default:` case) - unknown command
  types are silently rejected.

### Command-dedup ring (why sliding-window resends are safe)
Almost every gameplay case begins with: if `player[10] != 0`, scan a small ring `player[0x2f6 + i]`
(indexed modulo 0x10 by the current mark, derived from tick `DAT_0056eb1c >> lgCPPI `DAT_0056eb0c`,
back `commandTurnDelay` `DAT_0056eb18`) for the command's target token (`unit[4]`). If found, **skip**
(already executed); otherwise record it and execute. This is how the engine tolerates the same
command block arriving in several overlapping command turns - it executes each command once per mark.

### Unit struct
A unit is `unitId * 0x78 + DAT_0054f848` (stride **0x78 = 120 bytes**), `DAT_0054f848` = the unit
array base. Offsets the cases touch: `+0` vtable, `+0x04` unit type (`-1` = dead), `+0x10` the token
used for dedup, `+0x11` and `+0x44` flag dwords (e.g. `0x20000` = disabled/destructing, `0x42`
checked by self-destruct), `+0x1d` low nibble = **owner player**, `+0x21` = **current command
state** (set to the command type), `+0x6a`/`+0x6e`/`+0x72` per-command fields. `vtable+0x74` =
"is-selectable/valid for this player" (gate used everywhere).

### Selection encoding (helpers `FUN_00490920` count / `FUN_00490960` id list)
The first data byte is a **signed** selection code:
- **> 0** - an explicit count; that many `u16` ids follow immediately.
- **-1 .. -10** - a **saved control-group** reference: count/ids come from a per-context table
  (stride `0x41`, count at `+0x1f0`, ids at `+0x1f1`), i.e. a previously stored selection.
- **<= -11** - a `-0xc - code` count form (a bulk/"select-matching" variant; ids copied via
  `FUN_00481c60`).

## Appendix: full executor case reference
Every `switch` case in `FUN_0040e300`, with its header shape and what the handler does. "selection"
= count+id-list header; "unit" = single `u16` id; "special" = custom payload. Confirmed names are
exact; "?" = behaviour seen, name not pinned.

| case | header | handler behaviour |
|---|---|---|
| `0x00` `0x29` `0x2c` `0x2d` `0x2e` | - | fall through / no-op in this executor (`0x00` ctNop; `0x29` option/UI toggle handled elsewhere) |
| `0x01` | selection | sets up + calls `FUN_004238a0` per unit (unit command?) |
| `0x02` `0x03` `0x04` `0x26` | selection | `FUN_00423210`; sets unit `+0x21` = type = **move/route** |
| `0x27` | selection | as above but reads **two** coordinate pairs (`+0x62/+0x66` ... waypoint/drag) |
| `0x28` | selection | as above but reads **one** coordinate pair (`+0x5e`) |
| `0x05` | selection | sets command 5; if unit flag `0x10`, `FUN_0043aa10` (stop/halt?) |
| `0x06` | selection | reads a short from the tail; `vtable+0x84`; writes unit `+0x6a` |
| `0x07` | selection | `FUN_004235e0(list, tailShort)` |
| `0x08` | selection | `vtable+0x94(...)` with the tail (route/guard with target?) |
| `0x09` | unit | **build**: reads structure type (`command[4]`), a second ref (`+0x12`); checks/deducts ore (`DAT_0056ef40/ef3c`, `-10000` per kit); sets command 9 |
| `0x0a` | unit | `vtable`, `FUN_00482fd0` (dock/board?) |
| `0x0b` | unit | `FUN_00424090` |
| `0x0c` | unit | `FUN_00424240` |
| `0x0d` | unit | `vtable+0x10c(command[4], ...)` |
| `0x0e` | unit | sets command `0x0e` |
| `0x12` | unit | `FUN_0043abf0`; sets command `0x12`, `vtable+0x84`; sets `DAT_0056efbc[player]=1` (build-state) |
| `0x13` | unit | production/resource amounts (`player[0x29]/[0x2a]`); messages `0x183-0x188` ("not enough..."); set-production/dump |
| `0x14` | selection | if unit flag `0x42`, `FUN_0043a7a0` = **self-destruct** (confirmed) |
| `0x15` | selection | `FUN_004242f0(list, count)` |
| `0x16` | unit | `FUN_00472d10`; sets command `0x16`, reads `+0x12` |
| `0x17` | unit | sets command `0x17`; value from vtable, halved if `FUN_004028e0` |
| `0x18` | selection | `vtable+0x68`; "X transferred to Y" messages = cargo/control transfer |
| `0x19` | unit | checks `+0x6e/+0x72`; production counts (`DAT_0056efb0..b8`); `FUN_00424260(command[4], +0x12)` |
| `0x1c` `0x1d` `0x1e` | selection | `FUN_00423d90(type, count, list, ...)` (group command) |
| `0x1f` | unit | `FUN_004238d0(command+4, command[6])`; sets command `0x1f` |
| `0x20` | special | iterate the tail as `[short x, short y, short]` triples; `FUN_004467c0` writes map tiles (wall/tube place) |
| `0x23` | selection | toggles unit `+0x44` bit 0 with the tail value (per-unit on/off) |
| `0x24` `0x25` | selection | `FUN_00410300` + `FUN_00423970`; tile checks (group select/route with a mode bit) |
| `0x2a` | unit | sets unit `+0x44 |= 0x20000`, command `0x2a`, `+0x22 = 0x0b` (marks the destruct flag) |
| `0x2b` | special | `FUN_00422b00(command)` |
| `0x30` | special | **chat**: text at `+0x04`, recipient mask at `+0x0f`; displays `"%s: %s"` |
| `0x31` | special | **quit**: mode byte at `+0x0e` (0/1); net shutdown; "At tick %i player %i..." |
| `0x32` | special | **ally**: target player byte at `+0x04`; toggles alliance bitmask `DAT_0056ef20` |
| `0x33` | special | announce (message `0x1fe`) |
| `0x34` | special | **ctMachineSettings**: `player[0x130] = tail[0]`, `player[0x131] = +0x12` (machine-spec values: cpuSpeed/memoryMB/window WxH) |
| default | - | return 0 (unknown type rejected) |
