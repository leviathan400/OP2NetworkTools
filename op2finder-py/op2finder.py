#!/usr/bin/env python3
"""
op2finder - external LAN game-session finder for Outpost 2 (retail 1.3.6 / OPU 1.4.1).

Speaks OP2's UDP discovery protocol directly - no OP2 needed:
  * Broadcasts a 42-byte query to 255.255.255.255:47776.
  * Hosts running an open OP2 multiplayer session reply (81 bytes) from :47776.
  * We parse each reply: host IP, the game-creator's name, session GUID, ping.

Protocol reverse-engineered from Outpost2.exe:
  BroadcastForGames @0x4917E0, NetworkWaitForBroadcastReply @0x491A50,
  host responder FUN_0048c2c0 @0x48C2C0. See ..\docs\PROTOCOL.md for the full wire spec.

Usage:
    python op2finder.py            # live scan, refreshes every 3s
    python op2finder.py --once     # one scan, print results, exit
    python op2finder.py --raw      # also dump raw reply bytes (debug)
"""
import socket, struct, time, sys, argparse

DISCOVERY_PORT = 47776          # 0xBAA0
XORK           = 0xFDE24ACB
# OP2 game-type GUID, raw 16 bytes as stored/compared in Outpost2.exe
# ({5A55CF11-B841-11CE-9210-00AA006C4972}, Data1 little-endian).
GAME_GUID = bytes((0x11,0xCF,0x55,0x5A, 0x41,0xB8, 0xCE,0x11,
                   0x92,0x10,0x00,0xAA,0x00,0x6C,0x49,0x72))
QUERY_LEN = 42                  # 0x2A
REPLY_LEN = 81                  # 0x51

# Multiplayer mission-type code ((StartupFlags@0x2A >> 9) & 0x1F) -> name. Consecutive
# codes, ALL verified live by hosting each: 24=Last One Standing (0x3080), 25=Midas
# (0x3280), 26=Resource Race (0x3480), 27=Space Race (0x3680), 28=Land Rush (0x3880).
SCENARIO_NAMES = {24: "Last One Standing", 25: "Midas", 26: "Resource Race",
                  27: "Space Race", 28: "Land Rush"}

def scenario_name(code):
    return SCENARIO_NAMES.get(code, "Type %d" % code)


def _checksum(buf, start, ndwords, tail):
    """OP2 checksum: sum `ndwords` LE u32 from buf[start], + a 1- or 2-byte tail,
    XOR 0xFDE24ACB, masked to 32 bits. (FUN_00490eb0)"""
    s = 0
    off = start
    for _ in range(ndwords):
        s += struct.unpack_from('<I', buf, off)[0]
        off += 4
    if tail == 2:
        s += struct.unpack_from('<H', buf, off)[0]
    elif tail == 1:
        s += buf[off]
    return (s ^ XORK) & 0xFFFFFFFF


def build_query(token, reply_port):
    """42-byte discovery query (verified vs host responder @0x48C2C0, builder @0x4917E0).
    Layout: [0x00]cksum [0x04]type 0x1000 [0x06]16-byte GUID [0x16]reply port
    [0x1A]token. The host sends its reply to the port we name at [0x16], so it
    must be our own listening port or we never receive the reply."""
    q = bytearray(QUERY_LEN)
    struct.pack_into('<H', q, 0x04, 0x1000)                 # message type
    q[0x06:0x16] = GAME_GUID                                # game-type GUID (host matches here)
    struct.pack_into('<H', q, 0x16, reply_port & 0xFFFF)    # "reply to me on this port"
    struct.pack_into('<I', q, 0x1A, token & 0xFFFFFFFF)     # ping token (echoed at reply+0x06)
    struct.pack_into('<I', q, 0x00, _checksum(q, 0x04, 9, 2))   # checksum: 9 dwords + u16
    return bytes(q)


