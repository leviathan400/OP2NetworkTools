# Outpost 2: Divided Destiny - Network protocol - deep-dive synthesis

Synthesis of **32 networking-related function dumps** from the .udd extraction. Covers the full network stack from Winsock initialization through the lobby state machine, guaranteed-delivery layer, game-data sync, and player drop handling.

Covers the full stack beyond the receive-thread + wire-format basics: the **lobby protocol, GurManager reliability layer, and packet-format details**.

## Network stack layers (top to bottom)

```
┌─────────────────────────────────────────────────────────────┐
│  GameNetLayer        - per-tick command-packet exchange     │
│                        (Player gameplay commands, type 12)  │
├─────────────────────────────────────────────────────────────┤
│  GurManager          - Guaranteed delivery (ACK + resend)   │
│                        37-buffer pool, 800ms timeout,       │
│                        retry every 200ms                    │
├─────────────────────────────────────────────────────────────┤
│  NetTransportLayer   - Reliable framing on top of UDP       │
│                        64-buffer pool, free+data linked     │
│                        lists, receive thread per session    │
├─────────────────────────────────────────────────────────────┤
│  Winsock UDP         - sendto/recvfrom, ports 47800-47807   │
│                        (game data), 47776 (discovery),      │
│                        47777 (Network2 / alt)               │
└─────────────────────────────────────────────────────────────┘
```

## Layer 1: Winsock initialization

### `InitializeWinSock @ 0x00490E50` (6 comments)

```c
int InitializeWinSock() {
    if (winSockInitializeCount != 0) {
        winSockInitializeCount++;
        return 1;
    }
    if (WSAStartup(0x0101, &wsaData) != 0)
        return 0;     // Failed
    winSockInitializeCount++;
    return 1;
}
```

**Refcounted** so multiple components (Network, Network2, GameSearch) can initialize independently. Cleanup decrements; final shutdown calls `WSACleanup`.

## Layer 2: Network class (UDP transport)

### `Network` object layout (from `Constructor @ 0x00495C40` + `RemovePlayer` + other refs)

```c
struct Network {
    void* vtbl;                                    // +0x00
    int   socket;                                  // -1 (INVALID_SOCKET) initially
    int   threadID;                                // ReceiveGameplayPacket thread
    int   numPlayers;
    int   localPlayerNum;
    int   playerNetID;                             // our own NetID
    struct SOCKADDR_IN localAddress;               // bound address
    struct PlayerNetworkAddress  playerNetworkAddressList[6];   // 6 slots × 12 bytes each
    CRITICAL_SECTION dataListCriticalSection;
    CRITICAL_SECTION freeListCriticalSection;
    NetworkBufferLinkedListNode* freeListHead;
    NetworkBufferLinkedListNode* freeListTail;
    NetworkBufferLinkedListNode* dataListHead;
    NetworkBufferLinkedListNode* dataListTail;
    NetworkBufferLinkedListNode  bufferPool[64];   // pre-allocated free list
    int sentPacketCount, receivedPacketCount;
    int sentByteCount, receivedByteCount;
};

struct PlayerNetworkAddress {
    int  IPAddress;         // network byte order
    int  playerNetID;
    char playerStatus;      // 0=empty, 1=Joining, 2=Normal, 3=ReplicatedSuccessfully, 4=ReplicationFailed
    short port;             // network byte order
    char padding;
};
```

