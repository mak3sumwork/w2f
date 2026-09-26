"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed) or inside the editor.

Imports the fight-VFX meshes built by tools/mixamo/build_fx_meshes.py (<project>/SourceArt/FX/FX_*.glb) into /Game/W2F/FX, (re)builds the two effect materials and copies
tools/mixamo/fx.json (which effect each champion uses) to Content/W2F/Data/unit_fx.json, where UW2FFx reads it:
  M_W2FFx      unlit, ADDITIVE, two-sided: Emissive = Colour * Intensity * (Base + Fresnel * Rim) * Energy * DepthFade. Everything that glows.
  M_W2FFxDark  unlit, translucent, two-sided: Emissive = Colour * Intensity, Opacity = Opacity * Energy * DepthFade. Shadows and voids (Eclipse, Gravity Well).
     Energy (demo 1.3) = lerp(1, 2 x panning world-space noise (T_FX_Noise), Noise): the shells, beams and rings shimmer instead of looking like plastic;
     DepthFade softens every line where an effect cuts the ground or a unit.
  M_W2FSprite / M_W2FSpriteTint  instanced sprites; per-instance data r, g, b, alpha, erode, hot (see sprite_material).
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


def energy(mat, x, y):
    """lerp(1, 2 * noise, Noise): two octaves of T_FX_Noise sampled in world space and panned over time. Returns the expression."""
    noise_tex = assets.load_asset(DEST + "/Textures/T_FX_Noise")
    wp = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, x - 900, y)
    time = lib.create_material_expression(mat, unreal.MaterialExpressionTime, x - 900, y + 120)
    outs = []
    for k, (scale, speed, chan) in enumerate(((0.0045, (0.06, 0.035), "R"), (0.011, (-0.05, 0.08), "G"))):
        mask = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, x - 750, y + k * 200)
        mask.set_editor_property("r", True); mask.set_editor_property("g", k == 0); mask.set_editor_property("b", k == 1)
        mul = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x - 620, y + k * 200)
        mul.set_editor_property("const_b", scale)
        pan = lib.create_material_expression(mat, unreal.MaterialExpressionPanner, x - 480, y + k * 200)
        pan.set_editor_property("speed_x", speed[0]); pan.set_editor_property("speed_y", speed[1])
        smp = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, x - 330, y + k * 200)
        smp.set_editor_property("texture", noise_tex)
        smp.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        lib.connect_material_expressions(wp, "", mask, "")
        lib.connect_material_expressions(mask, "", mul, "A")
        lib.connect_material_expressions(mul, "", pan, "Coordinate")
        lib.connect_material_expressions(time, "", pan, "Time")
        lib.connect_material_expressions(pan, "", smp, "UVs")
        outs.append((smp, chan))
    both = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x - 180, y + 80)
    lib.connect_material_expressions(outs[0][0], outs[0][1], both, "A")
    lib.connect_material_expressions(outs[1][0], outs[1][1], both, "B")
    four = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x - 60, y + 80)
    four.set_editor_property("const_b", 4.0)                  # two ~0.5 fields multiplied average ~0.25: x4 brings the mean back to ~1 (x2 on the lerp's far end below)
    lib.connect_material_expressions(both, "", four, "A")
    amount = scalar(mat, "Noise", 0.55, x - 180, y + 250)
    one = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, x - 180, y + 330)
    one.set_editor_property("r", 1.0)
    lerp = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, x + 60, y + 150)
    lib.connect_material_expressions(one, "", lerp, "A")
    lib.connect_material_expressions(four, "", lerp, "B")
    lib.connect_material_expressions(amount, "", lerp, "Alpha")
    return lerp


def depth_fade(mat, x, y, distance=14.0):
    fade = lib.create_material_expression(mat, unreal.MaterialExpressionDepthFade, x, y)
    fade.set_editor_property("fade_distance_default", distance)
    return fade


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
    en = energy(mat, -250, 500)
    m3 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -100, 100)
    ok = lib.connect_material_expressions(m2, "", m3, "A") and lib.connect_material_expressions(en, "", m3, "B") and ok
    fade = depth_fade(mat, -100, 300)
    m4 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, 50, 150)
    ok = lib.connect_material_expressions(m3, "", m4, "A") and lib.connect_material_expressions(fade, "", m4, "B") and ok
    ok = lib.connect_material_property(m4, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
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
    en = energy(mat, -300, 400)
    o1 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 250)
    ok = lib.connect_material_expressions(opacity, "", o1, "A") and lib.connect_material_expressions(en, "", o1, "B") and ok
    sat = lib.create_material_expression(mat, unreal.MaterialExpressionSaturate, -50, 250)
    ok = lib.connect_material_expressions(o1, "", sat, "") and ok
    fade = depth_fade(mat, 50, 250)
    ok = lib.connect_material_expressions(sat, "", fade, "Opacity") and ok
    ok = lib.connect_material_property(m, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    ok = lib.connect_material_property(fade, "", unreal.MaterialProperty.MP_OPACITY) and ok
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
        nanite = mesh.get_editor_property("nanite_settings")   # demo 1.4: Nanite does not draw translucent / additive materials (it logged a warning per sprite component)
        nanite.set_editor_property("enabled", False)
        mesh.set_editor_property("nanite_settings", nanite)
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
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)   # uncompressed: R shape, G erosion, B heat
        assets.save_loaded_asset(tex, only_if_is_dirty=False)
        out[name] = tex
    log("W2F: %d sprite textures" % len(out))
    return out


