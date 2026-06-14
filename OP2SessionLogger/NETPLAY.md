# OP2SessionLogger - lobby / transport findings (1.3.6 vs OPU 1.4.1)

How our from-scratch client joins and **holds** an Outpost 2 multiplayer lobby, and the full story of
making it work against an **OPU 1.4.1** host (not just retail 1.3.6). This is the wire/transport +
lobby layer; the in-game command-packet layer is in [`FINDINGS.md`](FINDINGS.md).

Sources cross-checked: the retail 1.3.6 decompile (`D:\Oupost2Decompile\OP2KB-FULL` - especially
`docs/engine/NETWORK_PROTOCOL.md` and the `GurManager` / `MultiplayerPreGameSetupWnd` function dumps),
the OPU patch source (`D:\GITHUB\OPUPatch`), the NetHelper source (`D:\GITHUB\NetHelper`), and live
capture with OP2SessionLogger's `[trc]` packet trace.

## Wire packet header (14 bytes)
```
+0x00  u32  senderNetID     (low 3 bits = sender slot; validated against the roster)
+0x04  u32  destNetID       (our netID, or 0 = broadcast). NOT a timestamp.
+0x08  u8   sizeOfPayload
+0x09  u8   packetType      1 = transport-control, 0 = game/GUR message
+0x0A  u32  checksum        salted sum-of-DWORDs (XORK 0xFDE24ACB)
+0x0E  payload[sizeOfPayload]
```
Type-0 (game/GUR) payloads carry a GUR sub-header at +0x0E: `flags, seq, ackA, ackB, cmd, ...`.
Observed GUR flag bits: `0x02` = channel-A data, `0x04` = channel-B data, `0x08` = GUR-ack,
`0x01`/`0x10`/`0x20` = other channel/ack modes (not all pinned). Type-0 lobby `cmd` values: `1`
GameStartInfo, `2` status-update, `3` player-setup (name+version), `5` **player-removed notification**,
`6` chat, `7`/`8` start, `9` loaded/go, `0x0B` quit, `0x0C` in-game command turn.

Transport (type-1) commands (`Packet.h`): `0` JoinRequest, `1` JoinGranted, `2` JoinRefused,
`4` SetPlayersList, `5` SetPlayersListFailed, `6` UpdateStatus.

## Join + lobby flow (what the bot does)
1. **Discovery** (UDP broadcast :47776): host replies (81 B) with the session GUID, game name, etc.
2. **Bind** a game socket in 47800-47807 (see the same-machine note below).
3. **Join** (:47777): send JoinRequest with the session GUID + our port; host replies JoinGranted with
   our assigned netID (slot = netID & 7).
4. **Lobby**: host streams the 191-byte **GameStartInfo** (cmd-1) reliably; we ack, auto-ready, chat.
   The host advances us `Joining(1) -> Normal(2) -> Replicated(3)`; if we never advance, the host
   garbage-collects our slot.

## Host game version (TApp::Version)
The host's version is in the **GameStartInfo blob at wire `0x17`** (blob +0x04), a u32 e.g.
`0x01030004` = "1.3.0.4", `0x01040001` = "1.4.0.1". `parse_gsi` exposes it as `LobbyInfo.tappVersion`;
the client shows it in chat + the lobby panel ("Host Ver"). It is NOT in the discovery reply or the
join-accept, and the host does not send a cmd-3 back.

