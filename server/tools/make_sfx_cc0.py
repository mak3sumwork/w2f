"""W2F's sound effects from recorded CC0 sounds (demo 2.1, FEEDBACK V11: "The sounds are sooooo bad like this not an arcade game").

Until demo 2.0 every sound was synthesised in code (tools/make_sfx.py): pure tones, very bright (the busiest ones put 40-68% of their energy above 4 kHz)
and loud. This tool builds the same files (docs/sfx/SFX_<Name>.wav, 44.1 kHz mono 16-bit) from real recordings instead: Kenney's sound packs, released
as CC0 (public domain, free for any use, no credit needed -- https://kenney.nl, "License: Creative Commons Zero, CC0"). Each event gets a chosen
recording (sometimes two layered), then: leading silence trimmed, a gentle low-pass (the League-like warm sound: nothing shrill), a short soft room tail,
a fade, and a peak level by how often it plays (the frequent ones -- hovers, hits -- quietest). The client music (SFX_MusicClient) is not touched.

    ~/w2f_bpy/venv/bin/python tools/make_sfx_cc0.py        (needs numpy + soundfile; downloads the packs once into tools/.cc0_sounds, not committed)
    then: W2F_ONLY=Sfx tools/unreal/import_sfx.py (editor closed) to bring them into /Game/W2F/Sfx.
"""

import io
import os
import re
import sys
import urllib.request
import wave
import zipfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "docs", "sfx"))
CACHE = os.path.join(HERE, ".cc0_sounds")
RATE = 44100
GAIN_DB = 4   # on top of every peak below: the set sits a little under the old synthesised one (which peaked at -1..-5 dB), not far under
PACKS = ["interface-sounds", "ui-audio", "rpg-audio", "impact-sounds", "casino-audio", "music-jingles"]

