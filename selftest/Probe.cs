using System;
using System.Collections;
using System.Net;
using System.Reflection;
using System.Threading;

class P
{
    static void Main(string[] a)
    {
        var asm = Assembly.LoadFrom(@"D:\opu\op2network\OP2LanScan\OP2LanScan\bin\Debug\OP2LanScan.exe");
        var t = asm.GetType("OP2LanScan.Op2Discovery");
        var scan = t.GetMethod("Scan");

        IPAddress ip = (a.Length > 0 && a[0] != "-b") ? IPAddress.Parse(a[0]) : null;
        bool broadcast = (ip == null);
        Console.WriteLine(ip != null ? ("UNICAST probe -> " + ip + ":47776") : "BROADCAST probe");

        Action<string> raw = s => Console.WriteLine("  RAW reply from " + s);
        var cts = new CancellationTokenSource();
        var res = (IEnumerable)scan.Invoke(null, new object[] { 4000, broadcast, ip, cts.Token, raw });

        int n = 0;
        foreach (var s in res)
        {
            n++;
            var ty = s.GetType();
            Console.WriteLine("  FOUND host=" + ty.GetProperty("HostIp").GetValue(s)
                + "  creator=" + ty.GetProperty("Creator").GetValue(s)
                + "  max=" + ty.GetProperty("MaxPlayers").GetValue(s)
                + "  type=" + ty.GetProperty("ScenarioName").GetValue(s)
                + "  ping=" + ty.GetProperty("PingMs").GetValue(s)
                + "  session=" + ty.GetProperty("SessionId").GetValue(s));
        }
        Console.WriteLine("  => " + n + " session(s) parsed");
    }
}
