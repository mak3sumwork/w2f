"""Run headless (UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, editor closed) or inside the editor.

The splash studio's backdrops (demo 1.6): imports docs/icons/studio/T_StudioSky_<Origin>.png (tools/mixamo/make_studio_skies.py) into /Game/W2F/Studio and makes
M_StudioSky: unlit, two-sided, Emissive = Sky texture x Tint x Intensity. W2FStudio.cpp puts it on a card far behind the posed hero.
"""
import os
import sys

import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
SKIES = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "icons", "studio"))
DEST = "/Game/W2F/Studio"

lib = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def import_texture(png):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", png)
    task.set_editor_property("destination_path", DEST)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    tools.import_asset_tasks([task])
    name = os.path.splitext(os.path.basename(png))[0]
    tex = assets.load_asset("%s/%s" % (DEST, name))
    if tex is not None:
        tex.set_editor_property("never_stream", True)                 # a backdrop is always seen whole: no blurry mips on the first frame
        tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP)
        tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_SKYBOX)
        assets.save_loaded_asset(tex, only_if_is_dirty=False)
    return tex


def sky_material(default_tex):
    path = DEST + "/M_StudioSky"
    if assets.does_asset_exist(path):
        mat = assets.load_asset(path)
        lib.delete_all_material_expressions(mat)
    else:
        mat = tools.create_asset("M_StudioSky", DEST, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    sky = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -700, 0)
    sky.set_editor_property("parameter_name", "Sky")
    if default_tex is not None:
        sky.set_editor_property("texture", default_tex)
    tint = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, 300)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
    power = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, 450)
    power.set_editor_property("parameter_name", "Intensity")
    power.set_editor_property("default_value", 1.0)
    mul1 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 100)
    mul2 = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 150)
    ok = lib.connect_material_expressions(sky, "RGB", mul1, "A")
    ok = lib.connect_material_expressions(tint, "", mul1, "B") and ok
    ok = lib.connect_material_expressions(mul1, "", mul2, "A") and ok
    ok = lib.connect_material_expressions(power, "", mul2, "B") and ok
    ok = lib.connect_material_property(mul2, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    if not ok:
        unreal.log_error("W2F: M_StudioSky connections failed")
    lib.recompile_material(mat)
    assets.save_loaded_asset(mat, only_if_is_dirty=False)
    return mat


def main():
    if not assets.does_directory_exist(DEST):
        assets.make_directory(DEST)
    first = None
    for name in sorted(os.listdir(SKIES)):
        if name.startswith("T_StudioSky_") and name.endswith(".png"):
            tex = import_texture(os.path.join(SKIES, name))
            first = first or tex
            unreal.log("W2F: studio sky %s" % name)
    sky_material(first)
    unreal.log("W2F: studio ready")


main()
