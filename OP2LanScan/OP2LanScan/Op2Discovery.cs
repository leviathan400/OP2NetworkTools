using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace OP2LanScan
{
    /// <summary>One advertised Outpost 2 multiplayer session, as seen on the LAN.
    /// These are PROPERTIES (not fields) so DataGridView column binding works.</summary>
    public class Op2Session
    {
        public IPAddress HostIp { get; set; }       // datagram source of the reply
        public string Creator { get; set; }          // game-creator's name (reply +0x42)
        public string SessionId { get; set; }        // session GUID (reply +0x1A)
        public int MaxPlayers { get; set; }          // reply +0x3A
        public int ScenarioType { get; set; }        // reply +0x3E (game-rules code)
        public int PingMs { get; set; }              // round-trip of echoed token, -1 if unknown
        public DateTime LastSeen { get; set; }

        public string HostText { get { return HostIp == null ? "" : HostIp.ToString(); } }
        public string PingText { get { return PingMs >= 0 ? PingMs + " ms" : "-"; } }
        public string MaxPlayersText { get { return MaxPlayers > 0 ? MaxPlayers.ToString() : "-"; } }
        public string ScenarioName { get { return Op2Discovery.ScenarioTypeName(ScenarioType); } }
    }

    /// <summary>
    /// Outpost 2 LAN discovery protocol (retail 1.3.6 / OPU 1.4.1), reverse-engineered
    /// from Outpost2.exe: query builder BroadcastForGames @0x4917E0, reply parser
    /// NetworkWaitForBroadcastReply @0x491A50, host responder @0x48C2C0, checksum
    /// FUN_00490EB0. Pure managed Winsock - no Outpost 2 needed.
    /// </summary>
    public static class Op2Discovery
    {
        public const int DiscoveryPort = 47776;     // 0xBAA0
        private const uint Xor = 0xFDE24ACB;
        private const int QueryLen = 42;            // 0x2A
        private const int ReplyLen = 81;            // 0x51

        // OP2 game-type GUID, raw wire bytes (as they appear at query/reply +0x0A):
        // {5A55CF11-B841-11CE-9210-00AA006C4972}
        private static readonly byte[] GameGuid =
        {
            0x11,0xCF,0x55,0x5A, 0x41,0xB8, 0xCE,0x11,
            0x92,0x10,0x00,0xAA,0x00,0x6C,0x49,0x72
        };

        /// <summary>OP2 checksum: sum ndw LE u32 from buf[start] + 1/2-byte tail, XOR 0xFDE24ACB.</summary>
        private static uint Checksum(byte[] b, int start, int ndw, int tail)
        {
            uint s = 0;
            int off = start;
            for (int i = 0; i < ndw; i++) { s += BitConverter.ToUInt32(b, off); off += 4; }
            if (tail == 2) s += BitConverter.ToUInt16(b, off);
            else if (tail == 1) s += b[off];
            return s ^ Xor;
        }

        // Query layout (verified against host responder @0x48C2C0 and builder @0x4917E0):
        //   [0x00] checksum  [0x04] type 0x1000  [0x06] 16-byte GUID  [0x16] reply port (u16)
        //   [0x1A] ping token.  The host sends its reply to the port we name at [0x16],
        //   so we must put our own listening port there (else we never receive the reply).
        private static byte[] BuildQuery(uint token, ushort replyPort)
        {
            var q = new byte[QueryLen];
            q[0x04] = 0x00; q[0x05] = 0x10;                              // message type 0x1000
            Array.Copy(GameGuid, 0, q, 0x06, 16);                       // game-type GUID (host matches here)
            BitConverter.GetBytes(replyPort).CopyTo(q, 0x16);           // "reply to me on this port"
            BitConverter.GetBytes(token).CopyTo(q, 0x1A);               // ping token (echoed at reply+0x06)
            BitConverter.GetBytes(Checksum(q, 0x04, 9, 2)).CopyTo(q, 0x00); // checksum: 9 dwords + u16
            return q;
        }

        private static Op2Session ParseReply(byte[] d, int len, IPAddress src, uint token)
        {
            if (len != ReplyLen) return null;
            if (BitConverter.ToUInt16(d, 0x04) != 0x1001) return null;              // reply type
            if (Checksum(d, 0x04, 19, 1) != BitConverter.ToUInt32(d, 0x00)) return null;
            for (int i = 0; i < 16; i++) if (d[0x0A + i] != GameGuid[i]) return null; // OP2 GUID

            // The host echoes our query token (a TickCount) at +0x06, so now - echoed
            // is the round-trip. (token arg kept for API symmetry; echo is authoritative.)
            uint echoed = BitConverter.ToUInt32(d, 0x06);
            int ping = unchecked((int)((uint)Environment.TickCount - echoed));
            if (ping < 0 || ping > 60000) ping = -1;

            int nlen = 0;
            while (nlen < 14 && d[0x42 + nlen] != 0) nlen++;
            string name = Encoding.ASCII.GetString(d, 0x42, nlen).Trim();
            if (name.Length == 0) name = "(no name)";

            string sess = string.Format("{0:X8}-{1:X4}-{2:X4}-{3}",
                BitConverter.ToUInt32(d, 0x1A), BitConverter.ToUInt16(d, 0x1E),
                BitConverter.ToUInt16(d, 0x20), BitConverter.ToString(d, 0x22, 8).Replace("-", ""));

            // Game settings are bit-packed in the StartupFlags dword at reply +0x2A.
            // maxPlayers is the plain dword at +0x3A. The mission-type lives in the bits
            // above maxPlayers; (cfg >> 9) & 0x1F gives a stable per-mode code.
            uint cfg = BitConverter.ToUInt32(d, 0x2A);

            return new Op2Session
            {
                HostIp = src, Creator = name, SessionId = sess,
                MaxPlayers = (int)BitConverter.ToUInt32(d, 0x3A),
                ScenarioType = (int)((cfg >> 9) & 0x1F),
                PingMs = ping, LastSeen = DateTime.Now
            };
        }

        // Multiplayer mission-type code ((StartupFlags >> 9) & 0x1F) -> PICK SESSION name.
        // Codes are consecutive (config 0x2B byte steps by 2 per mode). ALL VERIFIED live
        // by hosting each: 24 = Last One Standing (0x3080), 25 = Midas (0x3280),
        // 26 = Resource Race (0x3480), 27 = Space Race (0x3680), 28 = Land Rush (0x3880).
        private static readonly Dictionary<int, string> ScenarioNames = new Dictionary<int, string>
        {
            { 24, "Last One Standing" },
            { 25, "Midas" },
            { 26, "Resource Race" },
            { 27, "Space Race" },
            { 28, "Land Rush" },
        };

        public static string ScenarioTypeName(int code)
        {
            string name;
            return ScenarioNames.TryGetValue(code, out name) ? name : ("Type " + code);
        }

        /// <summary>255.255.255.255 plus each local NIC's /24 directed broadcast.</summary>
        public static List<IPEndPoint> BroadcastTargets()
        {
            var seen = new HashSet<string>();
            var list = new List<IPEndPoint>();
            void Add(string ip) { if (seen.Add(ip)) list.Add(new IPEndPoint(IPAddress.Parse(ip), DiscoveryPort)); }
            Add("255.255.255.255");
            try
            {
                foreach (var ip in Dns.GetHostAddresses(Dns.GetHostName()))
                {
                    if (ip.AddressFamily != AddressFamily.InterNetwork) continue;
                    if (IPAddress.IsLoopback(ip)) continue;
                    var o = ip.GetAddressBytes();
                    Add(o[0] + "." + o[1] + "." + o[2] + ".255");
                }
            }
            catch { }
            return list;
        }

        /// <summary>
        /// Send the discovery query and collect replies for ~durationMs. Synchronous and
        /// blocking - call from a background thread. If <paramref name="broadcast"/> is true
        /// it sweeps the LAN (255.255.255.255 + each NIC's /24); if <paramref name="unicastIp"/>
        /// is non-null it ALSO queries that single host directly (the "Looking at the IP
        /// address" mode - works cross-subnet / over a VPN where broadcast can't reach).
        /// Both can be combined in one pass.
        /// </summary>
        public static List<Op2Session> Scan(int durationMs, bool broadcast, IPAddress unicastIp,
                                            CancellationToken ct, Action<string> rawLog = null)
        {
            var results = new Dictionary<string, Op2Session>();
            using (var sock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            {
                sock.EnableBroadcast = true;
                try { sock.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true); } catch { }
                sock.Bind(new IPEndPoint(IPAddress.Any, 0));
                ushort replyPort = (ushort)((IPEndPoint)sock.LocalEndPoint).Port;

                var targets = new List<IPEndPoint>();
                if (broadcast) targets.AddRange(BroadcastTargets());
                if (unicastIp != null) targets.Add(new IPEndPoint(unicastIp, DiscoveryPort));
                if (targets.Count == 0) return new List<Op2Session>();

                var buf = new byte[2048];
                const int sends = 6, gapMs = 200;
                int sent = 0, nextSend = 0;
                var sw = Stopwatch.StartNew();

                while (sw.ElapsedMilliseconds < durationMs && !ct.IsCancellationRequested)
                {
                    if (sent < sends && sw.ElapsedMilliseconds >= nextSend)
                    {
                        // Fresh token each send so the echoed value gives an accurate RTT.
                        byte[] q = BuildQuery((uint)Environment.TickCount, replyPort);
                        foreach (var ep in targets)
                            try { sock.SendTo(q, ep); } catch { }
                        sent++;
                        nextSend = (int)sw.ElapsedMilliseconds + gapMs;
                    }

                    if (sock.Poll(50000, SelectMode.SelectRead)) // 50 ms
                    {
                        EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                        int len;
                        try { len = sock.ReceiveFrom(buf, ref from); }
                        catch { continue; }
                        var src = ((IPEndPoint)from).Address;

                        if (rawLog != null)
                        {
                            var sb = new StringBuilder();
                            for (int i = 0; i < len; i++) sb.Append(buf[i].ToString("x2"));
                            rawLog(src + " (" + len + "): " + sb);
                        }

                        var s = ParseReply(buf, len, src, 0u);
                        if (s != null) results[src.ToString()] = s;
                    }
                }
            }
            return new List<Op2Session>(results.Values);
        }
    }
}