**Total size**: ~33,488 bytes (matches the `0x82D0` we observed earlier - that's the size of the bigger `Network` class which contains this).

### `Network::Constructor @ 0x00495C40` (24 comments)

```c
1. Set vtbl
2. Initialize 64-node NetworkBufferLinkedListNode free list:
   - bufferPool[0..63] linked as doubly-linked list
   - freeListHead = &bufferPool[0]
   - freeListTail = &bufferPool[63]
3. dataListHead = dataListTail = NULL  (empty receive queue)
4. InitializeWinSock()
5. MemClear 18 DWORDs (= sizeof(playerNetworkAddressList) = 72 bytes)
6. socket = INVALID_SOCKET (-1)
```

The 64-buffer pool is the **packet ring** for both send and receive. Receive-thread enqueues to `dataList`; simulation thread dequeues. Send goes through `dataList` opposite-direction.

### `NetTransportLayer:TCP::HostGame @ 0x00495ED0` (32 comments)

Called by `NetGameProtocol:TCP::DoCreateGame`. Sets up a hosting session.

```c
bool HostGame(char* gameHostQueryReplyPacket, char* password) {
    socket = create UDP socket
    if (socket == INVALID_SOCKET) return false;
    
    hostent = gethostbyname(localHostName);
    addr = hostent->h_addr_list[0];
    
    // Bind to first available port in 47800-47807
    for (portNum = 47800; portNum <= 47807; portNum++) {
        localAddress.sin_addr = addr;
        localAddress.sin_port = htons(portNum);
        if (bind(socket, &localAddress, ...) == 0) break;
    }
    if (portNum > 47807) return false;
    
    // Set up host's own slot
    playerNetworkAddressList[0].playerStatus = 2;          // Normal
    playerNetworkAddressList[0].playerNetID = ourPlayerNetID;
    playerNetworkAddressList[0].IPAddress = localAddr;
    localPlayerNum = 0;
    
    // Allocate and store password
    if (password) password_copy = strdup(password);
    
    // Spawn receive thread
    threadID = CreateThread(NULL, 0, ReceiveGameplayPacket, this, 0, &tid);
    if (!threadID) return false;
    
    InitializeTrafficCounters();
    return true;
}
```

### `NetTransportLayer:TCP::SetupSocketAndReceiveThreadAndAttemptGameJoin @ 0x00496440` (30 comments)

Client-side parallel. Same socket setup, but instead of binding to first free port it tries to JOIN an existing host:

```c
bool Join(int hostAddress, GUID* sessionId, char* password) {
    socket = create UDP socket
    bind to first available local port in 47800..47807
    
    // Attempt to join host at hostAddress
    success = JoinHost(hostAddress, sessionId, password);
    if (!success) return false;
    
    // Spawn receive thread
    threadID = CreateThread(..., ReceiveGameplayPacket, ...);
    InitializeReceiveCounters();
    return true;
}
```

### Per-peer state - `playerNetworkAddressList[]`

6 slots (max 6 players). Each entry tracks one peer:

| Field | Size | Purpose |
|---|---:|---|
| `IPAddress`     | 4 | network byte order |
| `playerNetID`   | 4 | unique session-wide ID (low 3 bits = playerNum) |
| `playerStatus`  | 1 | state enum |
| `port`          | 2 | network byte order |
| (padding)       | 1 | alignment |
| **Total**       | **12 bytes/slot × 6 slots = 72 bytes** | |

The `playerNetID & 7` formula recovers `playerNum` (so playerNetIDs are constrained to have unique low-3-bits - the netID generation has a per-session preamble in the upper bits).

### `playerStatus` enum (decoded from receive logic)

| Value | Name | Meaning |
|---:|---|---|
| 0 | Empty | slot unused |
| 1 | Joining | mid-handshake |
| 2 | Normal | active player |
| 3 | ReplicatedPlayersListSuccessfully | confirmed in roster |
| 4 | FailedToReplicatePlayersList | error state |

## Layer 3: GurManager (Guaranteed Send Manager)

The **reliability layer** that wraps the Network transport. Implements ACK-based reliable delivery, retries, and ordering.

### `GurManager::Constructor @ 0x0042D710` (7 comments)

```c
1. head = NULL
2. Initialize 37 NetBuffers (each is a `Packet` + bookkeeping fields)
3. Network* = NULL    (linked later via InitializeGuaranteedSendLayer)
```

**37 buffers** - total reliable-send capacity. Each NetBuffer holds one in-flight Packet awaiting ACK.

### `GurManager::InitializeGuaranteedSendLayer @ 0x0042D760` (27 comments)

```c
bool Initialize(NetTransportLayer* netTransportLayer) {
    Network = netTransportLayer;
    timeOut = 800;                       // ms before retry
    timeOut/4 = 200;                     // retry interval
    
    // Build linked list of free NetBuffers
    for (i = 36; i >= 0; i--) {
        NetBuffer[i].prev = tail;
        NetBuffer[i].next = NULL;
        tail->next = &NetBuffer[i];
        tail = &NetBuffer[i];
    }
    
    // Get list of peer NetIDs from transport layer
    numReturned = Network.GetOpponentNetIDList(netIDList, maxIDs);
    if (numReturned <= 0) return false;
    
    for (i = 0; i < numReturned; i++)
        AddPlayer(netIDList[i]);
    
    return true;
}
```

**Key constants:**
- `timeOut = 800ms` - if a packet isn't ACK'd in 800ms, retransmit
- `retryInterval = 200ms` - between retry attempts

### `GurManager::AddPlayer @ 0x0042D8C0` (39 comments)

```c
int AddPlayer(int playerNetID) {
    if (playerNetID == 0) return -1;
    
    // Find first empty slot
    for (currentPlayerNum = 0; currentPlayerNum < 5; currentPlayerNum++) {
        if (playerInfo[currentPlayerNum].playerNetID == 0) break;
    }
    if (currentPlayerNum >= 5) return -1;     // full
    
    playerInfo[currentPlayerNum].playerNetID = playerNetID;
    playerInfo[currentPlayerNum].timeOfLastReceivedPacket = timeGetTime();
    playerInfo[currentPlayerNum].numResentPackets = 0;
    
    numPlayers++;
    
    // Pull a free NetBuffer from head, set up a "Welcome" packet, send via NetTransportLayer
    NetBuffer = freeList.pop();
    NetBuffer.Packet.sizeOfPayload = 8;
    NetBuffer.Packet.destPlayerNetID = playerNetID;
    NetTransportLayer.Send(&NetBuffer.Packet);
    
    return currentPlayerNum;
}
```

### `GurManager::SendNetBuffer @ 0x0042DBD0` (38 comments)

```c
bool SendNetBuffer(NetBuffer* nb, char flags) {
    nb.playerAckBitMask = 0;     // no ACKs yet
    nb.ackCount = 0;
    nb.playerNum = -1;
    
    if (nb.Packet.destPlayerNetID == 0) {
        // BROADCAST: send to all known players
        flags.bSinglecast = 0;
    } else {
        // UNICAST: find playerNum for this NetID
        flags.bSinglecast = 1;
        for (i = 0; i < numPlayers; i++) {
            if (playerInfo[i].playerNetID == destNetID) {
                nb.playerNum = i;
                break;
            }
        }
    }
    
    nb.retryNum = 0;
    nb.sentTime = 0;
    
    // Append to dataList
    nb.prev = dataListTail;
    nb.next = NULL;
    dataListTail = &nb;
    if (!dataListHead) dataListHead = &nb;
    numDataBuffers++;
    
    // Actually send
    if (!NetTransportLayer.Send(&nb.Packet)) return false;
    
    nb.sentTime = timeGetTime();
    return true;
}
```

The `flags.bSinglecast` distinction matters because broadcast packets need ACKs from ALL peers (tracked via `playerAckBitMask`), while singlecast needs only one.

### `GurManager::CheckReceive @ 0x0042DE60` (44 comments)

```c
int CheckReceive(char* outBuffer) {
    Packet pkt;
    if (!Network.CheckReceive(&pkt)) return 0;     // nothing
    
    sourceNetID = pkt.sourcePlayerNetID;
    
    // Find player slot for this NetID
    for (i = 0; i < 6; i++)
        if (playerInfo[i].playerNetID == sourceNetID) break;
    if (i >= 6) {
        OP2OutputDebugString("Unknown NetID %x", sourceNetID);
        return 0;
    }
    
    // Update keepalive timestamp
    playerInfo[i].timeOfLastReceivedPacket = timeGetTime();
    
    commandType = pkt.GamePlayPacketHeader.commandType;
    
    if (commandType == TransportLayerMessage) {
        // Special control: AddPlayer notification, etc.
        if (commandType == NEW_PLAYER_NETID) {
            AddPlayer(pkt.Payload.newPlayerNetID);
        }
        return 3;     // handled
    }
    
    // Otherwise: gameplay packet - copy to outBuffer for sim thread
    memcpy(outBuffer, pkt.Payload, pkt.sizeOfPayload);
    return 3;
}
```

### `GurManager::Resend @ 0x0042E4D0` (55 comments)

The **retransmission loop**, called periodically:

```c
void Resend() {
    int now = timeGetTime();
    
    foreach NetBuffer in dataList:
        if (now - nb.sentTime < timeOut) continue;      // not yet expired
        
        // Compute who still hasn't ACK'd
        unacked = playerInfo bitmask AND NOT nb.playerAckBitMask
        
        if (nb.playerNum != -1) {
            // SINGLECAST - only one recipient
            if (unacked & (1 << nb.playerNum)) {
                Network.SendPacket(&nb.Packet);
                playerInfo[nb.playerNum].numResentPackets++;
            }
        } else {
            // BROADCAST - increment counter for every player who hasn't ACK'd
            for (i = 0; i < 6; i++) {
                if (unacked & (1 << i))
                    playerInfo[i].numResentPackets++;
            }
            Network.SendPacket(&nb.Packet);
        }
        
        nb.retryNum++;
        
        // Drop player who is too far behind
        for (i = 0; i < 6; i++) {
            if (playerInfo[i].playerNetID == 0) continue;
            if (now - playerInfo[i].timeOfLastSentPacket > timeOut) {
                // Keepalive timeout - drop them
                ...
            }
        }
}
```

**Insight**: a peer is considered "dropped" when:
1. They haven't ACK'd a buffer for `> timeOut` (800ms), AND
2. We haven't received any packet from them for `> timeOut`

Both clocks must expire for player-drop. This is the **lockstep stall detection** - once a player can't keep up, they're disconnected and the others continue.

## Layer 4: GameNetLayer (per-tick command exchange)

### `GameNetLayer::SendPlayerCommands @ 0x00420F30` (15 comments)

```c
void SendPlayerCommands(int sourcePlayerNum, bool bSendNetBuffer, int destPlayerNetID) {
    NetBuffer* nb = GurManager.AllocNetBuffer();
    if (!nb) return;
    
    Packet.GamePlayPacketHeader.type = 12;          // 0xC = Player gameplay commands
    Packet.destPlayerNetID = destPlayerNetID;
    
    // Copy this player's pending commands into the packet payload
    payloadSize = NetworkCommandPacket.CopyPlayerCommandsToBuffer(sourcePlayerNum);
    Packet.sizeOfPayload = payloadSize;
    
    GurManager.SendNetBuffer(nb, flags);
}
```

**Packet type 12 (0xC) = "Player gameplay commands"** - the lockstep tick packet. Contains 0..N `CommandPacket`s for one player.

### `GameNetLayer::ExchangeNetworkData @ 0x00421110` (10 comments)

The per-tick batch exchange:

```c
int ExchangeNetworkData() {
    cpInterval = TethysGame.commandPacketProcessingInterval;    // power of 2
    cpMask = cpInterval - 1;
    
    // Only run at boundary ticks
    if (tick & cpMask != 0) return 1;     // not at boundary
    if (TethysGame.GameTermReasons != 0) return 1;    // game ending
    
    // Run the state machine
    do {
        rc = stateHandlers[currentState]();
    } while (rc != 1);
    
    return (rc == 0);
}
```

`commandPacketProcessingInterval` is dynamically adjusted by the **adaptive pacer** (Slowest-Player-Wins netcode) - see `TethysGame.cpRate` = 4 default and `TethysGame.lgCpRate` = 2 (log2 of cpRate) set in `StartGame`.

## Layer 5: Game-discovery broadcast

### `NetGameSearchProtocol:TCP::BroadcastForGames @ 0x004917E0` (23 comments)

Periodic LAN broadcast to find games:

```c
void BroadcastForGames() {
    socket = create UDP socket
    setsockopt(socket, SO_BROADCAST, true);    // enable broadcast
    
    // Build query packet
    netGameHostQueryPacket.timeStamp = timeGetTime();
    
    // Compute checksum: sum of all DWORDs in packet data
    checksum = 0;
    for (i = 0; i < numDwords; i++)
        checksum += packetData.dword[i];
    netGameHostQueryPacket.checksum = checksum;
    
    // Broadcast to 255.255.255.255:47776
    sockaddr.sin_port = htons(47776);     // 0xBAA0
    sockaddr.sin_addr = INADDR_BROADCAST;
    sendto(socket, &netGameHostQueryPacket, sizeof(...), 0, &sockaddr, ...);
    
    closesocket(socket);
}
```

**Port 47776 (0xBAA0)** is the **discovery port**. Game-data ports are 47800-47807. So a complete OP2 LAN setup needs both ranges open in firewall.

### `NetworkWaitForBroadcastReply @ 0x00491A50` (45 comments, runs as a thread)

Receives reply packets from hosts after a broadcast:

```c
DWORD NetworkWaitForBroadcastReply(LPVOID lpParam) {
    while (recvfrom(socket, &readBuffer, ...) >= 0) {
        // Verify checksum (sum of DWORDs starting at +4)
        checksum = 0;
        for (i = 1; i < numDwords; i++)
            checksum += readBuffer.dword[i];
        if (readBuffer.checksum != checksum) continue;     // bad packet
        
        // Verify GUID matches our game type (16 bytes = sizeof(GUID))
        if (memcmp(&readBuffer.gameIdentifier, &myGameIdentifier, 16) != 0)
            continue;
        
        // Allocate NetGameSession (0x44 = 68 bytes)
        ngs = malloc(0x44);
        memset(ngs, 0, 0x44);
        ngs->hostAddress = readBuffer.hostAddress;
        memcpy(&ngs->sessionIdentifier, &readBuffer.sessionId, 16);
        // ... copy more fields ...
        CopyString(&ngs->gameCreatorName, &readBuffer.gameCreatorName);
        ngs->maxPlayers = readBuffer.maxPlayers;
        
        // Compute ping = response time - original query time
        ngs->pingTime = timeGetTime() - readBuffer.timeStamp;
        ngs->pingTimeDivisor = 1;
        
        // Add to session list (UI thread picks it up)
    }
    return 0;
}
```

**Ping** is computed as `responseTime - originalTimeStamp` - round-trip from the broadcast issue to the reply receipt. The `pingTimeDivisor` lets the UI average multiple replies for stability.

## Layer 6: Lobby state machine

### Game creation: `NetGameProtocol:TCP::DoCreateGame @ 0x0040DFB0` (63 comments)

```
1. Create StatusWnd ("Starting TCP/IP session.")
2. Allocate NetGameSearchProtocol:TCP (104 bytes)
3. Get local IP address as string
4. Display "Your local IP address is %s"
5. Allocate GurManager (0x4FD0 = ~20 KB)
6. Show NetGameCreateWnd (the host configuration dialog):
   - gameCreatorName (player's name)
   - password (forced to uppercase)
   - maxPlayers
   - gameParameters
7. SetupHostedGame(gameID, gameCreatorName, password, maxPlayers, gameParameters)
   → calls NetTransportLayer:TCP::HostGame
8. If network creation fails: MsgBox "Creating session failed."
9. Move to MultiplayerPreGameSetupWnd
```

### Game joining: `NetGameProtocol:TCP::DoJoinGame @ 0x0040DAC0` (78 comments)

```
1. Create StatusWnd ("Joining TCP/IP session...")
2. Allocate NetGameSearchProtocol:TCP
3. Display local IP
4. Show JoinNetGameWnd (browse list of discovered games)
5. Show FindSessionWnd (real-time game-discovery scroller)
6. User selects a session → RequestJoinGame(session)
   → calls NetTransportLayer:TCP::SetupSocketAndReceiveThreadAndAttemptGameJoin
7. If join succeeds: Move to MultiplayerPreGameSetupWnd as client
8. If join fails: MsgBox(stringIndexTitle="ERROR", stringIndexButtonText="OK")
```

### Player roster setup: `MultiplayerPreGameSetupWnd`

The pre-game lobby window. Periodically calls `DoNetExchange` to send/receive lobby state.

#### `DoNetExchange @ 0x0045F840` (34 comments)

```
1. Read latest gameFlags (player count, victory conditions, etc.)
2. Allocate NetBuffer for sending GameStartInfo
3. memcpy(packet.payload, gameFlags, sizeof(GameStartInfo) / 4)  // packed as DWORDs
4. Packet.sizeOfPayload = 0xBF (191 bytes)
5. GurManager.SendNetBuffer(nb, flags)
6. If TLobby is active: check for lobby messages (SIGS chat etc.)
```

**`GameStartInfo` is 191 bytes (0xBF)** - broadcast to all players when the host clicks "Start Game".

#### `ShowJoinGame @ 0x0045F2F0` (31 comments)

Client-side join packet:

```c
void ShowJoinGame(char* playerName, int hostPlayerNetID, ...) {
    nb = GurManager.AllocNetBuffer();
    memcpy(packet.payload, playerName, strlen(playerName) + 1);
    packet.destPlayerNetID = hostPlayerNetID;
    GurManager.SendNetBuffer(nb, flags);
    
    // Open the setup window
    mainWnd.DoModal(MultiplayerPreGameSetupWndTemplate);
}
```

#### `StartGame (host) @ 0x00461700` (101 comments)

The host clicks "Start Game":

```
1. Broadcast a "start" announcement: GurManager.SendNetBuffer with sizeofPayload = 0xBF
2. Wait for all players to respond with their start-ready ACK
3. Show status: "Player <name> quit the game during startup. The game is aborted." (if drop)
4. On all responses: "All remote players have responded. Game is starting."
5. Print debug: "Game with %i players started on all machines."
6. For each player slot, print "%i\t%i\t%s\n" (playerNum, netID, name)
7. Transition to TethysGame.StartGame(gameStartInfo)
```

If ANY player drops or fails to respond, the game-start is aborted and the host returns to the lobby.

## Layer 7: Player list replication

### `NetTransportLayer:TCP::ReplicatePlayersList @ 0x00496950` (34 comments)

The host broadcasts the canonical player roster to all clients:

```c
int ReplicatePlayersList() {
    // First: clean up any "Joining" slots that didn't progress
    for (i = 0; i < 6; i++) {
        if (playerNetworkAddressList[i].status != 1 /*Joining*/) continue;
        playerNetworkAddressList[i].IPAddress = 0;
        playerNetworkAddressList[i].playerNetID = 0;
        numPlayers--;
    }
    
    // Build a SetPlayerList packet (type 4)
    NetworkBuffer.type = 1;      // Data packet marker (will be sent reliably)
    NetworkBuffer.Payload.commandType = 4;     // SetPlayerList
    memcpy(NetworkBuffer.Payload.playerNetworkAddressList, playerNetworkAddressList, 18 * 4);
    
    // Mark Normal players as "Replicated"
    for (i = 0; i < 6; i++) {
        if (NetworkBuffer.Payload.playerNetworkAddressList[i].status == 2 /*Normal*/)
            NetworkBuffer.Payload.playerNetworkAddressList[i].status = 3 /*Replicated*/;
    }
    
    NetworkBuffer.checksum = CalcChecksum(&NetworkBuffer);
    
    // Send to all and wait for ACK
    if (RepeatSendPacketUntilStatusUpdate(&NetworkBuffer, successStatus=3)) {
        return 1;     // success
    } else {
        // Build failure notice (commandType = 5)
        NetworkBuffer.NetworkCommandPacket.flags = 5;
        SendToAll(failureNotice);
        return -1;
    }
}
```

The roster broadcast happens **every time a player joins/leaves** in the lobby. Each client receives the new full list, updates their local `playerNetworkAddressList`, and ACKs.

### `RepeatSendPacketUntilStatusUpdate @ 0x00496DC0` (28 comments)

```c
bool RepeatSendPacketUntilStatusUpdate(NetworkBuffer* nb, short expectedStatus) {
    nb->sa_family = AF_INET;
    int maxTries = 16;
    
    while (numTries < maxTries) {
        bool sentAtLeastOne = false;
        
        // Send to each player whose status hasn't yet matched
        netIDList = Network.GetOpponentNetIDList(maxIDs = 6);
        for each netID in netIDList:
            playerNum = netID & 7;
            entry = playerNetworkAddressList[playerNum];
            if (entry.status == expectedStatus) continue;   // already confirmed
            
            sockAddr.sin_addr = entry.IPAddress;
            sockAddr.sin_port = entry.port;
            nb->destPlayerNetID = netID;
            nb->checksum = nb->CalcChecksum();
            sendto(socket, nb, nb->sizeOfPayload, 0, &sockAddr, ...);
            sentAtLeastOne = true;
        
        if (!sentAtLeastOne) return true;     // all players matched expectedStatus
        
        Sleep / wait for ACKs
        numTries++;
    }
    
    return false;     // gave up after 16 retries
}
```

**16-retry limit** for status-confirmation broadcasts. After 16 failures, declare the player list replication failed and abort.

## Layer 8: Protocol control packets

### `NetTransportLayer:TCP::ProcessProtocolControlPacket @ 0x00497240` (44 comments)

Handler for `type=1 ProtocolControl` packets. Dispatches on `commandType`:

| commandType | Name | Sender | Purpose |
|---:|---|---|---|
| `1` | `TellHostUpdatedPlayerStatus` | client | "My status is now X" |
| `4` | `SetPlayerList` | host | Broadcast canonical roster (via `ReplicatePlayersList`) |
| `5` | `SendHostUpdatedPlayerStatus` | host | Reply to commandType=1 with confirmation |

### commandType 1: TellHostUpdatedPlayerStatus

```c
// Client → Host
Payload {
    sourcePlayerNetID
    newStatus
}
```

Host receives, updates `playerNetworkAddressList[playerNum].status`, replies with commandType=5.

### commandType 4: SetPlayerList

```c
// Host → All Clients
Payload {
    playerNetworkAddressList[6]   // 6 × 12 = 72 bytes
}
```

Each client copies the array verbatim into their own `Network.playerNetworkAddressList`, updates `numPlayers`. Then **echoes back** a commandType=3 packet to confirm receipt.

### commandType 5: SendHostUpdatedPlayerStatus

```c
// Host → Specific Client
Payload {
    playerNum
    newStatus
    sizeOfPayload = 4
    sourcePlayerNetID = 0   // host's NetID = 0
}
```

## Network2 - alternate transport (port 47777)

`Network2` (constructor at `0x0048BF50`) is an **alternate transport class** - separate from the main `Network`. It binds to port 47777 (one above the discovery port 47776).

**Likely purpose:** the SIGS lobby protocol uses Network2 for matchmaking traffic, leaving the main Network class purely for game data. SIGS is dead so Network2 may be entirely unused in practice - the OPU patcher leaves it intact but the code path is unreachable from the modern menu.

```c
struct Network2 {
    char* password;          // password string
    // ... + socket + receive thread + ...
};
```

## SIGS - `NetLobby:SIGS` family

The deeper SIGS reverse-engineering and the community-server project are a separate effort, kept outside this repo. In brief, that work covers:

- Both DLL contracts (`SierraNW.dll` 14 entry points + `SNWValid.dll`)
- Full class hierarchy: `NetProtocolManager:SIGS`, `NetLobby:SIGS`, `NetGameProtocol:SIGS`
- The complete handshake flow (8-stage sequence from menu pick to game start)
- The `OP2_UDP_LOCK%i` per-port mutex naming
- Why SIGS, not WON (WON was Sierra's other matchmaking service; OP2 used SIGS exclusively)
- 32 SIGS-related strings catalogued by VA
- 17 SIGS-specific function VAs

Quick summary: SIGS handled **matchmaking only** - once it produced the player roster (IPs + ports), gameplay traffic went peer-to-peer via the regular Winsock UDP transport documented above. SIGS is not in the data path during gameplay.

**All Sierra servers offline since ~2003**; OPU disables the entry point but the engine code remains intact. A stub `SierraNW.dll` against a modern backend would resurrect SIGS without engine changes.

## Game-creation dialog: `NetGameCreateWnd::DlgProc @ 0x0047C4A0` (25 comments)

Captures host configuration:

```
WM_INITDIALOG: populate combo boxes (game type, max players)
WM_COMMAND:
    On "Cancel": return without creating
    On "OK":
        Read gameCreatorName
        Read maxPlayers (1..6)
        Read password (uppercased)
        Read missionType from combo box
        Store in NetGameCreateWnd.startupFlags:
            startupFlags.maxPlayers = maxPlayers << 6
            startupFlags.missionType = missionType
        Trigger DoCreateGame
```

The `startupFlags` field packs `maxPlayers` into bits [6..8] and `missionType` into bits [0..5]. This packed flags value rides along in `GameStartInfo` to all clients.

## OPU enhancements (per CHANGES.md)

The OPU patcher adds:
- **NetHelper.dll** - UPnP/NAT-PMP port forwarding (auto-opens 47800-47807)
- **Multi-adapter binding** - vanilla bound to one adapter only, OPU binds to all interfaces
- **`@X,Y` map ping** - adds a new chat-message-parsed coordinate-ping system on top of `ctChat`
- **"Don't halve speed in multiplayer"** - disables the per-MP-player game-speed halving
- **Disabled IPX / modem / serial** - DirectPlay paths removed from the menu

The wire format is unchanged - OPU clients can play with vanilla clients.

## Port summary

| Port | Direction | Purpose |
|---:|---|---|
| 47776 (0xBAA0) | UDP broadcast | Game discovery (LAN find-games) |
| 47777 (0xBAA1) | UDP | Network2 - alternate transport (SIGS-era?) |
| 47800-47807 (0xBAB8-0xBABF) | UDP | Game data + lockstep commands |

For a remake, opening 47800-47807 plus 47776 is sufficient for LAN play. For internet play, also support NAT traversal (UPnP / hole punching).

## Wire format cheat sheet

```c
struct Packet {
    // Generic header (14 bytes)
    u32 sourcePlayerNetID;     // sender's NetID
    u32 timestamp;             // tick or wall-clock
    u8  sizeOfPayload;
    u8  type;                  // 1=control, 12=gameplay, etc.
    u32 checksum;              // sum of DWORDs
    
    // Payload
    u8  data[sizeOfPayload];
};

// Common payload shapes:
struct GamePlayPayload {
    u8  commandType;           // 1..6 for control
    u8  ...;
    // command-specific fields
};

struct CommandPacketsPayload {     // for type=12 packets
    CommandPacket[];               // one or more game commands
};
```

Checksum is sum-of-DWORDs, calculated over the entire packet excluding the checksum field itself. Receiver computes and compares; mismatch → discard packet.

## Remake-relevant takeaways

1. **Use Winsock UDP directly** (or modern equivalent) - no need for the legacy DirectPlay paths.
2. **Adopt the 3-layer architecture**: raw UDP / reliable-delivery / per-tick command exchange. Simplifies reasoning.
3. **Don't bind to a single interface** - bind to `0.0.0.0` like OPU does. Vanilla had a multi-NIC bug.
4. **Keep the 16-retry limit** for player-list replication - it's the natural "give up" threshold.
5. **The 800ms reliability timeout** is the lockstep tolerance - exceeding it triggers the resync state machine.
6. **The 191-byte GameStartInfo** is the canonical "session setup" payload. Document this struct fully if porting save/replay compatibility.
7. **Player-drop detection** requires both send-failure AND receive-silence to exceed the timeout - this avoids false-positive drops when the network is briefly congested.
8. **NetIDs are session-unique, with low 3 bits = playerNum** - preserve this if you want bit-compatible cmdlogs.