def sprite_material(name, translucent, default_mask):
    """Camera-facing (or flat) sprites drawn as instances. Per-instance custom data: 0-2 colour, 3 alpha, 4 erode, 5 hot.
    The Mask texture carries the shape (R), an erosion field (G) and a heat mask (B):
      alpha    = R * smoothstep-ish saturate((G - erode) * 6) * cd3            (erode 0 = whole, 1 = gone: smoke and fire break up into wisps as they age)
      emissive = (colour + hot * B) * Intensity                                 (hot pushes the core toward white)"""
    mat = fresh_material(name)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    tex = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1100, 300)
    tex.set_editor_property("parameter_name", "Mask")
    tex.set_editor_property("texture", default_mask)
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    cd = []
    for i in range(6):
        e = lib.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData, -1100, -400 + i * 110)
        e.set_editor_property("data_index", i)
        e.set_editor_property("const_default_value", 0.0 if i >= 4 else 1.0)
        cd.append(e)
    ok = True
    rg = lib.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -850, -350)
    rgb = lib.create_material_expression(mat, unreal.MaterialExpressionAppendVector, -700, -300)
    ok = lib.connect_material_expressions(cd[0], "", rg, "A") and lib.connect_material_expressions(cd[1], "", rg, "B") and ok
    ok = lib.connect_material_expressions(rg, "", rgb, "A") and lib.connect_material_expressions(cd[2], "", rgb, "B") and ok
    # erosion: saturate((G - erode) * 6)
    sub = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, -800, 300)
    ok = lib.connect_material_expressions(tex, "G", sub, "A") and lib.connect_material_expressions(cd[4], "", sub, "B") and ok
    sharp = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -680, 300)
    sharp.set_editor_property("const_b", 6.0)
    ok = lib.connect_material_expressions(sub, "", sharp, "A") and ok
    keep = lib.create_material_expression(mat, unreal.MaterialExpressionSaturate, -560, 300)
    ok = lib.connect_material_expressions(sharp, "", keep, "") and ok
    a1 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -450, 250)
    ok = lib.connect_material_expressions(tex, "R", a1, "A") and lib.connect_material_expressions(keep, "", a1, "B") and ok
    alpha = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -330, 250)
    ok = lib.connect_material_expressions(a1, "", alpha, "A") and lib.connect_material_expressions(cd[3], "", alpha, "B") and ok
    # colour + hot * heat
    heat = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -600, -100)
    ok = lib.connect_material_expressions(tex, "B", heat, "A") and lib.connect_material_expressions(cd[5], "", heat, "B") and ok
    hotcol = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -450, -200)
    ok = lib.connect_material_expressions(rgb, "", hotcol, "A") and lib.connect_material_expressions(heat, "", hotcol, "B") and ok
    intensity = scalar(mat, "Intensity", 1.5 if translucent else 4.0, -600, 50)
    col = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -300, -150)
    ok = lib.connect_material_expressions(hotcol, "", col, "A") and lib.connect_material_expressions(intensity, "", col, "B") and ok
    if translucent:
        ok = lib.connect_material_property(col, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
        ok = lib.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY) and ok
    else:
        out = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 0)
        ok = lib.connect_material_expressions(col, "", out, "A") and lib.connect_material_expressions(alpha, "", out, "B") and ok
        ok = lib.connect_material_property(out, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    if not ok:
        unreal.log_error("W2F: %s connections failed" % name)
    lib.recompile_material(mat)
    assets.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


def main():
    masks = import_textures()          # first: the mesh materials sample T_FX_Noise
    additive_material()
    dark_material()
    if os.environ.get("W2F_FX_SKIP_MESHES") != "1":
        import_meshes()
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
