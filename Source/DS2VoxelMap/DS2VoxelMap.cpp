#include "DS2VoxelMap.h"

#include "Modules/ModuleManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

class FDS2VoxelMapModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		// 把虚拟路径 /Project/VoxelMap 映射到工程 Shaders 目录，
		// 后续 .usf/.ush 里用 #include "/Project/VoxelMap/xxx" 引用。
		const FString ShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Project/VoxelMap"), ShaderDir);
	}

	virtual void ShutdownModule() override
	{
		ResetAllShaderSourceDirectoryMappings();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FDS2VoxelMapModule, DS2VoxelMap, "DS2VoxelMap");
