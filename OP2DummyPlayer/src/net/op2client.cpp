// op2client.cpp - the threaded OP2 multiplayer client. The protocol logic here is a direct
// port of the verified op2session.cpp (see ..\..\..\op2session\FINDINGS.md for the why of
// every step), restructured behind a thread-safe state snapshot for a GUI front-end.
#include "op2client.h"
#include "socket_compat.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#ifdef _WIN32
  #include <windows.h>
#endif

namespace op2 {

// ---- small utilities -------------------------------------------------------
static uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- debug log -------------------------------------------------------------
// Directory containing the running executable (with trailing separator), so the log lands
// next to the exe regardless of the working directory.
static std::string exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p(buf, (size_t)n);
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "" : p.substr(0, s + 1);
#else
    char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    std::string p(buf, (size_t)n);
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s + 1);
#endif
}

static std::string log_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

void debug_log(const std::string& line) {
    static std::mutex logMtx;
    static std::string path;
    std::lock_guard<std::mutex> lk(logMtx);
    if (path.empty()) path = exe_dir() + "op2dummyplayer_debug.txt";
    std::ofstream f(path, std::ios::app);
    if (f) f << log_stamp() << "  " << line << "\n";
}

// Local wall-clock time as "[hh:mm]" for chat timestamps.
static std::string hhmm() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[8]; snprintf(b, sizeof(b), "[%02d:%02d]", tmv.tm_hour, tmv.tm_min);
    return b;
}

// Local IPv4 (network order), the OP2 way: gethostname -> gethostbyname[0], skip loopback.
// Binding the game socket to THIS ip (not INADDR_ANY) is required so that on the same machine
// as the host our port scan skips the host's 47800 and our replies aren't stolen.
static uint32_t get_local_ip() {
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) return 0;
    struct hostent* he = gethostbyname(host);
    if (!he) return 0;
    for (int i = 0; he->h_addr_list[i]; i++) {
        uint32_t ip; memcpy(&ip, he->h_addr_list[i], 4);
        if (((uint8_t*)&ip)[0] == 127) continue;
        return ip;
    }
    return 0;
}

static std::string ip_str(uint32_t netOrderIp) {
    in_addr a; a.s_addr = netOrderIp;
    char buf[64];
#ifdef _WIN32
    strncpy(buf, inet_ntoa(a), sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;
#else
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
#endif
    return buf;
}

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Idle:         return "Idle";
        case Phase::Discovering:  return "Discovering";
        case Phase::Joining:      return "Joining";
        case Phase::InLobby:      return "In lobby";
        case Phase::Starting:     return "Starting";
        case Phase::InGame:       return "In game";
        case Phase::Disconnected: return "Disconnected";
        case Phase::Error:        return "Error";
    }
    return "?";
}

// ---- Client lifecycle ------------------------------------------------------
Client::Client() {}
Client::~Client() { disconnect(); }

void Client::setPlayerName(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!name.empty()) playerName_ = name.substr(0, 12);
}

bool Client::connectScan() {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();   // reap a previous self-exited worker (host ended)
    stop_ = false; leaving_ = false; sendQuit_ = false;
    { std::lock_guard<std::mutex> lk(mtx_); state_ = ClientState{}; state_.playerName = playerName_; }
    debug_log("connect: scan LAN");
    worker_ = std::thread(&Client::run, this, true, std::string());
    return true;
}

bool Client::connectIp(const std::string& ip) {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();   // reap a previous self-exited worker (host ended)
    stop_ = false; leaving_ = false; sendQuit_ = false;
    { std::lock_guard<std::mutex> lk(mtx_); state_ = ClientState{}; state_.playerName = playerName_; }
    debug_log("connect: join " + ip);
    worker_ = std::thread(&Client::run, this, false, ip);
    return true;
}

void Client::sendChat(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    pendingChat_.push_back(text);
}

void Client::disconnect() {
    if (worker_.joinable()) {
        Phase ph;
        { std::lock_guard<std::mutex> lk(mtx_); ph = state_.phase; }
        if (running_ && ph == Phase::InGame) {
            // In a running game: the worker self-sends a goodbye chat + ctQuit command turn
            // (see run()'s leave handler), then exits.
            leaving_ = true;
        } else if (running_ && ph == Phase::InLobby) {
            // In the lobby (game not started): no chat - send a real cmd-0x0B quit so the host
            // frees our slot, then exit.
            sendQuit_ = true; leaving_ = true;
        } else {
            stop_ = true;
        }
        worker_.join();
    }
    running_ = false;
    // Clean slate so the next join starts fresh (clears chat log, lobby, banner).
    std::lock_guard<std::mutex> lk(mtx_);
    state_ = ClientState{};
    pendingChat_.clear();
}

ClientState Client::snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    return state_;
}

