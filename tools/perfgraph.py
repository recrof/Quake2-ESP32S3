#!/usr/bin/env python3
"""Per frame performance graph from a "perf dump" capture (serial console log).

usage: perfgraph.py console.log [out.html]

On the board:  perf start [frames]  ...play / timedemo...  perf dump
Every frame is a stacked bar: the time of each part of the frame (server,
game, client, rendering, waiting for the second core or the display, file
reads, sound / image loading). Below, the events of the frame (files opened,
kB read, sounds and images loaded). Mouse wheel: zoom, drag: pan, hover: details.
"""
import html
import json
import re
import statistics
import sys

COLORS = {
    "other": "#9e9e9e", "server": "#8e6cc8", "game": "#c75bb0", "client": "#5b8fd9",
    "render": "#3fa66b", "parallel": "#a3c94a", "present": "#e0b43a",
    "file": "#e0563a", "sound": "#e08a3a", "asset": "#3ac2c9",
}
DESCR = {
    "other": "main loop, console, not timed",
    "server": "SV_Frame without the game",
    "game": "game logic (monsters, physics)",
    "client": "CL_Frame without rendering (demo parsing, prediction, sound mixing)",
    "render": "3D and 2D rendering on the game core",
    "parallel": "waiting for the second core to finish its half",
    "present": "waiting for the display to take the frame",
    "file": "SD card: file opens, seeks and reads",
    "sound": "sound loading (resampling), without the reads",
    "asset": "image and model loading, without the reads",
}


def parse(path):
    unit, slots, events, frames = 10, None, None, []
    for line in open(path, "rb").read().decode("latin-1").splitlines():
        m = re.search(r"\bPERFH (\d+) (.*) \| (.*)$", line)
        if m:
            unit, slots, events, frames = int(m.group(1)), m.group(2).split(), m.group(3).split(), []
            continue
        m = re.search(r"\bPERF (\d+(?: \d+)+)\s*$", line)
        if m and slots:
            v = [int(x) for x in m.group(1).split()]
            if len(v) != 2 + len(slots) + len(events):
                continue
            frames.append({
                "i": v[0], "sf": v[1],
                "s": [x * unit / 1000.0 for x in v[2:2 + len(slots)]],
                "e": v[2 + len(slots):],
            })
    if not frames:
        sys.exit("no PERF frames in %s (perf start, then perf dump)" % path)
    return slots, events, frames


def summary(slots, events, frames):
    tot = [sum(f["s"][k] for f in frames) for k in range(len(slots))]
    ms = [sum(f["s"]) for f in frames]
    all_ms = sum(ms)
    med = statistics.median(ms)
    srt = sorted(ms)
    p = lambda q: srt[min(len(srt) - 1, int(q * len(srt)))]
    rows = []
    for k, name in enumerate(slots):
        rows.append((name, tot[k], 100 * tot[k] / all_ms if all_ms else 0, tot[k] / len(frames)))
    # spikes: frames over 2x the median, by the slot that grew the most
    spikes = {}
    for f, t in zip(frames, ms):
        if t > 2 * med and t > med + 20:
            k = max(range(len(slots)), key=lambda j: f["s"][j])
            spikes.setdefault(slots[k], [0, 0.0])
            spikes[slots[k]][0] += 1
            spikes[slots[k]][1] += t - med
    worst = sorted(range(len(frames)), key=lambda i: -ms[i])[:20]
    return {
        "frames": len(frames), "total_s": all_ms / 1000, "fps": len(frames) * 1000 / all_ms if all_ms else 0,
        "median": med, "p90": p(0.9), "p99": p(0.99), "max": srt[-1], "rows": rows,
        "spikes": sorted(spikes.items(), key=lambda kv: -kv[1][1]), "worst": worst, "ms": ms,
        "ev_tot": [sum(f["e"][k] for f in frames) for k in range(len(events))],
    }


