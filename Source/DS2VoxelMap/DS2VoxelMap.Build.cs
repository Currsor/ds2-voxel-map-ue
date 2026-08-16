using UnrealBuildTool;

public class DS2VoxelMap : ModuleRules
{
	public DS2VoxelMap(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });

		// M0: 自定义全局 shader + RDG 所需依赖
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "Renderer", "RHI", "Projects" });
	}
}