# event -> ( [ (file, gain dB, start offset s) ... ], peak dBFS, low-pass Hz, room mix 0..1 )
I, IM, RPG, CAS, J = "interface-sounds/", "impact-sounds/", "rpg-audio/", "casino-audio/", "music-jingles/"
SOUNDS = {
    # ---- the client and the menus
    "UiHover":        ([(I + "select_002", 0, 0)], -26, 6000, 0.0),
    "Click":          ([(I + "click_003", 0, 0)], -19, 6000, 0.05),
    "UiTab":          ([(I + "select_001", 0, 0)], -18, 6500, 0.08),
    "UiConfirm":      ([(I + "confirmation_004", 0, 0)], -14, 7000, 0.12),
    "UiBack":         ([(I + "back_002", 0, 0)], -16, 6000, 0.08),
    "UiPlay":         ([(I + "maximize_006", 0, 0), (IM + "impactBell_heavy_002", -10, 0.02)], -11, 7000, 0.18),
    "MenuOpen":       ([(I + "maximize_009", 0, 0)], -19, 6000, 0.08),
    "MenuClose":      ([(I + "minimize_009", 0, 0)], -19, 6000, 0.08),
    "Slider":         ([(I + "tick_004", 0, 0)], -25, 6000, 0.0),
    "Error":          ([(I + "error_005", 0, 0)], -15, 5000, 0.06),
    "Toast":          ([(I + "glass_003", 0, 0)], -18, 6500, 0.12),
    "Notify":         ([(I + "question_004", 0, 0)], -14, 6000, 0.12),
    "FriendOnline":   ([(I + "glass_006", 0, 0)], -18, 6500, 0.12),
    "ChatIn":         ([(I + "drop_002", 0, 0)], -16, 6000, 0.08),
    "ChatOut":        ([(I + "drop_001", 0, 0)], -21, 6000, 0.05),
    "QueueStart":     ([(I + "maximize_008", 0, 0)], -14, 6500, 0.15),
    "QueueCancel":    ([(I + "minimize_008", 0, 0)], -16, 6000, 0.1),
    "QueuePop":       ([(J + "Pizzicato jingles/jingles_PIZZI11", 0, 0), (IM + "impactBell_heavy_000", -8, 0)], -9, 8000, 0.2),
    "Accept":         ([(I + "confirmation_002", 0, 0)], -12, 7000, 0.15),
    "Decline":        ([(I + "back_004", 0, 0)], -14, 6000, 0.1),
    "LobbyEnter":     ([(I + "maximize_007", 0, 0)], -16, 6500, 0.15),
    "LoadIn":         ([(IM + "impactBell_heavy_001", 0, 0)], -14, 6000, 0.25),
    "Welcome":        ([(J + "Pizzicato jingles/jingles_PIZZI03", 0, 0)], -13, 7000, 0.2),
    "IconSet":        ([(I + "glass_001", 0, 0)], -16, 6500, 0.12),
    "PartyInvite":    ([(I + "question_003", 0, 0)], -12, 7000, 0.15),
    "PartyJoin":      ([(I + "confirmation_001", 0, 0)], -14, 6500, 0.12),
    "PartyLeave":     ([(I + "minimize_006", 0, 0)], -16, 6000, 0.1),
    "Purchase":       ([(CAS + "Audio/chips-handle-5", -4, 0), (I + "confirmation_004", 0, 0.05)], -12, 6000, 0.15),
    "CoinGain":       ([(CAS + "Audio/chip-lay-3", 0, 0)], -16, 6000, 0.08),
    "MissionComplete": ([(J + "Pizzicato jingles/jingles_PIZZI12", 0, 0)], -12, 7500, 0.2),
    "LevelUp":        ([(J + "Hit jingles/jingles_HIT15", 0, 0)], -11, 7500, 0.2),
    "LpGain":         ([(I + "confirmation_003", 0, 0)], -14, 7000, 0.15),
    "LpLoss":         ([(I + "question_001", 0, 0)], -14, 6000, 0.12),
    "PostGame":       ([(IM + "impactBell_heavy_000", 0, 0)], -12, 6500, 0.25),
    # ---- the match
    "Buy":            ([(CAS + "Audio/chip-lay-3", 0, 0)], -14, 6000, 0.05),
    "Sell":           ([(CAS + "Audio/chips-stack-3", 0, 0)], -17, 4500, 0.05),
    "Reroll":         ([(CAS + "Audio/card-fan-1", 0, 0)], -16, 5000, 0.05),
    "BuyXp":          ([(I + "question_002", 0, 0)], -16, 6000, 0.12),
    "Lock":           ([(I + "toggle_001", 0, 0)], -18, 5000, 0.05),
    "ItemPick":       ([(RPG + "Audio/handleSmallLeather", 0, 0)], -16, 6000, 0.05),
    "Equip":          ([(RPG + "Audio/metalClick", 0, 0)], -16, 4500, 0.08),
    "Combine":        ([(IM + "impactBell_heavy_003", 0, 0), (I + "glass_002", -4, 0.03)], -12, 6500, 0.18),
    "Drop":           ([(IM + "impactSoft_medium_001", 0, 0), (I + "drop_004", -3, 0.02)], -14, 6000, 0.12),
    "CombatStart":    ([(IM + "impactBell_heavy_002", 0, 0)], -11, 6000, 0.25),
    "RoundStart":     ([(IM + "impactBell_heavy_004", 0, 0)], -15, 6000, 0.2),
    "HitMelee":       ([(IM + "impactPunch_medium_000", 0, 0)], -21, 5000, 0.0),
    "HitProjectile":  ([(IM + "impactWood_light_002", 0, 0)], -23, 5000, 0.0),
    "Crit":           ([(IM + "impactPunch_heavy_001", 0, 0)], -15, 5500, 0.05),
    "Cast":           ([(I + "maximize_006", 0, 0)], -20, 5000, 0.1),
    "Death":          ([(IM + "impactSoft_heavy_002", 0, 0)], -16, 4500, 0.08),
    "Heal":           ([(I + "glass_001", 0, 0)], -21, 5000, 0.15),
    "Shield":         ([(IM + "impactMetal_light_001", 0, 0)], -20, 4000, 0.1),
    "StarUp2":        ([(J + "Pizzicato jingles/jingles_PIZZI04", 0, 0)], -12, 7500, 0.15),
    "StarUp3":        ([(J + "Steel jingles/jingles_STEEL04", 0, 0)], -10, 7500, 0.2),
    "TraitUp1":       ([(I + "glass_003", 0, 0)], -17, 6500, 0.12),
    "TraitUp2":       ([(I + "glass_006", 0, 0)], -16, 6500, 0.14),
    "TraitUp3":       ([(I + "confirmation_004", 0, 0)], -14, 7000, 0.16),
    "TraitUp4":       ([(J + "Pizzicato jingles/jingles_PIZZI05", 0, 0)], -13, 7500, 0.18),
    "Unlock":         ([(J + "Steel jingles/jingles_STEEL03", 0, 0)], -10, 7500, 0.2),
    "Victory":        ([(J + "Steel jingles/jingles_STEEL06", 0, 0)], -9, 8000, 0.22),
    "Defeat":         ([(J + "Pizzicato jingles/jingles_PIZZI01", 0, 0)], -11, 6000, 0.2),
}


