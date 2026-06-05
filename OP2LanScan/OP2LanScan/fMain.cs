using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OP2LanScan
{
    public partial class fMain : Form
    {
        // Sessions currently shown, keyed by host IP. Bound to the grid.
        private readonly BindingList<Op2Session> _rows = new BindingList<Op2Session>();
        private readonly Dictionary<string, Op2Session> _byIp = new Dictionary<string, Op2Session>();
        private readonly System.Windows.Forms.Timer _autoTimer = new System.Windows.Forms.Timer();

        private bool _scanning;
        private DateTime _lastScan = DateTime.MinValue;

        // A "Looking at the IP address" target. When set, every scan also queries this
        // host directly (so a game on another subnet stays visible, not just one-shot).
        private IPAddress _pinnedIp;

        // Forget a session we haven't heard from in this long.
        private static readonly TimeSpan StaleAfter = TimeSpan.FromSeconds(12);

        public fMain()
        {
            InitializeComponent();

            // Window / taskbar icon from the embedded Plymouth.ico resource.
            using (var iconStream = System.Reflection.Assembly.GetExecutingAssembly()
                       .GetManifestResourceStream("OP2LanScan.Plymouth.ico"))
            {
                if (iconStream != null) this.Icon = new System.Drawing.Icon(iconStream);
            }

            dgvSessions.DataSource = _rows;

            _autoTimer.Interval = 3000;
            _autoTimer.Tick += async (s, e) => { if (chkAuto.Checked) await ScanAsync(false); };

            Load += async (s, e) =>
            {
                _autoTimer.Start();
                await ScanAsync(false);   // kick off an immediate first scan
            };
            FormClosing += (s, e) => _autoTimer.Stop();
        }

        // ---- UI events -------------------------------------------------------

        private async void btnScan_Click(object sender, EventArgs e)
        {
            await ScanAsync(true);   // manual scan: start from a clean list
        }

        // "Looking at the IP address": pin (or clear) a direct host to query each scan.
        private async void btnFindIp_Click(object sender, EventArgs e)
        {
            var text = txtIp.Text.Trim();
            if (text.Length == 0)
            {
                _pinnedIp = null;
                lblStatus.Text = "Direct IP cleared - broadcasting only.";
                await ScanAsync(true);
                return;
            }

            IPAddress ip;
            if (!IPAddress.TryParse(text, out ip))
            {
                try { ip = Dns.GetHostAddresses(text).FirstOrDefault(a => a.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork); }
                catch { ip = null; }
            }
            if (ip == null)
            {
                lblStatus.Text = "Could not resolve '" + text + "'.";
                return;
            }
            _pinnedIp = ip;
            await ScanAsync(true);   // clear the list, then query
        }

        private void chkAuto_CheckedChanged(object sender, EventArgs e)
        {
            if (chkAuto.Checked) _autoTimer.Start(); else _autoTimer.Stop();
        }

        // ---- scanning --------------------------------------------------------

        private async Task ScanAsync(bool clearFirst)
        {
            if (_scanning) return;
            _scanning = true;
            btnScan.Enabled = false;
            if (clearFirst)          // deliberate Query / Scan: start from an empty list
            {
                _byIp.Clear();
                _rows.Clear();
                _rows.ResetBindings();
            }
            var pinned = _pinnedIp;
            lblStatus.Text = pinned != null
                ? ("Scanning LAN + querying " + pinned + " …")
                : "Scanning LAN …";

            List<Op2Session> found;
            try
            {
                var cts = new CancellationTokenSource();
                found = await Task.Run(() => Op2Discovery.Scan(2200, true, pinned, cts.Token));
            }
            catch (Exception ex)
            {
                lblStatus.Text = "Scan error: " + ex.Message;
                _scanning = false;
                btnScan.Enabled = true;
                return;
            }

            Merge(found);
            _lastScan = DateTime.Now;
            UpdateStatus();

            _scanning = false;
            btnScan.Enabled = true;
        }

        private void Merge(List<Op2Session> found)
        {
            foreach (var s in found)
            {
                var key = s.HostIp.ToString();
                Op2Session existing;
                if (_byIp.TryGetValue(key, out existing))
                {
                    existing.Creator = s.Creator;
                    existing.SessionId = s.SessionId;
                    existing.MaxPlayers = s.MaxPlayers;
                    existing.ScenarioType = s.ScenarioType;
                    existing.PingMs = s.PingMs;
                    existing.LastSeen = s.LastSeen;
                }
                else
                {
                    _byIp[key] = s;
                    _rows.Add(s);
                }
            }

            // Age out sessions we haven't seen recently.
            var now = DateTime.Now;
            var stale = _byIp.Values.Where(v => now - v.LastSeen > StaleAfter).ToList();
            foreach (var v in stale)
            {
                _byIp.Remove(v.HostIp.ToString());
                _rows.Remove(v);
            }

            _rows.ResetBindings();
        }

        private void UpdateStatus()
        {
            int n = _rows.Count;
            string pin = "";
            if (_pinnedIp != null)
                pin = _byIp.ContainsKey(_pinnedIp.ToString())
                    ? "   •   " + _pinnedIp + ": GAME FOUND"
                    : "   •   " + _pinnedIp + ": no game hosted";
            lblStatus.Text = string.Format("{0} session{1} - last scan {2:HH:mm:ss}{3}",
                n, n == 1 ? "" : "s", _lastScan, pin);
        }
    }
}