**The host drops a peer whose advertised version doesn't match its own** (the host shows "Player X
joined but had wrong game version and was ejected"). We advertise our version in cmd-3 (`TAPP_VERSION`
in `op2proto.h`, default **1.4.0.1**), overridable with `--ver a.b.c.d`. Use `--ver 1.3.0.4` for retail
1.3.6 hosts; the default joins OPU 1.4.1. (OPU bumped the internal version to 1.4.0 specifically "so
modded games can't join unmodded by mistake".)

The client detects this case: once the GSI reveals the host's version, if it differs from ours we flag
a mismatch (chat warns immediately), and on the resulting drop the status reads **"Ejected: wrong game
version. Host is X, we sent Y. Pass --ver X."** rather than a generic timeout. Note that the version
gate is *separate* from the same-machine bind fix below - with the bind fixed, our cmd-3 reaches the
host and it rejects a bad version **fast (~4 s)**; with the bind broken it never read our version and
we fell through to the slow ~14 s retry-cap timeout, which is why the two problems looked like one.

## The 1.4.1 lobby drop - root cause and fix (the big one)
**Symptom:** advertising the correct version, the bot would join a 1.4.1 host, appear briefly, then be
removed at a deterministic **~14 s** ("Player has stopped responding and has been dropped"); START
never enabled. Retail 1.3.6 held the same idle bot indefinitely.

**The drop mechanism (decompile):** the host keeps a *reliable* GUR buffer outstanding to us (the
directed channel-A GameStartInfo). `GurManager::GetNetworkStats` sets `bDropped` when that buffer's
`retryNum > numRetries` (16-retry cap x 800 ms resend ~= 12.8 s ~= the +14 s). `CheckForDroppedPlayers`
then prints "stopped responding" and removes us. The `0x05` we saw is just the engine's
"player-removed" notification - it fires on a clean 1.3.6 quit too, so it is an effect, not a kick. It
is sent **unicast to the player being removed** (dst = that player's netID); a *remaining* player never
receives another player's `0x05` - it learns of departures from the GSI roster going short one entry.
(Confirmed in a 3-player lobby: ejecting another player produced no `0x05` for us, only a roster diff.)

**Why the buffer never confirmed (the actual bug):** the host never received our acks, so its directed
GSI never got confirmed. Not because of the port we replied to (both 1.3.6 and 1.4.1 hosts *send* from
an ephemeral port and *receive* on :47800 - identical), but because of a **same-machine bind
collision** introduced by **NetHelper**:

- NetHelper's `BindWrapper` forces every host socket bind to **`INADDR_ANY` (0.0.0.0)**
  (`D:\GITHUB\NetHelper\src\NetPatches.cpp`).
- Retail 1.3.6 binds its game socket to the host's **specific IP** `10.x:47800`. On the same machine,
  our specific-IP bind to `10.x:47800` **fails** (taken), so we skip to 47801. Our acks to
  `hostIp:47800` reach the host. Works.
- A 1.4.1 host binds `0.0.0.0:47800`. Windows then **lets our specific-IP bind to `10.x:47800`
  succeed** alongside the host's wildcard bind. So our bot grabs **47800 - the host's own port** - and
  when we send our ack to `hostIp:47800`, Windows delivers it to the **most-specific** socket = **our
  own bot**. Our acks loop back to us (this is the "echo" - we received our own relayed packets and
  misread them as `hostAckA=1`); the host never hears us; its buffer times out; we're dropped.

**The fix (`op2client.cpp`, port scan): MATCH THE HOST'S BIND STYLE, because on Windows a wildcard
(`0.0.0.0`) bind and a specific-IP bind to the same UDP port *co-exist*, and the most-specific socket
silently steals the traffic.** There is no single bind that's safe against both host types - the two must
be different modes. The determinant is **NetHelper ON vs OFF**, *not* the game version: NetHelper's
`BindWrapper` makes the host bind wildcard, and it can be toggled independently of 1.3.6/1.4.x (a 1.3.6
install ships with NetHelper too - it's just usually disabled). So this is really *NetHelper-off (vanilla)
vs NetHelper-on*, which happens to correlate with 1.3.6/1.4.1 only because of how they're typically run.

| Host | Host binds | **We bind** | Why |
|------|------------|-------------|-----|
| **NetHelper ON** (e.g. OPU 1.4.x default) | `0.0.0.0:47800` (wildcard) | **wildcard `0.0.0.0`** | wildcard-vs-wildcard *conflicts* → we skip 47800. A specific bind would co-exist with the host's wildcard and steal its traffic (the drop above). |
| **NetHelper OFF** (vanilla, e.g. 1.3.6 w/o NetHelper) | `10.x:47800` (specific) | **specific to our local IP** | specific-vs-specific *conflicts* → we skip 47800; a later real client skips us. A wildcard bind would co-exist with the host's (and a 3rd player's) specific bind and lose/steal traffic ("host stopped responding" / "Connecting to session failed"). |

**We DETECT the mode by probe (not from `--ver`).** Before the real bind we try a throwaway WILDCARD bind
to the host's game port (47800): on Windows two wildcard binds *conflict* but a wildcard + a specific bind
*co-exist*, so the probe **fails iff the host already holds 47800 wildcard** (NetHelper ON) and succeeds
otherwise (specific host co-exists, or port free → NetHelper OFF). This is decisive same-machine - the
only case where bind style matters; cross-machine the port is free so it reads OFF, which is harmless.
`--ver` stays purely for the *version gate* (which game we advertise/join), and no longer guesses the bind
mode. Plus, when the host is on **this machine** (`localIp == hostIp`) we skip its 47800 explicitly - a
client never needs 47800 (the host owns it and learns our port from the join request). The debug log
prints `bound game socket to <ip>:<port> [detected NetHelper ON - wildcard | OFF - specific] (same-machine ...)`.

> History: an earlier single-mode attempt bound *specific* (worked NetHelper-off, broke NetHelper-on); a
> second bound *wildcard* (worked on, broke off with "Connecting to session failed"). A `--ver`-derived
> dual mode fixed both *in the common config* but mis-inferred NetHelper when it was toggled against the
> version default - so the final design probes the host's actual bind style. A client must look like the
> other clients on the wire, and NetHelper is what changes what those look like.

**Same-machine multi-instance (3+ players).** Matching the host's style also fixes the inverse (a
later-joining local player stealing/los­ing our port): in each mode our bind conflicts cleanly with the
other same-style clients, so everyone lands on a distinct port. Verified live: host(47800) + bot(47801)
+ a real 3rd player(47802) all in one lobby on one machine, on both versions.

**In-game P2P: the host advertises itself as `0.0.0.0`.** Once a 3-player game starts, command turns go
peer-to-peer (the bot broadcasts each turn to every peer's game address from the cmd-4 roster). But the
host lists *itself* in that roster with its **bind** address - and a 1.4.1 host's bind address is
`0.0.0.0` (INADDR_ANY). Broadcasting our turns to `0.0.0.0:47800` goes nowhere, so the host never gets
our input and the lockstep stalls at the first command-turn boundary (~tick 8). Fix: substitute the
host's real IP (known from discovery) for any `0.0.0.0` roster entry -
`if (pe.addr.sin_addr.s_addr == 0) pe.addr.sin_addr.s_addr = hostIp;`. (Other peers carry a real IP -
the host learned theirs via NatFix.) Verified live: a 3-player 1.4.1 game ran to tick ~1472.

**In-game P2P: the host leaving does NOT end the game (lockstep clock failover).** Our in-game turn
generation was *host-driven*: we only produced our own command turn (`cmd-0x0C`) in reply to the
**host's** turn. That is fine while the host is present, but the engine's in-game model is fully P2P -
if the host leaves, the *remaining* players keep playing. With the host-only driver, the moment the
host quit we stopped emitting turns, so the surviving player's engine waited on our input and dropped us
~13 s later ("We have lost contact with player OPU"), and our own client wrongly declared the match
over. **Fix: a "lockstep clock" we mirror, which fails over.** It starts as the host; when the clock
player leaves (sends `ctQuit` 0x31, broadcasts `cmd-0x0B`, or just goes silent while another peer is
still streaming turns), we re-elect the clock to the lowest-slot surviving peer and keep mirroring
*its* turns - so our turns keep reaching the remaining players. The game-end watchdog now trips only
when **no** other player is still sending turns (we are the last one), not merely when the host left.
(Tracked per slot via `lastTurnMsBySlot[]`; re-elected by `electClock()`.)

**Known limitation - ungraceful disconnects (deferred; LAN-only is fine).** We robustly handle
*graceful* departures (a `ctQuit` from any player → instant detection/failover) and a silent **clock**
(3 s timeout → failover). We do **not** yet fully handle an *abrupt* drop with no `ctQuit` - a player's
net dropping, exe/PC crash, or kill:
- A non-clock **peer** that vanishes: we simply stop seeing its turns and keep playing (we mirror the
  clock, not that peer), but we don't print "*** X has left ***" for it (no `ctQuit`, and we don't run
  the engine's per-peer drop timer). The remaining real clients drop it via their own timeout.
- The **clock** vanishing: handled, but with up to a ~3 s gap before `electClock()` fails over (the
  other player may flash OP2's "lost contact" dialog in that window).
A fully robust client would track per-peer turn liveness and synthesize a "dropped" event on timeout
(mirroring `CheckForDroppedPlayers`), and optionally tighten the failover window. Deferred - this only
matters on a flaky WAN; on LAN drops are rare and the engine's own drop handling covers the real players.

> **How this maps to the real engine (important caveat).** Our "lockstep clock failover" is a
> deliberately simplified, app-level stand-in for a mechanism the engine implements formally. Per the
> retail decompile (`OP2KB-FULL/docs/engine/RNG_AND_DETERMINISM.md` §"Resync master election" and
> `GAME_LOOP.md`), OP2 lockstep runs through a **"master" player** who *aggregates* every player's
> per-tick command packet and rebroadcasts the consolidated set (`Got tick data from master.` /
> `Sending TICK data for player %i to master %i.`). The host is just the **initial master**. When the
> master goes offline, survivors run a real **resync-master election** - type-1 control messages
> `TM_RESYNC_MASTER` (0x0D), `TM_ACK_RESYNC_MASTER` (0x0E), `TM_SET_NEW_STATE` (0x0F),
> `TM_QUITTING_SESSION` (0x0B) - **lowest NetID wins** the tiebreak (which is why our "lowest-slot
> survivor" rule happens to agree with the engine), then re-simulate from a common tick.
>
> **RESOLVED on the wire (both 1.3.6 and 1.4.1).** The debug capture settles it:
> 1. **Topology = peer-direct.** Each player's command turns arrive tagged with **that player's own
>    netID** (`in-game turn source: slot=2 srcNetID=<peer>`), not a single master's. When the host
>    (initial master) leaves, **the sim keeps ticking with zero `RESYNC/ctrl` traffic** - our bot and
>    the surviving peer exchange turns *directly*. If steady-state play were master-relayed, it would
>    stall at the master's departure until a resync election; it doesn't. So steady-state in-game is
>    plain peer-to-peer broadcast, and the master-aggregation / resync-election machinery the KB
>    documents is the **recovery layer** (lag / drop / desync), not the per-tick path. *Confirmed
>    identical on retail 1.3.6 and OPU 1.4.1 - consistent with the engine code being ~99% shared.*
> 2. **No election on a clean leave.** We observed **no** `TM_RESYNC_MASTER`/ack on a host-leave on
>    either version, so our app-level `electClock()` re-point isn't merely a shortcut past an election
>    we're skipping - there *is* no election in this path. (The formal election only matters for the
>    recovery case, which a clean ctQuit/host-leave doesn't trigger.) Our broadcast-`dst=0`-to-every-peer
>    model is therefore protocol-faithful for steady-state play.

### Why each earlier hypothesis was wrong (the elimination trail)
- **Version gate** - real, but only gets us *into* the lobby; not the drop.
- **Players-list replication failure** - no; type-1 trace showed only `UpdateStatus`.
- **Idle-timing ("host starts within 3 s")** - wrong; 1.3.6 holds idle bots forever.
- **Reply to the host's source port** - wrong and regressed (`hostAck 1->0`): the host *sends* from an
  ephemeral port but *receives* on :47800. Both versions do this; not the difference.
- **Channel-A ack content** - red herring; the acks were fine, they just never reached the host.

The real differentiator was a single OPU change (NetHelper's `INADDR_ANY` bind) interacting with a
single-machine test setup. On two separate machines the collision can't happen, so this bug is
specific to running host + bot on one box.

## Connecting: quick reference
```
OP2SessionLogger.exe --scan                  # OPU 1.4.1 host (advertises 1.4.0.1)
OP2SessionLogger.exe --scan --ver 1.3.0.4    # retail 1.3.6 host
OP2SessionLogger.exe --debug                 # self-test: join, post a chat, leave cleanly, exit
OP2SessionLogger.exe --debug 5m              # TIMED run: scan+join, count down on the form, auto-exit
OP2SessionLogger.exe --debug 90s             #   after the period (accepts 5m / 90s / 2m30s / bare secs)
```
The host's version shows in chat + the lobby "Host Ver" line once joined, so you can pick the flag.
`--debug <dur>` is the headless capture mode: it auto-joins and runs for a fixed length while showing
an "auto-exit in M:SS" countdown on the form, then leaves cleanly (in-game `ctQuit`) and closes - so a
full live session is captured to `op2sessionlogger_debug.txt` without any clicking. Great for the
master-leave test: start the timed run, host the 3-player game, and have the master quit mid-match.

## Master-leave debug lines (what to look for in the log)
When testing a host/master leaving an in-progress game, two debug lines were added to settle how the
engine really continues (see the master/resync caveat above):
- `in-game turn source: slot=N srcNetID=.. (clock=.., host=..)` - logged once per distinct turn
  source. Tells relay (all turns from one master netID) from peer-direct (turns from each peer's own
  netID), and shows which netID takes over after the master leaves.
- `RESYNC/ctrl type-1 cc=0x.. src=.. dst=..` - logged once per distinct type-1 control code outside
  the lobby set. Captures the engine's master-election traffic (`TM_RESYNC_MASTER` 0x0D,
  `TM_ACK_RESYNC_MASTER` 0x0E, `TM_SET_NEW_STATE` 0x0F, `TM_QUITTING_SESSION` 0x0B) if the real
  clients run it.
- `clock failover -> slot N (name) netID=..` - our app-level mirror re-pointing to the new clock.
