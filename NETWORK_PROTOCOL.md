# Outpost 2 - Network protocol (architecture and behaviour)

A behavioural description of the Outpost 2 multiplayer network stack: the layering from raw
Winsock UDP up through reliable delivery, the lobby state machine, the player roster, and the
per-tick command exchange. This documents *what the stack does and the wire/data layouts it
uses* - no game source, just observed behaviour and formats.

## Network stack layers (top to bottom)
```
+-------------------------------------------------------------+
|  Per-tick command exchange  - gameplay commands (type 12)   |
|                               one packet per command mark    |
+-------------------------------------------------------------+
|  Guaranteed-delivery layer  - ACK + resend, ordering        |
|                               37-buffer pool, 800ms timeout, |
|                               retry every 200ms              |
+-------------------------------------------------------------+
|  Reliable transport         - framing on top of UDP,        |
|                               64-buffer pool, free/data      |
|                               lists, one receive thread      |
+-------------------------------------------------------------+
|  Winsock UDP                - sendto/recvfrom; 47800-47807   |
|                               (game), 47776 (discovery),     |
|                               47777 (alternate transport)    |
+-------------------------------------------------------------+
```

## Winsock initialisation
Winsock startup is **reference-counted**, so the several networking components (the main
transport, the alternate transport, and game discovery) can each initialise independently
without conflicting. `WSAStartup` is called once on the first initialise; each later
initialise just bumps the count; cleanup decrements it, and the final teardown calls
`WSACleanup`.

## Transport layer (UDP)

### Transport object
The transport owns: the UDP socket (invalid until bound), the receive-thread handle, the
player count, the local player number, our own NetID, the bound local address, a 6-slot peer
table, two critical sections (guarding the free-buffer and received-data lists), a pool of 64
pre-allocated buffer nodes (the packet ring used for both send and receive), and traffic
counters (packets/bytes sent and received). The receive thread enqueues incoming packets to
the data list; the simulation thread dequeues them.

### Per-peer table (6 slots, 12 bytes each = 72 bytes)
| Field | Size | Notes |
|---|---:|---|
| IP address | 4 | network byte order |
| player NetID | 4 | session-unique; low 3 bits = player number |
| player status | 1 | state enum (below) |
| port | 2 | network byte order |
| padding | 1 | alignment |

`playerNetID & 7` recovers the player number, so NetIDs are generated with unique low-3-bits
and a per-session preamble in the upper bits.

### Player-status enum
| Value | Name | Meaning |
|---:|---|---|
| 0 | Empty | slot unused |
| 1 | Joining | mid-handshake |
| 2 | Normal | active player |
| 3 | Replicated | confirmed in the broadcast roster |
| 4 | ReplicationFailed | error state |

### Hosting a game
Create a UDP socket, resolve the local host IP, and **bind to the first free port in
47800-47807** (on the resolved local interface, not a wildcard bind). The host then fills its
own peer slot (status Normal, with its NetID and IP), sets its local player number to 0,
stores the (uppercased) password if any, spawns the receive thread, and resets the traffic
counters.

### Joining a game (client)
The same socket setup, except the client binds the first free local port in 47800-47807 and
then attempts to join the host (by host address, session GUID, and password). On success it
spawns its receive thread.

## Guaranteed-delivery layer
This layer wraps the transport with ACK-based reliable delivery, retries, and ordering.

- **Capacity:** 37 in-flight reliable buffers (each holds one packet awaiting ACK).
- **Timeout:** 800 ms - if a packet isn't ACK'd within 800 ms, it is retransmitted.
- **Retry interval:** 200 ms between attempts.

On initialise it links to the transport, builds the free-buffer list, queries the transport
for the list of peer NetIDs, and registers each as a player.

**Adding a player:** find the first empty slot (max players), record the NetID, stamp the
last-received time, reset the resend counter, increment the player count, and send a small
"welcome" packet to that peer.

**Sending reliably:** a destination NetID of 0 means **broadcast** (send to all peers); a
non-zero NetID means **unicast** to one peer. The distinction matters for ACK tracking - a
broadcast must be ACK'd by *every* peer (tracked with an ack bitmask), a unicast by just one.
The buffer is appended to the in-flight list, sent, and stamped with its send time.

**Receiving:** pull a packet from the transport, find the peer slot matching the source NetID
(unknown NetIDs are discarded), and refresh that peer's last-received timestamp (any packet
counts as a keepalive). Transport-control messages (such as a new-player notification) are
handled in-layer; otherwise the gameplay payload is copied out for the simulation thread.

**Resending (periodic):** for each in-flight buffer older than the timeout, recompute which
peers still haven't ACK'd and resend - to the single recipient for a unicast, or to every
un-ACK'd peer for a broadcast - bumping the per-peer resend counters and the buffer's retry
count.

**Player-drop rule:** a peer is dropped only when **both** clocks exceed the 800 ms timeout -
no ACK received *and* no packet received. Requiring both avoids false-positive drops during
brief network congestion. This is the lockstep stall detector: a player who can't keep up is
disconnected and the others continue.

## Per-tick command exchange
Each player's pending gameplay commands are packed into a **type 12 (0x0C)** packet - the
lockstep tick packet - and sent reliably. A type-12 payload carries zero or more command
records for one player; an empty turn is a single no-op command and doubles as a keepalive.

The exchange runs in batches at **command-mark boundary ticks** only. The interval between
marks (the "command packet processing interval", CPPI) is a power of two (default 4, so its
log2 is 2) and is adjusted dynamically by the adaptive pacer - a slowest-player-wins scheme
that widens the interval when a peer falls behind.

