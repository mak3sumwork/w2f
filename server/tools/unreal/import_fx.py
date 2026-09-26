"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed) or inside the editor.

Imports the fight-VFX meshes built by tools/mixamo/build_fx_meshes.py (<project>/SourceArt/FX/FX_*.glb) into /Game/W2F/FX, (re)builds the two effect materials and copies
tools/mixamo/fx.json (which effect each champion uses) to Content/W2F/Data/unit_fx.json, where UW2FFx reads it:
  M_W2FFx      unlit, ADDITIVE, two-sided: Emissive = Colour * Intensity * (Base + Fresnel * Rim). Everything that glows.
  M_W2FFxDark  unlit, translucent, two-sided: Emissive = Colour * Intensity, Opacity = Opacity. Shadows and voids (Eclipse, Gravity Well).
"""
import os
import shutil

import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
PROJECT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SRC = os.path.join(PROJECT, "SourceArt", "FX")
DEST = "/Game/W2F/FX"
DATA_DIR = os.path.join(PROJECT, "Content", "W2F", "Data")
FX_JSON = os.path.normpath(os.path.join(HERE, "..", "mixamo", "fx.json"))

lib = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
LOG = []


def log(msg):
    LOG.append(msg)
    unreal.log(msg)


def fresh_material(name):
    path = "%s/%s" % (DEST, name)
    if assets.does_asset_exist(path):
        mat = assets.load_asset(path)
        lib.delete_all_material_expressions(mat)
    else:
        mat = tools.create_asset(name, DEST, unreal.Material, unreal.MaterialFactoryNew())
    return mat


def scalar(mat, name, value, x, y):
    e = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", value)
    return e


def additive_material():
    mat = fresh_material("M_W2FFx")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    colour = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -800, -200)
    colour.set_editor_property("parameter_name", "Colour")
    colour.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    intensity = scalar(mat, "Intensity", 3.0, -800, 0)
    base = scalar(mat, "Base", 1.0, -800, 150)
    rim = scalar(mat, "Rim", 0.0, -800, 250)
    fres = lib.create_material_expression(mat, unreal.MaterialExpressionFresnel, -800, 350)
    fres.set_editor_property("exponent", 2.5)
    rim_mul = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -550, 300)
    add = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -400, 200)
    m1 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -500, -100)
    m2 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 0)
    ok = lib.connect_material_expressions(fres, "", rim_mul, "A") and lib.connect_material_expressions(rim, "", rim_mul, "B")
    ok = lib.connect_material_expressions(base, "", add, "A") and lib.connect_material_expressions(rim_mul, "", add, "B") and ok
    ok = lib.connect_material_expressions(colour, "", m1, "A") and lib.connect_material_expressions(intensity, "", m1, "B") and ok
    ok = lib.connect_material_expressions(m1, "", m2, "A") and lib.connect_material_expressions(add, "", m2, "B") and ok
    ok = lib.connect_material_property(m2, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    if not ok:
        unreal.log_error("W2F: M_W2FFx connections failed")
    lib.recompile_material(mat)
    assets.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


def dark_material():
    mat = fresh_material("M_W2FFxDark")
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    colour = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -600, -100)
    colour.set_editor_property("parameter_name", "Colour")
    colour.set_editor_property("default_value", unreal.LinearColor(0.05, 0.0, 0.1, 1))
    intensity = scalar(mat, "Intensity", 1.0, -600, 50)
    opacity = scalar(mat, "Opacity", 0.8, -600, 200)
    m = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -300, 0)
    ok = lib.connect_material_expressions(colour, "", m, "A") and lib.connect_material_expressions(intensity, "", m, "B")
    ok = lib.connect_material_property(m, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    ok = lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY) and ok
    if not ok:
        unreal.log_error("W2F: M_W2FFxDark connections failed")
    lib.recompile_material(mat)
    assets.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


def import_meshes():
    files = sorted(f for f in os.listdir(SRC) if f.startswith("FX_") and f.endswith(".glb"))
    tasks = []
    for f in files:
        stale = "%s/%s" % (DEST, os.path.splitext(f)[0])
        if assets.does_directory_exist(stale):
            assets.delete_directory(stale)
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", os.path.join(SRC, f))
        t.set_editor_property("destination_path", DEST)
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", True)
        tasks.append(t)
    tools.import_asset_tasks(tasks)
    found = [p for p in assets.list_assets(DEST, recursive=True, include_folder=False) if isinstance(assets.load_asset(p), unreal.StaticMesh)]
    for p in found:   # the effects never cast shadows and never collide
        mesh = assets.load_asset(p)
        mesh.set_editor_property("light_map_resolution", 4)
        assets.save_loaded_asset(mesh, only_if_is_dirty=False)
    log("W2F: %d FX meshes: %s" % (len(found), ", ".join(p.rsplit(".", 1)[-1] for p in found)))


def import_textures():
    files = sorted(f for f in os.listdir(SRC) if f.startswith("T_FX_") and f.endswith(".png"))
    tasks = []
    for f in files:
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", os.path.join(SRC, f))
        t.set_editor_property("destination_path", DEST + "/Textures")
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", True)
        tasks.append(t)
    tools.import_asset_tasks(tasks)
    out = {}
    for f in files:
        name = os.path.splitext(f)[0]
        tex = assets.load_asset("%s/Textures/%s" % (DEST, name))
        if tex is None:
            continue
        tex.set_editor_property("srgb", False)                                  # a mask, not a colour
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
        assets.save_loaded_asset(tex, only_if_is_dirty=False)
        out[name] = tex
    log("W2F: %d sprite textures" % len(out))
    return out


def sprite_material(name, translucent, default_mask):
    """Camera-facing sprites drawn as instances: colour (r, g, b) and alpha come from the instance's custom data 0..3, the shape from the Mask texture."""
    mat = fresh_material(name)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    tex = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, 300)
    tex.set_editor_property("parameter_name", "Mask")
    tex.set_editor_property("texture", default_mask)
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    cd = []
    for i in range(4):
        e = lib.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData, -900, -300 + i * 110)
        e.set_editor_property("data_index", i)
        e.set_editor_property("const_default_value", 1.0)
        cd.append(e)
    rg = lib.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -650, -250)
    rgb = lib.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -500, -200)
    alpha = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -600, 200)
    intensity = scalar(mat, "Intensity", 1.5 if translucent else 4.0, -600, 50)
    col = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, -150)
    ok = lib.connect_material_expressions(cd[0], "", rg, "A") and lib.connect_material_expressions(cd[1], "", rg, "B")
    ok = lib.connect_material_expressions(rg, "", rgb, "A") and lib.connect_material_expressions(cd[2], "", rgb, "B") and ok
    ok = lib.connect_material_expressions(tex, "R", alpha, "A") and lib.connect_material_expressions(cd[3], "", alpha, "B") and ok
    ok = lib.connect_material_expressions(rgb, "", col, "A") and lib.connect_material_expressions(intensity, "", col, "B") and ok
    if translucent:
        ok = lib.connect_material_property(col, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
        ok = lib.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY) and ok
    else:
        out = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -200, 0)
        ok = lib.connect_material_expressions(col, "", out, "A") and lib.connect_material_expressions(alpha, "", out, "B") and ok
        ok = lib.connect_material_property(out, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    if not ok:
        unreal.log_error("W2F: %s connections failed" % name)
    lib.recompile_material(mat)
    assets.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


def main():
    additive_material()
    dark_material()
    import_meshes()
    masks = import_textures()
    if masks:
        glow = masks.get("T_FX_Glow") or next(iter(masks.values()))
        sprite_material("M_W2FSprite", False, glow)
        sprite_material("M_W2FSpriteTint", True, glow)
    os.makedirs(DATA_DIR, exist_ok=True)
    if os.path.exists(FX_JSON):
        shutil.copyfile(FX_JSON, os.path.join(DATA_DIR, "unit_fx.json"))
        log("W2F: fx.json copied to %s/unit_fx.json" % DATA_DIR)
    else:
        unreal.log_warning("W2F: no %s" % FX_JSON)


try:
    main()
except Exception:
    import traceback
    LOG.append(traceback.format_exc())
    unreal.log_error(traceback.format_exc())
finally:
    with open(os.path.join(SRC, "import_fx.log"), "w") as f:
        f.write("\n".join(LOG) + "\n")
