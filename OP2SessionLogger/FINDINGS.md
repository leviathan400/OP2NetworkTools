# OP2SessionLogger - in-game command-packet findings

What we have learned about the **in-game command packets** (the contents of the per-tick command
turns, engine cmd `0x0C`) using OP2SessionLogger plus the retail 1.3.6 decompile. This is the
command-data layer, distinct from the wire/transport protocol in `..\PROTOCOL.md` /
`..\FINDINGS.md`.

## Method
Two sources, cross-checked:
- **Decompile** (retail 1.3.6): the command **executor** `FUN_0040e300` is one big
  `switch(commandType)` - it defines every command type and what each handler reads. Its
  unit-selection helpers `FUN_00490920` (count) and `FUN_00490960` (id list) define the command
  header.
- **Live capture** (OP2SessionLogger): joins a game, categorises every command it receives, and
  logs a per-command decode + a command-type census to `op2sessionlogger_debug.txt`.

Two capture rules that make decoding tractable:
- **The sim is not on the wire.** OP2 is deterministic lockstep, so morale / population / deaths /
  disasters are computed locally on every machine and never sent. An **idle baseline** (host does
  nothing) shows only `ctNop` + the `0x34` sync. So in a 2-player game, **every other command is the
  host's deliberate action**.
- **Isolate one action.** Repeat a single action (e.g. self-destruct N units, or move one unit to
  several tiles); the command that appears N times is that action, and the bytes that change between
  repeats are its variable fields.

## Command block structure
Each command in the `0x0C` stream is:
```
+0  u8   type        the executor switch case
+1  u8   len         length of data[]
+2  u32  unk         a per-stream constant token (observed 0xBABE3624); MUST be non-zero or the
                     receiver discards the block. It is not a per-command id - mirror it verbatim.
+6  data[len]        command-specific (see below)
```

### The data header is a unit selection
`data` begins with a unit selection, decoded by the executor's helpers:
```
+0  s8   selectionCount   > 0  = N explicitly selected units; N u16 ids follow at +1
                          <= 0 = a reference to a saved control-group / select-all
+1  u16 x N   the selected unit ids   (when count > 0)
+...          command-specific fields follow the id list
```
Some commands instead take a **single** `u16` unit id at `+0` (the executor cases that read
`*(short*)data` directly, e.g. build/dock). This is why command length varied in early captures -
it was just different numbers of selected units (`sel=[18]` vs `sel=[19,18,17]`).

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

## Decoded commands (confirmed live)
- **`0x00` ctNop** - empty turn, `len 0`. Keepalive / lockstep filler.
- **`0x02` ctMoMove / `0x03` ctMoDock** - `[selection]` then `01 00`(u16, constant) `X`(u16) `Y`(u16) = the
  destination. The **high bit of X (`& 0x8000`)** appears to flag "target is a unit" vs a bare map
  tile (the executor does the same `& 0x80000000` test). Confirmed: the same unit moved to different
  tiles changes only X,Y.
  ```
  move sel=[19] 01 00 | 23 05 | 4f 00   -> to=(0x0523, 0x004f)
  move sel=[13] 01 00 | 56 85 | 5b 00   -> to=(0x0556, 0x005b) high bit set
  ```
  (Whether X/Y are tiles, pixels, or a scaled coordinate is not yet calibrated.)
- **`0x14` ctMoSelfDestruct** - `[selection]`, no other data. Confirmed: self-destructing N units emits
  exactly N x `0x14`, one per unit (`sel=[17] sel=[18] sel=[19] sel=[13]`).
- **`0x30` ctChat** - `[sender(1), recipientMask(1), text..., NUL]`. Display gated on
  `(1 << localPlayer) & recipientMask`. e.g. `00 ff 62 79 65 00` = sender 0, all, "bye".
- **`0x31` ctQuit** - 1 byte mode (`mode=2` observed on a normal leave). Triggers the host's net
  shutdown / "player has quit" path.
- **`0x32` ctAlly** - 1 byte target player. Toggles the alliance bitmask.
- **`0x03` ctMoDock** - same layout as ctMoMove (`[selection]` `01 00` `X` `Y`). Observed live with
  the high bit of X set, consistent with docking AT a structure ("target is a unit") - nice
  confirmation of the unit-target flag theory.

## Decoded live (field guess from 2 samples; one controlled capture to pin)
- **`0x06` ctMoBuild** (len 19) - `[selection]`(the dozer) then:
  ```
  01 00          u16  constant (same token the move group carries)
  X, Y           u16  target coordinate pair (same magnitude/scale as move destinations)
  x1,y1,x2,y2    u16  tile rect = the structure FOOTPRINT (both live samples were 4x4)
  ff ff          terminator
  ```
  Live: `01 00 f0 07 08 07 3c 00 6d 00 3f 00 70 00 ff ff` -> rect (60,109)-(63,112);
        `01 00 b0 06 38 07 32 00 70 00 35 00 73 00 ff ff` -> rect (50,112)-(53,115).
- **`0x16` ctMoResearch** (len 6) - `[u16 lab unit] [u16 techIndex] [u16 scientists]`.
  Live: `1c 00 1a 00 0b 00` (lab 28, tech 0x1a, 11 scientists) and
        `1c 00 1d 00 0a 00` (lab 28, tech 0x1d, 10 scientists) - tech + count both varied.
- **`0x0a` ctMoTransferCargo** (len 6) - `[u16 unit] [u16 bayIndex?] [u16 0]`.
  Live: `1a 00 02 00 00 00` and `1a 00 01 00 00 00` (same unit 26, bay 2 then bay 1).
- **`0x34` ctMachineSettings** - two dwords, sent ~5/sec. The deterministic-sync / desync-check
  heartbeat. Not a player action; a passive client can omit it (send `ctNop`) and the game tolerates
  it.

## Background vs player input
- **Background** (fires even when idle): `0x00 ctNop`, `0x34` sync.
- **Player input**: everything else. The engine sim never puts its events on the wire.

## Still to decode (names now canonical; field layouts still open)
- `0x09` **ctMoProduce** - structure-kit type id + a second ref (single-unit header).
- `0x18` ctMoTransfer, `0x2b` ctGameOpt, and the rest of the selection+coordinate group
  (`0x27` cargo-route, `0x28` patrol).
- Confirm the "decoded live" trio above: ctMoBuild (vary the structure type -> footprint),
  ctMoResearch (same tech, different scientist counts), ctMoTransferCargo (which bay is which).

The decoder (`cmd_name` / `decode_cmd` in `src/net/op2client.cpp`) encodes the above; extend it as
each command is pinned.

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
| `0x34` | special | **ctMachineSettings**: `player[0x130] = tail[0]`, `player[0x131] = +0x12` (sync values) |
| default | - | return 0 (unknown type rejected) |