## Game discovery (LAN broadcast)
To find games, a client opens a UDP socket with broadcast enabled, builds a query (stamped
with the current time and a sum-of-DWORDs checksum), and broadcasts it to
`255.255.255.255:47776`.

A background thread receives replies. For each reply it verifies the checksum and that the
16-byte game-type GUID matches, then records a session entry: host address, session GUID,
creator name, and max players. **Ping** is computed as `replyReceiveTime - queryTimestamp`
(the round-trip), and replies can be averaged for stability.

- **Discovery port:** 47776 (0xBAA0).
- **Game-data ports:** 47800-47807 (0xBAB8-0xBABF).

Both ranges must be open in the firewall for LAN play.

## Lobby state machine

### Creating a game (host)
A status window appears, the local IP is shown, and the host-configuration dialog collects the
creator name, password (uppercased), max players, and game parameters. The host session is set
up (binding a game port and starting the receive thread); on failure a "creating session
failed" message is shown, otherwise the flow moves to the pre-game setup window.

### Joining a game (client)
A status window appears, the local IP is shown, and the player browses a live list of
discovered sessions. Selecting one issues a join request; on success the client moves to the
pre-game setup window as a client, on failure an error dialog is shown.

### Pre-game lobby
The pre-game setup window periodically exchanges lobby state. The host packages all lobby
settings into the **191-byte GameStartInfo** block (player records with name/colour/race,
victory condition, resource level, packed `maxPlayers`/`missionType`, seed, and command-pacing
fields) and broadcasts it to all players. A joining client sends its player name to the host
reliably and opens the setup window.

### Starting the game (host)
The host broadcasts a start announcement (the 191-byte GameStartInfo) and waits for every
player to respond start-ready. If any player drops or fails to respond during startup, the
start is aborted and the host returns to the lobby. Once all have responded ("all remote
players have responded, the game is starting"), the simulation starts on every machine.

## Player-list replication
Whenever a player joins or leaves, the host broadcasts the canonical roster so every client
converges on the same player list:

1. Stale "Joining" slots that never progressed are cleared.
2. A **SetPlayerList** message (commandType 4) is built containing the 6-slot roster.
3. Normal players in the broadcast copy are marked Replicated.
4. The message is sent to all and the host waits for confirmation.

Each client copies the roster verbatim into its own peer table, updates its player count, and
acknowledges. Confirmation uses a **retry-until-status** loop: up to **16 rounds**, each round
re-sending only to players whose status hasn't yet reached the expected value, until all match
or the 16-retry limit is hit (after which replication is declared failed and the start
aborts).

## Protocol-control packets (type 1)
Control packets carry a `commandType`:

| commandType | Name | Sender | Purpose |
|---:|---|---|---|
| 1 | TellHostUpdatedPlayerStatus | client | "my status is now X" (host updates the roster, replies with 5) |
| 4 | SetPlayerList | host | broadcast the canonical 6-slot roster; clients copy it and confirm |
| 5 | SendHostUpdatedPlayerStatus | host | per-client confirmation of a status change |

## Alternate transport (port 47777)
A second transport class binds port 47777 (one above the discovery port). Its likely role was
the SIGS matchmaking traffic, leaving the main transport purely for game data. With SIGS
retired it is effectively unused - the engine code remains but the modern menu never reaches
it.

## SIGS (Sierra Internet Gaming System)
SIGS was Outpost 2's online matchmaking service. The deeper SIGS reverse-engineering and the
community-server work are a separate effort kept outside this repo. In short: SIGS handled
**matchmaking only** - once it produced the player roster (IPs and ports), gameplay traffic
went peer-to-peer over the regular UDP transport above; SIGS was never in the gameplay data
path. All Sierra servers have been offline since ~2003; OPU disables the entry point but the
engine code remains intact.

## Host-configuration dialog
The create-game dialog captures the creator name, max players (1-6), an uppercased password,
and the mission type. These are packed into a `startupFlags` value - **max players in bits
6-8, mission type in bits 0-5** - which then rides along in the GameStartInfo to all clients.

## OPU enhancements
The OPU patcher (per its CHANGES) adds:
- **NetHelper.dll** - UPnP / NAT-PMP automatic port forwarding for 47800-47807.
- **Multi-adapter binding** - binds all interfaces instead of one (vanilla had a single-NIC
  bug).
- **`@X,Y` map ping** - a coordinate-ping system layered on top of in-game chat.
- **"Don't halve speed in multiplayer"** - disables the per-MP-player game-speed halving.
- **Disabled IPX / modem / serial** - the legacy DirectPlay paths are removed from the menu.

The wire format is unchanged, so OPU clients interoperate with vanilla clients.

## Port summary
| Port | Direction | Purpose |
|---:|---|---|
| 47776 (0xBAA0) | UDP broadcast | game discovery (LAN find-games) |
| 47777 (0xBAA1) | UDP | alternate transport (SIGS-era; effectively unused) |
| 47800-47807 (0xBAB8-0xBABF) | UDP | game data + lockstep commands |

For a remake, opening 47800-47807 plus 47776 is enough for LAN play; for internet play, add
NAT traversal (UPnP / hole punching).

## Wire format
Every datagram is a 14-byte header followed by a payload:

| Offset | Size | Field |
|---:|---:|---|
| 0x00 | 4 | source player NetID |
| 0x04 | 4 | timestamp (tick or wall-clock) |
| 0x08 | 1 | payload size |
| 0x09 | 1 | type (1 = control, 12 = gameplay, ...) |
| 0x0A | 4 | checksum (sum of DWORDs, excluding itself) |
| 0x0E | N | payload |

A control payload begins with a `commandType` byte (1-6); a type-12 gameplay payload carries
one or more command records. The receiver recomputes the checksum over the whole packet
(excluding the checksum field) and discards the packet on mismatch.
