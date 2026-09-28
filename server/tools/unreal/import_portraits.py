"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed) or inside the editor.

Demo 1.6: a quick re-import of the champion art only, after tools/make_icons.py (the designer's painted splash arts) and tools/mixamo/studio_publish.py (the splash
studio's renders): docs/icons/T_Splash_* first, then every hero's T_Portrait_<id> / T_Splash_<id> from SourceArt/Mixamo (a painted splash wins over the render).
The whole set-up (models, arena, icons) stays setup_viewer.py.
"""
import os
import sys

import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402

tasks = []
if os.path.isdir(import_blockouts.ICONS_SRC):
    for name in sorted(os.listdir(import_blockouts.ICONS_SRC)):
        if name.startswith("T_Splash_") and name.endswith(".png"):
            t = unreal.AssetImportTask()
            t.set_editor_property("filename", os.path.join(import_blockouts.ICONS_SRC, name))
            t.set_editor_property("destination_path", import_blockouts.ICONS_DEST)
            t.set_editor_property("automated", True)
            t.set_editor_property("replace_existing", True)
            t.set_editor_property("save", True)
            tasks.append(t)
if tasks:
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
unreal.log("W2F: %d painted splash arts" % len(tasks))
import_blockouts.import_hero_portraits()
unreal.log("W2F: portraits ready")
