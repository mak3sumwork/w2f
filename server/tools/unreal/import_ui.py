"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed): re-imports only the UI glyphs and HUD art
(docs/icons/T_UI_*.png: tools/make_ui_icons.py, tools/make_hud_art.py) -- much faster than setup_viewer.py, which re-imports every model and icon."""
import os
import sys
import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402  (same folder)

import_blockouts.import_icons("T_UI_")
