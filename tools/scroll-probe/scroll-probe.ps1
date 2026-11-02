param([string]$Out = "$env:TEMP\scroll-probe", [int]$MonitorX = 2560, [int]$MonitorW = 2560, [int]$PageH = 1440, [switch]$Hybrid)
# Feasibility probe for desktop scrolling. Opens its own windows on one monitor, in two pages, and
# slides them past each other with three move methods, recording the screen. It only ever touches
# windows it opened itself, and it sends no mouse or keyboard input.
$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing, System.Drawing.Primitives, System.Drawing.Common, System.Private.Windows.GdiPlus, System.Private.Windows.Core, System.Windows.Forms.Primitives, System.ComponentModel.Primitives, System.Collections, Microsoft.Win32.Primitives, System.Threading, System.Threading.Thread, System.Linq, System.Runtime.InteropServices, System.Console -TypeDefinition @'
using System; using System.Collections.Generic; using System.Diagnostics; using System.Drawing; using System.Linq;
using System.Runtime.InteropServices; using System.Text; using System.Threading; using System.Windows.Forms;
public static class Scroll {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
  [StructLayout(LayoutKind.Sequential)] struct TP { public int flags; public R dest, src; public byte opacity; public int visible, clientOnly; }
  [DllImport("user32")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int cy, uint f);
  [DllImport("user32")] static extern IntPtr BeginDeferWindowPos(int n);
  [DllImport("user32")] static extern IntPtr DeferWindowPos(IntPtr d, IntPtr h, IntPtr a, int x, int y, int w, int cy, uint f);
  [DllImport("user32")] static extern bool EndDeferWindowPos(IntPtr d);
  [DllImport("user32")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("dwmapi")] static extern int DwmFlush();
  [DllImport("dwmapi")] static extern int DwmRegisterThumbnail(IntPtr dst, IntPtr src, out IntPtr id);
  [DllImport("dwmapi")] static extern int DwmUpdateThumbnailProperties(IntPtr id, ref TP p);
  [DllImport("dwmapi")] static extern int DwmUnregisterThumbnail(IntPtr id);
  const uint Move = 0x1 | 0x4 | 0x10, Async = 0x4000;