def fetch():
    """Downloads and unpacks the six packs once (kenney.nl, CC0)."""
    os.makedirs(CACHE, exist_ok=True)
    for pack in PACKS:
        folder = os.path.join(CACHE, pack)
        if os.path.isdir(folder):
            continue
        page = urllib.request.urlopen("https://kenney.nl/assets/" + pack, timeout=30).read().decode("utf-8", "replace")
        match = re.search(r'https://kenney\.nl/media/pages/assets/[^"]*\.zip', page)
        if not match:
            sys.exit("no download link on https://kenney.nl/assets/" + pack)
        data = urllib.request.urlopen(match.group(0), timeout=120).read()
        zipfile.ZipFile(io.BytesIO(data)).extractall(folder)
        print("downloaded", pack)


def find(name):
    """`pack/sub/path/stem` -> the .ogg (the packs keep their files under Audio/, some in sub-folders)."""
    pack, rest = name.split("/", 1)
    stem = os.path.basename(rest)
    for root, _, files in os.walk(os.path.join(CACHE, pack)):
        for f in files:
            if os.path.splitext(f)[0] == stem and f.endswith(".ogg"):
                return os.path.join(root, f)
    sys.exit("missing sound: " + name)


def load(name):
    a, rate = sf.read(find(name), always_2d=True)
    a = a.mean(axis=1)
    if rate != RATE:   # (the packs are 44.1 kHz already; a plain resample if one is not)
        a = np.interp(np.linspace(0, len(a) - 1, int(len(a) * RATE / rate)), np.arange(len(a)), a)
    above = np.nonzero(np.abs(a) > 10 ** (-50 / 20))[0]   # trim the silence before the sound
    return a[above[0]:] if len(above) else a


def lowpass(a, cutoff):
    """Two one-pole passes (12 dB / octave): takes the shrill top off."""
    k = np.exp(-2.0 * np.pi * cutoff / RATE)
    for _ in range(2):
        out = np.empty_like(a)
        y = 0.0
        for i, x in enumerate(a):
            y = (1.0 - k) * x + k * y
            out[i] = y
        a = out
    return a


def room(a, mix):
    """A short soft room (four damped feedback combs): a tail of ~0.4 s at most, so nothing sounds dry and clicky."""
    if mix <= 0:
        return a
    tail = int(0.45 * RATE)
    x = np.concatenate([a, np.zeros(tail)])
    wet = np.zeros_like(x)
    for delay_ms, feedback in ((29.7, 0.72), (37.1, 0.70), (41.1, 0.68), (43.7, 0.66)):
        d = int(delay_ms / 1000 * RATE)
        y = np.zeros_like(x)
        for i in range(d, len(x)):
            y[i] = x[i - d] + feedback * y[i - d]
        wet += y
    wet = lowpass(wet / 4.0, 3500)
    return x * (1 - mix * 0.5) + wet * mix


def build(name, layers, peak_db, cutoff, mix):
    parts = []
    for file, gain_db, offset in layers:
        part = load(file) * 10 ** (gain_db / 20)
        parts.append(np.concatenate([np.zeros(int(offset * RATE)), part]))
    a = np.zeros(max(len(p) for p in parts))
    for p in parts:
        a[:len(p)] += p
    a = lowpass(a, cutoff)
    a = room(a, mix)
    above = np.nonzero(np.abs(a) > 10 ** (-60 / 20))[0]   # drop the silent end, then fade the last 25 ms
    a = a[: above[-1] + 1] if len(above) else a
    fade = min(len(a), int(0.025 * RATE))
    a[len(a) - fade:] *= np.linspace(1, 0, fade)
    a *= 10 ** ((peak_db + GAIN_DB) / 20) / max(np.abs(a).max(), 1e-9)
    with wave.open(os.path.join(OUT, "SFX_%s.wav" % name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((np.clip(a, -1, 1) * 32767).astype("<i2").tobytes())
    return len(a) / RATE


def main():
    fetch()
    only = set(sys.argv[1:])
    for name, (layers, peak, cutoff, mix) in SOUNDS.items():
        if only and name not in only:
            continue
        seconds = build(name, layers, peak, cutoff, mix)
        print("SFX_%-16s %.2f s  peak %d dB" % (name, seconds, peak + GAIN_DB))


if __name__ == "__main__":
    main()
