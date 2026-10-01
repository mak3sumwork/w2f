"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed): re-imports only the sound effects
(docs/sfx/SFX_*.wav: tools/make_sfx.py) into /Game/W2F/Sfx -- much faster than setup_viewer.py."""
import os
import sys

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402  (same folder)

import_blockouts.import_sfx()
