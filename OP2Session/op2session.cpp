// op2session.exe - experimental Outpost 2 (retail 1.3.6) multiplayer client.
//
// Speaks OP2's own network protocol from scratch (no game engine) to join and PLAY a
// hosted multiplayer game. Full pipeline, all verified live against a retail 1.3.6 host:
//
//   discover (:47776) -> join handshake (:47777) -> GUR reliable transport (:47800)
//   -> cmd-3 ShowJoinGame (appear in the lobby as a named player)
//   -> cmd-6 lobby chat -> cmd-2 ready-up
//   -> cmd-4/5 roster handshake (reply cmd-6 status 3/4)
//   -> cmd-8 load&go -> cmd-9 "loaded" (the file checksum is a client-side self-check we
//      skip) -> host cmd-9 GO (the mission launches; we are a live player)
//   -> cmd-0x0C in-game command turns: reply a ctNop per mark so the lockstep sim advances,
//      while tracking OP2's own tick off the wire and timing the match (wall clock)
//   -> in-game chat (cmd-0x0C command block type 0x30): receive host messages AND send our
//      own, pinned to a fresh mark (sender + recipient-mask + text)
//
// Wire packet = 14-byte header [srcNetID(4) dstNetID(4) payloadSize(1) type(1) checksum(4)]
// + payload. type==1 = transport control; type==0 = the GUR reliable layer (4-byte
// sub-header at 0x0E: flags, seq, ackA, ackB) with the engine command byte at 0x12.
// Heavily diagnostic - this began as an iteration harness, so it dumps/decodes everything.
//
// Protocol spec: PROTOCOL.md (byte-exact) and FINDINGS.md (the RE journey + gotchas),
// both alongside this file. Key decompile anchors (RE'd from D:\op2_136_DECOMPILE):
//   wire checksum FUN_00490F10   join client FUN_004965A0   recv gate  FUN_004974A0
//   status sender FUN_00497190   roster ctrl FUN_00497240   host start FUN_00461700
//   client start  FUN_00462050   cmd-turn TX FUN_00420F30   cmd-turn RX FUN_00420E00
//   chat issuer   FUN_004116ED   chat display FUN_0040E300 (case 0x30) / FUN_00439070
//
// Build: build.bat   Run: op2session.exe [hostIP]   (hostIP optional; else auto-discover)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#pragma comment(lib, "ws2_32.lib")

// Verbose logging toggle. When on, every packet is dumped (hex + decoded header) - useful for
// debugging but very spammy. When off, only the high-level milestones print (clean for a
// screenshot). Controlled by op2session.ini next to the exe:  [log] verbose=0 or 1.
static bool g_verbose = true;

static void load_config() {
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    for (int i = (int)n - 1; i >= 0; --i)       // strip the exe filename
        if (path[i] == '\\' || path[i] == '/') { path[i+1] = 0; break; }
    strcat(path, "op2session.ini");
    g_verbose = GetPrivateProfileIntA("log", "verbose", g_verbose ? 1 : 0, path) != 0;
}

static const uint32_t XORK = 0xFDE24ACB;
static const int DISCOVERY_PORT = 47776;   // 0xBAA0
static const int JOIN_PORT      = 47777;   // 0xBAA1
static const int GAME_PORT_LO   = 47800;   // 0xBAB8
static const int GAME_PORT_HI   = 47807;   // 0xBABF

// OP2 game-type GUID (constant), raw 16 bytes.
static const uint8_t GAME_GUID[16] = {
    0x11,0xCF,0x55,0x5A, 0x41,0xB8, 0xCE,0x11,
    0x92,0x10,0x00,0xAA,0x00,0x6C,0x49,0x72
};

// Local IPv4 (network order), the way OP2 picks it: gethostname -> gethostbyname[0],
// skipping loopback. Binding the game socket to THIS ip (not INADDR_ANY) is required so
// that, on the same machine as the host, our port scan skips the host's 47800 and our
// replies aren't stolen by the host's more-specific binding.
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

static uint32_t rd32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static void wr32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void wr16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

