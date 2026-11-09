/* op2finder.exe - external LAN game-session finder for Outpost 2 (1.3.6 / OPU 1.4.1).
 *
 * Speaks OP2's UDP discovery protocol directly - no OP2 needed:
 *   - broadcast a 42-byte query to 255.255.255.255:47776
 *   - OP2 hosts with an open multiplayer session reply (81 bytes) from :47776
 *   - parse each reply: host IP, game-creator's name, session GUID, ping
 *
 * Protocol reverse-engineered from Outpost2.exe (BroadcastForGames @0x4917E0,
 * NetworkWaitForBroadcastReply @0x491A50, host responder @0x48C2C0). Full wire
 * spec in README.md. Standalone console exe (Winsock).
 *
 *   op2finder              live LAN scan, refresh every 3s
 *   op2finder <ip>         query ONE host directly ("Looking at the IP address")
 *   op2finder -once        scan once and exit
 *   op2finder -raw         also dump raw reply bytes
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")

#define DISCOVERY_PORT 47776
#define XORK 0xFDE24ACBu
#define QUERY_LEN 42
#define REPLY_LEN 81
#define MAX_FOUND 64
#define MAX_TARGETS 16

/* OP2 game-type GUID, raw 16 bytes as stored/compared in Outpost2.exe
   ({5A55CF11-B841-11CE-9210-00AA006C4972}, Data1 little-endian). */
static const unsigned char GAME_GUID[16] = {
    0x11,0xCF,0x55,0x5A, 0x41,0xB8, 0xCE,0x11,
    0x92,0x10,0x00,0xAA,0x00,0x6C,0x49,0x72
};

static unsigned rd32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }
static unsigned rd16(const unsigned char *p){ return p[0]|(p[1]<<8); }
static void wr32(unsigned char *p, unsigned v){ p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff; }

/* OP2 checksum (FUN_00490eb0): sum ndw LE dwords from buf[start], + 1/2-byte tail, XOR 0xFDE24ACB */
static unsigned op2cksum(const unsigned char *buf, int start, int ndw, int tail)
{
    unsigned s = 0; int off = start, i;
    for (i = 0; i < ndw; i++) { s += rd32(buf + off); off += 4; }
    if (tail == 2) s += rd16(buf + off);
    else if (tail == 1) s += buf[off];
    return s ^ XORK;
}

/* Query layout (verified vs host responder @0x48C2C0, builder @0x4917E0):
   [0x00]cksum [0x04]type 0x1000 [0x06]16-byte GUID [0x16]reply port (u16)
   [0x1A]token. The host replies to the port we name at [0x16], so it must be
   our own listening port or we never receive the reply. */
static void build_query(unsigned char *q, unsigned token, unsigned short reply_port)
{
    memset(q, 0, QUERY_LEN);
    q[0x04] = 0x00; q[0x05] = 0x10;             /* message type 0x1000 (LE) */
    memcpy(q + 0x06, GAME_GUID, 16);            /* game-type GUID (host matches here) */
    q[0x16] = (unsigned char)(reply_port & 0xFF);
    q[0x17] = (unsigned char)((reply_port >> 8) & 0xFF);   /* "reply to me on this port" */
    wr32(q + 0x1A, token);                      /* ping token (echoed at reply+0x06) */
    wr32(q + 0x00, op2cksum(q, 0x04, 9, 2));    /* checksum: 9 dwords + u16 */
}

typedef struct { unsigned ip; char name[24]; char sess[48]; int ping; int maxp; int scen; } Found;

/* multiplayer mission-type code ((StartupFlags@0x2A >> 9) & 0x1F) -> name. Consecutive
   codes, ALL verified live by hosting each: 24=Last One Standing (0x3080), 25=Midas
   (0x3280), 26=Resource Race (0x3480), 27=Space Race (0x3680), 28=Land Rush (0x3880). */
