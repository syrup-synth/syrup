#!/usr/bin/env python3
"""Syrup host tests: build the simulator from source/main.c, run every test, compare patch levels to the baseline.
   python3 tests/host/run_tests.py            run everything
   python3 tests/host/run_tests.py --update   accept the current patch levels as the new baseline"""
import math, os, subprocess, sys, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(ROOT, 'build', 'host'); BASE = os.path.join(ROOT, 'tests', 'baseline', 'levels.txt')
UPDATE = '--update' in sys.argv
fails = []
NEEDS_SENSOR = {'HAND WAVE'}   # silent until the camera sees movement
def sh(cmd, **k): return subprocess.run(cmd, cwd=BUILD, capture_output=True, text=True, **k)
os.makedirs(BUILD, exist_ok=True)
subprocess.run([os.path.join(HERE, 'gen.sh'), BUILD], check=True)
for f in ('fake3ds.h', 'fakecitro.h', 'harness.h', 'default_font.bin'): shutil.copy(os.path.join(HERE, f), BUILD)
for t in ('test_patch', 'test_buzz', 'test_timing', 'test_ui', 'test_kit', 'test_chip', 'test_vis', 'test_format', 'test_screens', 'test_seq', 'test_adv', 'test_tape', 'test_loop'):
    shutil.copy(os.path.join(HERE, t + '.c'), BUILD)
    r = sh(['gcc', '-O1', '-w', '-I', os.path.join(ROOT, 'source'), t + '.c', '-o', t, '-lm', '-lpthread', '-lcurl'])
    if r.returncode: print(r.stderr[-3000:]); sys.exit('build failed: ' + t)
def run(name, args, timeout=120):
    for f in ('seq.bin', 'settings.bin'):   # every test starts from defaults: first page, default pattern
        try: os.remove(os.path.join(BUILD, 'sd', f))
        except OSError: pass
    try: r = sh(['./' + name] + args, timeout=timeout)
    except subprocess.TimeoutExpired: fails.append(f'{name} {args}: timed out'); return None
    if r.returncode not in (0, 1): fails.append(f'{name} {args}: crashed (exit {r.returncode})'); print(r.stdout, r.stderr[-2000:]); return None
    sys.stdout.write(r.stdout)
    fails.extend(l[6:] for l in r.stdout.splitlines() if l.startswith('FAIL: '))
    return r
os.makedirs(os.path.join(BUILD, 'wav'), exist_ok=True)
run('test_timing', [])
run('test_buzz', ['core1', 'wav/buzz_core1.wav']); run('test_buzz', ['core2', 'wav/buzz_core2.wav']); run('test_buzz', ['core3', 'wav/buzz_core3.wav'])
run('test_ui', [])
for k in range(6): run('test_kit', [str(k)])
for k in range(4): run('test_chip', [str(k)])
run('test_vis', [])
run('test_format', [])
run('test_screens', [])
run('test_seq', [])
run('test_adv', [])
run('test_tape', [])
run('test_loop', [], timeout=300)
# every factory patch
base = {}
if os.path.exists(BASE):
    for l in open(BASE):
        if l.startswith('#'): continue
        p = l.split(None, 2)
        if len(p) == 3: base[p[2].strip()] = float(p[1])   # key: 'style/index NAME'
rows = []
for f in ('seq.bin', 'settings.bin'):
    try: os.remove(os.path.join(BUILD, 'sd', f))
    except OSError: pass
for st in range(32):
    for ix in range(16):
        r = sh(['./test_patch', str(st), str(ix)], timeout=60)
        if r.returncode == 3: continue
        if r.returncode:
            fails.append(f'patch {st}/{ix}: crashed (exit {r.returncode})'); continue
        p = r.stdout.split(None, 7); a, b, pk, clips, dc, name = float(p[2]), float(p[3]), int(p[4]), int(p[5]), float(p[6]), p[7].strip()
        lv = max(a, b); key = f'{st}/{ix} {name}'; rows.append((key, lv))
        bad = []
        if lv < 30 and name not in NEEDS_SENSOR: bad.append('silent')
        if clips > 0.01 * 2 * 32728 * 1.8: bad.append(f'clips {clips} samples')
        if abs(dc) > 4000: bad.append(f'DC offset {dc:.0f}')   # chip pulse waves carry some DC by nature; this catches runaway
        if key in base and not UPDATE:
            d = 20 * math.log10(max(lv, 1) / max(base[key], 1))
            if abs(d) > 3: bad.append(f'level {d:+.1f} dB vs baseline')
        elif not UPDATE: bad.append('not in baseline (run with --update)')
        if bad:
            fails.append(f'patch {name}: ' + ', '.join(bad)); sh(['./test_patch', str(st), str(ix), f'wav/{name}.wav'])
print(f'{len(rows)} patches played')
if UPDATE:
    os.makedirs(os.path.dirname(BASE), exist_ok=True)
    with open(BASE, 'w') as f:
        f.write('# patch level (rms of the louder window) written by run_tests.py --update\n')
        for n, lv in rows: f.write(f'level {lv:.1f} {n}\n')
    print('baseline updated:', BASE)
try:   # screenshots as PNG for anyone looking at the CI artifacts
    from PIL import Image
    sd = os.path.join(BUILD, 'shots')
    for f in os.listdir(sd):
        if f.endswith('.ppm'): Image.open(os.path.join(sd, f)).resize((Image.open(os.path.join(sd, f)).width * 2, 480), Image.NEAREST).save(os.path.join(sd, f[:-4] + '.png')); os.remove(os.path.join(sd, f))
except Exception as e: print('(no PNG conversion:', e, ')')
print('\n' + ('ALL PASSED' if not fails else f'{len(fails)} FAILED:\n  ' + '\n  '.join(fails)))
sys.exit(1 if fails else 0)