void Client::setPhase(Phase p, const std::string& status) {
    debug_log(std::string("[phase] ") + phase_name(p) + " - " + status);
    std::lock_guard<std::mutex> lk(mtx_);
    state_.phase = p;
    state_.statusText = status;
}
void Client::addChat(const std::string& from, const std::string& text, bool self) {
    std::lock_guard<std::mutex> lk(mtx_);
    ChatLine c; c.ts = hhmm(); c.from = from; c.text = text; c.self = self;
    state_.chat.push_back(c);
    if (state_.chat.size() > 500) state_.chat.erase(state_.chat.begin());
}
void Client::setDisconnected(const std::string& reason) {
    debug_log(std::string("[disconnect] ") + reason);
    // Post a visible system line into the chat log (and we deliberately do NOT clear the chat
    // here - only a user-initiated Disconnect/rejoin clears it, so the history stays readable).
    bool wasInGame;
    { std::lock_guard<std::mutex> lk(mtx_); wasInGame = (state_.phase == Phase::InGame); }
    std::string sysline = (reason.find("ject") != std::string::npos) ? "*** EJECTED FROM LOBBY ***"
                        : wasInGame ? "*** GAME DISCONNECTED ***" : "*** DISCONNECTED ***";
    addChat("", sysline, false);
    std::lock_guard<std::mutex> lk(mtx_);
    state_.phase = Phase::Disconnected;
    state_.disconnected = true;
    state_.disconnectReason = reason;
    state_.statusText = reason;
}