def build_html(path, slots, events, frames):
    s = summary(slots, events, frames)
    data = {"slots": slots, "events": events, "colors": [COLORS.get(n, "#888") for n in slots],
            "frames": [[f["sf"]] + [round(x, 2) for x in f["s"]] + f["e"] for f in frames]}
    rows = "".join(
        '<tr><td><span class="sw" style="background:%s"></span>%s</td><td>%s</td><td class="n">%.1f</td>'
        '<td class="n">%.1f%%</td><td class="n">%.2f</td></tr>'
        % (COLORS.get(n, "#888"), n, html.escape(DESCR.get(n, "")), t / 1000, pc, avg)
        for n, t, pc, avg in s["rows"])
    spikes = "".join('<tr><td>%s</td><td class="n">%d</td><td class="n">%.1f s</td></tr>' % (n, c, extra / 1000)
                     for n, (c, extra) in s["spikes"]) or '<tr><td colspan="3">none</td></tr>'
    worst = ""
    for i in s["worst"]:
        f = frames[i]
        parts = sorted(((f["s"][k], slots[k]) for k in range(len(slots))), reverse=True)[:3]
        ev = ", ".join("%s %d" % (events[k], f["e"][k]) for k in range(len(events)) if f["e"][k])
        worst += ('<tr data-f="%d"><td class="n">%d</td><td class="n">%d</td><td class="n">%.1f</td><td>%s</td><td>%s</td></tr>'
                  % (i, i, f["sf"], s["ms"][i], ", ".join("%s %.1f" % (n, v) for v, n in parts), ev or "-"))
    evt = ", ".join("%s %d" % (events[k], s["ev_tot"][k]) for k in range(len(events)))
    return TEMPLATE % {
        "src": html.escape(path), "frames": s["frames"], "total": s["total_s"], "fps": s["fps"],
        "median": s["median"], "p90": s["p90"], "p99": s["p99"], "max": s["max"], "rows": rows,
        "spikes": spikes, "worst": worst, "evt": html.escape(evt), "data": json.dumps(data, separators=(",", ":")),
    }


