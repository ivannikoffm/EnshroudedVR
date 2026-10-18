// EnshroudedVR-Config — desktop settings tool for the EnshroudedVR mod.
// Edits enshrouded_vr\vr.ini (same folder as this exe). The mod re-reads vr.ini within ~0.5 s, so changes apply live.
// Build: csc /target:winexe /out:EnshroudedVR-Config.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll EnshroudedVRConfig.cs
// C# 5 only (the .NET Framework csc): no $"", no ?., no nameof.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

static class Ini {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool WritePrivateProfileString(string s, string k, string v, string f);
    public static string Get(string f, string s, string k, string d) { var sb = new StringBuilder(64); GetPrivateProfileString(s, k, d, sb, 64, f); return sb.ToString(); }
    public static int GetInt(string f, string k, int d) { int v; return int.TryParse(Get(f, "vr", k, d.ToString()).Trim(), out v) ? v : d; }
    public static void Set(string f, string s, string k, string v) { WritePrivateProfileString(s, k, v, f); }
}

static class KeyNames {   // MUST match kKeyNames in xr.cpp
    public static readonly KeyValuePair<string, int>[] Table = {
        P("None", 0), P("Insert", 0x2D), P("Home", 0x24), P("PageUp", 0x21), P("PageDown", 0x22), P("End", 0x23), P("Delete", 0x2E), P("Pause", 0x13),
        P("ScrollLock", 0x91), P("Up", 0x26), P("Down", 0x28), P("Left", 0x25), P("Right", 0x27),
        P("Num0", 0x60), P("Num1", 0x61), P("Num2", 0x62), P("Num3", 0x63), P("Num4", 0x64), P("Num5", 0x65), P("Num6", 0x66), P("Num7", 0x67),
        P("Num8", 0x68), P("Num9", 0x69), P("Num+", 0x6B), P("Num-", 0x6D), P("Num*", 0x6A), P("Num/", 0x6F), P("NumDel", 0x6E),
        P("[", 0xDB), P("]", 0xDD), P("'", 0xDE), P(",", 0xBC), P(".", 0xBE), P("/", 0xBF), P("\\", 0xDC), P("-", 0xBD), P("=", 0xBB), P("`", 0xC0),
        P("F1", 0x70), P("F2", 0x71), P("F3", 0x72), P("F4", 0x73), P("F5", 0x74), P("F6", 0x75), P("F7", 0x76), P("F8", 0x77), P("F9", 0x78),
        P("F10", 0x79), P("F11", 0x7A), P("F12", 0x7B) };
    static KeyValuePair<string, int> P(string n, int v) { return new KeyValuePair<string, int>(n, v); }
    public static string Name(int vk) {
        foreach (var p in Table) if (p.Value == vk) return p.Key;
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return ((char)vk).ToString();
        return "0x" + vk.ToString("x2");
    }
    public static int Parse(string s) {
        s = s.Trim(); if (s.Length == 0) return 0;
        foreach (var p in Table) if (string.Equals(p.Key, s, StringComparison.OrdinalIgnoreCase)) return p.Value;
        if (s.Length == 1) { char c = char.ToUpperInvariant(s[0]); if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; }
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) { try { return Convert.ToInt32(s.Substring(2), 16) & 0xff; } catch { } }
        return -1;
    }
}

class Act { public string Id, Def, Text; public Act(string i, string d, string t) { Id = i; Def = d; Text = t; } }

