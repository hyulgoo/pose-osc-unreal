using UnrealBuildTool;

public class PoseOscUnrealEditorTarget : TargetRules
{
	public PoseOscUnrealEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("PoseOscUnreal");
	}
}