static const char *scen_name(int c)
{
    static char buf[16];
    switch (c) {
        case 24: return "Last One Standing";
        case 25: return "Midas";
        case 26: return "Resource Race";
        case 27: return "Space Race";
        case 28: return "Land Rush";
    }
    sprintf(buf, "Type %d", c);
    return buf;
}

static int parse_reply(const unsigned char *d, int len, unsigned src_ip,
                       unsigned now_ms, Found *out)
{
    unsigned echoed; int i, ping;
    if (len != REPLY_LEN) return 0;
    if (rd16(d + 0x04) != 0x1001) return 0;                 /* reply type */
    if (op2cksum(d, 0x04, 19, 1) != rd32(d + 0x00)) return 0;
    if (memcmp(d + 0x0A, GAME_GUID, 16) != 0) return 0;     /* reply GUID at +0x0A */

    out->ip = src_ip;
    echoed = rd32(d + 0x06);                                /* host echoed our token */
    ping = (int)(now_ms - echoed);
    out->ping = (ping < 0 || ping > 60000) ? -1 : ping;
    /* creator name: ASCII at +0x42, <=14, NUL-padded */
    memset(out->name, 0, sizeof(out->name));
    for (i = 0; i < 14 && d[0x42 + i]; i++) out->name[i] = (char)d[0x42 + i];
    if (!out->name[0]) strcpy(out->name, "(no name)");
    /* session GUID at +0x1a */
    sprintf(out->sess, "%08X-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X",
            rd32(d+0x1a), rd16(d+0x1e), rd16(d+0x20),
            d[0x22],d[0x23],d[0x24],d[0x25],d[0x26],d[0x27],d[0x28],d[0x29]);
    out->maxp = (int)rd32(d + 0x3a);                   /* max players */
    out->scen = (int)((rd32(d + 0x2a) >> 9) & 0x1f);   /* missionType: StartupFlags bits 9-13 */
    return 1;
}

static int build_targets(char targets[][20])
{
    int n = 0;
    char host[256];
    struct hostent *he;
    strcpy(targets[n++], "255.255.255.255");
    if (gethostname(host, sizeof(host)) == 0 && (he = gethostbyname(host)) != NULL) {
        int i;
        for (i = 0; he->h_addr_list[i] && n < MAX_TARGETS; i++) {
            unsigned char *a = (unsigned char *)he->h_addr_list[i];
            char b[20]; int dup = 0, k;
            if (a[0] == 127) continue;
            sprintf(b, "%u.%u.%u.255", a[0], a[1], a[2]);     /* /24 directed broadcast */
            for (k = 0; k < n; k++) if (strcmp(targets[k], b) == 0) { dup = 1; break; }
            if (!dup) strcpy(targets[n++], b);
        }
    }
    return n;
}

