// op2proto.h - pure Outpost 2 (retail 1.3.6 / OPU 1.4.1) wire-protocol helpers.
// Constants, byte readers/writers, the two checksum families, and the GameStartInfo
// decoder. No sockets, no state, no threads - just functions over byte buffers, so this
// layer is trivially testable and reusable. Byte-exact spec: ..\..\..\op2session\PROTOCOL.md.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace op2 {

// ---- ports & constants -----------------------------------------------------
static const uint32_t XORK            = 0xFDE24ACB;  // checksum constant (FUN_00490F10 family)
static const int      DISCOVERY_PORT  = 47776;       // 0xBAA0
static const int      JOIN_PORT       = 47777;       // 0xBAA1
static const int      GAME_PORT_LO    = 47800;       // 0xBAB8
static const int      GAME_PORT_HI    = 47807;       // 0xBABF
static const uint32_t TAPP_VERSION    = 0x01030004;  // TApp::Version "1.3.0.4"

// OP2 game-type GUID (constant), raw 16 bytes (used to validate discovery replies).
extern const uint8_t GAME_GUID[16];

// ---- little-endian byte access ---------------------------------------------
inline uint32_t rd32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
inline uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0]|(p[1]<<8)); }
inline void wr32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
inline void wr16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

// ---- checksums -------------------------------------------------------------
// Discovery checksum (FUN_00490EB0 family): sum of `ndw` LE dwords from buf[start] plus a
// 1- or 2-byte tail, XOR'd with XORK.
uint32_t disc_cksum(const uint8_t* buf, int start, int ndw, int tail);

// Full wire-packet checksum (FUN_00490F10). p = whole packet (14-byte header + payload).
uint32_t wire_cksum(const uint8_t* p);

// ---- decoded lobby state (from the 191-byte GameStartInfo, engine cmd 1/7/8) -----------
struct PlayerSlot {
    bool        occupied = false;
    std::string name;
    int         color = 0;       // 0 Blue,1 Red,2 Green,3 Yellow,4 Cyan,5 Magenta
    int         resources = 0;   // 0 Low,1 Med,2 High
    bool        eden = false;    // true Eden, false Plymouth
    bool        ready = false;
    uint32_t    netId = 0;
};

struct LobbyInfo {
    bool        valid = false;
    std::string mapDll;
    int         maxPlayers = 0;
    int         initVehicles = 0;
    int         gameSpeed = 0;
    bool        disasters = false, dayNight = false, morale = false;
    PlayerSlot  slot[6];
};

// Parse a GameStartInfo blob out of a wire packet (blob begins at wire 0x13). Returns a
// LobbyInfo with valid=false if the buffer is too small.
LobbyInfo parse_gsi(const uint8_t* p, int n);

// Human-readable names.
const char* color_name(int c);
const char* res_name(int r);
const char* scen_name(int c);   // discovery scenario/mission type

} // namespace op2
