"""Headless: rebuild M_CraftedPBR / M_CraftedPBR_N (with the hit-flash parameters) without re-importing anything; every hero's material instance picks the change up."""
import os
import sys

import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402

for normal in (False, True):
    mat = import_blockouts.crafted_parent_material(normal)
    mat.set_editor_property("used_with_skeletal_mesh", True)
    unreal.MaterialEditingLibrary.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat, only_if_is_dirty=False)
unreal.log("W2F: crafted materials rebuilt")