  public static List<IntPtr> Find(string cls) {
    var found = new List<IntPtr>(); var s = new StringBuilder(256);
    EnumWindows((h, l) => { if (IsWindowVisible(h)) { s.Clear(); GetClassName(h, s, 256); if (s.ToString() == cls) found.Add(h); } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }

  public static List<IntPtr> Windows = new(); public static List<int> Xs = new(), Ys = new();
  public static List<int> Hints = new();
  public static void Add(IntPtr h, int row = 0) { GetWindowRect(h, out var r); Windows.Add(h); Xs.Add(r.L); Ys.Add(r.T); Hints.Add(row); }
  public static void Place(int offset, string mode) {
    if (mode == "defer") {
      var d = BeginDeferWindowPos(Windows.Count);
      for (int i = 0; i < Windows.Count; i++) d = DeferWindowPos(d, Windows[i], IntPtr.Zero, Xs[i], Ys[i] + offset, 0, 0, Move);
      if (d == IntPtr.Zero || !EndDeferWindowPos(d)) Failures++;
    } else {
      uint f = mode == "async" ? Move | Async : Move;
      for (int i = 0; i < Windows.Count; i++) if (!SetWindowPos(Windows[i], IntPtr.Zero, Xs[i], Ys[i] + offset, 0, 0, f)) Failures++;
    }
  }
  public static int Failures;

  // Moves every window from one offset to another, one step per composed frame. Reports how long
  // the move calls took and how long the frames ran, in milliseconds.
  public static string Slide(string mode, int ms, int from, int to) {
    double f = Stopwatch.Frequency / 1000.0; var calls = new List<double>(); var gaps = new List<double>();
    long start = Stopwatch.GetTimestamp(), last = start;
    while (true) {
      double t = (Stopwatch.GetTimestamp() - start) / f, p = Math.Min(1, t / ms);
      long a = Stopwatch.GetTimestamp(); Place((int)(from + (to - from) * p), mode); long b = Stopwatch.GetTimestamp();
      calls.Add((b - a) / f); DwmFlush(); long c = Stopwatch.GetTimestamp(); gaps.Add((c - last) / f); last = c;
      Application.DoEvents();
      if (p >= 1) break;
    }
    calls.Sort(); gaps.Sort();
    Func<List<double>, double, double> q = (v, x) => v[(int)((v.Count - 1) * x)];
    return $"{mode,-6} {ms,4}ms frames {calls.Count,3} call p50 {q(calls, .5):F1} max {calls[^1]:F1} | frame p50 {q(gaps, .5):F1} p95 {q(gaps, .95):F1} max {gaps[^1]:F1} | took {(Stopwatch.GetTimestamp() - start) / f:F0}";
  }

  public static void Wait(int ms) { var w = Stopwatch.StartNew(); while (w.ElapsedMilliseconds < ms) { Application.DoEvents(); Thread.Sleep(5); } }

  // A window on its own thread that can stop answering messages, like a hung app.
  static Form hung;
  public static IntPtr StartHung(int x, int y, int w, int h) {
    var ready = new ManualResetEvent(false); IntPtr handle = IntPtr.Zero;
    var t = new Thread(() => { Aware();
      hung = new Form { FormBorderStyle = FormBorderStyle.None, BackColor = Color.DimGray, StartPosition = FormStartPosition.Manual, Location = new Point(x, y), Size = new Size(w, h), ShowInTaskbar = false, Text = "probe hung" };
      hung.Shown += (s, e) => { handle = hung.Handle; ready.Set(); };
      Application.Run(hung);
    });
    t.IsBackground = true; t.SetApartmentState(ApartmentState.STA); t.Start(); ready.WaitOne(5000);
    return handle;
  }
  public static void Hang(int ms) { hung.BeginInvoke((Action)(() => Thread.Sleep(ms))); Thread.Sleep(30); }
  public static void StopHung() { try { hung?.BeginInvoke((Action)(() => hung.Close())); } catch { } }

  // Shows live DWM thumbnails of the given windows and saves a screenshot of them.
  public static string Thumbs(IntPtr[] src, int x, int y, string png) {
    var f = new Form { FormBorderStyle = FormBorderStyle.None, BackColor = Color.Black, TopMost = true, ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Location = new Point(x, y), Size = new Size(320 * src.Length, 400) };
    f.Show(); var ids = new List<IntPtr>(); var res = new List<int>();
    for (int i = 0; i < src.Length; i++) {
      int hr = DwmRegisterThumbnail(f.Handle, src[i], out var id); res.Add(hr);
      var p = new TP { flags = 1 | 4 | 8, dest = new R { L = 320 * i + 5, T = 5, Rt = 320 * i + 315, B = 395 }, opacity = 255, visible = 1 };
      hr = DwmUpdateThumbnailProperties(id, ref p); res.Add(hr); ids.Add(id);
    }
    Wait(1500);
    using (var b = new Bitmap(f.Width, f.Height)) using (var g = Graphics.FromImage(b)) { g.CopyFromScreen(x, y, 0, 0, b.Size); b.Save(png); }
    foreach (var id in ids) DwmUnregisterThumbnail(id);
    f.Close();
    return string.Join(",", res);
  }

  // Hybrid scroll: one topmost window draws live DWM thumbnails of everything on the monitor. The
  // thumbnails animate while the real windows jump once, out of sight, then the overlay leaves.
  [DllImport("user32")] static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
  [DllImport("user32")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  [DllImport("user32")] static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("dwmapi")] static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int s);
  public static void Aware() { SetThreadDpiAwarenessContext((IntPtr)(-4)); }
  static Overlay over; static Rectangle area;
  public static string StartOverlay(int monitorX) {
    area = Screen.FromPoint(new Point(monitorX + 10, 10)).WorkingArea;
    over = new Overlay { FormBorderStyle = FormBorderStyle.None, BackColor = Color.Black, ShowInTaskbar = false, StartPosition = FormStartPosition.Manual };
    over.Show();
    SetWindowPos(over.Handle, (IntPtr)(-1), area.X, area.Y, area.Width, area.Height, 0x10 | 0x40);
    over.Refresh(); Wait(300); // stays on screen, fully see-through, so its surface is always painted
    return $"overlay {area}, dpi {GetDpiForWindow(over.Handle)}";
  }
  public static void StopOverlay() { over?.Close(); }
  // Each window has a home on screen and a hiding place: a one pixel sliver under the taskbar, so
  // the window stays on this monitor and keeps its DPI. The overlay is see-through except for the
  // thumbnails, so the desktop and every other window stay live behind them.
  sealed class Item { public int I; public IntPtr Id; }
  static List<int> Rows = new(), Homes = new(), Ws = new(), Hs = new(); static int page, hide;
  static int showing;
  public static void Park(int pageHeight) {
    page = pageHeight; hide = pageHeight - 1;
    foreach (var (h, i) in Windows.Select((h, i) => (h, i))) {
      GetWindowRect(h, out var r); int row = Hints[i];
      Rows.Add(row); Homes.Add(Ys[i]); Ws.Add(r.Rt - r.L); Hs.Add(r.B - r.T);
      if (row == 1) SetWindowPos(h, IntPtr.Zero, Xs[i], hide, 0, 0, Move);
    }
  }
  static void Send(int row, bool toHome) {
    for (int i = 0; i < Windows.Count; i++) if (Rows[i] == row) SetWindowPos(Windows[i], IntPtr.Zero, Xs[i], toHome ? Homes[i] : hide, 0, 0, Move | Async);
  }
  public static string Glide(int ms) {
    double f = Stopwatch.Frequency / 1000.0; long t0 = Stopwatch.GetTimestamp();
    int leaving = showing, entering = 1 - showing, dir = entering == 1 ? 1 : -1;
    var items = new List<Item>();
    for (int i = 0; i < Windows.Count; i++) if (DwmRegisterThumbnail(over.Handle, Windows[i], out var id) == 0) items.Add(new Item { I = i, Id = id });
    Action<double> frame = p => {
      foreach (var t in items) {
        double s = (Rows[t.I] == leaving ? p : p - 1) * dir * page;
        int y = Homes[t.I] - area.Y + (int)s;
        var q = new TP { flags = 1 | 4 | 8, opacity = 255, visible = 1, dest = new R { L = Xs[t.I] - area.X, T = y, Rt = Xs[t.I] - area.X + Ws[t.I], B = y + Hs[t.I] } };
        DwmUpdateThumbnailProperties(t.Id, ref q);
      }
    };
    frame(0); DwmFlush();
    SetWindowPos(over.Handle, (IntPtr)(-1), area.X, area.Y, area.Width, area.Height, 0x10 | 0x40); DwmFlush();
    double prep = (Stopwatch.GetTimestamp() - t0) / f;
    Send(leaving, false);

    var gaps = new List<double>(); long start = Stopwatch.GetTimestamp(), last = start;
    while (true) {
      double p = Math.Min(1, (Stopwatch.GetTimestamp() - start) / f / ms);
      frame(p); DwmFlush(); long c = Stopwatch.GetTimestamp(); gaps.Add((c - last) / f); last = c;
      if (p >= 1) break;
    }
    Send(entering, true);
    long w = Stopwatch.GetTimestamp(); int late; string which = "";
    do {
      late = 0;
      for (int i = 0; i < Windows.Count; i++) if (Rows[i] == entering) { GetWindowRect(Windows[i], out var r); if (r.T != Homes[i]) { late++; which = $"window {i}"; } }
      if (late > 0) Thread.Sleep(2);
    } while (late > 0 && (Stopwatch.GetTimestamp() - w) / f < 150);
    double land = (Stopwatch.GetTimestamp() - w) / f;
    DwmFlush(); DwmFlush();
    foreach (var t in items) DwmUnregisterThumbnail(t.Id); DwmFlush();
    Application.DoEvents();
    showing = entering; gaps.Sort();
    var sized = new List<string>();
    for (int i = 0; i < Windows.Count; i++) { GetWindowRect(Windows[i], out var r); if (r.Rt - r.L != Ws[i] || r.B - r.T != Hs[i]) sized.Add($"{Title(Windows[i])} {Ws[i]}x{Hs[i]} now {r.Rt - r.L}x{r.B - r.T} dpi {GetDpiForWindow(Windows[i])}"); }
    return $"thumb {ms,4}ms prep {prep:F0} | frames {gaps.Count,3} frame p50 {gaps[gaps.Count / 2]:F1} max {gaps[^1]:F1} | landed in {land:F0}, late {late} {which} | resized {sized.Count} {string.Join("; ", sized)}";
  }
}
public sealed class Overlay : Form {
  protected override CreateParams CreateParams { get { var c = base.CreateParams; c.ExStyle |= 0x08000000 | 0x80 | 0x20 | 0x00200000; /* no redirection surface: only the thumbnails show */ return c; } }
  protected override bool ShowWithoutActivation => true;
}
'@
[Scroll]::Aware()
$screen = [Windows.Forms.Screen]::FromPoint([Drawing.Point]::new($MonitorX + 10, 10)).Bounds; $MonitorW = $screen.Width; $PageH = $screen.Height
"monitor $screen"

$py = "$PSScriptRoot\analyze.py"
$url = ([Uri]"$PSScriptRoot\page.html").AbsoluteUri
$profile = "$Out\chrome-profile"
$chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
$w = 500; $h = 1300; $x = { param($i) $MonitorX + 20 + 504 * $i }
$labels = [Collections.Generic.List[string]]::new()
$log = [Collections.Generic.List[string]]::new()
function Say($s) { $log.Add($s); Write-Output $s }

$calcBefore = @(Get-Process CalculatorApp -ErrorAction SilentlyContinue).Id
$own = [Collections.Generic.List[IntPtr]]::new(); $ffmpeg = $null; $hungH = [IntPtr]::Zero
$before = @{}; foreach ($c in 'CabinetWClass', 'org.wezfurlong.wezterm', 'ApplicationFrameWindow') { $before[$c] = @([Scroll]::Find($c)) }
function NewWindows($cls, $want) {
  $t = [Diagnostics.Stopwatch]::StartNew()
  do { $new = @([Scroll]::Find($cls) | Where-Object { $before[$cls] -notcontains $_ -and $own -notcontains $_ -and [Scroll]::Title($_) }); if ($new.Count -ge $want) { break }; Start-Sleep -Milliseconds 200 } while ($t.Elapsed.TotalSeconds -lt 10)
  @($new | Select-Object -First $want)
}
function Lay($hwnd, $col, $row, $top = 20, $height = $h) {
  $y = if ($Hybrid) { $top } else { $top - ($row * $PageH) }  # hybrid: never leave the monitor, so no DPI change
  [void][Scroll]::SetWindowPos($hwnd, [IntPtr]::Zero, (& $x $col), $y, $w, $height, 0x14)
  [Scroll]::Add($hwnd, $row); $own.Add($hwnd)
}

try {
  # Page 0 is on screen, page 1 sits one page above it. Column 0/1 are Chrome, so the recording can track them.
  $colors = @{ A0 = 'ff00ff'; A1 = '00ffff'; B0 = 'ffff00'; B1 = '00ff00' }
  foreach ($k in $colors.Keys) {
    Start-Process $chrome "--user-data-dir=`"$profile`" --no-first-run --no-default-browser-check --window-size=$w,$h --app=$url`?c=$($colors[$k])&n=$k"
  }
  $t = [Diagnostics.Stopwatch]::StartNew(); $pages = @{}
  while ($pages.Count -lt 4 -and $t.Elapsed.TotalSeconds -lt 20) {
    foreach ($hw in [Scroll]::Find('Chrome_WidgetWin_1')) { $n = [Scroll]::Title($hw); if ($n -match '^probe (\w\d) ') { $pages[$Matches[1]] = $hw } }
    Start-Sleep -Milliseconds 200
  }
  if ($pages.Count -lt 4) { throw "only $($pages.Count) Chrome windows came up" }
  $pages.GetEnumerator() | Sort-Object Name | ForEach-Object { $i = [int]"$($_.Name[1])"; Lay $_.Value $i ($(if ($_.Name[0] -eq 'A') { 0 } else { 1 })) }

  Start-Process explorer 'C:\Windows'; Start-Process explorer 'C:\Users'
  $e = NewWindows 'CabinetWClass' 2
  & { param($a) if ($a.Count -gt 0) { Lay $a[0] 2 0 }; if ($a.Count -gt 1) { Lay $a[1] 2 1 } } $e
  foreach ($i in 1, 2) { Start-Process 'C:\Program Files\WezTerm\wezterm-gui.exe' 'start --always-new-process -- cmd /k dir C:\Windows\System32' }
  $z = NewWindows 'org.wezfurlong.wezterm' 2
  if ($z.Count -gt 0) { Lay $z[0] 3 0 }; if ($z.Count -gt 1) { Lay $z[1] 3 1 }
  Start-Process calc; Start-Sleep 1; Start-Process calc
  $c = NewWindows 'ApplicationFrameWindow' 2
  if ($c.Count -gt 0) { Lay $c[0] 4 0 20 700 }; if ($c.Count -gt 1) { Lay $c[1] 4 1 20 700 }
  $hungH = [Scroll]::StartHung((& $x 4), 760, $w, 500); [Scroll]::Add($hungH)
  Say "windows: chrome 4, explorer $($e.Count), wezterm $($z.Count), calc $($c.Count), hung-test 1"
  Start-Sleep 2

  # Finds which DXGI output is this monitor: only the right one shows page A's colours.
  $idx = -1
  foreach ($i in 0, 1) {
    $p = [Diagnostics.ProcessStartInfo]::new('ffmpeg')
    foreach ($a in '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', "ddagrab=output_idx=${i}:framerate=30:draw_mouse=0,hwdownload,format=bgra", '-frames:v', '1', '-vf', 'scale=64:36:flags=neighbor', '-f', 'rawvideo', '-pix_fmt', 'bgra', "$Out\id$i.raw") { $p.ArgumentList.Add($a) }
    $p.RedirectStandardInput = $true; $p.RedirectStandardError = $true
    $q = [Diagnostics.Process]::Start($p); if (-not $q.WaitForExit(8000)) { $q.Kill(); continue }
    $b = [IO.File]::ReadAllBytes("$Out\id$i.raw"); $hits = 0
    for ($j = 0; $j -lt $b.Length; $j += 4) { if ($b[$j + 2] -gt 240 -and (($b[$j] -gt 240 -and $b[$j + 1] -lt 15) -or ($b[$j] -lt 15 -and $b[$j + 1] -gt 240))) { $hits++ } }
    if ($hits -gt 10) { $idx = $i }
  }
  if ($idx -lt 0) { throw 'could not find the monitor among outputs 0 and 1' }

  $p = [Diagnostics.ProcessStartInfo]::new('ffmpeg')
  foreach ($a in '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', "ddagrab=output_idx=${idx}:framerate=60:draw_mouse=0,hwdownload,format=bgra",
    '-map', '0:v', '-vf', 'scale=64:720:flags=neighbor', '-f', 'rawvideo', '-pix_fmt', 'bgra', "$Out\edges.raw",
    '-map', '0:v', '-vf', 'scale=1280:720,format=yuv420p', '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '20', "$Out\slide.mp4") { $p.ArgumentList.Add($a) }
  $p.RedirectStandardInput = $true
  $ffmpeg = [Diagnostics.Process]::Start($p)
  [Scroll]::Wait(2000)

  if (-not $Hybrid) {
  foreach ($mode in 'seq', 'defer', 'async') {
    foreach ($ms in 250, 600) {
      foreach ($dir in 1, -1) {
        $from = if ($dir -gt 0) { 0 } else { $PageH }; $to = $PageH - $from
        Say ([Scroll]::Slide($mode, $ms, $from, $to)); $labels.Add("$mode $ms"); [Scroll]::Wait(1200)
      }
    }
  }

  # Page A goes off screen and stays there: does it stay put, does Chrome stop rendering, do thumbnails work.
  $a0 = $pages['A0']; $b0 = $pages['B0']
  [void][Scroll]::Slide('defer', 250, 0, $PageH); $labels.Add('hold out')
  Say "at park: A0='$([Scroll]::Title($a0))'  B0='$([Scroll]::Title($b0))'"
  $src = @($a0) + @($e | Select-Object -First 1) + @($z | Select-Object -First 1)
  [Scroll]::Wait(1000)
  Say "thumbnail HRESULTs: $([Scroll]::Thumbs([IntPtr[]]$src, $MonitorX + 700, 500, "$Out\thumbs.png"))"
  [Scroll]::Wait(5000)
  $ta = [Scroll]::Title($a0); Say "after 7 s off screen: A0='$ta'  B0='$([Scroll]::Title($b0))'"
  $r = [Scroll+R]::new(); [void][Scroll]::GetWindowRect($a0, [ref]$r); Say "A0 rect while parked: top $($r.T), left $($r.L)"
  [void][Scroll]::Slide('defer', 250, $PageH, 0); $labels.Add('hold back')
  $t = [Diagnostics.Stopwatch]::StartNew(); while ([Scroll]::Title($a0) -eq $ta -and $t.ElapsedMilliseconds -lt 3000) { [Windows.Forms.Application]::DoEvents() }
  Say "A0 rendering again $($t.ElapsedMilliseconds) ms after landing: '$([Scroll]::Title($a0))'"
  [Scroll]::Wait(1200)

  # One window stops answering for a second while the rest slide.
  foreach ($mode in 'seq', 'defer', 'async') {
    foreach ($dir in 1, -1) {
      $from = if ($dir -gt 0) { 0 } else { $PageH }; $to = $PageH - $from
      [Scroll]::Hang(1000); Say ("hung " + [Scroll]::Slide($mode, 600, $from, $to)); $labels.Add("hung $mode"); [Scroll]::Wait(1500)
    }
  }

  # Maximized window moved like the others.
  if ($e.Count) {
    $m = $e[0]; [void][Scroll]::ShowWindow($m, 3); [Scroll]::Wait(600)
    $r = [Scroll+R]::new(); [void][Scroll]::GetWindowRect($m, [ref]$r); $zb = [Scroll]::IsZoomed($m); $y0 = $r.T
    [void][Scroll]::SetWindowPos($m, [IntPtr]::Zero, $r.L, $r.T + $PageH, 0, 0, 0x15); [Scroll]::Wait(1000)
    [void][Scroll]::GetWindowRect($m, [ref]$r); Say "maximized: zoomed $zb, top $y0 then $($r.T), zoomed after $([Scroll]::IsZoomed($m))"
    [void][Scroll]::ShowWindow($m, 9); [Scroll]::Wait(400)
  }
  Say "move failures: $([Scroll]::Failures)"
  }
  else {
    Say ([Scroll]::StartOverlay($MonitorX))
    [Scroll]::Park($PageH); [Scroll]::Wait(800)
    foreach ($round in 1, 2) {
      foreach ($ms in 250, 400, 600) {
        foreach ($dir in 1, -1) { Say ([Scroll]::Glide($ms)); $labels.Add("thumb $ms"); [Scroll]::Wait(1200) }
      }
    }
    foreach ($dir in 1, -1) { [Scroll]::Hang(1000); Say ('hung ' + [Scroll]::Glide(400)); $labels.Add('hung thumb'); [Scroll]::Wait(2000) }
    [Scroll]::StopOverlay()
  }
}
finally {
  if ($ffmpeg) { try { $ffmpeg.StandardInput.Write('q'); $ffmpeg.StandardInput.Close(); [void]$ffmpeg.WaitForExit(8000) } catch {}; if (-not $ffmpeg.HasExited) { $ffmpeg.Kill() } }
  foreach ($hw in $own) {
    if ([Scroll]::Find('org.wezfurlong.wezterm') -contains $hw) { $tp = [uint32]0; [void][Scroll]::GetWindowThreadProcessId($hw, [ref]$tp); Stop-Process -Id $tp -Force -ErrorAction SilentlyContinue }
    else { [void][Scroll]::PostMessage($hw, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
  }
  [Scroll]::StopHung()
  Get-Process CalculatorApp -ErrorAction SilentlyContinue | Where-Object { $calcBefore -notcontains $_.Id } | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep 2
  Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like "*$profile*" } | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep 1
  Remove-Item -Recurse -Force $profile -ErrorAction SilentlyContinue
}
[IO.File]::WriteAllLines("$Out\labels.txt", $labels)
[IO.File]::WriteAllLines("$Out\log.txt", $log)
python $py "$Out\edges.raw" "$Out\labels.txt" $MonitorW $PageH