/* one scan: broadcast a few times, collect replies. returns count. */
static int scan(SOCKET s, unsigned short port, char targets[][20], int ntargets, int raw,
                Found *found, int maxf)
{
    unsigned char q[QUERY_LEN], buf[2048];
    int nfound = 0, sends = 6, sent = 0, t;
    DWORD start = GetTickCount(), gapNext = start;
    while ((GetTickCount() - start) < (DWORD)(sends * 200 + 1500)) {
        fd_set fds; struct timeval tv; struct sockaddr_in from; int fl = sizeof(from);
        if (sent < sends && GetTickCount() >= gapNext) {
            struct sockaddr_in to; memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET; to.sin_port = htons(DISCOVERY_PORT);
            build_query(q, (unsigned)GetTickCount(), port);   /* fresh token each send */
            for (t = 0; t < ntargets; t++) {
                to.sin_addr.s_addr = inet_addr(targets[t]);
                sendto(s, (char*)q, QUERY_LEN, 0, (struct sockaddr*)&to, sizeof(to));
            }
            sent++; gapNext = GetTickCount() + 200;
        }
        FD_ZERO(&fds); FD_SET(s, &fds);
        tv.tv_sec = 0; tv.tv_usec = 50000;
        if (select(0, &fds, NULL, NULL, &tv) > 0) {
            int len = recvfrom(s, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
            if (len == SOCKET_ERROR) continue;
            if (raw) {
                int i; printf("  raw %s (%d): ", inet_ntoa(from.sin_addr), len);
                for (i = 0; i < len; i++) printf("%02x", buf[i]); printf("\n");
            }
            Found f;
            if (parse_reply(buf, len, from.sin_addr.s_addr, (unsigned)GetTickCount(), &f)) {
                int i, dup = 0;
                for (i = 0; i < nfound; i++) if (found[i].ip == f.ip) { found[i] = f; dup = 1; break; }
                if (!dup && nfound < maxf) found[nfound++] = f;
            }
        }
    }
    return nfound;
}

static void print_table(Found *found, int n)
{
    int i;
    if (n == 0) { printf("  (no Outpost 2 sessions found)\n"); return; }
    printf("  %-15s  %-15s  %-3s  %-18s  %-6s  %s\n",
           "HOST IP", "GAME (creator)", "MAX", "SCENARIO TYPE", "PING", "SESSION");
    printf("  --------------------------------------------------------------------------------\n");
    for (i = 0; i < n; i++) {
        struct in_addr a; a.s_addr = found[i].ip;
        char ping[16], mp[8];
        if (found[i].ping >= 0) sprintf(ping, "%dms", found[i].ping); else strcpy(ping, "-");
        if (found[i].maxp > 0) sprintf(mp, "%d", found[i].maxp); else strcpy(mp, "-");
        printf("  %-15s  %-15s  %-3s  %-18s  %-6s  %s\n",
               inet_ntoa(a), found[i].name, mp, scen_name(found[i].scen), ping, found[i].sess);
    }
}

int main(int argc, char **argv)
{
    WSADATA wsa; SOCKET s; struct sockaddr_in local;
    char targets[MAX_TARGETS][20]; int ntargets, i, once = 0, raw = 0;
    Found found[MAX_FOUND]; int bcast = 1;
    unsigned short bound_port = 0;
    char *unicast = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-once") || !strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "-raw") || !strcmp(argv[i], "--raw")) raw = 1;
        else if (argv[i][0] != '-') unicast = argv[i];   /* a bare IP = direct-IP mode */
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 1; }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { printf("socket failed %d\n", WSAGetLastError()); return 1; }
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&bcast, sizeof(bcast));
    { int reuse = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse)); }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_addr.s_addr = INADDR_ANY; local.sin_port = 0;
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        printf("bind failed %d\n", WSAGetLastError()); return 1;
    }
    {   /* learn our ephemeral port so the host can reply to it (query +0x16) */
        struct sockaddr_in sn; int snl = sizeof(sn);
        if (getsockname(s, (struct sockaddr*)&sn, &snl) == 0) bound_port = ntohs(sn.sin_port);
    }

    printf("op2finder - finding Outpost 2 sessions (UDP :%d)\n", DISCOVERY_PORT);
    if (unicast) {                       /* direct-IP mode: query just this host */
        strncpy(targets[0], unicast, sizeof(targets[0]) - 1);
        targets[0][sizeof(targets[0]) - 1] = '\0';
        ntargets = 1;
        printf("querying %s directly\n", unicast);
    } else {
        ntargets = build_targets(targets);
        printf("broadcasting to:");
        for (i = 0; i < ntargets; i++) printf(" %s", targets[i]);
        printf("\n");
    }

    for (;;) {
        int n = scan(s, bound_port, targets, ntargets, raw, found, MAX_FOUND);
        SYSTEMTIME st; GetLocalTime(&st);
        printf("\n[%02d:%02d:%02d] %d session(s):\n", st.wHour, st.wMinute, st.wSecond, n);
        print_table(found, n);
        if (once) break;
        Sleep(3000);
    }
    closesocket(s);
    WSACleanup();
    return 0;
}
