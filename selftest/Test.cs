using System;
using System.Net;
using System.Reflection;
using System.Text;

class T
{
    // OP2 game-type GUID, raw 16 bytes as stored in Outpost2.exe (Data1 little-endian).
    static readonly byte[] GAME = {0x11,0xCF,0x55,0x5A,0x41,0xB8,0xCE,0x11,0x92,0x10,0x00,0xAA,0x00,0x6C,0x49,0x72};
    const uint XOR = 0xFDE24ACB;

    static uint Cksum(byte[] b, int start, int ndw, int tail)
    {
        uint s = 0; int off = start;
        for (int i = 0; i < ndw; i++) { s += BitConverter.ToUInt32(b, off); off += 4; }
        if (tail == 2) s += BitConverter.ToUInt16(b, off);
        else if (tail == 1) s += b[off];
        return s ^ XOR;
    }

    static void Eq(string what, object a, object b)
    {
        if (!a.Equals(b)) throw new Exception("FAIL " + what + ": got " + a + " expected " + b);
        Console.WriteLine("  ok  " + what + " = " + a);
    }

    static void Main()
    {
        var asm = Assembly.LoadFrom(@"D:\opu\op2network\OP2LanScan\OP2LanScan\bin\Debug\OP2LanScan.exe");
        var t = asm.GetType("OP2LanScan.Op2Discovery");
        var build = t.GetMethod("BuildQuery", BindingFlags.NonPublic | BindingFlags.Static);
        var parse = t.GetMethod("ParseReply", BindingFlags.NonPublic | BindingFlags.Static);

        // 1) Query is something a real host accepts (checksum/type/GUID@0x06/port@0x16/token@0x1A).
        uint token = (uint)Environment.TickCount;
        ushort replyPort = 50001;
        byte[] q = (byte[])build.Invoke(null, new object[] { token, replyPort });
        Eq("query length", q.Length, 42);
        Eq("query type 0x1000", BitConverter.ToUInt16(q, 4), (ushort)0x1000);
        for (int i = 0; i < 16; i++) if (q[0x06 + i] != GAME[i]) throw new Exception("query GUID@0x06 mismatch");
        Console.WriteLine("  ok  query GUID at +0x06 matches");
        Eq("query reply-port @0x16", BitConverter.ToUInt16(q, 0x16), replyPort);
        Eq("query token @0x1A", BitConverter.ToUInt32(q, 0x1A), token);
        Eq("query checksum (host recompute)", Cksum(q, 4, 9, 2), BitConverter.ToUInt32(q, 0));

        // 2) Build a synthetic host reply and parse it back (GUID@0x0A, session@0x1A, name@0x42).
        byte[] r = new byte[81];
        r[4] = 0x01; r[5] = 0x10;                          // reply type 0x1001
        BitConverter.GetBytes(token).CopyTo(r, 6);         // echoed token
        Array.Copy(GAME, 0, r, 0x0A, 16);                  // game GUID at reply +0x0A
        for (int i = 0; i < 16; i++) r[0x1A + i] = (byte)(0x10 + i);   // session GUID
        BitConverter.GetBytes((uint)0x00003280).CopyTo(r, 0x2A);       // StartupFlags: (>>9)&1F = 25 -> Midas
        BitConverter.GetBytes((uint)4).CopyTo(r, 0x3A);                // max players
        Encoding.ASCII.GetBytes("Leviathan").CopyTo(r, 0x42);          // creator name
        BitConverter.GetBytes(Cksum(r, 4, 19, 1)).CopyTo(r, 0);        // checksum

        object sess = parse.Invoke(null, new object[] { r, 81, IPAddress.Parse("192.168.1.50"), 0u });
        if (sess == null) throw new Exception("ParseReply returned null on a VALID reply!");
        var ty = sess.GetType();
        Eq("parsed creator", (string)ty.GetProperty("Creator").GetValue(sess), "Leviathan");
        Eq("parsed host ip", ty.GetProperty("HostIp").GetValue(sess).ToString(), "192.168.1.50");
        Eq("parsed maxPlayers", (int)ty.GetProperty("MaxPlayers").GetValue(sess), 4);
        Eq("parsed scenario name", (string)ty.GetProperty("ScenarioName").GetValue(sess), "Midas");
        Console.WriteLine("  ok  session id = " + (string)ty.GetProperty("SessionId").GetValue(sess));

        // 3) Negative cases must be rejected.
        byte[] bad = (byte[])r.Clone(); bad[0] ^= 0xFF;
        if (parse.Invoke(null, new object[] { bad, 81, IPAddress.Parse("1.2.3.4"), 0u }) != null)
            throw new Exception("accepted a corrupt checksum!");
        Console.WriteLine("  ok  bad checksum rejected");

        byte[] wrongType = (byte[])r.Clone(); wrongType[4] = 0x00; wrongType[5] = 0x10;
        BitConverter.GetBytes(Cksum(wrongType, 4, 19, 1)).CopyTo(wrongType, 0);
        if (parse.Invoke(null, new object[] { wrongType, 81, IPAddress.Parse("1.2.3.4"), 0u }) != null)
            throw new Exception("accepted wrong message type!");
        Console.WriteLine("  ok  wrong message type rejected");

        if (parse.Invoke(null, new object[] { r, 80, IPAddress.Parse("1.2.3.4"), 0u }) != null)
            throw new Exception("accepted wrong length!");
        Console.WriteLine("  ok  wrong length rejected");

        Console.WriteLine("\nALL TESTS PASSED");
    }
}
