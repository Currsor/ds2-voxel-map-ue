#pragma once

#include "CoreMinimal.h"
#include "VoxelMapGenerator.h"

/** Builder 输入：一个已占用体素及其最终 GPU 打包颜色。 */
struct FVoxelMapSourceVoxel
{
	FIntVector Coordinate = FIntVector::ZeroValue;
	uint32 PackedData = 0;
};

/**
 * 将任意体素源统一打包为 Renderer 使用的稀疏 4x4x4 Block 数据。
 * 仅输出非空 Block，但 ShapeLevelAndLocation 中保留完整网格的线性 Block 索引。
 */
class FVoxelMapDataBuilder
{
public:
	using FVoxelSampler = TFunctionRef<bool(const FIntVector& Coordinate, uint32& OutPackedData)>;

	static bool Build(const FIntVector& GridSize, const TArray<FVoxelMapSourceVoxel>& SourceVoxels, FVoxelMapData& OutData);
	static bool BuildFromSampler(const FIntVector& GridSize, FVoxelSampler Sampler, FVoxelMapData& OutData);
};