static void hexdump(const uint8_t* d, int n) {
    if (!g_verbose) return;
    for (int i = 0; i < n; i += 16) {
        printf("    %04x  ", i);
        for (int j = 0; j < 16; j++) {
            if (i+j < n) printf("%02x ", d[i+j]); else printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (int j = 0; j < 16 && i+j < n; j++) {
            uint8_t c = d[i+j]; printf("%c", (c>=32&&c<127)?c:'.');
        }
        printf("|\n");
    }
}

// ---- checksums -------------------------------------------------------------

// Discovery checksum (FUN_00490EB0): sum ndw LE dwords from buf[start] + tail, XOR.
static uint32_t disc_cksum(const uint8_t* buf, int start, int ndw, int tail) {
    uint32_t s = 0; int off = start;
    for (int i = 0; i < ndw; i++) { s += rd32(buf+off); off += 4; }
    if (tail == 2) s += rd16(buf+off);
    else if (tail == 1) s += buf[off];
    return s ^ XORK;
}

// Wire-packet checksum (FUN_00490F10). p = full packet (14-byte header + payload).
static uint32_t wire_cksum(const uint8_t* p) {
    uint8_t  size = p[0x08];
    uint16_t sizeType = rd16(p + 0x08);                 // payloadSize | (type<<8)
    uint32_t acc = (rd32(p+0x00) + rd32(p+0x04) + sizeType) ^ XORK;
    uint32_t psum = 0;
    int ndw = size >> 2;
    for (int i = 0; i < ndw; i++) psum += rd32(p + 0x0E + i*4);
    int tail = size & 3;
    if (tail) {
        uint32_t t = 0;
        memcpy(&t, p + 0x0E + ndw*4, tail);
        psum = (psum + t) ^ XORK;
    }
    return acc + psum;
}

// ---- discovery (get host IP + session GUID) --------------------------------

struct Game {
    uint32_t hostIp;       // network order
    uint8_t  gameGuid[16];
    uint8_t  sessionGuid[16];
    int      maxPlayers;
    int      scenType;     // missionType = (cfg>>9)&0x1f
    char     name[16];
    bool     found;
};

// ---- lobby data names ------------------------------------------------------
static const char* scen_name(int c){
    switch(c){case 24:return"Last One Standing";case 25:return"Midas";case 26:return"Resource Race";
              case 27:return"Space Race";case 28:return"Land Rush";} static char b[16]; sprintf(b,"Type %d",c); return b;
}
static const char* color_name(int c){
    static const char* n[]={"Blue","Red","Green","Yellow","Cyan","Magenta"};
    return (c>=0&&c<6)?n[c]:"?";
}
static const char* res_name(int r){ static const char* n[]={"Low","Med","High"}; return (r>=0&&r<3)?n[r]:"?"; }

static Game discover(SOCKET s) {
    Game g; memset(&g, 0, sizeof(g));
    uint8_t q[42]; memset(q, 0, sizeof(q));
    uint32_t token = GetTickCount();
    // bound port of s -> reply port at [0x16]
    sockaddr_in sn; int snl = sizeof(sn);
    getsockname(s, (sockaddr*)&sn, &snl);
    q[0x04] = 0x00; q[0x05] = 0x10;                 // type 0x1000
    memcpy(q + 0x06, GAME_GUID, 16);                // GUID at +0x06
    wr16(q + 0x16, ntohs(sn.sin_port));             // reply to our port
    wr32(q + 0x1a, token);                          // token
    wr32(q + 0x00, disc_cksum(q, 0x04, 9, 2));      // checksum

    sockaddr_in bc; memset(&bc, 0, sizeof(bc));
    bc.sin_family = AF_INET; bc.sin_port = htons(DISCOVERY_PORT);
    bc.sin_addr.s_addr = INADDR_BROADCAST;

    DWORD start = GetTickCount();
    while (GetTickCount() - start < 2000 && !g.found) {
        sendto(s, (char*)q, 42, 0, (sockaddr*)&bc, sizeof(bc));
        Sleep(150);
        for (;;) {
            uint8_t buf[600]; sockaddr_in from; int fl = sizeof(from);
            int n = recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n <= 0) break;
            if (n != 81) continue;
            if (rd16(buf+0x04) != 0x1001) continue;
            if (memcmp(buf+0x0a, GAME_GUID, 16) != 0) continue;
            g.hostIp = from.sin_addr.s_addr;
            memcpy(g.gameGuid, buf+0x0a, 16);
            memcpy(g.sessionGuid, buf+0x1a, 16);
            uint32_t cfg = rd32(buf+0x2a);
            g.maxPlayers = (int)rd32(buf+0x3a);
            g.scenType = (int)((cfg >> 9) & 0x1f);
            int k=0; for (; k<14 && buf[0x42+k]; k++) g.name[k]=buf[0x42+k]; g.name[k]=0;
            g.found = true;
            break;
        }
    }
    return g;
}

// ---- join handshake (port 47777) -------------------------------------------

// Build the 50-byte join request (14 hdr + 36 payload).
static int build_join(uint8_t* p, const uint8_t guid[16], uint16_t ourGamePort, const char* password) {
    memset(p, 0, 50);
    // header: src/dest NetID = 0 (not assigned yet; host ignores but checksum covers them)
    p[0x08] = 0x24;                 // payloadSize = 36
    p[0x09] = 0x01;                 // type = 1 (ProtocolControl)
    // payload @0x0E:
    wr32(p + 0x0E, 0);              // opcode = 0 (join request)
    memcpy(p + 0x12, guid, 16);     // session/game GUID
    wr32(p + 0x22, ourGamePort);    // our bound game port (host replies here)
    if (password && *password) {    // 11 bytes, uppercased
        char up[12]; memset(up, 0, sizeof(up));
        for (int i=0;i<11 && password[i];i++) up[i]=(char)toupper((unsigned char)password[i]);
        memcpy(p + 0x26, up, 11);
    }
    wr32(p + 0x0A, wire_cksum(p));  // checksum
    return 50;
}

// Transport status enum (roster[slot].status @ host 0x826a + slot*0xc). The roster/start
// handshake walks us up these: 2=Normal (in lobby) -> 3=Replicated (answer to cmd-4) ->
// 4=Finalized (answer to cmd-5). FUN_00496950/FUN_00496dc0 wait for these exact values.
static const char* status_name(int s){
    switch(s){case 0:return"Empty";case 1:return"Joining";case 2:return"Normal";
              case 3:return"Replicated";case 4:return"Finalized";default:return"?";}
}

static void decode_packet(const uint8_t* d, int n, const char* srcdesc) {
    if (n < 14) { printf("  [%s] runt %d bytes\n", srcdesc, n); return; }
    uint32_t src = rd32(d+0x00), dst = rd32(d+0x04);
    uint8_t  size = d[0x08], type = d[0x09];
    uint32_t ck = rd32(d+0x0A);
    bool ckok = (wire_cksum(d) == ck);
    printf("  [%s] %d bytes  src=%08X dst=%08X size=%u type=%u cksum=%s",
           srcdesc, n, src, dst, size, type, ckok?"OK":"BAD");
    if (type == 1 && n >= 0x12) {
        uint32_t cmd = rd32(d+0x0E);
        printf("  control cmd=%u", cmd);
        if (cmd == 6 && n >= 0x14) printf(" status=%u", rd16(d+0x12));
        if (cmd == 4 && n >= 0x16) printf(" numPlayers=%u", rd32(d+0x12));
    }
    printf("\n");
    hexdump(d, n);
}

// Parsed lobby state (from the 191-byte GameStartInfo). blob base = wire 0x13 (after the
// 5-byte GUR sub-header). Field map RE'd from MultiplayerPreGameSetupWnd (blob N = win 0xC8+N).
struct LobbyState {
    bool valid;
    int maxp, initVeh, speed;
    bool disasters, dayNight, morale;
    char mapdll[33];
    struct Slot { bool occ; char name[14]; int color, res, eden, ready; uint32_t netid; } slot[6];
};

