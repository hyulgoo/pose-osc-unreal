"""Creates /Game/FX/M_PoseFlower, the glowing petal material for UPoseFlowerFieldComponent (DESIGN_DECISIONS D14).

The component writes per-instance custom data [0..2] = linear RGB and [3] = intensity; this material only turns
that into additive emissive light with brighter rims. Re-running the script rebuilds the graph in place.

Run from the repo root with the editor closed (PowerShell):
  & "C:\\Program Files\\Epic Games\\UE_5.7\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe" "$PWD\\unreal\\PoseOscUnreal.uproject" `
      -run=pythonscript -script="$PWD\\unreal\\Scripts\\create_flower_material.py" -unattended -nosplash
or inside the editor: Tools > Execute Python Script.
"""

import unreal

PACKAGE_PATH = "/Game/FX"
ASSET_NAME = "M_PoseFlower"


def _material_editing_library():
    # MaterialEditingLibrary lives in the MaterialEditor module, which a commandlet does not load on its own.
    if not hasattr(unreal, "MaterialEditingLibrary"):
        unreal.load_module("MaterialEditor")
    return unreal.MaterialEditingLibrary


def main() -> None:
    mel = _material_editing_library()
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    asset_path = f"{PACKAGE_PATH}/{ASSET_NAME}"

    if assets.does_asset_exist(asset_path):
        material = assets.load_asset(asset_path)
        mel.delete_all_material_expressions(material)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            ASSET_NAME, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew()
        )

    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property("two_sided", True)
    # Without this usage flag the engine swaps in the default material on instanced meshes.
    material.set_editor_property("used_with_instanced_static_meshes", True)

    color = mel.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData3Vector, -900, -150)
    color.set_editor_property("data_index", 0)
    intensity = mel.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData, -900, 50)
    intensity.set_editor_property("data_index", 3)

    # Rims brighter than the face: thin ellipsoid petals read as translucent light instead of solid shapes.
    fresnel = mel.create_material_expression(material, unreal.MaterialExpressionFresnel, -900, 250)
    fresnel.set_editor_property("exponent", 2.0)
    fresnel.set_editor_property("base_reflect_fraction", 0.0)
    rim = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -600, 250)
    rim.set_editor_property("const_a", 0.35)
    rim.set_editor_property("const_b", 1.0)
    mel.connect_material_expressions(fresnel, "", rim, "Alpha")

    tinted = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -600, -50)
    mel.connect_material_expressions(color, "", tinted, "A")
    mel.connect_material_expressions(intensity, "", tinted, "B")
    emissive = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -300, 50)
    mel.connect_material_expressions(tinted, "", emissive, "A")
    mel.connect_material_expressions(rim, "", emissive, "B")
    mel.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    mel.recompile_material(material)
    if not assets.save_loaded_asset(material, only_if_is_dirty=False):
        raise RuntimeError(f"{asset_path} 저장 실패")
    unreal.log(f"[create_flower_material] 저장했어요: {asset_path}")


main()
