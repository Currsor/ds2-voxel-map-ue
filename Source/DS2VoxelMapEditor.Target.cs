using UnrealBuildTool;
using System.Collections.Generic;

public class DS2VoxelMapEditorTarget : TargetRules
{
	public DS2VoxelMapEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("DS2VoxelMap");
	}
}