static LobbyState parse_gsi(const uint8_t* p, int n) {
    LobbyState s; memset(&s, 0, sizeof(s));
    if (n < 0x13 + 0xBA) return s;
    const uint8_t* b = p + 0x13;
    uint32_t flags = rd32(b + 0x00);
    s.maxp = (flags>>6)&7; s.initVeh = (flags>>17)&0xf; s.speed = rd16(b+0xAE)>>2;
    s.disasters=(flags&1)!=0; s.dayNight=(flags&2)!=0; s.morale=(flags&4)!=0;
    memcpy(s.mapdll, b+0x10, 32);
    for (int i = 0; i < 6; i++) {
        uint32_t netid = rd32(b + 0x48 + i*4);
        s.slot[i].netid = netid; s.slot[i].occ = (netid != 0);
        uint32_t st = rd32(b + 0x30 + i*4);
        s.slot[i].color=(st>>5)&7; s.slot[i].res=(st>>3)&3; s.slot[i].eden=(st>>2)&1; s.slot[i].ready=(st>>1)&1;
        memcpy(s.slot[i].name, b + 0x60 + i*0xD, 13);
    }
    s.valid = true; return s;
}

static void print_lobby(const LobbyState& s, const char* scen) {
    printf("\n========================= LOBBY =========================\n");
    printf("  Scenario : %dP, %s, \"%s\"\n", s.maxp, scen, s.mapdll);
    printf("  Options  : Disasters=%s  Day/Night=%s  MoraleSteady=%s  InitVehicles=%d  Speed=%d\n",
           s.disasters?"ON":"off", s.dayNight?"ON":"off", s.morale?"ON":"off", s.initVeh, s.speed);
    printf("  Players  :  %-13s %-7s %-9s %-5s %-5s\n", "Name", "Color", "Race", "Res", "Ready");
    for (int i = 0; i < 6; i++) {
        if (!s.slot[i].occ) continue;
        printf("    [%d]     %-13s %-7s %-9s %-5s %-5s  netID=%08X\n",
               i, s.slot[i].name[0]?s.slot[i].name:"(unnamed)", color_name(s.slot[i].color),
               s.slot[i].eden?"Eden":"Plymouth", res_name(s.slot[i].res),
               s.slot[i].ready?"YES":"no", s.slot[i].netid);
    }
    printf("=========================================================\n");
}

static bool lobby_eq(const LobbyState& a, const LobbyState& b) {
    if (a.maxp!=b.maxp||a.initVeh!=b.initVeh||a.speed!=b.speed||a.disasters!=b.disasters||
        a.dayNight!=b.dayNight||a.morale!=b.morale||strcmp(a.mapdll,b.mapdll)) return false;
    for (int i=0;i<6;i++)
        if (a.slot[i].occ!=b.slot[i].occ||a.slot[i].color!=b.slot[i].color||a.slot[i].res!=b.slot[i].res||
            a.slot[i].eden!=b.slot[i].eden||a.slot[i].ready!=b.slot[i].ready||strcmp(a.slot[i].name,b.slot[i].name))
            return false;
    return true;
}

// Report exactly what the host changed between two lobby states.
static void diff_lobby(const LobbyState& a, const LobbyState& b) {
    printf("\n>>> HOST CHANGED A SETTING:\n");
    if (strcmp(a.mapdll,b.mapdll)||a.maxp!=b.maxp)
        printf("    Scenario: %dP \"%s\"  ->  %dP \"%s\"\n", a.maxp,a.mapdll,b.maxp,b.mapdll);
    if (a.disasters!=b.disasters) printf("    Disasters: %s -> %s\n", a.disasters?"ON":"off", b.disasters?"ON":"off");
    if (a.dayNight !=b.dayNight)  printf("    Day/Night: %s -> %s\n", a.dayNight?"ON":"off",  b.dayNight?"ON":"off");
    if (a.morale   !=b.morale)    printf("    MoraleSteady: %s -> %s\n", a.morale?"ON":"off", b.morale?"ON":"off");
    if (a.initVeh  !=b.initVeh)   printf("    Initial Vehicles: %d -> %d\n", a.initVeh, b.initVeh);
    if (a.speed    !=b.speed)     printf("    Speed: %d -> %d\n", a.speed, b.speed);
    for (int i = 0; i < 6; i++) {
        const LobbyState::Slot& oa = a.slot[i]; const LobbyState::Slot& ob = b.slot[i];
        if (!oa.occ && ob.occ) { printf("    Player[%d] JOINED: %s (%s, %s, %s, Ready=%s)\n", i,
            ob.name[0]?ob.name:"(unnamed)", color_name(ob.color), ob.eden?"Eden":"Plymouth", res_name(ob.res), ob.ready?"YES":"no"); continue; }
        if (oa.occ && !ob.occ) { printf("    Player[%d] LEFT (was %s)\n", i, oa.name[0]?oa.name:"(unnamed)"); continue; }
        if (!ob.occ) continue;
        const char* who = ob.name[0]?ob.name:"(unnamed)";
        if (strcmp(oa.name,ob.name)) printf("    Player[%d] name: '%s' -> '%s'\n", i, oa.name, ob.name);
        if (oa.color!=ob.color) printf("    Player[%d] %s color: %s -> %s\n", i, who, color_name(oa.color), color_name(ob.color));
        if (oa.eden !=ob.eden)  printf("    Player[%d] %s race: %s -> %s\n", i, who, oa.eden?"Eden":"Plymouth", ob.eden?"Eden":"Plymouth");
        if (oa.res  !=ob.res)   printf("    Player[%d] %s resources: %s -> %s\n", i, who, res_name(oa.res), res_name(ob.res));
        if (oa.ready!=ob.ready) printf("    Player[%d] %s Ready: %s -> %s\n", i, who, oa.ready?"YES":"no", ob.ready?"YES":"no");
    }
}

