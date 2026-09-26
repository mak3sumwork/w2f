"""The sound effects of the Unreal client (docs/sfx/SFX_*.wav): synthesised with numpy -- tones, FM bells, shaped noise, pitch sweeps -- so they are original
and need no downloads. 44.1 kHz, mono, 16-bit. Any Python 3 with numpy:

    python3 tools/make_sfx.py            (or ~/w2f_bpy/venv/bin/python tools/make_sfx.py)

tools/unreal/import_blockouts.py import_sfx() brings them into /Game/W2F/Sfx; AW2FArena::PlaySfx plays them by name (SFX_<Name>).
"""
import os
import wave

import numpy as np

OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "sfx"))
SR = 44100
rng = np.random.default_rng(1234)


def t_of(seconds):
    return np.arange(int(SR * seconds), dtype=np.float64) / SR


def env(t, attack=0.005, decay=0.3, curve=4.0):
    """Fast attack, exponential decay."""
    a = np.clip(t / max(attack, 1e-4), 0, 1)
    return a * np.exp(-curve * np.maximum(t - attack, 0) / max(decay, 1e-4))


def tone(freq, seconds, partials=((1, 1.0),), attack=0.004, decay=0.4, curve=4.0, sweep=0.0):
    """A pitched tone with harmonic partials; `sweep` = octaves glided over the sound."""
    t = t_of(seconds)
    f = freq * (2.0 ** (sweep * t / seconds))
    phase = 2 * np.pi * np.cumsum(f) / SR
    out = sum(a * np.sin(k * phase) for k, a in partials)
    return out * env(t, attack, decay, curve)


def bell(freq, seconds, index=2.5, ratio=3.5, decay=0.6):
    """FM bell: bright, metallic -- coins, chimes."""
    t = t_of(seconds)
    mod_env = np.exp(-6 * t / seconds)
    mod = index * mod_env * np.sin(2 * np.pi * freq * ratio * t)
    return np.sin(2 * np.pi * freq * t + mod) * env(t, 0.002, decay, 4.0)


def noise(seconds, lowpass=None, highpass=None, attack=0.002, decay=0.15, curve=5.0):
    t = t_of(seconds)
    n = rng.standard_normal(len(t))
    if lowpass:
        a = np.exp(-2 * np.pi * lowpass / SR)
        y = np.empty_like(n); acc = 0.0
        for i in range(len(n)): acc = a * acc + (1 - a) * n[i]; y[i] = acc
        n = y * 3
    if highpass:
        a = np.exp(-2 * np.pi * highpass / SR)
        y = np.empty_like(n); acc = 0.0; prev = 0.0
        for i in range(len(n)): acc = a * (acc + n[i] - prev); prev = n[i]; y[i] = acc
        n = y
    return n * env(t, attack, decay, curve)


def mix(*parts, at=None):
    """Adds sounds (optionally starting at the given times, seconds)."""
    at = at or [0.0] * len(parts)
    length = max(int(s * SR) + len(p) for p, s in zip(parts, at))
    out = np.zeros(length)
    for p, s in zip(parts, at):
        i = int(s * SR); out[i:i + len(p)] += p
    return out


def save(name, x, gain=0.8):
    x = np.asarray(x, dtype=np.float64)
    peak = np.max(np.abs(x)) or 1.0
    x = x / peak * gain
    fade = min(len(x), int(0.01 * SR))                     # no click at the end
    x[-fade:] *= np.linspace(1, 0, fade)
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, "SFX_%s.wav" % name), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
    print("wrote SFX_%s.wav (%.2f s)" % (name, len(x) / SR))


