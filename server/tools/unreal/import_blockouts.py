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


MODELS_SRC = os.environ.get("W2F_MODELS") or os.path.normpath(os.path.join(HERE, "..", "..", "docs", "models"))


def crafted_names():
    """Names of the hand-crafted, textured models (docs/models/*.glb): they keep their own materials and replace the blockout of the same name."""
    if not os.path.isdir(MODELS_SRC):
        return set()
    return {os.path.splitext(f)[0] for f in os.listdir(MODELS_SRC) if f.startswith("SM_") and f.endswith(".glb")}


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
    crafted = []
    for name in sorted(crafted_names()):
        stale = "%s/%s" % (DEST, name)
        if unreal.EditorAssetLibrary.does_directory_exist(stale):   # a fresh import: no leftover material slots from the blockout of the same name
            unreal.EditorAssetLibrary.delete_directory(stale)
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", os.path.join(MODELS_SRC, name + ".glb"))
        task.set_editor_property("destination_path", DEST)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("replace_existing_settings", True)
        task.set_editor_property("save", True)
        crafted.append(task)
    if crafted:
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(crafted)
        unreal.log("W2F: imported %d crafted models from %s" % (len(crafted), MODELS_SRC))
    return len(files)


ICONS_SRC = os.environ.get("W2F_ICONS") or os.path.normpath(os.path.join(HERE, "..", "..", "docs", "icons"))
ICONS_DEST = "/Game/W2F/Icons"


def import_icons():
    """Imports docs/icons/*.png (item icons, champion portraits, UI glyphs) as UI textures: no mipmaps, no compression artefacts, never streamed."""
    if not os.path.isdir(ICONS_SRC):
        unreal.log_warning("W2F: no icons folder at %s (run tools/make_icons.py)" % ICONS_SRC)
        return 0
    tasks = []
    for f in sorted(os.listdir(ICONS_SRC)):
        if not f.endswith(".png"):
            continue
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", os.path.join(ICONS_SRC, f))
        task.set_editor_property("destination_path", ICONS_DEST)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("replace_existing_settings", True)
        task.set_editor_property("save", False)
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    count = 0
    for asset_path in unreal.EditorAssetLibrary.list_assets(ICONS_DEST, recursive=True, include_folder=False):
        texture = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not isinstance(texture, unreal.Texture2D):
            continue
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        texture.set_editor_property("never_stream", True)
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
        unreal.EditorAssetLibrary.save_loaded_asset(texture)
        count += 1
    unreal.log("W2F: imported %d icon textures from %s" % (count, ICONS_SRC))
    return count


def crafted_parent_material():
    """M_CraftedPBR: base colour and emissive from two textures (parameters BaseColorTexture / EmissiveTexture), constant roughness / metallic. Used by the crafted models instead of the
    material Interchange makes from the glTF (that one is a Substrate material which rendered plain white in game mode)."""
    path = "%s/M_CraftedPBR" % MAT_DIR
    lib = unreal.MaterialEditingLibrary
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_CraftedPBR", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    albedo = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -600, -100)
    albedo.set_editor_property("parameter_name", "BaseColorTexture")
    emit = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -600, 250)
    emit.set_editor_property("parameter_name", "EmissiveTexture")
    boost = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -600, 480)
    boost.set_editor_property("parameter_name", "EmissiveBoost"); boost.set_editor_property("default_value", 1.5)
    times = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 300)
    rough = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -400, 100)
    rough.set_editor_property("parameter_name", "Roughness"); rough.set_editor_property("default_value", 0.65)
    metal = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -400, 200)
    metal.set_editor_property("parameter_name", "Metallic"); metal.set_editor_property("default_value", 0.1)
    ok = lib.connect_material_property(albedo, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    ok = lib.connect_material_expressions(emit, "RGB", times, "A") and lib.connect_material_expressions(boost, "", times, "B") and ok
    ok = lib.connect_material_property(times, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR) and ok
    ok = lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS) and ok
    ok = lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC) and ok
    if not ok:
        unreal.log_error("W2F: some connections of M_CraftedPBR failed")
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def apply_crafted_materials():
    """A material instance per crafted model, with its own albedo / emissive textures, assigned to the mesh."""
    names = crafted_names()
    if not names:
        return
    parent = crafted_parent_material()
    lib = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    for name in sorted(names):
        base = "%s/%s" % (DEST, name)
        textures = {}
        for asset_path in unreal.EditorAssetLibrary.list_assets(base + "/Textures", recursive=True, include_folder=False):
            texture = unreal.EditorAssetLibrary.load_asset(asset_path)
            if isinstance(texture, unreal.Texture2D):
                textures[texture.get_name().rsplit("_", 1)[-1]] = texture
        mesh = unreal.EditorAssetLibrary.load_asset("%s/StaticMeshes/%s" % (base, name))
        if mesh is None or "albedo" not in textures:
            unreal.log_warning("W2F: crafted model %s has no mesh or albedo texture" % name)
            continue
        inst_name = "MI_" + name
        inst_dir = base + "/Materials"
        if unreal.EditorAssetLibrary.does_asset_exist("%s/%s" % (inst_dir, inst_name)):
            unreal.EditorAssetLibrary.delete_asset("%s/%s" % (inst_dir, inst_name))
        inst = tools.create_asset(inst_name, inst_dir, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        inst.set_editor_property("parent", parent)
        lib.set_material_instance_texture_parameter_value(inst, "BaseColorTexture", textures["albedo"])
        if "emit" in textures:
            lib.set_material_instance_texture_parameter_value(inst, "EmissiveTexture", textures["emit"])
        unreal.EditorAssetLibrary.save_loaded_asset(inst)
        for slot in range(len(mesh.static_materials)):
            mesh.set_material(slot, inst)
        unreal.EditorAssetLibrary.save_loaded_asset(mesh)
        unreal.log("W2F: %s uses %s" % (name, inst_name))


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
    apply_crafted_materials()
    done = 0
    for asset_path in unreal.EditorAssetLibrary.list_assets(DEST, recursive=True, include_folder=False):
        asset = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not isinstance(asset, unreal.StaticMesh):
            continue
        if asset.get_name() in crafted_names():   # a crafted model keeps its own textured material
            continue
        for slot in range(len(asset.static_materials)):
            asset.set_material(slot, material)
        unreal.EditorAssetLibrary.save_loaded_asset(asset)
        done += 1
    unreal.log("W2F: %s applied to %d static meshes" % (MAT_NAME, done))


if __name__ == "__main__":
    if import_models():
        assign_material(vertex_colour_material())