// ===========================================================================
//  Worker: the full protocol flow.
// ===========================================================================
void Client::run(bool scan, std::string targetIp) {
    std::string myName;
    { std::lock_guard<std::mutex> lk(mtx_); myName = playerName_; }

    if (!net_startup()) { setDisconnected("socket init failed"); running_ = false; return; }

    uint32_t localIp = get_local_ip();
    { std::lock_guard<std::mutex> lk(mtx_); state_.localIp = localIp ? ip_str(localIp) : "0.0.0.0"; }

    // ---- 1) discovery (broadcast, or directed at a specific host) ----
    setPhase(Phase::Discovering, scan ? "Scanning LAN for a hosted game..." : ("Querying host " + targetIp + "..."));
    op2_sock_t ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ds == OP2_INVALID_SOCK) { setDisconnected("socket() failed"); net_cleanup(); running_ = false; return; }
    set_broadcast(ds, true);
    set_rcv_timeout(ds, 250);
    { sockaddr_in any{}; any.sin_family = AF_INET; any.sin_addr.s_addr = INADDR_ANY; bind(ds, (sockaddr*)&any, sizeof(any)); }

    uint8_t  sessGuid[16]; memset(sessGuid, 0, 16);
    uint32_t hostIp = 0; int maxPlayers = 0, scenType = 0; char gameName[16] = {0};
    bool found = false;

    sockaddr_in disc{}; disc.sin_family = AF_INET; disc.sin_port = htons(DISCOVERY_PORT);
    disc.sin_addr.s_addr = scan ? INADDR_BROADCAST : inet_addr(targetIp.c_str());

    uint8_t q[42]; memset(q, 0, sizeof(q));
    { sockaddr_in sn; socklen_t snl = sizeof(sn); getsockname(ds, (sockaddr*)&sn, &snl);
      q[0x04] = 0x00; q[0x05] = 0x10;                 // type 0x1000
      memcpy(q + 0x06, GAME_GUID, 16);
      wr16(q + 0x16, ntohs(sn.sin_port));             // reply to our port
      wr32(q + 0x1a, (uint32_t)now_ms());             // token
      wr32(q + 0x00, disc_cksum(q, 0x04, 9, 2)); }

    for (uint64_t start = now_ms(); now_ms() - start < 3000 && !found && !stop_; ) {
        sendto(ds, (char*)q, 42, 0, (sockaddr*)&disc, sizeof(disc));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (;;) {
            uint8_t buf[600]; sockaddr_in from; socklen_t fl = sizeof(from);
            int nr = recvfrom(ds, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (nr <= 0) break;
            if (nr != 81) continue;
            if (rd16(buf + 0x04) != 0x1001) continue;
            if (memcmp(buf + 0x0a, GAME_GUID, 16) != 0) continue;
            hostIp = from.sin_addr.s_addr;
            memcpy(sessGuid, buf + 0x1a, 16);
            uint32_t cfg = rd32(buf + 0x2a);
            maxPlayers = (int)rd32(buf + 0x3a);
            scenType   = (int)((cfg >> 9) & 0x1f);
            int k = 0; for (; k < 14 && buf[0x42 + k]; k++) gameName[k] = buf[0x42 + k]; gameName[k] = 0;
            found = true; break;
        }
    }
    close_sock(ds);
    if (stop_)  { setDisconnected("cancelled"); net_cleanup(); running_ = false; return; }
    if (!found) { setPhase(Phase::Error, "No hosted game found."); running_ = false; return; }

    debug_log("discovered host " + ip_str(hostIp) + " game '" + gameName + "' maxPlayers=" + std::to_string(maxPlayers));
    { std::lock_guard<std::mutex> lk(mtx_);
      state_.hostIp = ip_str(hostIp); state_.gameName = gameName; }

    // ---- 2) bind our game socket to the local IP, first free 47800-47807 ----
    op2_sock_t gs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gs == OP2_INVALID_SOCK) { setDisconnected("socket() failed"); net_cleanup(); running_ = false; return; }
    set_rcv_timeout(gs, 200);
    uint16_t ourPort = 0;
    for (int p = GAME_PORT_LO; p <= GAME_PORT_HI; p++) {
        sockaddr_in la{}; la.sin_family = AF_INET;
        la.sin_addr.s_addr = localIp ? localIp : INADDR_ANY; la.sin_port = htons((uint16_t)p);
        if (bind(gs, (sockaddr*)&la, sizeof(la)) == 0) { ourPort = (uint16_t)p; break; }
    }
    if (!ourPort) { setDisconnected("could not bind game port 47800-47807"); close_sock(gs); net_cleanup(); running_ = false; return; }

    // ---- 3) join handshake ----
    setPhase(Phase::Joining, "Sending join request...");
    sockaddr_in joinAddr{}; joinAddr.sin_family = AF_INET; joinAddr.sin_port = htons(JOIN_PORT); joinAddr.sin_addr.s_addr = hostIp;
    sockaddr_in gameAddr{}; gameAddr.sin_family = AF_INET; gameAddr.sin_port = htons(GAME_PORT_LO); gameAddr.sin_addr.s_addr = hostIp;

    uint8_t req[50]; memset(req, 0, 50);
    req[0x08] = 0x24; req[0x09] = 0x01;            // payloadSize 36, type 1
    wr32(req + 0x0E, 0);                            // opcode 0 = join request
    memcpy(req + 0x12, sessGuid, 16);              // SESSION guid (not the game-type guid!)
    wr32(req + 0x22, ourPort);                      // host replies to this port
    wr32(req + 0x0A, wire_cksum(req));

    bool accepted = false; uint32_t assignedNetID = 0, hostNetID = 0; sockaddr_in replyFrom{};
    for (int attempt = 0; attempt < 16 && !accepted && !stop_; attempt++) {
        sendto(gs, (char*)req, 50, 0, (sockaddr*)&joinAddr, sizeof(joinAddr));
        uint8_t buf[600]; sockaddr_in from; socklen_t fl = sizeof(from);
        int nr = recvfrom(gs, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (nr <= 0) continue;
        if (nr == 0x26 && buf[0x09] == 1 && buf[0x08] == 0x18) {
            uint32_t result = rd32(buf + 0x0E);
            hostNetID     = rd32(buf + 0x00);
            assignedNetID = rd32(buf + 0x22);
            if (result == 1) { accepted = true; replyFrom = from; }
            else if (result == 2) { setDisconnected("Session is full."); close_sock(gs); net_cleanup(); running_ = false; return; }
        }
    }
    if (!accepted) {
        setDisconnected(stop_ ? "cancelled" : "Host did not accept the join.");
        close_sock(gs); net_cleanup(); running_ = false; return;
    }
    { char nb[16]; snprintf(nb, sizeof(nb), "%08X", assignedNetID);
      debug_log(std::string("join accepted: netID=") + nb + " slot=" + std::to_string(assignedNetID & 7)); }
    { std::lock_guard<std::mutex> lk(mtx_);
      state_.assignedNetId = assignedNetID; state_.ourSlot = (int)(assignedNetID & 7); }

    // ---- packet builders (channel-A reliable unless noted) ----
    uint8_t  ackA = 0, ackB = 0, hostAckA = 0, hostAckB = 0;
    uint8_t  ourSeqA = 1;                       // host expects our first channel-A seq = 1
    uint8_t  ourPlayerNum = (uint8_t)(assignedNetID & 7);

    // PHASE-1: peer table, parsed from the cmd-4 roster (declared before the send lambdas so they
    // can broadcast in-game command turns to every peer's game address).
    struct PeerInfo { uint32_t netID; sockaddr_in addr; };
    PeerInfo peers[6]; int peerCount=0; uint32_t peerSig=0;

    auto send_status = [&](sockaddr_in dst, uint16_t status){
        uint8_t p[20]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08] = 6; p[0x09] = 1;
        wr32(p+0x0E, 6); wr16(p+0x12, status);
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,20,0,(sockaddr*)&dst,sizeof(dst));
    };
    auto send_gur_ack = [&](sockaddr_in dst){
        uint8_t p[22]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08] = 8; p[0x09] = 0;
        p[0x0E] = 0x08; p[0x10] = ackA; p[0x11] = ackB;
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,22,0,(sockaddr*)&dst,sizeof(dst));
    };
    auto send_cmd3 = [&](sockaddr_in dst){
        int nameLen = (int)myName.size();
        int payloadSize = nameLen + 0x3e, total = 14 + payloadSize;
        uint8_t p[256]; memset(p, 0, total);
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08]=(uint8_t)payloadSize; p[0x09]=0;
        p[0x0E]=0x0A; p[0x0F]=ourSeqA; p[0x10]=ackA; p[0x11]=ackB;
        p[0x12]=3; wr32(p+0x13, TAPP_VERSION);
        memcpy(p+0x17, myName.data(), nameLen);
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,total,0,(sockaddr*)&dst,sizeof(dst));
    };
    auto send_lobby_chat = [&](const std::string& msg){
        int msgLen = (int)msg.size();
        int payloadSize = msgLen + 10, total = 14 + payloadSize;
        uint8_t p[256]; memset(p, 0, total);
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08]=(uint8_t)payloadSize; p[0x09]=0;
        p[0x0E]=0x0A; p[0x0F]=ourSeqA; p[0x10]=ackA; p[0x11]=ackB;
        p[0x12]=6; wr32(p+0x13, assignedNetID);
        memcpy(p+0x17, msg.data(), msgLen);
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,total,0,(sockaddr*)&gameAddr,sizeof(gameAddr));
    };
    auto send_status_update = [&](uint32_t field, uint32_t value){
        uint8_t p[32]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08]=13; p[0x09]=0;
        p[0x0E]=0x0A; p[0x0F]=ourSeqA; p[0x10]=ackA; p[0x11]=ackB;
        p[0x12]=2; wr32(p+0x13, field); wr32(p+0x17, value);
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,14+13,0,(sockaddr*)&gameAddr,sizeof(gameAddr));
    };
    auto send_loaded = [&](){
        uint8_t p[32]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08]=5; p[0x09]=0;
        p[0x0E]=0x0A; p[0x0F]=ourSeqA; p[0x10]=ackA; p[0x11]=ackB;
        p[0x12]=9;
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,14+5,0,(sockaddr*)&gameAddr,sizeof(gameAddr));
    };
    // cmd-0x0B quit (FUN_004615D0): leave the lobby so the host frees our slot. Channel-A
    // reliable, engine cmd 0x0B at 0x12, no data (payloadSize 5).
    auto send_quit = [&](){
        uint8_t p[32]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08]=5; p[0x09]=0;
        p[0x0E]=0x0A; p[0x0F]=++ourSeqA; p[0x10]=ackA; p[0x11]=ackB;
        p[0x12]=0x0B;
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,14+5,0,(sockaddr*)&gameAddr,sizeof(gameAddr));
    };
    auto send_cmd0c = [&](uint32_t execTick, const uint8_t* blocks, int blocksLen){
        if (blocksLen < 6) blocksLen = 6; if (blocksLen > 500) blocksLen = 500;
        int payloadSize = 10 + blocksLen, total = 14 + payloadSize;
        uint8_t p[600]; memset(p, 0, total);
        wr32(p+0x00, assignedNetID);
        p[0x08]=(uint8_t)payloadSize; p[0x09]=0;
        p[0x0E]=0x0A; p[0x0F]=++ourSeqA; p[0x10]=ackA; p[0x11]=ackB;
        p[0x12]=0x0C; p[0x13]=ourPlayerNum; wr32(p+0x14, execTick);
        memcpy(p+0x18, blocks, blocksLen);
        if (peerCount >= 2) {
            // 3+ players: gameplay is peer-to-peer and command turns are BROADCAST (dst 0) with
            // GUR flags = 0x00 - the engine's own format (observed on the wire). flags 0 means no
            // channel + no seq validation: the turn is accepted by every peer and deduped by its
            // execution mark. This is essential because our channel-A stream was only ever sent to
            // the host during the lobby, so a 3rd player would reject channel-A turns as
            // out-of-sequence. Send the same turn to every peer's game address (gate accepts
            // dest == theirNetID || 0).
            p[0x0E] = 0x00;
            wr32(p+0x04, 0);
            wr32(p+0x0A, wire_cksum(p));
            for (int i = 0; i < peerCount; i++)
                sendto(gs,(char*)p,total,0,(sockaddr*)&peers[i].addr,sizeof(peers[i].addr));
        } else {
            // 2 players: the only peer is the host (the original, proven path).
            wr32(p+0x04, hostNetID);
            wr32(p+0x0A, wire_cksum(p));
            sendto(gs,(char*)p,total,0,(sockaddr*)&gameAddr,sizeof(gameAddr));
        }
    };

    // progress the host into the lobby/GUR phase, then announce ourselves (cmd-3)
    send_status(replyFrom, 2);
    send_status(gameAddr, 2);
    send_cmd3(gameAddr);

    // ---- lobby/game state machine ----
    bool lobbyJoined=false, gameStarting=false, gameLive=false;
    bool loadedSent=false;
    LobbyInfo prevLobby;
    // in-game tick + chat tracking (see FINDINGS.md #13-15)
    bool haveFirstExec=false, haveLastExec=false;
    int  firstExec=0, maxExec=0, lookahead=0, gameTick=0, cppi=4, lastExec=0, maxSentMark=-1000000;
    uint64_t gameStartMs=0;
    bool chatActive=false; int chatMark=0; std::string chatText, lastRecvChat, lastLobbyChat;
    // ally-back: if the host allies with us (command block type 0x32), ally back at a fresh mark
    bool allyActive=false; int allyMark=0; std::string lastAllyData; uint8_t allyData[16]; int allyDataLen=0;
    // clean in-game leave: when leaving, self-initiate a cmd-0x0C carrying a goodbye chat (0x30)
    // and a ctQuit (0x31) block at fresh marks - independent of incoming host turns, because on a
    // one-machine setup the host stops sending turns the instant it loses focus (when you click
    // the bot). Self-sending makes the host remove us cleanly instead of stalling until our drop
    // timeout (~800 ms) and showing the "lost contact" dialog.
    bool quitInGameSent=false; uint64_t quitDoneMs=0;
    bool playerLeft[6] = {false,false,false,false,false,false};   // announced "<name> has left" once per slot
    std::string lastChatBySlot[6];                                // de-dup in-game chat per sender slot
    long cmd0cSent=0, cmd0cRecv=0;
    bool glhfSent=false, quitSent=false;  // auto-greeting / lobby-quit one-shots
    uint64_t lastKeep=0, lastRecv=now_ms(), lastCmdTurnMs=0, leaveStartMs=0;
    uint8_t leaveBuf[200]; int leaveBufLen=0, leaveExec=0;   // pre-built goodbye+ctQuit leave turn

    while (!stop_) {
        uint8_t buf[1400]; sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(gs, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n > 0) {
            lastRecv = now_ms();
            // GUR ack/seq tracking from host type-0 packets
            if (n >= 0x12 && buf[0x09] == 0) {
                uint8_t flags = buf[0x0E], seq = buf[0x0F];
                if (flags & 0x02) ackA = seq;
                if (flags & 0x04) ackB = seq;
                if (flags & 0x20) ackB = buf[0x12];
                hostAckA = buf[0x10]; hostAckB = buf[0x11];
            }
            // GameStartInfo (engine cmd 1): decode + publish lobby changes
            if (buf[0x09]==0 && buf[0x08]==0xBF && n>=0xCD && buf[0x12]==1) {
                LobbyInfo cur = parse_gsi(buf, n);
                if (cur.valid) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    state_.lobby = cur;
                    int s = (int)(assignedNetID & 7);
                    if (s >= 0 && s < 6) { state_.ourColor = cur.slot[s].color; state_.ourReady = cur.slot[s].ready; }
                    prevLobby = cur;
                }
            }
            // Incoming LOBBY chat (engine cmd 6, type 0, channel-A, relayed by the host).
            // Layout: cmd 6 @0x12, sender NetID @0x13, text @0x17. Skip our own echo; de-dup
            // the host's reliable resends by last text.
            if (buf[0x09]==0 && n>=0x18 && buf[0x12]==6 && (buf[0x0E]&0x06) && rd32(buf+0x00)==hostNetID) {
                uint32_t sender = rd32(buf+0x13);
                const char* txt = (const char*)(buf+0x17);
                if (sender != assignedNetID && lastLobbyChat != txt) {
                    lastLobbyChat = txt;
                    std::string from;
                    { std::lock_guard<std::mutex> lk(mtx_);
                      for (int i=0;i<6;i++) if (state_.lobby.slot[i].occupied && state_.lobby.slot[i].netId==sender) { from = state_.lobby.slot[i].name; break; }
                      if (from.empty()) from = state_.gameName.empty() ? "host" : state_.gameName; }
                    addChat(from, txt, false);
                }
            }
            // START-time roster control (type-1): cmd-4 -> status(3), cmd-5 -> status(4)
            if (buf[0x09]==1 && n>=0x12) {
                uint32_t cc = rd32(buf+0x0E);
                if (cc==4) {
                    send_status(gameAddr, 3);
                    // Parse the SetPlayerList roster into our peer table - every other
                    // player's netID + real game address (bound port from the roster, NOT the
                    // ephemeral source port). Layout at payload 0x12: numPlayers(u32), then 6 slots
                    // of [IP(4, net order) | port(2, host order) | status(2) | netID(4, LE)].
                    {
                        PeerInfo np6[6]; int newCount = 0;
                        for (int s = 0; s < 6; s++) {
                            const uint8_t* e = buf + 0x16 + s*12;
                            if (e + 12 > buf + n) break;
                            uint32_t pid = rd32(e + 8);
                            if (pid == 0 || pid == assignedNetID) continue;   // skip empty + ourselves
                            PeerInfo& pe = np6[newCount];
                            pe.netID = pid;
                            memset(&pe.addr, 0, sizeof(pe.addr));
                            pe.addr.sin_family = AF_INET;
                            memcpy(&pe.addr.sin_addr, e, 4);          // IP is already network order
                            pe.addr.sin_port = htons(rd16(e + 4));    // port is stored host order
                            newCount++;
                        }
                        // adopt + log only when the set changes (cmd-4 repeats during the handshake)
                        uint32_t sig = (uint32_t)newCount;
                        for (int i = 0; i < newCount; i++) sig = sig * 131 + np6[i].netID;
                        if (sig != peerSig) {
                            peerSig = sig; peerCount = newCount;
                            std::string log = "peer table (" + std::to_string(peerCount) + "): ";
                            for (int i = 0; i < newCount; i++) {
                                peers[i] = np6[i];
                                unsigned char* ib = (unsigned char*)&peers[i].addr.sin_addr; char t[64];
                                snprintf(t, sizeof(t), "%08X@%u.%u.%u.%u:%u ", peers[i].netID,
                                         ib[0], ib[1], ib[2], ib[3], (unsigned)ntohs(peers[i].addr.sin_port));
                                log += t;
                            }
                            debug_log(log);
                        }
                    }
                }
                else if (cc==5) send_status(gameAddr, 4);
            }
            // lobby/connection EVENTS (sequenced host packet, engine cmd at 0x12)
            if (buf[0x09]==0 && buf[0x08]>=5 && n>=0x13 && (buf[0x0E]&0x06) && rd32(buf+0x00)==hostNetID) {
                uint8_t ec = buf[0x12];
                if (ec == 0x0B) {
                    send_gur_ack(gameAddr);
                    // cmd-0x0B targeted at us = eject; broadcast (dst 0) = host closed the lobby.
                    uint32_t dst = rd32(buf+0x04);
                    setDisconnected(dst == assignedNetID ? "Ejected from the lobby by the host." : "Host ended the lobby.");
                    break;
                }
                if (ec == 10)   { send_gur_ack(gameAddr); setDisconnected("Host canceled the game start."); break; }
                if ((ec==7 || ec==8) && !gameStarting) { gameStarting = true; setPhase(Phase::Starting, "Host is starting the game..."); }
                // cmd-8 load&go -> reply cmd-9 "loaded" (skip the checksum). Use the NEXT
                // channel-A seq (not a hardcoded 4 - we may have sent lobby chats already, and
                // a seq regression would break the ordered stream). Resend on each cmd-8 the
                // host repeats; ourSeqA is stable through the start phase so the seq stays put.
                if (ec == 8) { if (!loadedSent) { ++ourSeqA; loadedSent = true; } send_loaded(); }
                if (ec == 9 && loadedSent) {
                    send_gur_ack(gameAddr);
                    // Announce the start once. The host resends the cmd-9 "GO" reliably, and the
                    // cmd-0x0C handler also flips gameLive on its first turn - guard so we don't
                    // print "*** GAME STARTED ***" twice.
                    if (!gameLive) {
                        gameLive = true; gameStartMs = now_ms();
                        { std::lock_guard<std::mutex> lk(mtx_); state_.gameStarted = true; }
                        setPhase(Phase::InGame, "Game started.");
                        addChat("", "*** GAME STARTED ***", false);
                    }
                }
            }
            // IN-GAME command turn (engine cmd 0x0C): track tick, surface/inject chat, reply.
            // Skip entirely once leaving: the leave handler self-sends our final turn (goodbye +
            // ctQuit) at a pinned mark, so we must stop mirroring or maxSentMark would advance past
            // it and the host would reject the quit.
            if (!leaving_ && buf[0x09]==0 && n>=0x18 && buf[0x12]==0x0C && rd32(buf+0x00)==hostNetID) {
                if (!gameLive) { gameLive = true; gameStartMs = now_ms(); { std::lock_guard<std::mutex> lk(mtx_); state_.gameStarted = true; } setPhase(Phase::InGame, "Game started."); addChat("", "*** GAME STARTED ***", false); }
                cmd0cRecv++;
                int execTick = (int)rd32(buf+0x14);
                if (!haveFirstExec) { haveFirstExec=true; firstExec=execTick; lookahead=-execTick; }
                if (execTick > maxExec) maxExec = execTick;
                gameTick = maxExec - lookahead;
                if (haveLastExec) { int d = execTick - lastExec; if (d>0 && d<=64) cppi = d; }
                lastExec = execTick; haveLastExec = true;
                lastCmdTurnMs = now_ms();        // for game-end (command-turn silence) detection

                // auto-greeting: once 50 OP2 ticks have elapsed, say "Good Luck! Have Fun!"
                // (fires at tick 50 - half the old 100, i.e. 50% sooner into the game).
                if (!glhfSent && gameTick >= 50) {
                    glhfSent = true;
                    std::lock_guard<std::mutex> lk(mtx_);
                    pendingChat_.insert(pendingChat_.begin(), "Good Luck! Have Fun!");
                }

                // pop one queued chat -> pin to a fresh (never-committed) mark
                if (!chatActive) {
                    std::string msg;
                    { std::lock_guard<std::mutex> lk(mtx_); if (!pendingChat_.empty()) { msg = pendingChat_.front(); pendingChat_.erase(pendingChat_.begin()); } }
                    if (!msg.empty()) { chatActive = true; chatMark = maxSentMark + cppi; chatText = msg; addChat(myName, msg, true); }
                }
                if (chatActive && execTick > chatMark) chatActive = false;
                if (allyActive && execTick > allyMark) allyActive = false;

                uint8_t ob[400]; int ol=0; bool injectedChat=false, injectedAlly=false; int off=0x18, idx=0;
                while (off + 6 <= n) {
                    uint8_t blen = buf[off+1];
                    if (off + 6 + (int)blen > n) break;
                    uint32_t unk = rd32(buf+off+2);
                    int blockMark = execTick + idx*cppi;
                    // incoming host chat block (type 0x30): data = [sender, mask, text NUL]
                    if (buf[off]==0x30 && blen>=3) {
                        const char* txt = (const char*)(buf + off + 6 + 2);
                        if (lastRecvChat != txt) { lastRecvChat = txt; std::string gn; { std::lock_guard<std::mutex> lk(mtx_); gn = state_.gameName; } addChat(gn.empty()?"host":gn, txt, false); }
                    }
                    // incoming host ALLY block (command type 0x32, `AllyWith`/FUN_0040E300). In a
                    // 2-player game any ally from the host targets us -> ally back. The 4-byte data
                    // is two u16 player fields; our perspective is the fields swapped. (Bytes are
                    // logged so the exact layout can be confirmed against a live ally.)
                    if (buf[off]==0x32 && blen>=4 && blen<=12) {
                        std::string d((const char*)(buf+off+6), blen);
                        if (lastAllyData != d) {
                            lastAllyData = d;
                            char hex[40]={0}; for (int z=0; z<blen && z<12; z++) snprintf(hex+z*3, 4, "%02x ", buf[off+6+z]);
                            debug_log(std::string("incoming ALLY (0x32) data: ") + hex);
                            allyDataLen = blen; memcpy(allyData, buf+off+6, blen);
                            uint16_t f0 = rd16(buf+off+6), f1 = rd16(buf+off+8);   // swap the two player fields
                            wr16(allyData, f1); wr16(allyData+2, f0);
                            allyActive = true; allyMark = maxSentMark + cppi;
                            addChat("", "*** A PLAYER ALLIED WITH US - allying back ***", false);
                        }
                    }
                    if (ol + 64 <= (int)sizeof(ob)) {
                        if (chatActive && !injectedChat && blockMark==chatMark && unk!=0) {
                            int tl=(int)chatText.size(), dl=2+tl+1;
                            ob[ol]=0x30; ob[ol+1]=(uint8_t)dl; wr32(ob+ol+2, unk);
                            ob[ol+6]=ourPlayerNum; ob[ol+7]=0xFF; memcpy(ob+ol+8, chatText.data(), tl); ob[ol+8+tl]=0;
                            ol += 6+dl; injectedChat=true;
                        } else if (allyActive && !injectedAlly && blockMark==allyMark && unk!=0) {
                            ob[ol]=0x32; ob[ol+1]=(uint8_t)allyDataLen; wr32(ob+ol+2, unk);
                            memcpy(ob+ol+6, allyData, allyDataLen);
                            ol += 6+allyDataLen; injectedAlly=true;
                        } else { ob[ol]=0; ob[ol+1]=0; wr32(ob+ol+2, unk); ol += 6; }
                    }
                    off += 6 + blen; idx++;
                }
                if (idx > 0) { int last = execTick + (idx-1)*cppi; if (last > maxSentMark) maxSentMark = last; }
                if (ol < 6) { ob[0]=0; ob[1]=0; wr32(ob+2, 0xBABE3624u); ol = 6; }
                send_cmd0c((uint32_t)execTick, ob, ol);
                cmd0cSent++;
                if (injectedAlly) {
                    char hex[40]={0}; for (int z=0; z<allyDataLen && z<12; z++) snprintf(hex+z*3, 4, "%02x ", allyData[z]);
                    debug_log(std::string("sent ally-back (0x32) data: ") + hex + " at mark " + std::to_string(allyMark));
                }

                std::lock_guard<std::mutex> lk(mtx_);
                state_.gameTick = gameTick;
                state_.gameMark = gameTick / 100;     // OP2 game-time mark (matches the engine)
                state_.gameDurationSec = gameStartMs ? (now_ms()-gameStartMs)/1000.0 : 0.0;
                state_.cmdSent = cmd0cSent; state_.cmdRecv = cmd0cRecv;
            }
            // Every OTHER player's command turn (peer or host): surface their in-game chat (block
            // 0x30) and detect them leaving (ctQuit 0x31). In-game chat is peer-to-peer - each
            // player's chat rides in THEIR OWN turn (turn playerNum at wire 0x13 = sender slot), so
            // the host-only mirror above never sees a peer's messages. This pass is read-only (it
            // does not mirror) and walks any non-self turn. (The host's own chat is still shown by
            // the mirror handler, so 0x30 here is gated to non-host sources to avoid duplicates.)
            if (buf[0x09]==0 && n>=0x18 && buf[0x12]==0x0C) {
                uint32_t lsrc = rd32(buf+0x00);
                int lslot = buf[0x13];
                if (lsrc != assignedNetID && lslot >= 0 && lslot < 6 && lslot != (int)ourPlayerNum) {
                    std::string nm;
                    { std::lock_guard<std::mutex> lk(mtx_);
                      if (state_.lobby.slot[lslot].occupied && !state_.lobby.slot[lslot].name.empty())
                          nm = state_.lobby.slot[lslot].name; }
                    if (nm.empty()) nm = "Player " + std::to_string(lslot);
                    for (int o = 0x18; o + 6 <= n; ) {
                        uint8_t bt = buf[o], bl = buf[o+1];
                        if (o + 6 + bl > n) break;
                        if (bt == 0x30 && bl >= 3 && lsrc != hostNetID) {
                            // in-game chat: data = [sender(1), recipientMask(1), text, NUL].
                            const char* txt = (const char*)(buf + o + 6 + 2);
                            if (lastChatBySlot[lslot] != txt) { lastChatBySlot[lslot] = txt; addChat(nm, txt, false); }
                        } else if (bt == 0x31 && !playerLeft[lslot]) {   // ctQuit - this player left
                            playerLeft[lslot] = true;
                            addChat("", std::string("*** ") + nm + " has left the game ***", false);
                            debug_log("peer left: slot " + std::to_string(lslot) + " (" + nm + ") sent ctQuit (0x31)");
                        }
                        o += 6 + bl;
                    }
                }
            }
            // latch lobby join + auto ready-up
            if (!lobbyJoined && hostAckA >= ourSeqA) {
                lobbyJoined = true;
                setPhase(Phase::InLobby, "In the lobby.");
                ourSeqA = 2;
                for (int r=0;r<3;r++) send_status_update(4, 1);   // auto-ready (field 4 = Ready)
                ++ourSeqA;                                         // auto-greeting (lobby chat)
                for (int r=0;r<3;r++) send_lobby_chat("Hello from OP2DummyPlayer!");
                addChat(myName, "Hello from OP2DummyPlayer!", true);
            }
            // lobby chat from the GUI (cmd-6), once joined and before the game starts
            if (lobbyJoined && !gameStarting) {
                std::string msg;
                { std::lock_guard<std::mutex> lk(mtx_); if (!pendingChat_.empty()) { msg = pendingChat_.front(); pendingChat_.erase(pendingChat_.begin()); } }
                if (!msg.empty()) { ++ourSeqA; for (int r=0;r<3;r++) send_lobby_chat(msg); addChat(myName, msg, true); }
            }
            send_gur_ack(gameAddr);
            if (!lobbyJoined) send_cmd3(gameAddr);     // resend cmd-3 until acked
        }
        // periodic keepalive (acks) while joined
        if (now_ms() - lastKeep > 200) {
            if (!lobbyJoined) { send_status(replyFrom, 2); send_status(gameAddr, 2); send_cmd3(gameAddr); }
            send_gur_ack(gameAddr);
            lastKeep = now_ms();
        }
        // graceful leave: deliver the goodbye/quit, then exit.
        // Lobby: a real cmd-0x0B quit (host frees our slot), which acks fast.
        // In-game: SELF-SEND a command turn (cmd-0x0C) carrying a goodbye chat block (0x30) at a
        // fresh mark and a ctQuit block (0x31) at the next mark. This does not wait for an incoming
        // host turn - on a one-machine setup the host stops sending turns the moment it loses focus
        // (when you click the bot), so a turn-driven quit would never get out. We build the turn
        // once (pinning the marks + synthetic non-zero unk tokens, the same way our filler turns
        // use 0xBABE3624), then resend it each loop tick for ~1.2s so it survives loss / a paused
        // host's socket buffer, then exit.
        if (leaving_) {
            if (leaveStartMs == 0) leaveStartMs = now_ms();
            if (sendQuit_) {
                if (!quitSent) { for (int r=0;r<3;r++) send_quit(); quitSent = true; debug_log("sent lobby quit (cmd-0x0B)"); }
                if (now_ms() - leaveStartMs > 700) break;
            } else if (gameLive) {
                static const char* GOODBYE = "I am leaving the game.";
                if (leaveBufLen == 0) {
                    int baseMark = maxSentMark + cppi; if (baseMark < cppi) baseMark = cppi;
                    leaveExec = baseMark;
                    int ol = 0, tl = (int)strlen(GOODBYE), dl = 2 + tl + 1;
                    // block 0: in-game chat "I am leaving the game." at baseMark
                    leaveBuf[ol]=0x30; leaveBuf[ol+1]=(uint8_t)dl; wr32(leaveBuf+ol+2, 0xBABE3624u);
                    leaveBuf[ol+6]=ourPlayerNum; leaveBuf[ol+7]=0xFF; memcpy(leaveBuf+ol+8, GOODBYE, tl); leaveBuf[ol+8+tl]=0;
                    ol += 6 + dl;
                    // block 1: ctQuit (no data - the quitting player is the turn's playerNum) at baseMark+cppi
                    leaveBuf[ol]=0x31; leaveBuf[ol+1]=0; wr32(leaveBuf+ol+2, 0xBABE3625u); ol += 6;
                    leaveBufLen = ol; maxSentMark = baseMark + cppi;
                    addChat(myName, GOODBYE, true);
                    debug_log("self-send leave: goodbye(0x30)+ctQuit(0x31) at marks " + std::to_string(baseMark) + "," + std::to_string(baseMark+cppi));
                    quitInGameSent = true; quitDoneMs = now_ms();
                }
                send_cmd0c((uint32_t)leaveExec, leaveBuf, leaveBufLen);
                if (now_ms() - quitDoneMs > 1200) break;
            } else {
                if (now_ms() - leaveStartMs > 1000) break;   // not in-game yet - just exit
            }
        }
        // refresh duration even when no in-game packet arrived this tick
        if (gameLive) { std::lock_guard<std::mutex> lk(mtx_); state_.gameDurationSec = (now_ms()-gameStartMs)/1000.0; }
        // GAME ENDED: once command turns have been flowing, the host streams them continuously
        // (~30/s). A multi-second gap means the host ended/left the match. (Set only after the
        // first turn, so it never trips during the host's pre-game load pause.)
        if (!leaving_ && lastCmdTurnMs && now_ms() - lastCmdTurnMs > 6000) {
            setDisconnected("Game ended (host left or the match is over).");
            break;
        }
        // host-silence disconnect (longer grace once starting/live: host pauses to load).
        // In the lobby, the host going silent on a well-behaved client is almost always an
        // EJECT (the host removes us and stops sending) - report it as such.
        uint64_t silence = gameLive ? 30000u : (gameStarting ? 20000u : 5000u);
        if (lobbyJoined && now_ms() - lastRecv > silence) {
            if (gameLive || gameStarting) setDisconnected("Host stopped responding (left or crashed).");
            else setDisconnected("Ejected from the lobby (host stopped responding).");
            break;
        }
    }

    if (stop_ && !snapshot().disconnected) setDisconnected("Disconnected.");
    close_sock(gs);
    net_cleanup();
    running_ = false;
}

} // namespace op2