TEMPLATE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Quake 2 frame profile</title>
<style>
:root { --bg:#fbfbfa; --fg:#1d1d1f; --mute:#6b6b70; --line:#e2e2e0; --panel:#ffffff; --grid:#ececea; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg:#17181a; --fg:#e8e8ea; --mute:#9a9aa0; --line:#2c2d30; --panel:#1f2023; --grid:#2a2b2e; } }
:root[data-theme="dark"] { --bg:#17181a; --fg:#e8e8ea; --mute:#9a9aa0; --line:#2c2d30; --panel:#1f2023; --grid:#2a2b2e; }
* { box-sizing: border-box; }
body { margin:0; padding:20px 16px 40px; background:var(--bg); color:var(--fg);
  font:14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
main { max-width:1280px; margin:0 auto; }
h1 { font-size:20px; margin:0 0 4px; } h2 { font-size:15px; margin:28px 0 8px; }
.sub { color:var(--mute); margin-bottom:16px; }
.kpis { display:flex; flex-wrap:wrap; gap:10px; margin-bottom:16px; }
.kpi { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:8px 12px; min-width:96px; }
.kpi b { display:block; font-size:18px; font-variant-numeric:tabular-nums; } .kpi span { color:var(--mute); font-size:12px; }
.card { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:10px; }
#legend { display:flex; flex-wrap:wrap; gap:6px 14px; margin:0 0 8px; user-select:none; }
#legend label { cursor:pointer; color:var(--fg); font-size:13px; } #legend input { vertical-align:-2px; }
.sw { display:inline-block; width:10px; height:10px; border-radius:2px; margin-right:6px; vertical-align:0; }
canvas { display:block; width:100%%; touch-action:none; }
#tip { position:fixed; pointer-events:none; background:var(--panel); border:1px solid var(--line); border-radius:8px;
  padding:8px 10px; font-size:12px; box-shadow:0 4px 16px rgba(0,0,0,.15); display:none; z-index:5; font-variant-numeric:tabular-nums; }
#tip table td { padding:0 6px 0 0; }
.hint { color:var(--mute); font-size:12px; margin-top:6px; }
.scroll { overflow-x:auto; }
table.t { border-collapse:collapse; width:100%%; font-size:13px; }
table.t th, table.t td { text-align:left; padding:5px 8px; border-bottom:1px solid var(--line); }
table.t th { color:var(--mute); font-weight:600; } td.n, th.n { text-align:right; font-variant-numeric:tabular-nums; }
tr[data-f] { cursor:pointer; } tr[data-f]:hover { background:var(--grid); }
</style></head><body><main>
<h1>Quake 2 frame profile</h1>
<div class="sub">%(src)s</div>
<div class="kpis">
 <div class="kpi"><b>%(frames)d</b><span>frames</span></div>
 <div class="kpi"><b>%(total).1f s</b><span>recorded</span></div>
 <div class="kpi"><b>%(fps).1f</b><span>average fps</span></div>
 <div class="kpi"><b>%(median).1f ms</b><span>median frame</span></div>
 <div class="kpi"><b>%(p90).1f ms</b><span>90th percentile</span></div>
 <div class="kpi"><b>%(p99).1f ms</b><span>99th percentile</span></div>
 <div class="kpi"><b>%(max).1f ms</b><span>worst frame</span></div>
</div>
<div class="card">
 <div id="legend"></div>
 <canvas id="c" height="420"></canvas>
 <div class="hint">Frame time (ms) per frame, stacked by part. Below the bars: files opened (ticks), kB read (bars), sounds (orange dots), images/models (cyan dots) loaded. Scroll to zoom, drag to pan, double click to reset.</div>
</div>
<h2>Where the time goes</h2>
<div class="card scroll"><table class="t"><tr><th>part</th><th>what</th><th class="n">total s</th><th class="n">share</th><th class="n">ms / frame</th></tr>%(rows)s</table></div>
<h2>Spikes (frames over twice the median) by their largest part</h2>
<div class="card scroll"><table class="t"><tr><th>largest part</th><th class="n">frames</th><th class="n">time over median</th></tr>%(spikes)s</table></div>
<h2>Worst frames</h2>
<div class="card scroll"><table class="t"><tr><th class="n">frame</th><th class="n">server frame</th><th class="n">ms</th><th>largest parts (ms)</th><th>events</th></tr>%(worst)s</table></div>
<div class="hint">Events over the capture: %(evt)s. Click a row to zoom the graph on that frame.</div>
</main>
<div id="tip"></div>
<script>
const D = %(data)s;
const NS = D.slots.length, NE = D.events.length, N = D.frames.length;
const on = D.slots.map(() => true);
const cv = document.getElementById('c'), ctx = cv.getContext('2d'), tip = document.getElementById('tip');
let v0 = 0, v1 = N;            // visible frame range
const EVH = 46, AX = 44;       // event strip height, axis width
const legend = document.getElementById('legend');
D.slots.forEach((n, k) => {
  const l = document.createElement('label');
  l.innerHTML = `<input type="checkbox" checked> <span class="sw" style="background:${D.colors[k]}"></span>${n}`;
  l.querySelector('input').onchange = e => { on[k] = e.target.checked; draw(); };
  legend.appendChild(l);
});
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
function total(f) { let t = 0; for (let k = 0; k < NS; k++) if (on[k]) t += f[1 + k]; return t; }
function draw() {
  const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = 420;
  cv.width = W * dpr; cv.height = H * dpr; ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const gh = H - EVH - 18, pw = W - AX - 4;
  let mx = 0; for (let i = Math.floor(v0); i < Math.min(N, Math.ceil(v1)); i++) mx = Math.max(mx, total(D.frames[i]));
  mx = Math.max(mx, 10);
  const steps = [5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000];
  let st = steps.find(s => mx / s <= 6) || 10000; const top = Math.ceil(mx / st) * st;
  ctx.font = '11px -apple-system, sans-serif'; ctx.fillStyle = css('--mute'); ctx.strokeStyle = css('--grid'); ctx.lineWidth = 1;
  for (let y = 0; y <= top; y += st) {
    const py = gh - y / top * gh + 4.5;
    ctx.beginPath(); ctx.moveTo(AX, Math.round(py) + .5); ctx.lineTo(W, Math.round(py) + .5); ctx.stroke();
    ctx.textAlign = 'right'; ctx.fillText(y + ' ms', AX - 4, py + 4);
  }
  // 50 ms (20 fps) and 33 ms reference lines
  [[33.3, '30 fps'], [50, '20 fps']].forEach(([ms, t]) => { if (ms < top) {
    const py = Math.round(gh - ms / top * gh + 4.5) + .5;
    ctx.save(); ctx.setLineDash([4, 4]); ctx.strokeStyle = css('--mute'); ctx.beginPath(); ctx.moveTo(AX, py); ctx.lineTo(W, py); ctx.stroke();
    ctx.restore(); ctx.textAlign = 'left'; ctx.fillText(t, AX + 4, py - 3); } });
  const span = v1 - v0, bw = pw / span;
  const i0 = Math.max(0, Math.floor(v0)), i1 = Math.min(N, Math.ceil(v1));
  // aggregate when there are more frames than pixels: the max frame of each pixel column
  const cols = Math.min(i1 - i0, Math.ceil(pw));
  for (let c = 0; c < cols; c++) {
    const a = i0 + Math.floor(c * (i1 - i0) / cols), b = Math.max(a + 1, i0 + Math.floor((c + 1) * (i1 - i0) / cols));
    let best = a; for (let i = a; i < b; i++) if (total(D.frames[i]) > total(D.frames[best])) best = i;
    const f = D.frames[best], x = AX + (a - v0) * bw, w = Math.max(1, (b - a) * bw - (bw > 4 ? 1 : 0));
    let y = gh + 4;
    for (let k = 0; k < NS; k++) { if (!on[k] || !f[1 + k]) continue;
      const h = f[1 + k] / top * gh; ctx.fillStyle = D.colors[k]; ctx.fillRect(x, y - h, w, h); y -= h; }
    // events strip
    const ey = gh + 18, e = i => { let s = 0; for (let j = a; j < b; j++) s += D.frames[j][1 + NS + i]; return s; };
    const kb = e(1); if (kb) { const h = Math.min(20, 4 + Math.log2(1 + kb) * 2); ctx.fillStyle = '#e0563a'; ctx.globalAlpha = .55; ctx.fillRect(x, ey + 22 - h, w, h); ctx.globalAlpha = 1; }
    if (e(0)) { ctx.fillStyle = css('--mute'); ctx.fillRect(x, ey + 24, Math.max(1, w), 3); }
    if (e(2)) { ctx.fillStyle = '#e08a3a'; ctx.beginPath(); ctx.arc(x + w / 2, ey + 4, 3, 0, 7); ctx.fill(); }
    if (e(3)) { ctx.fillStyle = '#3ac2c9'; ctx.beginPath(); ctx.arc(x + w / 2, ey + 12, 3, 0, 7); ctx.fill(); }
  }
  ctx.fillStyle = css('--mute'); ctx.textAlign = 'center';
  const fs = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000].find(s => span / s <= 10) || 5000;
  for (let i = Math.ceil(v0 / fs) * fs; i < v1; i += fs) ctx.fillText(i, AX + (i - v0 + .5) * bw, H - 2);
}
function frameAt(px) { const pw = cv.clientWidth - AX - 4; return Math.floor(v0 + (px - AX) / pw * (v1 - v0)); }
cv.addEventListener('wheel', e => { e.preventDefault();
  const r = cv.getBoundingClientRect(), f = v0 + (e.clientX - r.left - AX) / (cv.clientWidth - AX - 4) * (v1 - v0);
  const z = e.deltaY > 0 ? 1.25 : 0.8; let s = Math.min(N, Math.max(20, (v1 - v0) * z));
  v0 = Math.max(0, Math.min(N - s, f - (f - v0) * s / (v1 - v0))); v1 = v0 + s; draw(); }, { passive: false });
let drag = null;
cv.addEventListener('pointerdown', e => { drag = { x: e.clientX, v0, v1 }; cv.setPointerCapture(e.pointerId); });
cv.addEventListener('pointerup', () => drag = null);
cv.addEventListener('dblclick', () => { v0 = 0; v1 = N; draw(); });
cv.addEventListener('pointermove', e => {
  const r = cv.getBoundingClientRect();
  if (drag) { const d = (e.clientX - drag.x) / (cv.clientWidth - AX - 4) * (drag.v1 - drag.v0);
    const s = drag.v1 - drag.v0; v0 = Math.max(0, Math.min(N - s, drag.v0 - d)); v1 = v0 + s; draw(); tip.style.display = 'none'; return; }
  const i = frameAt(e.clientX - r.left); if (i < 0 || i >= N || e.clientX - r.left < AX) { tip.style.display = 'none'; return; }
  const f = D.frames[i]; let t = 0, rows = '';
  for (let k = 0; k < NS; k++) { t += f[1 + k]; if (f[1 + k] >= 0.05)
    rows += `<tr><td><span class="sw" style="background:${D.colors[k]}"></span>${D.slots[k]}</td><td style="text-align:right">${f[1 + k].toFixed(2)} ms</td></tr>`; }
  const ev = D.events.map((n, k) => f[1 + NS + k] ? `${n} ${f[1 + NS + k]}` : '').filter(Boolean).join(', ');
  tip.innerHTML = `<b>frame ${i}</b> (server frame ${f[0]}): <b>${t.toFixed(1)} ms</b><table>${rows}</table>${ev ? '<div>' + ev + '</div>' : ''}`;
  tip.style.display = 'block';
  tip.style.left = Math.min(window.innerWidth - tip.offsetWidth - 8, e.clientX + 14) + 'px';
  tip.style.top = Math.min(window.innerHeight - tip.offsetHeight - 8, e.clientY + 14) + 'px';
});
cv.addEventListener('pointerleave', () => tip.style.display = 'none');
document.querySelectorAll('tr[data-f]').forEach(tr => tr.onclick = () => {
  const i = +tr.dataset.f; v0 = Math.max(0, i - 60); v1 = Math.min(N, v0 + 120); draw(); cv.scrollIntoView({ behavior: 'smooth', block: 'center' }); });
window.addEventListener('resize', draw);
draw();
</script></body></html>
"""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else re.sub(r"(\.\w+)?$", "", src) + "-perf.html"
    slots, events, frames = parse(src)
    open(out, "w").write(build_html(src, slots, events, frames))
    print("%d frames -> %s" % (len(frames), out))


if __name__ == "__main__":
    main()
