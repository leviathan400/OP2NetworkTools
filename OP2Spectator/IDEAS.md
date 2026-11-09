# OP2Spectator - future ideas (not scheduled)

Parking lot for things we have discussed but are deliberately NOT building right now. Each entry notes
the gist and why it is / isn't worth it.

## 1. Capture a scripted-AI's behavior by running OP2Lua missions in multiplayer
**Status: promising - the most natural high-value use of OP2Spectator.**

The OP2Lua colony missions are currently **single-player** with scripted AI. In single-player the AI's
decisions are computed locally and never leave the process. But Outpost 2 multiplayer is deterministic
**lockstep**, so in an MP game *every* player's per-tick decisions - including a scripted / AI player's -
are emitted as **command packets on the wire**. So if a mission is run (or has a variant) as multiplayer,
OP2Spectator can join a slot and **record the AI player's entire action timeline with zero Lua
instrumentation** - the wire is the source of truth.

Why it's compelling:
- **External & non-invasive.** No logging hooks in the mission script - OP2Spectator captures the AI's
  build order, production, research, movement, combat orders, diplomacy, and timing straight off the wire.
- **An agent can fully review the AI run.** The structured `.log` / `.jsonl` / `.summary` give a complete,
  mark-by-mark decision trace of what the scripted AI did during the mission.
- **Deterministic & reproducible.** Lockstep means the same script + seed -> the same command stream, so
  runs are repeatable and comparable across script versions (a regression/diff tool for AI behavior).
- **Free AI-vs-human discriminator.** Idle AI players send `ctInvalidCommand` every cycle while humans
  send `ctNop` - so the spectator can tell which slots are AI just from the idle heartbeat.
- **Doubles as a mission-debug / dataset tool.** Validate that the scripted AI actually does what's
  intended, and build a behavior dataset for analysis.

What needs checking / doing before it's real:
- Make (or add) a **multiplayer variant** of the colony mission so the scripted AI occupies a real player
  slot in an MP game that OP2Spectator can join.
- **Verify** that a Lua / TethysGame-issued AI order travels the same command-packet path on the wire as
  a human's (it should - all commands funnel through the same processor - but confirm, since some scripted
  effects might bypass the command stream).
- The host runs the MP mission (driving the scripted AI); OP2Spectator joins a slot to watch. The AI's
  commands then appear under that AI player's slot in the recorded timeline.

Cross-refs: OP2Lua (mission scripting), this tool (OP2Spectator), and the `ctNop` vs `ctInvalidCommand`
idle-heartbeat distinction noted in the command-packet reference.

## 2. Resolve unit/structure IDs to names + positions
**Status: candidate refinement.**

The recorder logs raw unit ids and `MapID` item numbers (`item=39`, `target-unit=65`). A lookup table
(unit-type names from the stat sheets / `MapID` enum) would render `ctMoProduce item=39` as
`Produce: Scout` etc. - much more readable build orders. The ids/positions of units *built* can also be
tracked from the command stream itself (create/build/produce), though combat losses are off-wire so the
model would drift (see the "no live composition" caveat in the README).

## 3. Live action feed in the GUI
**Status: nice-to-have.**

Right now the GUI shows lobby + chat + a `REC` counter; the action timeline only goes to the files. A
scrolling live feed of decoded actions (the same lines as the `.log`) would make it a proper at-a-glance
spectator view. Low effort - pipe the recorded action lines into a bounded list in `ClientState`.

## 4. Replay / post-match report
**Status: future.**

The `.jsonl` action stream is already a recording. A small offline tool could replay it into a timeline
view, or produce a per-player match report (build-order comparison, timing milestones, action histograms).
Pure post-processing - no protocol work needed.
