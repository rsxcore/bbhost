#!/usr/bin/env python3
"""Summarises a graphics journal (BBHOST_GFX_LOG, src/host/gfx_journal.h).

    tools/gfx_journal_report.py logs/gfx-20261013-001240.jsonl [--skip SECONDS] [--hitch MS]
    tools/gfx_journal_report.py logs/gfx-....jsonl --at 21:35 [--window SECONDS]

--at lists the seconds and the long frames around a time of day (HH:MM or
HH:MM:SS, the player's clock; --window, 60 by default, is the span on each
side), second by second, with the memory readings.

Skips the first SECONDS (default 45: the title and the load), then says how
the frames went and, for each frame over the hitch limit, what the journal
found in it: the cause is the largest of the main loop's own work, the GPU's
busy time and the events (a pipeline built where a draw waited, a fence wait,
an allocation, a texture upload ...). Frames with an event that marks a wrong
picture are listed apart, and the second-by-second memory lines show a drive
or system memory running out.
"""
import collections
import json
import sys


def load(path):
    frames, seconds, head = [], [], {}
    for line in open(path, encoding='utf-8'):
        d = json.loads(line)
        if 'schema' in d:
            head = d
        elif 'flip' in d:
            frames.append(d)
        elif 'sec' in d:
            seconds.append(d)
    return head, frames, seconds


def cause(f):
    parts = [('main loop', f.get('main', 0.0)), ('gpu busy', f.get('gpu', 0.0))]
    for k, v in f.get('ev', {}).items():
        parts.append((k, v[1]))
    parts.sort(key=lambda p: -p[1])
    return parts[0]


def seconds_of(clk):
    h, m, rest = clk.split(':')
    return int(h) * 3600 + int(m) * 60 + float(rest)


def around(path, at, window, hitch):
    _, frames, seconds = load(path)
    parts = [int(x) for x in at.split(':')] + [0, 0]
    centre = parts[0] * 3600 + parts[1] * 60 + parts[2]
    near = lambda d: abs(seconds_of(d['clk']) - centre) <= window
    print('seconds near %s (clock, frames, avg/worst ms, hitches, gpu %%, free MiB: video/system/commit/disk, events):' % at)
    for sec in filter(near, seconds):
        ev = ' '.join('%s:%d/%.0fms' % (k, v[0], v[1]) for k, v in sec.get('ev', {}).items())
        print('  %s %3d fr %5.1f/%5.1f %2d hitch gpu %4.1f%%  %5d/%5d/%5d/%6d  %s' % (
            sec['clk'][:8], sec['frames'], sec['avg'], sec['worst'], sec['hitches'], sec['gpu_pct'], sec['vram_free_mb'],
            sec['sys_free_mb'], sec['commit_free_mb'], sec['disk_free_mb'], ev))
    print('long or wrong frames near %s:' % at)
    for f in filter(near, frames):
        if f['dt'] >= hitch or f.get('wrong'):
            c = cause(f)
            print('  %s flip %-7d %6.1f ms main %5.1f gpu %5.1f draws %-5d fail %-3d fallback %-4d pipes %-3d -> %s %.1f ms%s' % (
                f['clk'], f['flip'], f['dt'], f.get('main', 0), f['gpu'], f['draws'], f['fail'], f['fallback'], f['pipes'], c[0], c[1],
                '  WRONG' if f.get('wrong') else ''))
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    path = args[0]
    if '--at' in args:
        window = float(args[args.index('--window') + 1]) if '--window' in args else 60.0
        hitch = float(args[args.index('--hitch') + 1]) if '--hitch' in args else 20.0
        return around(path, args[args.index('--at') + 1], window, hitch)
    skip = float(args[args.index('--skip') + 1]) if '--skip' in args else 45.0
    hitch = float(args[args.index('--hitch') + 1]) if '--hitch' in args else 20.0
    head, frames, seconds = load(path)
    first = next((f['t'] for f in frames if f['draws'] > 500), 0.0)
    frames = [f for f in frames if f['t'] >= first + skip]
    seconds = [s for s in seconds if s['t'] >= first + skip]
    if not frames:
        print('no frames after the first %.0f s of the world' % skip)
        return 1
    print('mode %s: the journal holds the frames over %.0f ms and wrong ones only' % (head.get('mode'), head.get('hitch_ms', 0))
          if head.get('mode') == 1 else 'mode 2: every frame')
    dts = sorted(f['dt'] for f in frames)
    n = len(dts)
    print('%d frames: median %.2f ms, p99 %.2f, worst %.2f' % (n, dts[n // 2], dts[min(n - 1, int(n * 0.99))], dts[-1]))
    bad = [f for f in frames if f['dt'] >= hitch]
    print('%d frames over %.0f ms' % (len(bad), hitch))
    why = collections.Counter(cause(f)[0] for f in bad)
    for k, c in why.most_common():
        print('  %-18s %d' % (k, c))
    for f in sorted(bad, key=lambda f: -f['dt'])[:10]:
        c = cause(f)
        print('  flip %-7d %6.1f ms  main %5.1f  gpu %5.1f  draws %-5d  -> %s %.1f ms' % (f['flip'], f['dt'], f.get('main', 0), f['gpu'], f['draws'], c[0], c[1]))
    wrong = [f for f in frames if f.get('wrong')]
    print('%d frames with something wrong in the picture' % len(wrong))
    for f in wrong[:10]:
        what = [e for e in f.get('list', []) if e['k'] in ('target_replaced', 'copy_refused', 'dlss_fail', 'draw_fail')]
        print('  flip %d: %s%s' % (f['flip'], f.get('fail_why', ''), ' '.join('%s(%s)' % (e['k'], e.get('tag', e['a'])) for e in what)))
    if seconds:
        lows = lambda key: min((s[key] for s in seconds if s.get(key, -1) >= 0), default=None)
        print('lowest free: video memory %s MiB, system memory %s MiB, commit %s MiB, cache drive %s MiB; working set up to %s MiB' % (
            lows('vram_free_mb'), lows('sys_free_mb'), lows('commit_free_mb'), lows('disk_free_mb'), max(s['ws_mb'] for s in seconds)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
