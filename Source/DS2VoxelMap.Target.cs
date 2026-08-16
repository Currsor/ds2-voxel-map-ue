using UnrealBuildTool;
using System.Collections.Generic;

public class DS2VoxelMapTarget : TargetRules
{
	public DS2VoxelMapTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("DS2VoxelMap");
	}
}