int main(int argc, char** argv) {
    load_config();   // op2session.ini -> g_verbose (per-packet dumps on/off)
    printf("OP2Session - external Outpost 2 multiplayer client%s\n",
           g_verbose ? "  [verbose logging: ON]" : "");
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa)) { printf("WSAStartup failed\n"); return 1; }

    // ---- 1) ephemeral socket for discovery ----
    SOCKET ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1; setsockopt(ds, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));
    DWORD to = 250; setsockopt(ds, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    sockaddr_in any; memset(&any,0,sizeof(any)); any.sin_family=AF_INET; any.sin_addr.s_addr=INADDR_ANY;
    bind(ds, (sockaddr*)&any, sizeof(any));

    Game g;
    if (argc > 1) {
        memset(&g, 0, sizeof(g));
        inet_pton(AF_INET, argv[1], &g.hostIp);
        memcpy(g.gameGuid, GAME_GUID, 16);
        // still discover to grab the session GUID/name
        Game d = discover(ds);
        if (d.found && d.hostIp == g.hostIp) g = d;
        else { memcpy(g.sessionGuid, GAME_GUID, 16); g.found = true; strcpy(g.name,"(by IP)"); }
    } else {
        printf("Discovering a hosted game...\n");
        g = discover(ds);
    }
    closesocket(ds);
    if (!g.found) { printf("No hosted game found.\n"); WSACleanup(); return 1; }

    char ips[32]; struct in_addr ia; ia.s_addr = g.hostIp; strcpy(ips, inet_ntoa(ia));
    printf("Host: %s  game:\"%s\"  maxPlayers=%d\n", ips, g.name, g.maxPlayers);
    printf("  game GUID:    "); for(int i=0;i<16;i++)printf("%02x",g.gameGuid[i]); printf("\n");
    printf("  session GUID: "); for(int i=0;i<16;i++)printf("%02x",g.sessionGuid[i]); printf("\n");

    // ---- 2) bind our game socket to first free 47800-47807 ----
    SOCKET gs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    to = 400; setsockopt(gs, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    uint32_t localIp = get_local_ip();
    { struct in_addr la2; la2.s_addr = localIp; printf("Binding game socket to local IP %s\n", localIp?inet_ntoa(la2):"(ANY)"); }
    uint16_t ourPort = 0;
    for (int p = GAME_PORT_LO; p <= GAME_PORT_HI; p++) {
        sockaddr_in la; memset(&la,0,sizeof(la)); la.sin_family=AF_INET;
        la.sin_addr.s_addr = localIp ? localIp : INADDR_ANY; la.sin_port=htons((u_short)p);
        if (bind(gs, (sockaddr*)&la, sizeof(la)) == 0) { ourPort=(uint16_t)p; break; }
    }
    if (!ourPort) { printf("Could not bind a game port 47800-47807\n"); WSACleanup(); return 1; }
    printf("Our game socket bound to port %u\n\n", ourPort);

    // ---- 3) send join request to host:47777, loop until reply ----
    sockaddr_in joinAddr; memset(&joinAddr,0,sizeof(joinAddr));
    joinAddr.sin_family=AF_INET; joinAddr.sin_port=htons(JOIN_PORT); joinAddr.sin_addr.s_addr=g.hostIp;

    uint8_t req[50];
    build_join(req, g.sessionGuid, ourPort, /*password*/ "");  // join uses the SESSION GUID (reply[0x1a]), not game-type
    printf("JOIN request (50 bytes) -> %s:%d:\n", ips, JOIN_PORT);
    hexdump(req, 50);

    bool accepted = false; uint32_t assignedNetID = 0, hostNetID = 0;
    sockaddr_in replyFrom; memset(&replyFrom, 0, sizeof(replyFrom));
    for (int attempt = 0; attempt < 16 && !accepted; attempt++) {
        sendto(gs, (char*)req, 50, 0, (sockaddr*)&joinAddr, sizeof(joinAddr));
        printf("  attempt %d sent, waiting for reply...\n", attempt+1);
        uint8_t buf[600]; sockaddr_in from; int fl = sizeof(from);
        int n = recvfrom(gs, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        if (g_verbose) {
            char fdesc[48]; sprintf(fdesc, "%s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            printf("\n<-- reply from %s:\n", fdesc);
            decode_packet(buf, n, fdesc);
        }
        // reply (FUN_004965a0): payloadSize[0x08]=0x18, type[0x09]=1, result[0x0E]=1ok/2full,
        // GUID echo[0x12], host NetID = header source[0x00], assigned client NetID[0x22].
        if (n == 0x26 && buf[0x09] == 1 && buf[0x08] == 0x18) {
            uint32_t result = rd32(buf + 0x0E);
            hostNetID = rd32(buf + 0x00);
            assignedNetID = rd32(buf + 0x22);
            bool guidEcho = (memcmp(buf + 0x12, g.sessionGuid, 16) == 0);
            printf("  -> result=%u (1=accepted,2=full)  hostNetID=%08X  assignedNetID=%08X  slot=%u  guidEcho=%s\n",
                   result, hostNetID, assignedNetID, assignedNetID & 7, guidEcho?"ok":"NO");
            if (result == 1) { accepted = true; replyFrom = from; }
            else if (result == 2) { printf("  Session is FULL.\n"); break; }
        }
    }

    if (!accepted) {
        printf("\nNot accepted (no result==1). See dumps above; iterate on the join request.\n");
        WSACleanup(); return 2;
    }
    printf("\n=== JOIN ACCEPTED ===  assignedNetID=%08X slot=%u\n", assignedNetID, assignedNetID&7);

    // ---- 4) settle into the lobby: send everything to the host GAME socket (47800) ----
    // The host RECEIVES on its bound game socket 47800 (even though it SENDS from an
    // ephemeral port). To stay in the GUR layer we must: (a) raise our status to Normal
    // (cmd-6, type 1) so our game packets are accepted, and (b) ACK the host's reliable
    // GUR packets (flags 0x08 carrying ackA/ackB) so its resend counter doesn't exhaust
    // and drop us (FUN_0042e800 / FUN_004601f0).
    sockaddr_in gameAddr; memset(&gameAddr,0,sizeof(gameAddr));
    gameAddr.sin_family=AF_INET; gameAddr.sin_addr.s_addr=g.hostIp; gameAddr.sin_port=htons(GAME_PORT_LO);

    auto send_status = [&](sockaddr_in dst, uint16_t status){
        uint8_t st[20]; memset(st,0,sizeof(st));
        wr32(st+0x00, assignedNetID); wr32(st+0x04, hostNetID);
        st[0x08]=6; st[0x09]=1;            // payloadSize=6, type=1
        wr32(st+0x0E, 6); wr16(st+0x12, status);   // cmd=6, status
        wr32(st+0x0A, wire_cksum(st));
        sendto(gs,(char*)st,20,0,(sockaddr*)&dst,sizeof(dst));
    };
    uint8_t ackA = 0, ackB = 0;             // highest seq received from host per channel
    uint8_t hostAckA = 0, hostAckB = 0;     // host's ack OF US (proves it processed our seq'd pkt)
    auto send_gur_ack = [&](sockaddr_in dst){
        uint8_t p[22]; memset(p,0,sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08]=8; p[0x09]=0;              // payloadSize=8, type=0 (game/GUR)
        p[0x0E]=0x08;                      // flags: carries ACK info
        p[0x10]=ackA; p[0x11]=ackB;        // payload+2/+3 = ackA/ackB
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,22,0,(sockaddr*)&dst,sizeof(dst));
    };
    // cmd-3 ShowJoinGame (FUN_0045f2f0): a channel-A singlecast to the host carrying our
    // player name + engine version. This is the LOBBY-level join that makes the host's
    // GAME SETTINGS window add us to a visible slot. payload: +4 cmd=3, +5 version(4),
    // +9 name; payloadSize = nameLen + 0x3E. flags 0x0A = channel-A seq + ack info.
    const char* PLAYER_NAME = "OPU";
    uint8_t ourSeqA = 1;                    // our channel-A send seq (host expects 1 first)
    bool lobbyJoined = false;
    auto send_cmd3 = [&](sockaddr_in dst){
        int nameLen = (int)strlen(PLAYER_NAME);
        int payloadSize = nameLen + 0x3e;
        int total = 14 + payloadSize;
        uint8_t p[256]; memset(p, 0, total);
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);   // singlecast to host
        p[0x08] = (uint8_t)payloadSize; p[0x09] = 0;            // type 0 (GUR)
        p[0x0E] = 0x0A; p[0x0F] = ourSeqA;                     // flags chanA+ack, seq
        p[0x10] = ackA; p[0x11] = ackB;                        // ack host channels
        p[0x12] = 3;                                            // engine cmd 3 = ShowJoinGame
        wr32(p+0x13, 0x01030004);                              // TApp::Version "1.3.0.4"
        memcpy(p+0x17, PLAYER_NAME, nameLen);                  // name at payload+9
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,total,0,(sockaddr*)&dst,sizeof(dst));
    };

    // cmd-6 lobby CHAT (FUN_00461380): channel-A reliable; payload +4 cmd=6, +5 our NetID,
    // +9 message; payloadSize = msgLen + 10. Our next channel-A seq after cmd-3.
    const char* CHAT_MSG  = "Hello!";                // sent in the lobby once we join
    const char* GLHF_MSG  = "Good Luck! Have Fun!";  // sent in-game at tick 100
    bool chatSent = false, chatAcked = false;
    // cmd-6 chat (engine cmd 6). Parameterised so the same builder serves both the lobby
    // greeting and the in-game message. Uses the CURRENT channel-A seq (caller decides when
    // to advance it). Engine cmd 6 is the same reliable GUR message in the lobby and in-game.
    auto send_chat = [&](sockaddr_in dst, const char* msg){
        int msgLen = (int)strlen(msg);
        int payloadSize = msgLen + 10;
        int total = 14 + payloadSize;
        uint8_t p[256]; memset(p, 0, total);
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08] = (uint8_t)payloadSize; p[0x09] = 0;
        p[0x0E] = 0x0A; p[0x0F] = ourSeqA;        // channel A seq + ack
        p[0x10] = ackA; p[0x11] = ackB;
        p[0x12] = 6;                               // engine cmd 6 = chat
        wr32(p+0x13, assignedNetID);               // source NetID (payload+5)
        memcpy(p+0x17, msg, msgLen);               // message (payload+9)
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,total,0,(sockaddr*)&dst,sizeof(dst));
    };

    // cmd-2 player-status update (FUN_0045fac0 case 2): channel-A reliable; payload +4 cmd=2,
    // +5 field selector (dword; 4=Ready, 2=Race/Eden), +9 value (dword; 1=on). Sets the bit
    // in our slot's status dword. payloadSize = 13. Our next channel-A seq after the chat.
    bool readySent = false, readyAcked = false;
    auto send_status_update = [&](sockaddr_in dst, uint32_t field, uint32_t value){
        uint8_t p[32]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08] = 13; p[0x09] = 0;                 // payloadSize=13, type 0 (GUR)
        p[0x0E] = 0x0A; p[0x0F] = ourSeqA;         // channel A seq + ack
        p[0x10] = ackA; p[0x11] = ackB;
        p[0x12] = 2;                               // engine cmd 2 = status update
        wr32(p+0x13, field);                       // field selector (4 = Ready)
        wr32(p+0x17, value);                       // value (1 = on)
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,14+13,0,(sockaddr*)&dst,sizeof(dst));
    };
    auto send_ready = [&](sockaddr_in dst){ send_status_update(dst, 4, 1); };

    // cmd-9 "LOADED" marker (client side of the start handshake, FUN_00462050): a tiny
    // channel-A reliable carrying ONLY the GUR sub-header + engine cmd 9 at 0x12.
    // Host (FUN_00461700) counts each client whose marker byte (wire 0x12) == '\t'(9);
    // when all players are loaded it broadcasts the final cmd-9 GO and the game starts.
    // Internal send struct +0x24=5 -> wire payloadSize=5 (= sub-header 4 + cmd 1); no data.
    // We SKIP the file-checksum entirely (it's a pure client-side self-check the host never
    // inspects), so we just emit "loaded" to make the host proceed.
    bool loadedSent = false, loadedAcked = false;
    auto send_loaded = [&](sockaddr_in dst){
        uint8_t p[32]; memset(p, 0, sizeof(p));
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);
        p[0x08] = 5; p[0x09] = 0;                  // payloadSize=5, type 0 (GUR)
        p[0x0E] = 0x0A; p[0x0F] = ourSeqA;         // channel A seq + ack
        p[0x10] = ackA; p[0x11] = ackB;
        p[0x12] = 9;                               // engine cmd 9 = "loaded"
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,14+5,0,(sockaddr*)&dst,sizeof(dst));
    };

    // In-game COMMAND TURN (engine cmd 0x0C, FUN_00420F30/FUN_00420BA0). The running game is
    // lockstep: each mark, every player must submit a command packet or the host's sim halts
    // (we saw it freeze at mark 0 / tick 8). Body = playerNum(u8) + executionTick(u32) +
    // command blocks; each block = {u8 type, u8 len, u32 unk, data[len]}, one per consecutive
    // mark. An EMPTY turn block is a 6-byte ctNop (type=0,len=0) but its unk MUST stay non-zero
    // (the receiver discards unk==0). For every host cmd-0x0C we reply our own turn covering the
    // same marks (a ctNop each, keeping that mark's unk) so the host advances without us running
    // a sim. Channel-A singlecast to the host (one peer) so we reuse our chanA seq.
    uint8_t ourPlayerNum = (uint8_t)(assignedNetID & 7);
    long cmd0cSent = 0;
    // Send one in-game command turn (engine cmd 0x0C) under OUR player index, carrying a
    // pre-built block region (the caller builds it: a ctNop per mark, keeping each mark's
    // non-zero unk token so the receiver FUN_00420e00 stores it, with our chat block injected
    // at a fresh mark when we have something to say). One block = one consecutive mark.
    auto send_cmd0c = [&](uint32_t execTick, const uint8_t* blocks, int blocksLen){
        if (blocksLen < 6) blocksLen = 6;
        if (blocksLen > 500) blocksLen = 500;
        int payloadSize = 5 /*subheader+cmd*/ + 5 /*playerNum+tick*/ + blocksLen;
        int total = 14 + payloadSize;
        uint8_t p[600]; memset(p, 0, total);
        wr32(p+0x00, assignedNetID); wr32(p+0x04, hostNetID);   // chanA singlecast to host
        p[0x08] = (uint8_t)payloadSize; p[0x09] = 0;
        p[0x0E] = 0x0A; p[0x0F] = ++ourSeqA;       // next channel-A seq (after cmd-9 = 4)
        p[0x10] = ackA; p[0x11] = ackB;
        p[0x12] = 0x0C;                            // engine cmd 0x0C = command turn
        p[0x13] = ourPlayerNum;                    // store under OUR ring (player index)
        wr32(p+0x14, execTick);                    // execution tick (matches the host's turn)
        memcpy(p+0x18, blocks, blocksLen);         // caller-built blocks (ctNops + opt. chat)
        wr32(p+0x0A, wire_cksum(p));
        sendto(gs,(char*)p,total,0,(sockaddr*)&gameAddr,sizeof(gameAddr));
        cmd0cSent++;
    };

    printf("Raising status + sending cmd-3 ShowJoinGame (name='%s', version 1.3.0.4)...\n", PLAYER_NAME);
    send_status(replyFrom, 2);   // join-ack to the reply source (47783) -> progresses host to lobby
    send_status(gameAddr, 2);    // raise status on the game socket
    send_cmd3(gameAddr);

    DWORD start = GetTickCount(), lastKeep = 0, lastTick = 0, lastRecv = GetTickCount(); int hb = 0;
    uint8_t last[1300]; int lastn = 0, dupCount = 0;
    LobbyState prevLobby; prevLobby.valid = false;
    bool gameStarting = false, gameLive = false;
    // ---- in-game clock + duration timer ----
    // We do NOT run the simulation, so we cannot derive the tick from game logic. Instead we
    // read OP2's OWN tick value off the wire: every host cmd-0x0C is stamped with the
    // executionTick it applies to (a real OP2 tick, on command-mark boundaries = CPPI ticks
    // apart). In lockstep that synchronized stream IS the authoritative clock every peer uses,
    // so mirroring it gives us OP2's exact tick - more correct than a free-running local timer,
    // which would drift from the host. `lookahead` = how far ahead of execution the host
    // stamps (the negative pre-roll seen at start, e.g. -12); gameTick = the tick currently
    // executing on screen.
    bool   haveFirstExec = false;
    int    firstExecTick = 0, maxExecTick = 0, lookahead = 0, gameTick = 0;
    int    cppi = 4, lastExecSeen = 0; bool haveLastExec = false;   // command-mark stride
    DWORD  gameStartMs = 0;            // real-world ms at GO (for game-duration wall clock)
    bool   glhfSent = false;          // in-game "Good Luck! Have Fun!" fired once at tick 100
    // In-game chat (command block type 0x30 inside cmd-0x0C). To SEND, we pin the chat to a
    // single future mark and inject the block into our turn for that mark in every overlapping
    // window until the mark passes (exactly how the host does it -> displays once).
    bool   chatActive = false; int chatMark = 0; char chatText[200] = {0};
    int    maxSentMark = -1000000;    // furthest mark we've ever committed (ctNop'd)
    char   lastRecvChat[200] = {0};   // de-dup incoming host chat (re-sent across windows)
    while (GetTickCount() - start < 600000) {   // stay ~10 min so we persist in the lobby
        uint8_t buf[1300]; sockaddr_in from; int fl = sizeof(from);
        int n = recvfrom(gs, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n > 0) {
            lastRecv = GetTickCount();
            if (buf[0x09] == 1)
                printf("  <<TYPE1 n=%d cmd=%u>>\n", n, (n >= 0x12) ? rd32(buf + 0x0E) : 9999u);
            if (n >= 0x12 && buf[0x09] == 0) {
                uint8_t flags = buf[0x0E], seq = buf[0x0F];
                if (flags & 0x02) ackA = seq;
                if (flags & 0x04) ackB = seq;
                if (flags & 0x20) ackB = buf[0x12];
                hostAckA = buf[0x10]; hostAckB = buf[0x11];
            }
            // GameStartInfo (engine cmd 1): parse and report CHANGES (first one in full).
            // Diffing the parsed state ignores the seq/ack churn, so it fires only on real edits.
            if (buf[0x09] == 0 && buf[0x08] == 0xBF && n >= 0xCD && buf[0x12] == 1) {
                LobbyState cur = parse_gsi(buf, n);
                if (cur.valid) {
                    if (!prevLobby.valid) print_lobby(cur, scen_name(g.scenType));
                    else if (!lobby_eq(prevLobby, cur)) diff_lobby(prevLobby, cur);
                    prevLobby = cur;
                }
            }
            // START-time transport roster replication (type-1 control). The host's
            // "Sending players list to all players..." = ReplicatePlayersList (FUN_00496950):
            // cmd-4 SetPlayerList -> we reply status Replicated(3); cmd-5 finalize -> status(4).
            // (FUN_00497240 cases 4/5.) Reply cmd-6 to the host game socket so it completes.
            if (buf[0x09] == 1 && n >= 0x12) {
                uint32_t cc = rd32(buf + 0x0E);
                // Host ReplicatePlayersList (FUN_00496950) bumps occupied slots 2->3 then
                // FUN_00496dc0 WAITS for every client to report status 3 (16 retries, else it
                // falls back to cmd-5 and FAILS). So cmd-4 -> reply cmd-6 status(3); the
                // cmd-5 fallback wants status(4).
                if (cc == 4) { send_status(gameAddr, 3); printf("  [start] cmd-4 SetPlayerList -> replied cmd-6 status(3)\n"); }
                else if (cc == 5) { send_status(gameAddr, 4); printf("  [start] cmd-5 finalize -> replied cmd-6 status(4)\n"); }
            }
            // Lobby/connection EVENTS from the host (sequenced packet, engine cmd at wire 0x12).
            // (FUN_0045fac0 dispatch: 0x0B quit/eject, 10 abort, 7/8 start.)
            if (buf[0x09] == 0 && buf[0x08] >= 5 && n >= 0x13 &&
                (buf[0x0E] & 0x06) && rd32(buf + 0x00) == hostNetID) {
                uint8_t ec = buf[0x12];
                if (ec == 0x0B) {         // host left / ended the lobby / ejected us
                    send_gur_ack(gameAddr);
                    printf("\n*** HOST ENDED THE LOBBY *** (cmd 0x0B from host) - the host quit / closed the game / ejected us. Disconnecting.\n");
                    break;
                }
                if (ec == 10) {           // start aborted -> game canceled
                    send_gur_ack(gameAddr);
                    printf("\n*** GAME CANCELED *** host aborted the start (cmd 10). Disconnecting.\n");
                    break;
                }
                if ((ec == 7 || ec == 8) && !gameStarting) {   // host clicked START
                    gameStarting = true;
                    printf("\n*** HOST IS STARTING THE GAME *** (cmd %u) - running the start handshake.\n", ec);
                }
                // cmd-8 = load&go: reply our cmd-9 "loaded" marker (skip the checksum/load).
                if (ec == 8 && !loadedSent) {
                    ourSeqA = 4;                          // next channel-A seq after Ready(3)
                    printf("Sending cmd-9 LOADED marker (seq %u) - skipping file checksum (client-side self-check).\n", ourSeqA);
                    send_loaded(gameAddr); loadedSent = true;
                }
                // host's final cmd-9 = GO: all players loaded, the simulation is starting.
                if (ec == 9 && loadedSent) {
                    send_gur_ack(gameAddr);
                    printf("\n*** GAME STARTED *** host broadcast cmd-9 GO - all players loaded, the match is live! Entering in-game phase.\n");
                    gameLive = true;
                }
            }
            // IN-GAME COMMAND TURN from the host (engine cmd 0x0C). NOTE these can carry
            // flags=0x00 (no channel bits), so they are NOT caught by the events block above.
            // Each turn is a list of blocks {u8 type, u8 len, u32 unk, u8 data[len]}, one per
            // consecutive mark (mark = execTick + index*CPPI). We (a) decode the tick, (b)
            // surface any incoming chat (block type 0x30), and (c) reply our OWN turn: a ctNop
            // per mark (keeping each block's non-zero unk so the receiver stores it), with our
            // chat block injected at the pinned mark when we have something to say.
            if (buf[0x09] == 0 && n >= 0x18 && buf[0x12] == 0x0C &&
                rd32(buf + 0x00) == hostNetID) {
                if (!gameLive) gameLive = true;
                int execTick = (int)rd32(buf + 0x14);        // signed OP2 tick (starts negative)
                if (!haveFirstExec) { haveFirstExec = true; firstExecTick = execTick; lookahead = -execTick; }
                if (execTick > maxExecTick) maxExecTick = execTick;
                gameTick = maxExecTick - lookahead;           // tick currently executing on screen
                // Derive the command-mark stride (CPPI) from the gap between distinct exec ticks.
                if (haveLastExec) { int d = execTick - lastExecSeen; if (d > 0 && d <= 64) cppi = d; }
                lastExecSeen = execTick; haveLastExec = true;

                // Fire our greeting once: pin it to the first mark we have NOT yet committed
                // (maxSentMark + cppi). Pinning within an already-echoed window would fail - the
                // receiver won't overwrite the ctNop we already stored for that mark (tick<=stored).
                if (!glhfSent && gameTick >= 100) {
                    glhfSent = true; chatActive = true; chatMark = maxSentMark + cppi;
                    strncpy(chatText, GLHF_MSG, sizeof(chatText)-1);
                    printf("\n*** IN-GAME CHAT *** tick %d -> sending '%s' at fresh mark %d\n", gameTick, chatText, chatMark);
                }
                if (chatActive && execTick > chatMark) chatActive = false;   // mark passed; stop

                // Walk the host's blocks: display incoming chat, and build our reply turn.
                uint8_t ob[400]; int ol = 0; bool injected = false;
                int off = 0x18, idx = 0;
                while (off + 6 <= n) {
                    uint8_t blen = buf[off+1];
                    if (off + 6 + (int)blen > n) break;
                    uint32_t unk = rd32(buf + off + 2);
                    int blockMark = execTick + idx*cppi;
                    // (b) incoming chat from the host (block type 0x30:
                    // data = [sender(1)][recipientMask(1)][text NUL]); text starts at data+2.
                    if (buf[off] == 0x30 && blen >= 3) {
                        const char* txt = (const char*)(buf + off + 6 + 2);
                        if (strncmp(txt, lastRecvChat, sizeof(lastRecvChat)-1) != 0) {
                            strncpy(lastRecvChat, txt, sizeof(lastRecvChat)-1);
                            printf("\n[in-game chat] %s: %s\n", g.name[0]?g.name:"host", txt);
                        }
                    }
                    // (c) our reply block for this mark
                    if (ol + 64 <= (int)sizeof(ob)) {
                        if (chatActive && !injected && blockMark == chatMark && unk != 0) {
                            int tl = (int)strlen(chatText); int dl = 2 + tl + 1;   // [sender][mask][text][00]
                            ob[ol]=0x30; ob[ol+1]=(uint8_t)dl; wr32(ob+ol+2, unk);
                            ob[ol+6]=ourPlayerNum; ob[ol+7]=0xFF; memcpy(ob+ol+8, chatText, tl); ob[ol+8+tl]=0;
                            ol += 6 + dl; injected = true;
                        } else {
                            ob[ol]=0; ob[ol+1]=0; wr32(ob+ol+2, unk);   // ctNop, keep mark's unk
                            ol += 6;
                        }
                    }
                    off += 6 + blen; idx++;
                }
                if (idx > 0) { int last = execTick + (idx-1)*cppi; if (last > maxSentMark) maxSentMark = last; }
                if (ol < 6) { ob[0]=0; ob[1]=0; wr32(ob+2, 0xBABE3624u); ol = 6; }
                send_cmd0c((uint32_t)execTick, ob, ol);
                if (injected) printf("  [ingame] injected our chat block at mark %d (seq %u)\n", chatMark, ourSeqA);
                if (cmd0cSent <= 10 || (cmd0cSent % 100) == 0)
                    printf("  [ingame] gameTick=%d (frontier=%d)  replied turn as player %u (seq %u, sent=%ld)\n",
                           gameTick, maxExecTick, ourPlayerNum, ourSeqA, cmd0cSent);
            }
            hb++;
            if (!lobbyJoined && hostAckA >= ourSeqA) {
                lobbyJoined = true;
                printf("\n*** LOBBY JOIN ACKED *** host acked our cmd-3 (seq %u) - we show as '%s' in the lobby!\n", ourSeqA, PLAYER_NAME);
                ourSeqA = 2;                     // next channel-A seq, for the chat
                Sleep(300);
                printf("Saying '%s' in lobby chat (seq %u)...\n", CHAT_MSG, ourSeqA);
                send_chat(gameAddr, CHAT_MSG); chatSent = true;
            }
            if (chatSent && !chatAcked && hostAckA >= ourSeqA) {
                chatAcked = true;
                printf("\n*** CHAT DELIVERED *** host acked our '%s' (seq %u)\n", CHAT_MSG, ourSeqA);
                ourSeqA = 3;                       // next channel-A seq, for the Ready update
                printf("Setting %s READY (cmd-2 field=4 value=1, seq %u)...\n", PLAYER_NAME, ourSeqA);
                send_ready(gameAddr); readySent = true;
            }
            if (readySent && !readyAcked && hostAckA >= ourSeqA) {
                readyAcked = true;
                printf("\n*** READY SET *** %s is now Ready - the host can START the game.\n", PLAYER_NAME);
            }
            if (loadedSent && !loadedAcked && hostAckA >= 4) {
                loadedAcked = true;
                printf("\n*** LOADED ACKED *** host acked our cmd-9 (seq 4) - waiting for the GO broadcast.\n");
            }
            if (g_verbose) {                              // per-packet dump (off = clean output)
                bool dup = (n == lastn && memcmp(buf, last, n) == 0);
                if (!dup) {
                    if (dupCount) { printf("   (x%d)\n", dupCount); dupCount = 0; }
                    char fdesc[48]; sprintf(fdesc, "%s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    printf("\n<-- %s\n", fdesc); decode_packet(buf, n, fdesc);
                    memcpy(last, buf, n); lastn = n;
                } else dupCount++;
            }
            send_gur_ack(gameAddr);                  // ack host's reliable packets (keepalive)
            if (!lobbyJoined) send_cmd3(gameAddr);    // resend cmd-3 until acked
            else if (chatSent && !chatAcked) send_chat(gameAddr, CHAT_MSG);   // resend chat
            else if (readySent && !readyAcked) send_ready(gameAddr); // resend Ready
            else if (loadedSent && !loadedAcked) send_loaded(gameAddr); // resend cmd-9 until acked
        }
        if (GetTickCount() - lastKeep > 200) {
            if (!lobbyJoined) {                       // only re-announce while still joining
                send_status(replyFrom, 2);
                send_status(gameAddr, 2);
                send_cmd3(gameAddr);
            } else if (chatSent && !chatAcked) {
                send_chat(gameAddr, CHAT_MSG);
            } else if (readySent && !readyAcked) {
                send_ready(gameAddr);
            } else if (loadedSent && !loadedAcked) {
                send_loaded(gameAddr);
            }
            send_gur_ack(gameAddr);                   // once joined: acks only (status persists)
            lastKeep = GetTickCount();
        }
        // Start the real-world game-duration clock the moment the match goes live.
        if (gameLive && gameStartMs == 0) gameStartMs = GetTickCount();
        // HOST STOPPED RESPONDING: once joined, a removed/gone host stops sending us packets.
        // Covers host crash / network loss / host closed the game / we were ejected silently.
        // (Allow a longer gap once the host is starting - it pauses to load the scenario.)
        DWORD silenceLimit = gameLive ? 30000u : (gameStarting ? 20000u : 5000u);
        if (lobbyJoined && GetTickCount() - lastRecv > silenceLimit) {
            printf("\n*** HOST STOPPED RESPONDING *** no packets from the host for >%lus - it left, ended the game, crashed, or stalled. Disconnecting.\n", (unsigned long)(silenceLimit/1000));
            break;
        }
        if (GetTickCount() - lastTick > 5000) {
            unsigned long gdur = gameStartMs ? (GetTickCount()-gameStartMs)/1000 : 0;
            printf("  [t=%lus  from-host=%d  joined=%s  live=%s  gameTick=%d  gameDur=%lus  weAck(A,B)=%u,%u  HOST-ACKS-US(A)=%u  cmd0c=%ld]\n",
                   (unsigned long)((GetTickCount()-start)/1000), hb, lobbyJoined?"YES":"no", gameLive?"YES":"no", gameTick, gdur, ackA, ackB, hostAckA, cmd0cSent);
            lastTick = GetTickCount();
        }
    }
    if (dupCount) printf("   (x%d)\n", dupCount);

    if (gameStartMs) {
        unsigned long gdur = (GetTickCount()-gameStartMs)/1000;
        printf("\nGame summary: duration %lus (%lum %lus), final tick %d, %ld command turns sent.\n",
               gdur, gdur/60, gdur%60, gameTick, cmd0cSent);
    }
    printf("\nDone. (status enum: 0/1/2/3/4 = %s/%s/%s/%s/%s)\n",
           status_name(0),status_name(1),status_name(2),status_name(3),status_name(4));
    closesocket(gs); WSACleanup();
    return 0;
}