class MainForm : Form {
    // [keys] actions, MUST match kActs in xr.cpp (public defaults). Diagnostic-only actions are not listed.
    static readonly Act[] Acts = {
        new Act("recenter", "Home", "Recenter view"),
        new Act("ipd_up", "Num+", "Stereo strength (IPD) +10%"), new Act("ipd_down", "Num-", "Stereo strength (IPD) -10%"),
        new Act("ipd_up_alt", "]", "Stereo strength + (alternative key)"), new Act("ipd_down_alt", "[", "Stereo strength - (alternative key)"),
        new Act("ipd_reset", "Num0", "Stereo strength reset to 100%"),
        new Act("cam_closer", "Num4", "Camera closer (-10%)"), new Act("cam_farther", "Num6", "Camera farther (+10%)"), new Act("cam_reset", "Num3", "Camera distance reset"),
        new Act("hud_mode", "NumDel", "HUD: in image / panel 55\u00b0 / panel 70\u00b0"),
        new Act("fov_toggle", "Num*", "Headset FOV on/off"), new Act("fov_fill_fit", "Num/", "FOV mode FILL / FIT"),
        new Act("head_look", "Num5", "Head look on/off (off = fixed stereo screen)"), new Act("six_dof", "Num8", "6DoF head position on/off"),
        new Act("toggle_tracking", "Insert", "Head tracking on/off"), new Act("stereo_mode", "ScrollLock", "Stereo / swapped eyes / mono"),
        new Act("pose_lag", "None", "(advanced) pose lag 0..3"), new Act("pair_lock", "None", "(advanced) eye pair lock"),
        new Act("yaw_sign", "None", "(advanced) flip yaw"), new Act("pitch_sign", "None", "(advanced) flip pitch"),
        new Act("roll_mode", "None", "(advanced) roll mode"), new Act("pair_rate", "None", "(advanced) pair rate"),
    };
    readonly string ini, dir;
    DateTime lastWrite; bool loading;
    readonly Dictionary<string, TrackBar> bars = new Dictionary<string, TrackBar>();
    readonly Dictionary<string, Label> barVals = new Dictionary<string, Label>();
    readonly Dictionary<string, CheckBox> checks = new Dictionary<string, CheckBox>();
    readonly Dictionary<string, ComboBox> combos = new Dictionary<string, ComboBox>();
    readonly Dictionary<string, int[]> comboVals = new Dictionary<string, int[]>();
    readonly Dictionary<string, int> defaults = new Dictionary<string, int>();
    readonly Dictionary<string, ComboBox> sCombos = new Dictionary<string, ComboBox>();   // string-valued settings
    readonly Dictionary<string, string[]> sVals = new Dictionary<string, string[]>();
    readonly Dictionary<string, string> sDefs = new Dictionary<string, string>();
    readonly HashSet<string> pending = new HashSet<string>();
    readonly Timer debounce = new Timer(), watch = new Timer();
    ListView keyList; Label status; CheckBox flatMode; public TabControl Tabs;

