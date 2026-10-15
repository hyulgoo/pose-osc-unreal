using UnrealBuildTool;

public class PoseOscUnreal : ModuleRules
{
	public PoseOscUnreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		// OSC: UOSCServer and FOSCMessage (engine plugin, enabled in the .uproject).
		// Sockets / Networking: bind probe before Listen (UOSCServer::IsActive() stays true even when bind fails).
		PrivateDependencyModuleNames.AddRange(new string[] { "OSC", "Sockets", "Networking" });
	}
}
