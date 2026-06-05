// op2proto.cpp - implementation of the pure wire helpers (see op2proto.h).
#include "op2proto.h"

namespace op2 {

const uint8_t GAME_GUID[16] = {
    0x11,0xCF,0x55,0x5A, 0x41,0xB8, 0xCE,0x11,
    0x92,0x10,0x00,0xAA,0x00,0x6C,0x49,0x72
};

uint32_t disc_cksum(const uint8_t* buf, int start, int ndw, int tail) {
    uint32_t s = 0; int off = start;
    for (int i = 0; i < ndw; i++) { s += rd32(buf + off); off += 4; }
    if (tail == 2)      s += rd16(buf + off);
    else if (tail == 1) s += buf[off];
    return s ^ XORK;
}

uint32_t wire_cksum(const uint8_t* p) {
    uint8_t  size     = p[0x08];
    uint16_t sizeType = rd16(p + 0x08);                 // payloadSize | (type<<8)
    uint32_t acc      = (rd32(p + 0x00) + rd32(p + 0x04) + sizeType) ^ XORK;
    uint32_t psum     = 0;
    int ndw = size >> 2;
    for (int i = 0; i < ndw; i++) psum += rd32(p + 0x0E + i * 4);
    int tail = size & 3;
    if (tail) {
        uint32_t t = 0;
        memcpy(&t, p + 0x0E + ndw * 4, tail);
        psum = (psum + t) ^ XORK;
    }
    return acc + psum;
}

LobbyInfo parse_gsi(const uint8_t* p, int n) {
    LobbyInfo s;
    if (n < 0x13 + 0xBA) return s;          // need the full 191-byte blob
    const uint8_t* b = p + 0x13;            // blob base = wire 0x13 (after 5-byte GUR sub-header)
    uint32_t flags = rd32(b + 0x00);
    s.maxPlayers   = (flags >> 6) & 7;
    s.initVehicles = (flags >> 17) & 0xf;
    s.gameSpeed    = rd16(b + 0xAE) >> 2;
    s.disasters    = (flags & 1) != 0;
    s.dayNight     = (flags & 2) != 0;
    s.morale       = (flags & 4) != 0;
    char dll[33]; memcpy(dll, b + 0x10, 32); dll[32] = 0;
    s.mapDll = dll;
    for (int i = 0; i < 6; i++) {
        uint32_t netid = rd32(b + 0x48 + i * 4);
        s.slot[i].netId    = netid;
        s.slot[i].occupied = (netid != 0);
        uint32_t st = rd32(b + 0x30 + i * 4);
        s.slot[i].color     = (st >> 5) & 7;
        s.slot[i].resources = (st >> 3) & 3;
        s.slot[i].eden      = ((st >> 2) & 1) != 0;
        s.slot[i].ready     = ((st >> 1) & 1) != 0;
        char nm[14]; memcpy(nm, b + 0x60 + i * 0xD, 13); nm[13] = 0;
        s.slot[i].name = nm;
    }
    s.valid = true;
    return s;
}

const char* color_name(int c) {
    static const char* n[] = {"Blue","Red","Green","Yellow","Cyan","Magenta"};
    return (c >= 0 && c < 6) ? n[c] : "?";
}
const char* res_name(int r) {
    static const char* n[] = {"Low","Med","High"};
    return (r >= 0 && r < 3) ? n[r] : "?";
}
const char* scen_name(int c) {
    switch (c) {
        case 24: return "Last One Standing";
        case 25: return "Midas";
        case 26: return "Resource Race";
        case 27: return "Space Race";
        case 28: return "Land Rush";
    }
    return "Multiplayer";
}

} // namespace op2