def parse_reply(data, src_ip, now_ms):
    if len(data) != REPLY_LEN:
        return None
    if struct.unpack_from('<H', data, 0x04)[0] != 0x1001:               # reply type
        return None
    if _checksum(data, 0x04, 19, 1) != struct.unpack_from('<I', data, 0x00)[0]:
        return None                                                     # bad checksum
    if data[0x0A:0x1A] != GAME_GUID:                                    # reply GUID at +0x0A
        return None
    echoed = struct.unpack_from('<I', data, 0x06)[0]                    # host echoed our token
    name   = data[0x42:0x50].split(b'\x00', 1)[0].decode('latin-1', 'replace').strip()
    sess   = data[0x1A:0x2A]
    sess_str = "%08X-%04X-%04X-%s" % (
        struct.unpack_from('<I', sess, 0)[0],
        struct.unpack_from('<H', sess, 4)[0],
        struct.unpack_from('<H', sess, 6)[0],
        sess[8:16].hex().upper())
    ping = (now_ms - echoed) & 0xFFFFFFFF                               # round-trip (ms)
    if ping > 60000:
        ping = None
    cfg  = struct.unpack_from('<I', data, 0x2A)[0]                      # StartupFlags bitfield
    maxp = struct.unpack_from('<I', data, 0x3A)[0]                      # max players
    scen = (cfg >> 9) & 0x1F                                            # missionType (bits 9-13)
    return {'ip': src_ip, 'name': name or '(no name)', 'session': sess_str,
            'ping': ping, 'max': maxp, 'scenario': scen}


def scan(listen_s, broadcast_addrs, sends=6, gap=0.2, listen_for=1.5, raw=False):
    """Broadcast the query a few times, collect replies. Returns {ip: info}."""
    port = listen_s.getsockname()[1]                # host replies to this port (query +0x16)
    found = {}
    deadline = time.time() + (sends * gap) + listen_for
    next_send, sent = time.time(), 0
    listen_s.setblocking(False)
    while time.time() < deadline:
        if sent < sends and time.time() >= next_send:
            q = build_query(int(time.time() * 1000) & 0xFFFFFFFF, port)  # fresh token each send
            for b in broadcast_addrs:
                try: listen_s.sendto(q, (b, DISCOVERY_PORT))
                except OSError: pass
            sent += 1
            next_send = time.time() + gap
        try:
            data, addr = listen_s.recvfrom(2048)
        except (BlockingIOError, OSError):
            time.sleep(0.02); continue
        if raw:
            print("  raw from %s: %s" % (addr[0], data.hex()))
        info = parse_reply(data, addr[0], int(time.time() * 1000))
        if info:
            found[info['ip']] = info
    return found


def broadcast_targets():
    """255.255.255.255 always; plus each interface's directed broadcast if resolvable."""
    targets = ['255.255.255.255']
    try:
        host = socket.gethostname()
        for ip in {ai[4][0] for ai in socket.getaddrinfo(host, None, socket.AF_INET)}:
            octets = ip.split('.')
            if len(octets) == 4 and not ip.startswith('127.'):
                targets.append('.'.join(octets[:3] + ['255']))   # /24 directed broadcast
    except OSError:
        pass
    return sorted(set(targets))


def make_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError: pass
    s.bind(('', 0))                       # ephemeral port on all interfaces
    return s


def print_table(found):
    if not found:
        print("  (no Outpost 2 sessions found)")
        return
    print("  %-15s  %-15s  %-3s  %-18s  %-6s  %s" %
          ("HOST IP", "GAME (creator)", "MAX", "SCENARIO TYPE", "PING", "SESSION"))
    print("  " + "-" * 84)
    for ip, g in sorted(found.items()):
        ping = ("%dms" % g['ping']) if g['ping'] is not None else "-"
        mp = str(g['max']) if g.get('max', 0) > 0 else "-"
        print("  %-15s  %-15s  %-3s  %-18s  %-6s  %s" %
              (ip, g['name'][:15], mp, scenario_name(g.get('scenario', -1)), ping, g['session']))


def main():
    ap = argparse.ArgumentParser(description="Outpost 2 LAN session finder")
    ap.add_argument('--once', action='store_true', help="scan once and exit")
    ap.add_argument('--raw', action='store_true', help="dump raw reply bytes")
    ap.add_argument('--interval', type=float, default=3.0, help="live refresh seconds")
    args = ap.parse_args()

    targets = broadcast_targets()
    print("op2finder - scanning LAN for Outpost 2 sessions (UDP :%d)" % DISCOVERY_PORT)
    print("broadcasting to: %s" % ", ".join(targets))
    s = make_socket()
    try:
        while True:
            found = scan(s, targets, raw=args.raw)
            ts = time.strftime("%H:%M:%S")
            print("\n[%s] %d session(s):" % (ts, len(found)))
            print_table(found)
            if args.once:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        s.close()


if __name__ == '__main__':
    main()