def main():
    # ---- the shop and items
    save("Buy", mix(bell(1318, 0.35, 2.0, 3.5, 0.25), bell(1760, 0.35, 1.5, 3.5, 0.25), at=[0, 0.05]), 0.6)
    save("Sell", mix(bell(988, 0.3, 2.0, 3.5, 0.2), bell(740, 0.35, 2.0, 3.5, 0.25), noise(0.15, lowpass=3000, decay=0.08), at=[0, 0.06, 0]), 0.6)
    save("Reroll", mix(noise(0.35, lowpass=6000, highpass=800, attack=0.05, decay=0.25, curve=3.0), tone(600, 0.3, ((1, 1), (2, 0.3)), 0.01, 0.2, 3, sweep=1.0) * 0.3), 0.55)
    save("BuyXp", mix(*[tone(f, 0.25, ((1, 1), (2, 0.4), (3, 0.2)), 0.004, 0.18) for f in (523, 659, 784, 1046)], at=[0, 0.06, 0.12, 0.18]), 0.55)
    save("Lock", mix(noise(0.05, highpass=2000, decay=0.02), tone(1800, 0.06, ((1, 1),), 0.001, 0.03)), 0.5)
    save("Click", mix(noise(0.03, highpass=3000, decay=0.012), tone(2400, 0.03, ((1, 1),), 0.001, 0.015)), 0.35)
    save("ItemPick", tone(880, 0.12, ((1, 1), (2, 0.3)), 0.002, 0.07, sweep=0.5), 0.45)
    save("Equip", mix(bell(620, 0.4, 3.0, 2.76, 0.3), noise(0.08, lowpass=2500, decay=0.04), at=[0, 0]), 0.6)
    save("Combine", mix(bell(440, 0.8, 4.0, 2.76, 0.6), bell(660, 0.8, 3.0, 2.76, 0.6), bell(880, 0.9, 2.0, 3.5, 0.7), at=[0, 0.08, 0.16]), 0.6)
    # ---- upgrades and loot
    save("StarUp2", mix(*[bell(f, 0.9, 1.8, 3.5, 0.6) for f in (784, 988, 1175, 1568)], at=[0, 0.07, 0.14, 0.21]), 0.7)
    save("StarUp3", mix(*[bell(f, 1.4, 2.0, 3.5, 0.9) for f in (523, 659, 784, 1046, 1318, 1568)] + [tone(261, 1.2, ((1, 1), (2, 0.5), (3, 0.3)), 0.02, 1.0, 2.5)],
                        at=[0, 0.06, 0.12, 0.18, 0.24, 0.30, 0]), 0.75)
    save("Drop", mix(tone(700, 0.2, ((1, 1), (2, 0.3)), 0.002, 0.12, sweep=1.2), bell(1400, 0.4, 1.5, 3.5, 0.3), at=[0, 0.08]), 0.55)
    save("Unlock", mix(*[bell(f, 1.2, 2.5, 3.5, 0.8) for f in (392, 494, 587, 784)] + [noise(0.6, lowpass=2000, attack=0.2, decay=0.4, curve=3.0) * 0.3],
                       at=[0, 0.1, 0.2, 0.3, 0]), 0.7)
    # ---- the round
    save("RoundStart", mix(tone(196, 1.6, ((1, 1), (2, 0.6), (3, 0.35), (4.2, 0.2), (5.4, 0.12)), 0.01, 1.2, 3.0), noise(0.4, lowpass=800, decay=0.3) * 0.4), 0.6)
    save("CombatStart", mix(*[mix(tone(70, 0.4, ((1, 1), (1.5, 0.4)), 0.002, 0.25, 5.0, sweep=-0.4), noise(0.2, lowpass=400, decay=0.1)) for _ in range(3)],
                            tone(330, 0.8, ((1, 1), (2, 0.5), (3, 0.3)), 0.05, 0.6, 3.0) * 0.5, at=[0, 0.22, 0.44, 0.44]), 0.7)
    save("Victory", mix(*[tone(f, 0.6, ((1, 1), (2, 0.5), (3, 0.25)), 0.01, 0.5, 3.0) for f in (523, 659, 784)] + [tone(1046, 1.2, ((1, 1), (2, 0.5), (3, 0.25)), 0.01, 1.0, 2.5)],
                        at=[0, 0.12, 0.24, 0.36]), 0.65)
    save("Defeat", mix(tone(311, 0.7, ((1, 1), (2, 0.4)), 0.02, 0.6, 3.0), tone(277, 0.7, ((1, 1), (2, 0.4)), 0.02, 0.6, 3.0), tone(233, 1.3, ((1, 1), (2, 0.4)), 0.02, 1.2, 2.5),
                       at=[0, 0.25, 0.5]), 0.6)
    # ---- combat (short and soft: they play often)
    save("HitMelee", mix(noise(0.12, lowpass=1500, decay=0.05, curve=6), tone(110, 0.12, ((1, 1),), 0.001, 0.06, 6, sweep=-0.8)), 0.5)
    save("HitProjectile", mix(noise(0.08, lowpass=4000, highpass=600, decay=0.03), tone(900, 0.08, ((1, 1),), 0.001, 0.04, 6, sweep=-1.5) * 0.4), 0.4)
    save("Crit", mix(noise(0.18, highpass=2500, decay=0.08), tone(1500, 0.15, ((1, 1), (2, 0.3)), 0.001, 0.08, 5, sweep=-1.0) * 0.5, tone(90, 0.15, ((1, 1),), 0.001, 0.08, 6) * 0.6), 0.55)
    save("Cast", mix(noise(0.6, lowpass=5000, highpass=1500, attack=0.08, decay=0.4, curve=3) * 0.6, tone(660, 0.6, ((1, 1), (1.5, 0.5), (2, 0.3)), 0.05, 0.4, 3, sweep=1.0) * 0.5), 0.5)
    save("Heal", mix(*[bell(f, 0.6, 1.0, 2.0, 0.5) for f in (1046, 1318, 1568)], at=[0, 0.05, 0.1]), 0.35)
    save("Shield", mix(bell(523, 0.5, 3.0, 1.41, 0.4), noise(0.3, lowpass=2500, attack=0.03, decay=0.2) * 0.4), 0.4)
    save("Death", mix(noise(0.5, lowpass=1200, attack=0.01, decay=0.35, curve=3), tone(160, 0.5, ((1, 1), (2, 0.3)), 0.005, 0.4, 3, sweep=-1.2) * 0.6), 0.5)


main()
