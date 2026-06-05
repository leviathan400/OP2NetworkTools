namespace OP2LanScan
{
    partial class fMain
    {
        private System.ComponentModel.IContainer components = null;

        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        private void InitializeComponent()
        {
            this.pnlTop = new System.Windows.Forms.Panel();
            this.btnScan = new System.Windows.Forms.Button();
            this.chkAuto = new System.Windows.Forms.CheckBox();
            this.lblFindIp = new System.Windows.Forms.Label();
            this.txtIp = new System.Windows.Forms.TextBox();
            this.btnFindIp = new System.Windows.Forms.Button();
            this.dgvSessions = new System.Windows.Forms.DataGridView();
            this.colHost = new System.Windows.Forms.DataGridViewTextBoxColumn();
            this.colCreator = new System.Windows.Forms.DataGridViewTextBoxColumn();
            this.colMax = new System.Windows.Forms.DataGridViewTextBoxColumn();
            this.colType = new System.Windows.Forms.DataGridViewTextBoxColumn();
            this.colPing = new System.Windows.Forms.DataGridViewTextBoxColumn();
            this.colSession = new System.Windows.Forms.DataGridViewTextBoxColumn();
            this.statusStrip = new System.Windows.Forms.StatusStrip();
            this.lblStatus = new System.Windows.Forms.ToolStripStatusLabel();
            this.pnlTop.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)(this.dgvSessions)).BeginInit();
            this.statusStrip.SuspendLayout();
            this.SuspendLayout();
            //
            // pnlTop
            //
            this.pnlTop.Controls.Add(this.btnScan);
            this.pnlTop.Controls.Add(this.chkAuto);
            this.pnlTop.Controls.Add(this.lblFindIp);
            this.pnlTop.Controls.Add(this.txtIp);
            this.pnlTop.Controls.Add(this.btnFindIp);
            this.pnlTop.Dock = System.Windows.Forms.DockStyle.Top;
            this.pnlTop.Location = new System.Drawing.Point(0, 0);
            this.pnlTop.Name = "pnlTop";
            this.pnlTop.Padding = new System.Windows.Forms.Padding(8, 8, 8, 8);
            this.pnlTop.Size = new System.Drawing.Size(820, 46);
            this.pnlTop.TabIndex = 0;
            //
            // btnScan
            //
            this.btnScan.Location = new System.Drawing.Point(11, 11);
            this.btnScan.Name = "btnScan";
            this.btnScan.Size = new System.Drawing.Size(96, 25);
            this.btnScan.TabIndex = 0;
            this.btnScan.Text = "Scan LAN";
            this.btnScan.UseVisualStyleBackColor = true;
            this.btnScan.Click += new System.EventHandler(this.btnScan_Click);
            //
            // chkAuto
            //
            this.chkAuto.AutoSize = true;
            this.chkAuto.Checked = true;
            this.chkAuto.CheckState = System.Windows.Forms.CheckState.Checked;
            this.chkAuto.Location = new System.Drawing.Point(120, 15);
            this.chkAuto.Name = "chkAuto";
            this.chkAuto.Size = new System.Drawing.Size(125, 17);
            this.chkAuto.TabIndex = 1;
            this.chkAuto.Text = "Auto-refresh (3s)";
            this.chkAuto.UseVisualStyleBackColor = true;
            this.chkAuto.CheckedChanged += new System.EventHandler(this.chkAuto_CheckedChanged);
            //
            // lblFindIp
            //
            this.lblFindIp.AutoSize = true;
            this.lblFindIp.Location = new System.Drawing.Point(300, 16);
            this.lblFindIp.Name = "lblFindIp";
            this.lblFindIp.Size = new System.Drawing.Size(80, 13);
            this.lblFindIp.TabIndex = 2;
            this.lblFindIp.Text = "Find at IP/host:";
            //
            // txtIp
            //
            this.txtIp.Location = new System.Drawing.Point(386, 13);
            this.txtIp.Name = "txtIp";
            this.txtIp.Size = new System.Drawing.Size(150, 20);
            this.txtIp.TabIndex = 3;
            //
            // btnFindIp
            //
            this.btnFindIp.Location = new System.Drawing.Point(542, 11);
            this.btnFindIp.Name = "btnFindIp";
            this.btnFindIp.Size = new System.Drawing.Size(75, 25);
            this.btnFindIp.TabIndex = 4;
            this.btnFindIp.Text = "Query";
            this.btnFindIp.UseVisualStyleBackColor = true;
            this.btnFindIp.Click += new System.EventHandler(this.btnFindIp_Click);
            //
            // dgvSessions
            //
            this.dgvSessions.AllowUserToAddRows = false;
            this.dgvSessions.AllowUserToDeleteRows = false;
            this.dgvSessions.AllowUserToResizeRows = false;
            this.dgvSessions.AutoGenerateColumns = false;
            this.dgvSessions.AutoSizeColumnsMode = System.Windows.Forms.DataGridViewAutoSizeColumnsMode.Fill;
            this.dgvSessions.BackgroundColor = System.Drawing.SystemColors.Window;
            this.dgvSessions.BorderStyle = System.Windows.Forms.BorderStyle.None;
            this.dgvSessions.ColumnHeadersHeightSizeMode = System.Windows.Forms.DataGridViewColumnHeadersHeightSizeMode.AutoSize;
            this.dgvSessions.Columns.AddRange(new System.Windows.Forms.DataGridViewColumn[] {
            this.colHost,
            this.colCreator,
            this.colMax,
            this.colType,
            this.colPing,
            this.colSession});
            this.dgvSessions.Dock = System.Windows.Forms.DockStyle.Fill;
            this.dgvSessions.EditMode = System.Windows.Forms.DataGridViewEditMode.EditProgrammatically;
            this.dgvSessions.Location = new System.Drawing.Point(0, 46);
            this.dgvSessions.Name = "dgvSessions";
            this.dgvSessions.ReadOnly = true;
            this.dgvSessions.RowHeadersVisible = false;
            this.dgvSessions.SelectionMode = System.Windows.Forms.DataGridViewSelectionMode.FullRowSelect;
            this.dgvSessions.Size = new System.Drawing.Size(820, 374);
            this.dgvSessions.TabIndex = 1;
            //
            // colHost
            //
            this.colHost.DataPropertyName = "HostText";
            this.colHost.FillWeight = 22F;
            this.colHost.HeaderText = "Host IP";
            this.colHost.Name = "colHost";
            this.colHost.ReadOnly = true;
            //
            // colCreator
            //
            this.colCreator.DataPropertyName = "Creator";
            this.colCreator.FillWeight = 20F;
            this.colCreator.HeaderText = "Game (creator)";
            this.colCreator.Name = "colCreator";
            this.colCreator.ReadOnly = true;
            //
            // colMax
            //
            this.colMax.DataPropertyName = "MaxPlayersText";
            this.colMax.FillWeight = 8F;
            this.colMax.HeaderText = "Max Players";
            this.colMax.Name = "colMax";
            this.colMax.ReadOnly = true;
            //
            // colType
            //
            this.colType.DataPropertyName = "ScenarioName";
            this.colType.FillWeight = 18F;
            this.colType.HeaderText = "Scenario Type";
            this.colType.Name = "colType";
            this.colType.ReadOnly = true;
            //
            // colPing
            //
            this.colPing.DataPropertyName = "PingText";
            this.colPing.FillWeight = 9F;
            this.colPing.HeaderText = "Ping";
            this.colPing.Name = "colPing";
            this.colPing.ReadOnly = true;
            //
            // colSession
            //
            this.colSession.DataPropertyName = "SessionId";
            this.colSession.FillWeight = 30F;
            this.colSession.HeaderText = "Session ID";
            this.colSession.Name = "colSession";
            this.colSession.ReadOnly = true;
            //
            // statusStrip
            //
            this.statusStrip.Items.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.lblStatus});
            this.statusStrip.Location = new System.Drawing.Point(0, 420);
            this.statusStrip.Name = "statusStrip";
            this.statusStrip.Size = new System.Drawing.Size(820, 22);
            this.statusStrip.TabIndex = 2;
            //
            // lblStatus
            //
            this.lblStatus.Name = "lblStatus";
            this.lblStatus.Size = new System.Drawing.Size(39, 17);
            this.lblStatus.Text = "Ready.";
            //
            // fMain
            //
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(820, 442);
            this.Controls.Add(this.dgvSessions);
            this.Controls.Add(this.statusStrip);
            this.Controls.Add(this.pnlTop);
            this.MinimumSize = new System.Drawing.Size(560, 280);
            this.Name = "fMain";
            this.Text = "OP2LanScan - Outpost 2 Session Finder";
            this.pnlTop.ResumeLayout(false);
            this.pnlTop.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)(this.dgvSessions)).EndInit();
            this.statusStrip.ResumeLayout(false);
            this.statusStrip.PerformLayout();
            this.ResumeLayout(false);
            this.PerformLayout();
        }

        #endregion

        private System.Windows.Forms.Panel pnlTop;
        private System.Windows.Forms.Button btnScan;
        private System.Windows.Forms.CheckBox chkAuto;
        private System.Windows.Forms.Label lblFindIp;
        private System.Windows.Forms.TextBox txtIp;
        private System.Windows.Forms.Button btnFindIp;
        private System.Windows.Forms.DataGridView dgvSessions;
        private System.Windows.Forms.DataGridViewTextBoxColumn colHost;
        private System.Windows.Forms.DataGridViewTextBoxColumn colCreator;
        private System.Windows.Forms.DataGridViewTextBoxColumn colMax;
        private System.Windows.Forms.DataGridViewTextBoxColumn colType;
        private System.Windows.Forms.DataGridViewTextBoxColumn colPing;
        private System.Windows.Forms.DataGridViewTextBoxColumn colSession;
        private System.Windows.Forms.StatusStrip statusStrip;
        private System.Windows.Forms.ToolStripStatusLabel lblStatus;
    }
}
