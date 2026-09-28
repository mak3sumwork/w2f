"""Publishes the splash studio's renders (demo 1.6) next to each hero's build, where tools/unreal/import_portraits.py (or setup_viewer.py) picks them up.

    python3 tools/mixamo/studio_publish.py [--studio <project>/Saved/W2F/Studio] [--mixamo <project>/SourceArt/Mixamo]

The studio (UE: -w2fstudio=all, see W2FStudio.cpp) writes splash_<id>.png (1920 x 1080) and portrait_<id>.png (1024 x 1024). This copies them to
<mixamo>/<id>_<Name>/T_Splash_<id>.png (as is) and T_Portrait_<id>.png (512 x 512, macOS sips). A painted splash (splash_arts/<Name>.jpg -> docs/icons, tools/make_icons.py)
still wins over the render when Unreal imports them.
"""
import argparse
import json
import os
import shutil
import subprocess

PROJECT = os.path.expanduser("~/Desktop/work2fightgame/work2fightgame")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--studio", default=os.path.join(PROJECT, "Saved", "W2F", "Studio"))
    ap.add_argument("--mixamo", default=os.path.join(PROJECT, "SourceArt", "Mixamo"))
    a = ap.parse_args()
    with open(os.path.join(a.mixamo, "unit_visuals.json")) as f:
        units = json.load(f)["units"]
    splashes = portraits = 0
    for hid, entry in sorted(units.items()):
        folder = os.path.join(a.mixamo, entry["folder"])
        splash = os.path.join(a.studio, "splash_%s.png" % hid)
        portrait = os.path.join(a.studio, "portrait_%s.png" % hid)
        if os.path.exists(splash):
            shutil.copyfile(splash, os.path.join(folder, "T_Splash_%s.png" % hid))
            splashes += 1
        if os.path.exists(portrait):
            subprocess.run(["sips", "-Z", "512", portrait, "--out", os.path.join(folder, "T_Portrait_%s.png" % hid)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            portraits += 1
    print("published %d splash arts and %d portraits into %s" % (splashes, portraits, a.mixamo))


if __name__ == "__main__":
    main()
