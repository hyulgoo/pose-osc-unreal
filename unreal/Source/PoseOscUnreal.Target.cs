using UnrealBuildTool;

public class PoseOscUnrealTarget : TargetRules
{
	public PoseOscUnrealTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("PoseOscUnreal");
	}
}
