# Generates StageLayouts.vectors.txt: golden vectors from the StageProto C# layout engine (layouts.cs).
# Reads layouts.cs READ-ONLY and compiles only that file (pure, no Win32, nothing touches the screen), so it is safe to run any time.
# Also runs the C# LayoutTests and prints their case counts (the C++ spec asserts the same counts).
# Drift guard: rerun after any layouts.cs change and diff the vectors file; StageLayouts.Spec.cpp (GoldenVectors) compares the C++ port to it.
#   pwsh tools\stage-vectors\gen.ps1 [-Source <layouts.cs>] [-Out <vectors file>]
param(
  [string]$Source = 'C:\Users\carlo\Tools\StageProto\layouts.cs',
  [string]$Out = (Join-Path $PSScriptRoot 'StageLayouts.vectors.txt')
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Run with pwsh (PowerShell 7+): layouts.cs needs a modern C# compiler.' }
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture

$layouts = [System.IO.File]::ReadAllText($Source)
# The scenarios group does not report its case count in layouts.cs; patch the log line IN MEMORY (the file is untouched) so it does.
$marker = '"  scenarios (fill, 50/50, strip, margins, master, hidden flag): "'
if ($layouts.Contains($marker)) { $layouts = $layouts.Replace($marker, '"  scenarios (fill, 50/50, strip, margins, master, hidden flag): " + cases + " checks "') }
else { Write-Warning 'scenarios log line not found: scenarios case count will not be printed' }

$gen = @'
namespace StageVectors {
using System;
using System.Collections.Generic;
using System.Text;
using StageLayouts;

public static class Gen {
  static StringBuilder sb;
  static void Line(string s) { sb.Append(s).Append('\n'); }
  static string R(LRect r) { return r.L + "," + r.T + "," + r.R + "," + r.B; }
  static string Sp(Spacing s) { return s.Gap + "," + s.MarginL + "," + s.MarginT + "," + s.MarginR + "," + s.MarginB; }
  static string Ids(List<int> o) { return string.Join(",", o); }
  static string Wins(List<LWin> w) { var l = new List<string>(); foreach (var x in w) l.Add(x.Id + ":" + (int)x.Flags + ":" + x.MinW + ":" + x.MinH); return string.Join(" ", l); }
  static string P(Plan p) {
    var b = new StringBuilder();
    b.Append("strip=" + (p.HasStrip ? 1 : 0) + " area=" + R(p.Area) + " zone=" + R(p.Strip) + " park=" + R(p.Park) + " tile=" + p.TileW + "x" + p.TileH + " gap=" + p.Gap + " docked=" + p.Docked + " |");
    foreach (var it in p.Items) b.Append(" " + it.Id + ":" + (int)it.Role + ":" + it.Slot + ":" + R(it.Rect));
    return b.ToString();
  }
  static ILayout Make(string name) { if (name == "pair") return new PairLayout(); if (name == "master") return new MasterLayout(); return new StageLayout(); }
  static void Case(string tag, ILayout lay, LRect w, Spacing sp, List<LWin> wins, string extra) {
    var inp = new LayoutInput { Work = w, Spacing = sp }; inp.Wins = wins; var p = lay.Compute(inp);
    Line(tag + " " + lay.Name + " w=" + R(w) + " sp=" + Sp(sp) + " wins=" + Wins(wins) + extra + " => " + P(p));
  }
  static List<LWin> Plain(int n, int hiddenIdx) { var l = new List<LWin>(); for (int k = 0; k < n; k++) l.Add(new LWin(k + 1, k == hiddenIdx ? WinFlags.Hidden : WinFlags.None)); return l; }
  static long Ppm(double x) { return (long)Math.Round(x * 1e6, MidpointRounding.AwayFromZero); }
  static uint seed;
  static int Rnd(int m) { seed = unchecked(seed * 1664525u + 1013904223u); return (int)((seed >> 8) % (uint)m); }

  public static string Generate() {
    sb = new StringBuilder();
    var works = new[] { new LRect(0, 24, 2560, 1392), new LRect(2560, 0, 5120, 1392), new LRect(-1920, 31, -1, 1048), new LRect(0, 0, 1919, 1077), new LRect(100, 50, 1437, 903) };
    var spacings = new[] { new Spacing(0, 0, 0, 0, 0), Spacing.MuntinDefault, new Spacing(10, 10, 10, 10, 10), new Spacing(25, 5, 10, 15, 20), new Spacing(7, 0, 30, 0, 0), new Spacing(100, 500, 500, 500, 500), new Spacing(8, 3000, 3000, 3000, 3000), new Spacing(-5, -5, -5, -5, -5) };
    var names = new[] { "stage", "pair", "master" };
    // G: geometry for every layout x work x spacing x 0..9 windows
    foreach (var nm in names) foreach (var w in works) foreach (var sp in spacings) for (int n = 0; n <= 9; n++) Case("G", Make(nm), w, sp, Plain(n, -1), "");
    // H: the same with the second window flagged Hidden
    foreach (var nm in names) foreach (var w in works) for (int si = 0; si < 3; si++) for (int n = 2; n <= 9; n++) Case("H", Make(nm), w, spacings[si], Plain(n, 1), "");
    // M: pseudo-random minimum sizes and hidden flags
    seed = 12345;
    var areas = new[] { works[0], new LRect(0, 0, 1919, 1077), new LRect(2560, 0, 6400, 2088) };
    foreach (var nm in new[] { "pair", "master" }) for (int round = 0; round < 400; round++) {
      var work = areas[round % areas.Length]; int n = 1 + round % 8; var wins = new List<LWin>();
      for (int k = 0; k < n; k++) { int mw = Rnd(4) == 0 ? Rnd(1500) : 0; int mh = Rnd(3) == 0 ? Rnd(900) : 0; int hid = Rnd(10) == 0 ? 1 : 0; wins.Add(new LWin(k + 1, hid == 1 ? WinFlags.Hidden : WinFlags.None, mw, mh)); }
      Case("M", Make(nm), work, round % 2 == 0 ? Spacing.MuntinDefault : new Spacing(10, 10, 10, 10, 10), wins, "");
    }
    // C: movable cut (pair ratio, master share), clamped, with CutSpan
    foreach (var nm in new[] { "pair", "master" }) foreach (var c in new[] { 0.01, 0.15, 0.3, 0.5, 0.7, 0.85, 0.99 }) foreach (var wi in new[] { 0, 3 }) for (int n = 2; n <= 6; n++) {
      var lay = Make(nm); lay.Cut = c; var sp = wi == 0 ? Spacing.MuntinDefault : new Spacing(10, 10, 10, 10, 10); var inp = new LayoutInput { Work = works[wi], Spacing = sp }; inp.Wins = Plain(n, -1); var p = lay.Compute(inp);
      Line("C " + nm + " cut=" + c.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + " w=" + R(works[wi]) + " sp=" + Sp(sp) + " n=" + n + " cutppm=" + Ppm(lay.Cut) + " span=" + R(lay.CutSpan(p.Area, p.Strip, n)) + " => " + P(p));
    }
    // R: master stack heights, MoveRow sequence driven by the heights of the current plan
    for (int n = 3; n <= 6; n++) {
      var m = new MasterLayout(); var ops = new[] { new[] { 0, 100 }, new[] { 1, -60 }, new[] { n - 3, 37 }, new[] { 0, -100000 }, new[] { 1, 100000 } };
      foreach (var op in ops) {
        var inp = new LayoutInput { Work = works[0], Spacing = Spacing.MuntinDefault }; inp.Wins = Plain(n, -1); var p0 = m.Compute(inp);
        var hs = new List<int>(); for (int i = 1; i < p0.Docked; i++) hs.Add(p0.Items[i].Rect.H);
        bool ok = m.MoveRow(hs.ToArray(), op[0], op[1]); var rows = m.Rows; var rs = "null"; if (rows != null) { var l = new List<string>(); foreach (var x in rows) l.Add(Ppm(x).ToString()); rs = string.Join(",", l); }
        var p1 = m.Compute(inp);
        Line("R n=" + n + " heights=" + Ids(hs) + " op=" + op[0] + "," + op[1] + " ok=" + (ok ? 1 : 0) + " rows=" + rs + " => " + P(p1));
      }
    }
    // O: order policy, pseudo-random operation sequences per layout
    for (int li = 0; li < names.Length; li++) {
      var lay = Make(names[li]); seed = (uint)(1000 + li); var o = new List<int>(); int nextId = 1;
      for (int step = 0; step < 300; step++) {
        int op = Rnd(8); if (o.Count >= 10 && op <= 2) op = 6; if (o.Count == 0 && op >= 3) op = 0;
        string args = "-"; string ret = "-";
        switch (op) {
          case 0: case 1: { int id = nextId++; int f = Rnd(2); lay.Add(o, id, f == 1); args = id + "," + f; break; }
          case 2: { int id = nextId++; int d = Rnd(2); lay.Join(o, id, d == 1); args = id + "," + d; break; }
          case 3: { int id = o[Rnd(o.Count)]; ret = lay.Promote(o, id) ? "1" : "0"; args = "" + id; break; }
          case 4: { int id = o[Rnd(o.Count)]; ret = lay.PromoteHidden(o, id) ? "1" : "0"; args = "" + id; break; }
          case 5: { int id = o[Rnd(o.Count)]; ret = lay.Demote(o, id) ? "1" : "0"; args = "" + id; break; }
          case 6: { int id = o[Rnd(o.Count)]; lay.Remove(o, id); args = "" + id; break; }
          default: { int dir = Rnd(2) == 0 ? -1 : 1; ret = lay.Rotate(o, dir) ? "1" : "0"; args = "" + dir; break; }
        }
        Line("O " + names[li] + " step=" + step + " op=" + op + " args=" + args + " ret=" + ret + " list=" + Ids(o) + " slots=" + lay.Slots(o.Count) + " strip=" + (lay.HasStrip(o.Count) ? 1 : 0));
      }
    }
    return sb.ToString();
  }
}
}
'@

Add-Type -TypeDefinition ($layouts + "`n" + $gen) -Language CSharp

Write-Host 'C# LayoutTests (case counts the C++ spec must match):'
$log = New-Object 'System.Collections.Generic.List[string]'
$fails = [StageLayouts.LayoutTests]::Run($log)
$log | ForEach-Object { Write-Host $_ }
Write-Host "C# failed checks: $fails"
if ($fails -ne 0) { throw 'C# LayoutTests failed; not writing vectors' }

$rev = 'unknown'
try { $rev = (& git --no-optional-locks -C (Split-Path $Source -Parent) rev-parse --short HEAD 2>$null) } catch {}
$body = [StageVectors.Gen]::Generate()
$header = "# StageLayouts golden vectors, generated by tools/stage-vectors/gen.ps1 from StageProto layouts.cs (StageProto HEAD $rev). Do not edit by hand.`n# Lines starting with # are ignored by the C++ spec; every other line must match the C++ port exactly.`n"
[System.IO.File]::WriteAllText($Out, $header + $body, (New-Object System.Text.UTF8Encoding($false)))
$count = ($body -split "`n" | Where-Object { $_ -ne '' }).Count
Write-Host "Wrote $count vector lines to $Out"
