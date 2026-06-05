// op2client.h - the stateful Outpost 2 multiplayer client, on its own thread.
//
// This is the entire net-client surface the GUI (or any front-end) needs. It owns a worker
// thread that runs the full protocol flow (discover -> join -> lobby -> start -> in-game),
// ported from the verified op2session.cpp. The GUI never touches sockets or wire bytes: it
// calls a few commands and reads an immutable ClientState snapshot each frame.
//
// Threading contract:
//   - All public methods are safe to call from the GUI thread.
//   - snapshot() returns a by-value copy taken under a mutex (never returns internal refs).
//   - The worker thread only mutates state under the same mutex.
#pragma once

#include "op2proto.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

namespace op2 {

enum class Phase {
    Idle,          // not started
    Discovering,   // looking for a host
    Joining,       // join handshake in progress
    InLobby,       // visible in the GAME SETTINGS lobby
    Starting,      // host pressed START; roster + load handshake
    InGame,        // mission running; exchanging command turns
    Disconnected,  // cleanly left / kicked / host gone
    Error          // could not connect
};

const char* phase_name(Phase p);

// Append a timestamped line to the debug log (op2dummyplayer_debug.txt, next to the exe).
// Thread-safe; opens/closes per call so the file is always flushed even on a crash.
void debug_log(const std::string& line);

struct ChatLine {
    std::string ts;     // "[hh:mm]" local time the line was logged
    std::string from;   // sender display name ("" for system lines)
    std::string text;
    bool        self = false;   // true if we sent it
};

// A flat, copyable view of everything the GUI displays. Snapshotted under a mutex.
struct ClientState {
    Phase       phase = Phase::Idle;
    std::string statusText;          // short human-readable status / last event

    // identity (known before/at join)
    std::string playerName;
    std::string localIp;

    // host / session (known once discovered/joined)
    std::string hostIp;
    std::string gameName;            // creator/game name from discovery
    uint32_t    assignedNetId = 0;
    int         ourSlot = -1;

    // lobby (valid once InLobby)
    LobbyInfo   lobby;
    int         ourColor = -1;       // convenience: lobby.slot[ourSlot].color
    bool        ourReady = false;

    // in-game (valid once InGame)
    bool        gameStarted = false;
    int         gameTick = 0;        // OP2 tick currently executing (from the command stream)
    int         gameMark = 0;        // OP2 game-time mark (= gameTick / 100)
    double      gameDurationSec = 0; // wall-clock since GO
    long        cmdSent = 0;         // in-game command turns (0x0C) we have sent
    long        cmdRecv = 0;         // in-game command turns we have received from the host

    // chat (lobby + in-game, unified log)
    std::vector<ChatLine> chat;

    // disconnect
    bool        disconnected = false;
    std::string disconnectReason;
};

class Client {
public:
    Client();
    ~Client();

    // Configure the player name shown in the lobby (call before connecting). Default "OPU".
    void setPlayerName(const std::string& name);

    // Start a session. Either scan the LAN by broadcast, or target a specific host IP.
    // Returns false if a session is already running.
    bool connectScan();
    bool connectIp(const std::string& ip);

    // Queue a chat message. Routed automatically: lobby chat while InLobby, in-game chat
    // (command block 0x30) while InGame. Ignored in other phases.
    void sendChat(const std::string& text);

    // Stop the worker and leave the game/lobby. Safe to call repeatedly.
    void disconnect();

    bool running() const { return running_.load(); }

    // Thread-safe immutable snapshot for the GUI to render.
    ClientState snapshot();

private:
    void run(bool scan, std::string targetIp);   // worker entry

    std::thread             worker_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       stop_{false};
    std::atomic<bool>       leaving_{false};   // graceful leave: send goodbye/quit, then exit
    std::atomic<bool>       sendQuit_{false};  // leaving from the lobby: send a real cmd-0x0B quit

    std::mutex              mtx_;
    ClientState             state_;               // guarded by mtx_
    std::vector<std::string> pendingChat_;        // guarded by mtx_
    std::string             playerName_ = "OPU";  // guarded by mtx_ (set before start)

    // helpers (run on worker thread; lock as noted)
    void setPhase(Phase p, const std::string& status);
    void addChat(const std::string& from, const std::string& text, bool self);
    void setDisconnected(const std::string& reason);
};

} // namespace op2
