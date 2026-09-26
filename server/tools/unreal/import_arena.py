"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed): re-imports the classic stage pieces
(docs/models/SM_Arena*.glb, tools/blender/arena_classic.py) with their materials. W2F_ONLY=SM_ArenaFloor,SM_ArenaBase limits it to those pieces."""
import os
import sys
import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402  (same folder)

only = [n for n in os.environ.get("W2F_ONLY", "").split(",") if n]
names = only or ["SM_ArenaFloor", "SM_ArenaBase", "SM_ArenaScenery", "SM_ArenaTrees"]
import_blockouts.import_crafted(names)
