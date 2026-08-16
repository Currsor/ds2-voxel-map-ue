#pragma once

#include "CoreMinimal.h"
#include "Math/IntVector.h"
#include "VoxelMapTypes.h"

/** 体素化生成参数（CPU，M1） */
struct FVoxelMapConfig
{
	FIntVector GridSize = FIntVector(128, 128, 128); // (X, Y=高度, Z)
	int32 Seed = 1337;
	float TerrainHeight = 48.0f;    // 基准表面高度（体素单位）
	float TerrainAmplitude = 32.0f; // 噪声振幅
	float NoiseFrequency = 0.03f;   // 噪声频率（越大起伏越密）
	int32 NoiseOctaves = 4;
	bool bFlatTerrain = false;      // 调试：完全平面地形（忽略噪声，表面高度 = TerrainHeight）
};

/** 生成结果：可直接上传 GPU 的两份 uint32 缓冲 + 元数据 */
struct FVoxelMapData
{
	FIntVector BlockGridSize = FIntVector::ZeroValue; // block 网格尺寸
	int32 BlockCount = 0;                             // block 总数
	int32 OccupiedVoxelCount = 0;                     // 占用体素总数

	TArray<uint32> BlockData; // 每 block 4×uint32（FBlockRenderData 展平）
	TArray<uint32> VoxelData; // 每占用 voxel 1×uint32（FVoxelRenderData 展平）

	void Reset()
	{
		BlockGridSize = FIntVector::ZeroValue;
		BlockCount = 0;
		OccupiedVoxelCount = 0;
		BlockData.Reset();
		VoxelData.Reset();
	}
};

/** 纯 CPU 体素化生成器（M1）：地形源 = 程序化 fBm 噪声高度场 */
class FVoxelMapGenerator
{
public:
	static void Generate(const FVoxelMapConfig& Config, FVoxelMapData& OutData);

	/** 一致性自检：前缀和 / 粗掩码 / 体素数（bit 打包错误会在 M4 着色器里难查，提前拦下） */
	static bool Validate(const FVoxelMapData& Data);

private:
	static float SampleSurfaceHeight(int32 X, int32 Z, const FVoxelMapConfig& Config);
	static void SampleColor(float HeightT, uint8& OutR, uint8& OutG, uint8& OutB, uint8& OutExtra);
	static uint32 Hash(int32 X, int32 Y, int32 Seed);
	static float ValueNoise(float X, float Y, int32 Seed);
	static float Fbm(float X, float Y, int32 Seed, int32 Octaves);
};
