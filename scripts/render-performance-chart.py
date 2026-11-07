#!/usr/bin/env python3
"""Render the README's dependency-free SVG performance chart."""
import json
from html import escape
from math import ceil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'docs/benchmarks/chat-memory/400k'
summary = json.loads((DATA / 'summary.json').read_text())
environment = json.loads((DATA / 'environment.json').read_text())
context_label = f"{environment['content_tokens'] // 1000}K"
summary.update(json.loads((DATA / 'codex-standard/summary.json').read_text()))
ram_max = max(300, ceil(max(s['rss_mib'] for a in summary.values() for s in a.values()) / 100) * 100)
parts = ['''<svg xmlns="http://www.w3.org/2000/svg" width="1040" height="560" viewBox="0 0 1040 560" role="img" aria-labelledby="title desc">
<title id="title">Coding agent memory, install size, and startup time</title>
<desc id="desc">Resident memory (RSS), install size, and startup time. RAM panels use the same zero-based scale. All RAM values are medians of three fresh launches sampled 15–17 seconds after startup. Methodology and raw results are in docs/benchmarks.</desc>
<style>
text { font-family: 'DejaVu Sans Mono', 'SFMono-Regular', Consolas, monospace; fill: #dedede; }
.heading { font-size: 17px; font-weight: 600; }
.label { font-size: 15px; }
.value { font-size: 15px; font-variant-numeric: tabular-nums; }
.muted { fill: #949494; font-size: 12px; }
.grid { stroke: #303030; stroke-width: 1; }
.rule { stroke: #444444; }
.bar { fill: #767676; }
.phi { fill: #d7af5f; }
@media (prefers-color-scheme: light) {
  text { fill: #242424; }
  .muted { fill: #656565; }
  .grid { stroke: #dedede; }
  .rule { stroke: #bdbdbd; }
  .phi { fill: #9c772c; }
}
</style>
''']


def text(x, y, value, cls='label', extra=''):
    parts.append(f'<text x="{x}" y="{y}" class="{cls}" {extra}>{escape(str(value))}</text>')


def panel(left, title, unit, maximum, ticks, rows, top, step):
    start, width, end = left + 132, 244, left + 456
    text(left, top, title, 'heading')
    text(left + 456, top, unit, 'muted', 'text-anchor="end"')
    axis = top + 32
    bottom = axis + 20 + (len(rows) - 1) * step + 24
    for tick in ticks:
        x = start + width * tick / maximum
        parts.append(f'<line x1="{x:g}" y1="{axis + 8}" x2="{x:g}" y2="{bottom}" class="grid"/>')
        text(f'{x:g}', axis, tick, 'muted', 'text-anchor="middle"')
    for i, (name, value, label) in enumerate(rows):
        y = axis + 22 + i * step
        bar_class = 'bar phi' if name == 'Phi' else 'bar'
        text(left, y + 16, name, extra='font-weight="600"' if name == 'Phi' else '')
        if value is not None:
            parts.append(f'<rect x="{start}" y="{y}" width="{width * value / maximum:.3f}" height="22" class="{bar_class}"/>')
        text(end, y + 16, label, 'value', 'text-anchor="end"')


agents = [('Phi', 'phi'), ('Pi', 'pi'), ('Claude Code', 'claude'), ('Codex', 'codex')]
for left, scenario, title in [(32, 'empty', 'RAM · idle'), (552, 'loaded', f'RAM · {context_label} chat')]:
    rows = []
    for name, key in agents:
        value = summary[key][scenario]['rss_mib']
        label = f'{value:.1f}'
        rows.append((name, value, label))
    panel(left, title, 'RSS / MiB', ram_max, [0, ram_max // 2, ram_max], rows, 44, 44)
parts.append('<line x1="32" y1="280" x2="1008" y2="280" class="rule"/>')
panel(32, 'Install', 'MiB', 600, [0, 300, 600],
      [('Phi', 1.4, '1.4'), ('Pi', 304, '304'), ('Claude Code', 316, '316'), ('Codex', 516, '516')], 324, 44)
panel(552, 'Startup', 'ms', 1500, [0, 750, 1500],
      [('Phi', 33, '33'), ('Pi', 360, '360'), ('Claude Code', 528, '528'), ('Codex', 1205, '1,205')], 324, 44)
parts.append('</svg>\n')
(ROOT / 'docs/images/memory-comparison.svg').write_text('\n'.join(parts))