    public MainForm(string iniPath) {
        ini = iniPath; dir = Path.GetDirectoryName(iniPath);
        Text = "EnshroudedVR settings"; Font = new Font("Segoe UI", 9.5f); AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(620, 590); FormBorderStyle = FormBorderStyle.FixedSingle; MaximizeBox = false; StartPosition = FormStartPosition.CenterScreen;
        Tabs = new TabControl { Dock = DockStyle.Top, Height = 468 };
        Tabs.TabPages.Add(BuildComfort()); Tabs.TabPages.Add(BuildDisplay()); Tabs.TabPages.Add(BuildKeys()); Tabs.TabPages.Add(BuildHelp());
        Controls.Add(Tabs);
        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 118 };
        var preset = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Left = 12, Top = 10, Width = 210 };
        preset.Items.AddRange(new object[] { "Preset: Defaults", "Preset: Deep stereo", "Preset: Combat (HUD 55\u00b0)", "Preset: Exploring (HUD 70\u00b0)" });
        preset.SelectedIndex = 0;
        var apply = new Button { Text = "Apply preset", Left = 230, Top = 9, Width = 110 };
        apply.Click += delegate { ApplyPreset(preset.SelectedIndex); };
        var log = new Button { Text = "Open log", Left = 350, Top = 9, Width = 100 };
        log.Click += delegate { var f = Path.Combine(dir, "vrlog.txt"); if (File.Exists(f)) Process.Start("notepad.exe", "\"" + f + "\""); else SetStatus("No vrlog.txt yet - start the game once."); };
        var folder = new Button { Text = "Open folder", Left = 460, Top = 9, Width = 110 };
        folder.Click += delegate { Process.Start("explorer.exe", "\"" + dir + "\""); };
        flatMode = new CheckBox { Text = "Disable VR (play flat) - takes effect on next game start", Left = 12, Top = 44, Width = 560, AutoSize = true };
        flatMode.Checked = File.Exists(Path.Combine(dir, "xr_off.txt"));
        flatMode.CheckedChanged += delegate { var f = Path.Combine(dir, "xr_off.txt"); if (flatMode.Checked) File.WriteAllText(f, "delete this file to enable VR\r\n"); else if (File.Exists(f)) File.Delete(f); SetStatus(flatMode.Checked ? "VR disabled for the next start." : "VR enabled for the next start."); };
        status = new Label { Left = 12, Top = 68, Width = 596, Height = 44, AutoSize = false, ForeColor = Color.DimGray };
        bottom.Controls.AddRange(new Control[] { preset, apply, log, folder, flatMode, status });
        Controls.Add(bottom);
        debounce.Interval = 300; debounce.Tick += delegate { debounce.Stop(); Flush(); };
        watch.Interval = 1000; watch.Tick += delegate { CheckExternal(); }; watch.Start();
        FixButtons(this);
        LoadAll();
        SetStatus(File.Exists(ini) ? "Editing " + ini + "  -  changes apply live while the game runs." : "vr.ini will be created at " + ini);
    }

    static void FixButtons(Control c) { foreach (Control k in c.Controls) { if (k is Button) k.Height = 30; FixButtons(k); } }
    TabPage BuildComfort() {
        var p = new TabPage("Comfort") { AutoScroll = true, Padding = new Padding(8) }; int y = 12;
        AddBar(p, ref y, "ipd_pct", "Stereo strength (IPD)", 10, 400, 10, 100, "%", "Higher = stronger 3D but the world looks smaller (toy-like).");
        AddBar(p, ref y, "cam_dist_pct", "Camera distance (3rd person)", 20, 150, 10, 100, "%", "Closer camera = more depth without shrinking the world.");
        AddCombo(p, ref y, "hud_mode", "HUD", new[] { "In the game image", "Panel 55\u00b0 (combat)", "Panel 70\u00b0 (exploring)" }, new[] { 0, 1, 2 }, 1);
        AddBar(p, ref y, "hud_dist_cm", "HUD panel distance", 50, 500, 10, 150, " cm", null);
        AddCheck(p, ref y, "fov_override", "Use the headset's full field of view", 1);
        AddCombo(p, ref y, "fov_axis", "FOV mode", new[] { "FILL - full height (wider, softer)", "FIT - full width (sharper, bands)" }, new[] { 0, 1 }, 0);
        AddCheck(p, ref y, "head_look", "Head turns the camera (off = fixed stereo screen, mouse only)", 1);
        AddCheck(p, ref y, "head_pos", "6DoF: lean with your head (positional tracking)", 0);
        AddCheck(p, ref y, "hud_sync", "HUD from one eye (stops doubled world prompts)", 1);
        AddCombo(p, ref y, "stereo", "Stereo", new[] { "Stereo", "Stereo (swap eyes)", "Mono" }, new[] { 1, -1, 0 }, 1);
        return p;
    }

    TabPage BuildDisplay() {
        var p = new TabPage("Display & Headset") { AutoScroll = true, Padding = new Padding(8) }; int y = 12;
        AddSCombo(p, ref y, "force_window", "Game frame size", new[] {
            "Square 2160 x 2160 (recommended)", "Square 1620 x 1620 (weaker GPU)", "Square 2560 x 2560 (RTX 4080/5080+)",
            "4:3  2496 x 1872 (menus closer to normal)", "Off - use the game's own resolution" },
            new[] { "2160x2160", "1620x1620", "2560x2560", "2496x1872", "0" }, "2160x2160");
        p.Controls.Add(new Label { Text = "Works only when the game's Display Mode is WINDOWED (Settings > Graphics). Headsets see a nearly " +
            "square field of view, so a square frame gives ~33% more sharpness for the same GPU cost. The game window becomes larger " +
            "than your monitor - that's expected. Restart the game after changing.", Left = 12, Top = y, Width = 580, Height = 64, ForeColor = Color.DimGray, Font = new Font("Segoe UI", 8.5f) });
        y += 70;
        AddCheck(p, ref y, "layer_eyefov", "Fit the image to each eye's lens (required for Quest 3 and other angled-lens headsets; keep on)", 1);
        AddSCombo(p, ref y, "xr_runtime", "OpenXR runtime", new[] {
            "Auto (active runtime, then Meta, then Virtual Desktop)", "Meta Quest Link / Air Link", "Virtual Desktop (VDXR)", "SteamVR", "PICO Connect", "Windows active runtime only" },
            new[] { "auto", "meta", "vd", "steamvr", "pico", "active" }, "auto");
        p.Controls.Add(new Label { Text = "Auto picks the first runtime that has a headset connected. Choose explicitly if you have several VR apps installed " +
            "and the wrong one starts. Takes effect on the next game start.", Left = 12, Top = y, Width = 580, Height = 48, ForeColor = Color.DimGray, Font = new Font("Segoe UI", 8.5f) });
        y += 54;
        AddBar(p, ref y, "fov_pct", "Rendered FOV (of headset)", 60, 100, 5, 100, "%", "Lower = sharper centre and faster, black edges in the headset.");
        return p;
    }
    void AddSCombo(TabPage p, ref int y, string key, string label, string[] items, string[] vals, string def) {
        p.Controls.Add(new Label { Text = label, Left = 12, Top = y + 3, Width = 160 });
        var c = new ComboBox { Left = 175, Top = y, Width = 415, DropDownStyle = ComboBoxStyle.DropDownList };
        c.Items.AddRange(items); c.SelectedIndexChanged += delegate { Changed(key); };
        p.Controls.Add(c); sCombos[key] = c; sVals[key] = vals; sDefs[key] = def; y += 34;
    }
    void AddBar(TabPage p, ref int y, string key, string label, int min, int max, int step, int def, string unit, string hint) {
        p.Controls.Add(new Label { Text = label, Left = 12, Top = y + 4, Width = 210 });
        var tb = new TrackBar { Left = 225, Top = y, Width = 290, Minimum = min / step, Maximum = max / step, TickFrequency = Math.Max(1, (max - min) / step / 10), SmallChange = 1, LargeChange = 1, Tag = step };
        var val = new Label { Left = 520, Top = y + 4, Width = 70, Font = new Font(Font, FontStyle.Bold) };
        tb.ValueChanged += delegate { val.Text = (tb.Value * step) + unit; Changed(key); };
        p.Controls.Add(tb); p.Controls.Add(val); bars[key] = tb; barVals[key] = val; defaults[key] = def;
        val.Tag = unit;
        y += 40;
        if (hint != null) { p.Controls.Add(new Label { Text = hint, Left = 225, Top = y - 6, Width = 370, ForeColor = Color.DimGray, Font = new Font("Segoe UI", 8.5f) }); y += 20; }
    }
    void AddCheck(TabPage p, ref int y, string key, string label, int def) {
        var cb = new CheckBox { Text = label, Left = 12, Top = y, Width = 570 };
        cb.CheckedChanged += delegate { Changed(key); };
        p.Controls.Add(cb); checks[key] = cb; defaults[key] = def; y += 30;
    }
    void AddCombo(TabPage p, ref int y, string key, string label, string[] items, int[] vals, int def) {
        p.Controls.Add(new Label { Text = label, Left = 12, Top = y + 3, Width = 210 });
        var c = new ComboBox { Left = 225, Top = y, Width = 290, DropDownStyle = ComboBoxStyle.DropDownList };
        c.Items.AddRange(items); c.SelectedIndexChanged += delegate { Changed(key); };
        p.Controls.Add(c); combos[key] = c; comboVals[key] = vals; defaults[key] = def; y += 34;
    }

    TabPage BuildKeys() {
        var p = new TabPage("Keys") { Padding = new Padding(8) };
        keyList = new ListView { View = View.Details, FullRowSelect = true, Left = 8, Top = 8, Width = 590, Height = 370, HideSelection = false, MultiSelect = false };
        keyList.Columns.Add("Action", 410); keyList.Columns.Add("Key", 150);
        foreach (var a in Acts) keyList.Items.Add(new ListViewItem(new[] { a.Text, a.Def }) { Tag = a });
        var change = new Button { Text = "Change key...", Left = 8, Top = 386, Width = 130 };
        change.Click += delegate { ChangeKey(); };
        keyList.DoubleClick += delegate { ChangeKey(); };
        var clear = new Button { Text = "Unbind", Left = 146, Top = 386, Width = 100 };
        clear.Click += delegate { if (keyList.SelectedItems.Count == 1) SetKey(keyList.SelectedItems[0], 0); };
        var reset = new Button { Text = "Default keys", Left = 254, Top = 386, Width = 120 };
        reset.Click += delegate { foreach (ListViewItem it in keyList.Items) SetKey(it, KeyNames.Parse(((Act)it.Tag).Def)); };
        p.Controls.AddRange(new Control[] { keyList, change, clear, reset });
        p.Controls.Add(new Label { Text = "No numpad? Use F-keys. Avoid Ctrl (= sneak).", Left = 384, Top = 386, Width = 220, Height = 40, ForeColor = Color.DimGray, Font = new Font("Segoe UI", 8.5f) });
        return p;
    }
    void ChangeKey() {
        if (keyList.SelectedItems.Count != 1) { SetStatus("Select an action first."); return; }
        var it = keyList.SelectedItems[0];
        using (var dlg = new Form { Text = "Press a key", ClientSize = new Size(360, 90), FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, KeyPreview = true, MinimizeBox = false, MaximizeBox = false }) {
            dlg.Controls.Add(new Label { Text = "Press the new key for:\r\n" + ((Act)it.Tag).Text + "\r\n(Esc = cancel)", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter });
            int got = -1;
            dlg.KeyDown += delegate(object s, KeyEventArgs e) {
                if (e.KeyCode == Keys.Escape) { dlg.Close(); return; }
                if (e.KeyCode == Keys.ControlKey || e.KeyCode == Keys.ShiftKey || e.KeyCode == Keys.Menu) return;
                got = (int)e.KeyCode & 0xff; e.Handled = true; e.SuppressKeyPress = true; dlg.Close();
            };
            dlg.ShowDialog(this);
            if (got > 0) {
                foreach (ListViewItem o in keyList.Items) if (o != it && KeyNames.Parse(o.SubItems[1].Text) == got) { SetKey(o, 0); SetStatus("Key " + KeyNames.Name(got) + " was used by '" + ((Act)o.Tag).Text + "' - that action is now unbound."); }
                SetKey(it, got);
            }
        }
    }
    void SetKey(ListViewItem it, int vk) {
        it.SubItems[1].Text = KeyNames.Name(vk);
        Ini.Set(ini, "keys", ((Act)it.Tag).Id, KeyNames.Name(vk)); Stamp();
        SetStatus("Saved: " + ((Act)it.Tag).Text + " = " + KeyNames.Name(vk));
    }

    TabPage BuildHelp() {
        var p = new TabPage("Help");
        var t = new TextBox { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill, ScrollBars = ScrollBars.Vertical, BackColor = SystemColors.Window, Font = new Font("Segoe UI", 9.5f) };
        t.Text = "EnshroudedVR - quick help\r\n\r\n" +
            "\u2022 Start your OpenXR runtime (Virtual Desktop / Quest Link / SteamVR) first, then the game. Press the Recenter key once in the world.\r\n" +
            "\u2022 Every hotkey shows a short message inside the headset and beeps.\r\n" +
            "\u2022 This window can stay open while you play: every change is written to vr.ini and the mod applies it within a second.\r\n\r\n" +
            "Depth vs. world size: stereo strength (IPD) above 100% gives more 3D but makes the world look smaller. " +
            "For depth at normal scale move the camera closer (Camera distance) and enable 6DoF.\r\n\r\n" +
            "Sharper image: set the game's Display Mode to WINDOWED - the mod then renders a square 2160x2160 frame (tab Display & Headset), ~33% sharper in the headset. " +
            "Keep 72+ fps: DLSS Quality, and switch off the game's automatic render scale.\r\n\r\n" +
            "Quest via Air Link: raise the Link bitrate (Oculus Debug Tool > Encode Bitrate 150) for a cleaner image.\r\n\r\n" +
            "Menus/inventory (mouse cursor visible) are shown on a flat virtual screen - that's intended.\r\n\r\n" +
            "Problems: open the log (button below) and attach vrlog.txt to your report. Uninstall: delete dinput8.dll from the game folder.";
        t.GotFocus += delegate { t.SelectionLength = 0; }; t.TabStop = false;
        p.Controls.Add(t); return p;
    }

    void Changed(string key) { if (loading) return; pending.Add(key); debounce.Stop(); debounce.Start(); }
    void Flush() {
        var names = new List<string>();
        foreach (var k in pending) {
            ComboBox sc; if (sCombos.TryGetValue(k, out sc)) { string v = sVals[k][Math.Max(0, sc.SelectedIndex)]; Ini.Set(ini, "vr", k, v); names.Add(k + "=" + v); continue; }
            Ini.Set(ini, "vr", k, Value(k).ToString()); names.Add(k + "=" + Value(k)); }
        pending.Clear(); Stamp();
        if (names.Count > 0) SetStatus("Saved " + string.Join(", ", names.ToArray()) + "  (applies live)");
    }
    int Value(string k) {
        TrackBar tb; CheckBox cb; ComboBox c;
        if (bars.TryGetValue(k, out tb)) return tb.Value * (int)tb.Tag;
        if (checks.TryGetValue(k, out cb)) return cb.Checked ? 1 : 0;
        if (combos.TryGetValue(k, out c)) return comboVals[k][Math.Max(0, c.SelectedIndex)];
        return 0;
    }
    void SetValue(string k, int v) {
        TrackBar tb; CheckBox cb; ComboBox c;
        if (bars.TryGetValue(k, out tb)) { int s = (int)tb.Tag; tb.Value = Math.Max(tb.Minimum, Math.Min(tb.Maximum, v / s)); barVals[k].Text = (tb.Value * s) + (string)barVals[k].Tag; }
        else if (checks.TryGetValue(k, out cb)) cb.Checked = v != 0;
        else if (combos.TryGetValue(k, out c)) { int i = Array.IndexOf(comboVals[k], v); c.SelectedIndex = i < 0 ? Array.IndexOf(comboVals[k], defaults[k]) : i; }
    }
    void LoadAll() {
        loading = true;
        foreach (var k in defaults.Keys) SetValue(k, Ini.GetInt(ini, k, defaults[k]));
        foreach (var k in sCombos.Keys) { string v = Ini.Get(ini, "vr", k, sDefs[k]).Trim(); int i = Array.IndexOf(sVals[k], v);
            if (i < 0) i = Array.IndexOf(sVals[k], sDefs[k]); sCombos[k].SelectedIndex = Math.Max(0, i); }
        foreach (ListViewItem it in keyList.Items) {
            var a = (Act)it.Tag; int vk = KeyNames.Parse(Ini.Get(ini, "keys", a.Id, a.Def)); if (vk < 0) vk = KeyNames.Parse(a.Def);
            it.SubItems[1].Text = KeyNames.Name(vk);
        }
        loading = false; Stamp();
    }
    void ApplyPreset(int i) {
        var v = new Dictionary<string, int>(defaults);
        if (i == 1) { v["ipd_pct"] = 150; v["cam_dist_pct"] = 80; v["head_pos"] = 1; }
        if (i == 2) { v["hud_mode"] = 1; v["cam_dist_pct"] = 120; }
        if (i == 3) { v["hud_mode"] = 2; }
        foreach (var k in v.Keys) { SetValue(k, v[k]); pending.Add(k); }
        Flush(); SetStatus("Preset applied (live).");
    }
    void Stamp() { lastWrite = File.Exists(ini) ? File.GetLastWriteTimeUtc(ini) : DateTime.MinValue; }
    void CheckExternal() {   // the game changed vr.ini (hotkey) -> reflect it here
        if (pending.Count > 0 || !File.Exists(ini)) return;
        var t = File.GetLastWriteTimeUtc(ini);
        if (t != lastWrite) { LoadAll(); SetStatus("Updated from the game (hotkey) at " + DateTime.Now.ToString("HH:mm:ss")); }
    }
    void SetStatus(string s) { status.Text = s; }
    public void FlushNow() { debounce.Stop(); Flush(); }
    public void SetForTest(string k, int v) { SetValue(k, v); pending.Add(k); }
    public int GetForTest(string k) { return Value(k); }
    public void ReloadForTest() { LoadAll(); }
    public void SetSForTest(string k, int idx) { sCombos[k].SelectedIndex = idx; pending.Add(k); }
    public string GetSForTest(string k) { return sVals[k][sCombos[k].SelectedIndex]; }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [STAThread]
    static int Main(string[] args) {
        try { SetProcessDPIAware(); } catch { }
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        string exeDir = AppDomain.CurrentDomain.BaseDirectory;
        string ini = Path.Combine(exeDir, "vr.ini");
        if (args.Length >= 2 && args[0] == "--ini") ini = Path.GetFullPath(args[1]);
        else if (!Directory.Exists(Path.Combine(exeDir, "..")) || !File.Exists(Path.Combine(exeDir, "openxr_loader.dll"))) {
            if (!File.Exists(ini)) MessageBox.Show("Put EnshroudedVR-Config.exe into <Enshrouded>\\enshrouded_vr\\ (next to openxr_loader.dll).\r\nvr.ini will be created here:\r\n" + ini, "EnshroudedVR");
        }
        var f = new MainForm(ini);
        int a = 2;   // --selftest / --screenshot for the build pipeline
        if (args.Length > a && args[a] == "--selftest") {
            f.SetForTest("ipd_pct", 230); f.SetForTest("hud_mode", 0); f.SetForTest("stereo", -1); f.SetForTest("head_pos", 0); f.SetSForTest("force_window", 1); f.FlushNow();
            f.SetForTest("ipd_pct", 10); f.ReloadForTest();
            bool ok = f.GetForTest("ipd_pct") == 230 && f.GetForTest("hud_mode") == 0 && f.GetForTest("stereo") == -1 && f.GetForTest("head_pos") == 0 && f.GetSForTest("force_window") == "1620x1620";
            Console.WriteLine(ok ? "SELFTEST PASS" : "SELFTEST FAIL"); return ok ? 0 : 1;
        }
        if (args.Length > a + 1 && args[a] == "--screenshot") {
            f.Show(); Application.DoEvents();
            for (int i = 0; i < f.Tabs.TabCount; ++i) {
                f.Tabs.SelectedIndex = i; Application.DoEvents();
                using (var bmp = new Bitmap(f.Width, f.Height)) { f.DrawToBitmap(bmp, new Rectangle(0, 0, f.Width, f.Height)); bmp.Save(args[a + 1] + "_" + i + ".png"); }
            }
            return 0;
        }
        Application.Run(f); return 0;
    }
}
