"""Run INSIDE the Unreal Editor or headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed).

Imports the Mixamo stand-in heroes built by tools/mixamo/build_heroes.py (SourceArt/Mixamo/<id>_<Name>/SKM_*.fbx + T_*.png + unit_visuals.json):
  * each FBX becomes /Game/W2F/Heroes/<id>_<Name>/ : skeletal mesh + its own skeleton + one AnimSequence per clip (A_<clip>),
  * the PNG textures are imported next to it, and every material slot gets a material instance: M_CraftedPBR (albedo + emissive) for the body, M_W2FSolid (flat colour + glow)
    for the props,
  * unit_visuals.json is copied to Content/W2F/Data/, where AW2FArena reads it.
Environment: W2F_MIXAMO_OUT = the SourceArt/Mixamo folder (default: <project>/SourceArt/Mixamo); W2F_ONLY = comma-separated hero ids.
"""
import json
import os
import shutil
import sys

import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402  (M_CraftedPBR)

PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SRC = os.environ.get("W2F_MIXAMO_OUT") or os.path.join(PROJECT, "SourceArt", "Mixamo")
DEST = "/Game/W2F/Heroes"
MAT_DIR = "/Game/W2F/Materials"
DATA_DIR = os.path.join(PROJECT, "Content", "W2F", "Data")
ONLY = set(filter(None, os.environ.get("W2F_ONLY", "").split(",")))

lib = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def solid_material():
    """M_W2FSolid: flat BaseColor / Glow vectors, Metallic / Roughness scalars (the props)."""
    name, path = "M_W2FSolid", MAT_DIR + "/M_W2FSolid"
    if assets.does_asset_exist(path):
        mat = assets.load_asset(path)
        lib.delete_all_material_expressions(mat)
    else:
        mat = tools.create_asset(name, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    base = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -500, -100)
    base.set_editor_property("parameter_name", "BaseColor")
    glow = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -500, 200)
    glow.set_editor_property("parameter_name", "Glow")
    glow.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 1))
    metal = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -500, 50)
    metal.set_editor_property("parameter_name", "Metallic")
    rough = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -500, 120)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.5)
    ok = lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    ok = lib.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    ok = lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC) and ok
    ok = lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS) and ok
    if not ok:
        unreal.log_error("W2F: M_W2FSolid connections failed")
    mat.set_editor_property("used_with_skeletal_mesh", True)
    lib.recompile_material(mat)
    assets.save_loaded_asset(mat)
    return mat


def import_texture(png, folder):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", png)
    task.set_editor_property("destination_path", folder)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    tools.import_asset_tasks([task])
    name = os.path.splitext(os.path.basename(png))[0]
    return assets.load_asset("%s/%s" % (folder, name))


