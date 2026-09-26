"""Run INSIDE the Unreal Editor (or headless: UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file>, with the editor closed).

Imports the blockout models, (re)builds the vertex-colour material, and creates the viewer level /Game/W2F/Maps/L_Viewer: an EMPTY level holding one W2FArena actor (the actor builds its own floor,
lights and camera). Set W2F_TOOLS to this folder if __file__ is not defined in your Python host.
"""
import os
import sys
import unreal

HERE = os.environ.get("W2F_TOOLS") or (os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd())
sys.path.insert(0, HERE)
import import_blockouts   # noqa: E402  (same folder)

MAP = "/Game/W2F/Maps/L_Viewer"

import_blockouts.import_models()   # (re)import every SM_*.glb, replacing what is there
import_blockouts.import_icons()    # ... and the UI icons (T_Item_*, T_Portrait_*, T_Coin, T_Lock*)
import_blockouts.import_hero_portraits()   # ... then the portraits rendered from the Mixamo heroes over the generated busts
import_blockouts.assign_material(import_blockouts.vertex_colour_material())

levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if unreal.EditorAssetLibrary.does_asset_exist(MAP):
    levels.load_level(MAP)
else:
    levels.new_level(MAP)
already = [a for a in unreal.EditorLevelLibrary.get_all_level_actors() if a.get_class().get_name() == "W2FArena"]
if not already:
    arena = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.W2FArena, unreal.Vector(0.0, 0.0, 0.0))
    arena.set_actor_label("W2FArena")
levels.save_current_level()
unreal.log("W2F: viewer level %s ready" % MAP)
