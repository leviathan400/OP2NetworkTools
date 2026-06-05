# Outpost 2: Divided Destiny - game-start file/version checksum

The check that runs when a multiplayer game starts to ensure every player has identical
OP2 files. Fully RE'd from the game decompile. **Verdict up front: this is NOT a barrier
for our external client** - it's a *client-side self-check*, and it's a deterministic hash
of files we have on disk, so we can either skip it (just don't self-abort) or reproduce
the exact value. (This is separate from the in-game runtime sync checksum `ChecksumGame`
@0x40C0B0.)

## How it works
At start, each machine computes a **14-dword array** (`FUN_0044FFE0`), hashing these
resources in this exact order with the kernel hash `FUN_0040C050`:

| idx | resource | how |
|----:|--|--|
| 0 | BUILDING.TXT | sheet (loose .txt on disk, else its sheets.vol stream) |
| 1 | MINES.TXT | sheet |
| 2 | MORALE.TXT | sheet |
| 3 | SPACE.TXT | sheet |
| 4 | VEHICLES.TXT | sheet |
| 5 | WEAPONS.TXT | sheet |
| 6 | edentek.txt | sheet |
| 7 | ply_tek.txt | sheet |
| 8 | multitek.txt | sheet |
| 9 | **OP2Shell.dll** | whole file (`SearchPathA`) |
| 10 | **Outpost2.exe** | whole file (`GetModuleFileNameA(NULL)`) |
| 11 | level/mission DLL | composite "levelDllHash" (`FUN_00403650` over DescBlock+MapName+LevelDesc+TechtreeName) - **depends on the chosen MP map DLL** |
| 12 | level techtree | the level's tech resource stream |
| 13 | **MASTER** | `FUN_0040C050(0, 0x34, &array[0])` over the first 13 dwords |

Only **dword [13], the master**, is exchanged/compared. Indices 0-10 are fixed for a
given 1.3.6 install; 11-12 depend on which MP map the host picked (e.g. `ml2_08.dll`).

## Kernel hash `FUN_0040C050` (rotate-right-1 XOR fold over LE dwords)
NOT CRC32, NOT the 0xFDE24ACB family.
```c
uint32_t op2_filehash(const uint8_t* p, uint32_t len) {
    uint32_t h = 0;
    if ((len & 0xFFFF) != 0) {                  // low-16-bits test (engine quirk)
        uint32_t dwords = len >> 2, tail = len & 3, v = 0;
        if (tail) {                              // 1-3 leftover bytes folded FIRST
            for (uint32_t i = 0; i < tail; i++) v = (v << 8) | *p++;
            h = (v & 0x00FFFFFF) << 7;
            if ((uint16_t)dwords == 0) return h ? h : 1;
        }
        while (dwords--) {                        // each LE dword
            uint32_t x = h ^ (*(const uint32_t*)p);
            h = (x >> 1) | ((x & 1) << 31);        // rotate-right 1
            p += 4;
        }
    }
    return h ? h : 1;                             // never 0
}
```
Called `FUN_0040C050(ignored, byteLen, dataPtr)`; `dataPtr` = MapViewOfFile base, `byteLen`
= GetFileSize. Each sub-checksum returns 0 on file-open failure → master build fails →
"Could not calculate local game checksum." (msg 0x262).

## Exchange & compare
- The master rides inside the **GameStartInfo blob**: host stores it at `netplayObj+0x17E`;
  the blob is copied from `netplayObj+0xC8`, so the master sits at **blob offset 0xB6** =
  wire offset **0xC9** of a start packet. (In the *lobby poll* GameStartInfo, GUR cmd 1, this
  field is 0; it's filled in the **start** packets, GUR cmd 7/8.)
- Start sub-commands (GUR engine code at payload+4): **7** prepare, **8** start-info,
  **9** OK/go, **10** abort+reason.
- **The compare runs on the CLIENT** (`FUN_00462050`): recompute local array; then
  `if (host_master == local_master)` → load scenario & proceed; else → msg **0x261**
  "Host and client checksums don't match." → send a **cmd-10 abort** → host aborts the start.
- The per-file "...don't match for %s. Show additional errors?" dialog (msg 0x260) is a
  **dead/leftover** string in 1.3.6 - nothing references msg 0x260. The live check is the
  single master equality with the generic message.

## What this means for our client
Because the compare is **client-side self-validation** (a real client aborts *itself* on
mismatch; the host does not independently verify our files), our custom client has two easy
options:
1. **Skip it** - just never send the abort. Respond to the host's cmd-8 with **cmd-9 (OK/go)**
   and we sail past. The host only aborts if a client *reports* a mismatch (cmd-10).
2. **Reproduce it** - compute the master with the kernel above over the files in `D:\OP2-136`
   (sheets 0-8, OP2Shell.dll, Outpost2.exe, + the chosen map DLL/techtree) and confirm it
   equals the host's master we read at wire 0xC9 of the start packet. Pure, deterministic,
   no per-machine/runtime inputs - so it always matches if the files match.

Either way the game-start checksum is **not** a wall. CONFIRMED LIVE (2026-06-05): the
`op2session` client used **option 1** - it skips the checksum entirely (never opens a file,
never computes a master) and just replies cmd-9 "loaded" to the host's cmd-8. The host
accepted it, broadcast GO, and the game launched normally. The checksum is purely a
client-side self-check; an external client simply declines to self-abort.

## Function VAs
| VA | role | VA | role |
|--|--|--|--|
| 0x40C050 | kernel hash (rotate-xor) | 0x44FFE0 | builds the 14-dword array |
| 0x4712A0 | ChecksumStream (sheet by name) | 0x488010 | ChecksumExe (Outpost2.exe) |
| 0x4880F0 | ChecksumShell (OP2Shell.dll) | 0x403650 | level-DLL composite hash |
| 0x471590 | GetFilePath (ART_PATH→install→CD) | 0x484590 | open+mmap file |
| 0x461700 | host StartGame (broadcast master) | 0x462050 | client StartGame (**compare**) |
| 0x461F50 | client receive GameStartInfo blob | - | master @ blob+0xB6 / wire+0xC9 |
