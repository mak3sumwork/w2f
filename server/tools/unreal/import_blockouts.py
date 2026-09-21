"""Run INSIDE the Unreal Editor (5.x, Python Editor Script Plugin enabled): imports the W2F blockout models and gives them one vertex-colour material.

    In the editor: Tools > Execute Python Script... > choose this file.        (or, in the Output Log's Cmd box:  py "/full/path/to/import_blockouts.py")

What it does (idempotent: run it again after regenerating the models):
  1. imports every SM_*.glb of the blockout folder into /Game/W2F/Blockouts (replacing what is there),
  2. creates /Game/W2F/Materials/M_BlockoutVC (Base Color = the mesh's vertex colour), if it does not exist,
  3. puts that material on every imported static mesh and saves everything.
Set the environment variable W2F_BLOCKOUTS to the folder if this file is not inside the repo (the default is ../../docs/blockouts next to this script).
"""
import os
import unreal

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
SRC = os.environ.get("W2F_BLOCKOUTS") or os.path.normpath(os.path.join(HERE, "..", "..", "docs", "blockouts"))
DEST = "/Game/W2F/Blockouts"
MAT_DIR = "/Game/W2F/Materials"
MAT_NAME = "M_BlockoutVC"


def import_models():
    files = sorted(f for f in os.listdir(SRC) if f.startswith("SM_") and f.endswith(".glb"))
    if not files:
        unreal.log_error("W2F: no SM_*.glb files in %s (run tools/make_blockouts.py first)" % SRC)
        return 0
    tasks = []
    for f in files:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", os.path.join(SRC, f))
        task.set_editor_property("destination_path", DEST)
        task.set_editor_property("automated", True)          # no import dialog
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("replace_existing_settings", True)
        task.set_editor_property("save", True)
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    unreal.log("W2F: imported %d models from %s" % (len(files), SRC))
    return len(files)


def vertex_colour_material():
    """M_BlockoutVC: Base Color = the mesh's vertex colour. (Re)builds the graph every time, so an earlier broken version is repaired.
    NB: the Vertex Color node's outputs are all UNNAMED, the first one ("") is RGB: naming it "RGB" connects nothing and the material renders black."""
    path = "%s/%s" % (MAT_DIR, MAT_NAME)
    lib = unreal.MaterialEditingLibrary
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        material = unreal.EditorAssetLibrary.load_asset(path)
        lib.delete_all_material_expressions(material)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(MAT_NAME, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    colour = lib.create_material_expression(material, unreal.MaterialExpressionVertexColor, -400, 0)
    if not lib.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR):
        unreal.log_error("W2F: could not connect the vertex colour to Base Color")
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("W2F: %s rebuilt" % path)
    return material


def assign_material(material):
    done = 0
    for asset_path in unreal.EditorAssetLibrary.list_assets(DEST, recursive=True, include_folder=False):
        asset = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not isinstance(asset, unreal.StaticMesh):
            continue
        for slot in range(len(asset.static_materials)):
            asset.set_material(slot, material)
        unreal.EditorAssetLibrary.save_loaded_asset(asset)
        done += 1
    unreal.log("W2F: %s applied to %d static meshes" % (MAT_NAME, done))


if __name__ == "__main__":
    if import_models():
        assign_material(vertex_colour_material())
