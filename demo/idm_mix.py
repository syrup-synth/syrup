import wave, numpy as np
from scipy.signal import fftconvolve, butter, sosfilt
SR = 32728; BPM = 156
def ld(i):
    w = wave.open(f'sd/tape/track{i}.wav'); return np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).astype(float) / 32768
S = [ld(i) for i in range(1, 5)]
w = wave.open('drums/sd/tape/track1.wav'); D = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).astype(float) / 32768
n = max(len(x) for x in S + [D]); S = [np.vstack([x, np.zeros((n - len(x), 2))]) for x in S]; D = np.vstack([D, np.zeros((n - len(D), 2))])
def f(x, kind, fc, o=2): return sosfilt(butter(o, fc, kind, fs=SR, output='sos'), x, axis=0)
def shelf_hi(x, fc, g): return x + (10 ** (g / 20) - 1) * f(x, 'high', fc, 1)
def dip(x, lo, hi, g): return x + (10 ** (g / 20) - 1) * f(x, 'band', [lo, hi])
# clean up each track: only the sub and the kick keep the bottom; scoop the 200-450 Hz mud out of the chords / melody
S[0] = shelf_hi(dip(S[0], 220, 450, -3), 6000, 2)          # drums + acid
S[1] = shelf_hi(dip(f(S[1], 'high', 160), 200, 500, -5), 5000, 3)   # chords
S[2] = shelf_hi(f(S[2], 'high', 220), 6000, 2)              # melody
S[3] = f(f(S[3], 'low', 900), 'high', 30)                   # sub: only the sub
G   = [0.6, 1.5, 0.85, 0.75]
REV = [0.08, 0.45, 0.4, 0.0]
DLY = [0.08, 0.08, 0.3, 0.0]
for i in range(4): print('track', i + 1, 'peak', round(np.abs(S[i]).max(), 2), 'rms', round(np.sqrt((S[i] ** 2).mean()), 3))
rng = np.random.default_rng(3); L = int(SR * 3.0); t = np.arange(L) / SR
ir = np.zeros((L, 2))
for c in range(2): ir[:, c] = f(rng.standard_normal(L) * np.exp(-t * 2.2), 'low', 9000)
ir /= np.sqrt((ir ** 2).sum(0)); ir = np.vstack([np.zeros((int(SR * 0.04), 2)), ir])
send_r = f(sum(S[i] * G[i] * REV[i] for i in range(4)), 'high', 450)
rev = np.stack([fftconvolve(send_r[:, c], ir[:, c])[:n] for c in range(2)], 1)
send_d = f(sum(S[i] * G[i] * DLY[i] for i in range(4)), 'band', [400, 6000])
d = int(SR * 60 / BPM * 0.75); dl = np.zeros_like(send_d); fb = 0.45
for k in range(1, 7):
    sh = d * k
    if sh >= n: break
    tap = send_d[:n - sh] * fb ** (k - 1)
    if k % 2: dl[sh:, 1] += tap[:, 0] * 0.85 + tap[:, 1] * 0.15
    else: dl[sh:, 0] += tap[:, 1] * 0.85 + tap[:, 0] * 0.15
# DRUMS: dry and up front - a kick shelf, air on top, and a squashed copy underneath (parallel compression) for weight
def sh_lo(x, fc, g): return x + (10 ** (g / 20) - 1) * f(x, 'low', fc, 1)
Dp = shelf_hi(sh_lo(f(D, 'high', 28), 90, 3), 5000, 3)
env = np.maximum(np.abs(Dp).max(1), 1e-6); e = np.copy(env)
for i in range(1, len(e)): e[i] = max(env[i], e[i - 1] * 0.9995)
sq = Dp * np.minimum(1, 0.12 / e)[:, None] * 3.5
drums = Dp + sq * 0.35
room = np.stack([fftconvolve(f(drums, 'high', 600)[:, c] * 0.05, ir[:int(SR * 0.6), c])[:n] for c in range(2)], 1)
mix = sum(S[i] * G[i] for i in range(4)) + rev + dl * 0.8 + drums * 1.0 + room
pk = np.abs(mix).max(); print('summed peak', round(pk, 2), '(%.1f dBFS)' % (20 * np.log10(pk)))
# headroom: the mix is levelled so it peaks at -1 dBFS with no saturation; a 3 ms look-ahead limiter only catches the odd transient above -3 dB
mix *= 10 ** (-1 / 20) / np.percentile(np.abs(mix), 99.999)
env = np.maximum.accumulate(np.zeros(1))  # (placeholder)
a = np.abs(mix).max(1); thr = 10 ** (-1 / 20)
g = np.minimum(1, thr / np.maximum(a, 1e-9)); w = int(SR * 0.003)
g = np.minimum.reduce([np.roll(g, -k) for k in range(w)]) ; gs = np.copy(g)
for i in range(1, len(gs)): gs[i] = min(g[i], gs[i - 1] + (1 - gs[i - 1]) * 0.0015)
mix *= gs[:, None]
fo = int(SR * 2.5); mix[-fo:] *= np.linspace(1, 0, fo)[:, None]
print('final peak %.2f dBFS, rms %.1f dBFS, limited samples %d' % (20 * np.log10(np.abs(mix).max()), 20 * np.log10(np.sqrt((mix ** 2).mean())), int((gs < 0.999).sum())))
w = wave.open('syrup_idm3.wav', 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(mix, -1, 1) * 32767).astype(np.int16).tobytes()); w.close()
