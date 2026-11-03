"""Validate the full offline payload and every city glyph, without the Internet."""
import gzip
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, 'tools/generate_city_catalog.py', '--check'], cwd=ROOT, check=True)
raw = gzip.decompress((ROOT / 'main/apps/home/cities.json.gz').read_bytes())
rows = json.loads(raw)
assert rows == json.loads((ROOT / 'main/apps/home/cities.json').read_text('utf-8'))
assert len({r[0] for r in rows}) == len(rows)
assert len({r[4] for r in rows}) == 34
assert {'CN', 'HK', 'MO', 'TW'} == {r[6] for r in rows}
for key, maximum in ((1,47),(4,31),(5,47)):
    assert all(len(r[key].encode('utf-8')) <= maximum for r in rows)
assert all(-90 <= r[2] <= 90 and -180 <= r[3] <= 180 for r in rows)
glyphs = {int(c,16) for c in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6})',
                                     (ROOT/'main/fonts/buddy_font_16.c').read_text('utf-8'))}
missing = {c for r in rows for c in r[1]+r[4]+r[5] if ord(c) not in glyphs}
assert not missing, f'missing city glyphs: {missing}'
zhuhai = next(r for r in rows if r[1]=='珠海' and r[4]=='广东')
assert zhuhai[2:4] == [22.2707,113.5767] and zhuhai[5]=='珠海'
chongqing = next(r for r in rows if r[1]=='重庆')
assert chongqing[2:4] == [29.563,106.5516]
print(f'Offline catalog: {len(rows)} places, all coordinates/ids/profile bounds and city glyphs: PASS')