def import_hero(hid, entry):
    folder = entry["folder"]
    src = os.path.join(SRC, folder)
    dest = "%s/%s" % (DEST, folder)
    if assets.does_directory_exist(dest):
        assets.delete_directory(dest)       # a clean re-import: no stale clips or material slots
    fbx = os.path.join(src, entry["mesh"] + ".fbx")
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", fbx)
    task.set_editor_property("destination_path", dest)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    tools.import_asset_tasks([task])
    imported = list(task.get_editor_property("imported_object_paths") or [])
    mesh = None
    for path in assets.list_assets(dest, recursive=True, include_folder=False):
        a = assets.load_asset(path)
        if isinstance(a, unreal.SkeletalMesh):
            mesh = a
    if mesh is None:
        unreal.log_error("W2F: %s: no skeletal mesh after importing %s (%s)" % (folder, fbx, imported))
        return False
    anims = [p for p in assets.list_assets(dest, recursive=True, include_folder=False) if isinstance(assets.load_asset(p), unreal.AnimSequence)]
    unreal.log("W2F: %s: mesh %s, %d clips: %s" % (folder, mesh.get_path_name(), len(anims), ", ".join(p.rsplit(".", 1)[-1] for p in anims)))

    # materials
    parent = import_blockouts.crafted_parent_material(False)
    if not parent.get_editor_property("used_with_skeletal_mesh"):
        parent.set_editor_property("used_with_skeletal_mesh", True)
        lib.recompile_material(parent)
        assets.save_loaded_asset(parent, only_if_is_dirty=False)
    solid = solid_material()
    tex_dir = dest + "/Textures"
    by_slot = {}
    for m in entry.get("materials", []):
        inst_name = "MI_%s_%s" % (folder, m["slot"].replace(".", "_"))
        inst = tools.create_asset(inst_name, dest + "/Materials", unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        if "albedo" in m:
            inst.set_editor_property("parent", parent)
            lib.set_material_instance_texture_parameter_value(inst, "BaseColorTexture", import_texture(os.path.join(src, m["albedo"] + ".png"), tex_dir))
            emit_png = os.path.join(src, m.get("emit", "") + ".png")
            emit = import_texture(emit_png, tex_dir) if "emit" in m and os.path.exists(emit_png) else None
            if emit is not None:
                lib.set_material_instance_texture_parameter_value(inst, "EmissiveTexture", emit)
                lib.set_material_instance_scalar_parameter_value(inst, "EmissiveBoost", float(m.get("emitBoost", 4.0)))
            else:   # never leave the parameter empty: its default is the engine's WHITE texture, which makes the whole hero glow
                lib.set_material_instance_texture_parameter_value(inst, "EmissiveTexture", unreal.load_object(None, "/Engine/EngineResources/Black.Black"))
            lib.set_material_instance_scalar_parameter_value(inst, "Metallic", 0.35)
            lib.set_material_instance_scalar_parameter_value(inst, "Roughness", 0.5)
        else:
            inst.set_editor_property("parent", solid)
            c = m.get("colour", [0.5, 0.5, 0.5])
            lib.set_material_instance_vector_parameter_value(inst, "BaseColor", unreal.LinearColor(c[0], c[1], c[2], 1))
            g = m.get("glow")
            if g:
                lib.set_material_instance_vector_parameter_value(inst, "Glow", unreal.LinearColor(g[0] * 3, g[1] * 3, g[2] * 3, 1))
            lib.set_material_instance_scalar_parameter_value(inst, "Metallic", m.get("metallic", 0.5))
            lib.set_material_instance_scalar_parameter_value(inst, "Roughness", m.get("roughness", 0.5))
        assets.save_loaded_asset(inst)
        by_slot[m["slot"]] = inst
    mats = mesh.get_editor_property("materials")
    for i, sm in enumerate(mats):
        name = str(sm.get_editor_property("material_slot_name"))
        inst = by_slot.get(name) or next((v for k, v in by_slot.items() if k.lower() == name.lower() or name.lower().startswith(k.lower())), None)
        if inst is None:
            unreal.log_warning("W2F: %s: no material for slot %s (have %s)" % (folder, name, list(by_slot)))
            continue
        sm.set_editor_property("material_interface", inst)
        mats[i] = sm
    mesh.set_editor_property("materials", mats)
    mesh.modify()
    assets.save_loaded_asset(mesh, only_if_is_dirty=False)   # set_editor_property does not mark the package dirty
    return True


def main():
    index_path = os.path.join(SRC, "unit_visuals.json")
    with open(index_path) as f:
        index = json.load(f)
    done = 0
    for hid, entry in sorted(index["units"].items()):
        if ONLY and hid not in ONLY:
            continue
        if import_hero(hid, entry):
            done += 1
    # the rendered portraits replace the generated busts (same names, /Game/W2F/Icons/T_Portrait_<id>)
    portraits = []
    for hid, entry in sorted(index["units"].items()):
        if ONLY and hid not in ONLY:
            continue
        png = os.path.join(SRC, entry["folder"], "T_Portrait_%s.png" % hid)
        if os.path.exists(png):
            t = unreal.AssetImportTask()
            t.set_editor_property("filename", png)
            t.set_editor_property("destination_path", "/Game/W2F/Icons")
            t.set_editor_property("automated", True)
            t.set_editor_property("replace_existing", True)
            t.set_editor_property("save", True)
            portraits.append(t)
    if portraits:
        tools.import_asset_tasks(portraits)
        unreal.log("W2F: %d hero portraits" % len(portraits))
    os.makedirs(DATA_DIR, exist_ok=True)
    shutil.copyfile(index_path, os.path.join(DATA_DIR, "unit_visuals.json"))
    unreal.log("W2F: imported %d heroes; unit_visuals.json copied to %s" % (done, DATA_DIR))


LOG_LINES = []
_log = unreal.log


def _tee(msg):
    LOG_LINES.append(str(msg))
    _log(msg)


unreal.log = _tee
try:
    main()
except Exception:   # a headless run hides Python errors in the middle of the engine log: keep them next to the input as well
    import traceback
    LOG_LINES.append(traceback.format_exc())
    unreal.log_error(traceback.format_exc())
finally:
    with open(os.path.join(SRC, "import_heroes.log"), "w") as f:
        f.write("\n".join(LOG_LINES) + "\n")
