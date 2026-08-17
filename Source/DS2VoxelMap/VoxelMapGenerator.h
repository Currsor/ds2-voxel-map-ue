#pragma once

#include "CoreMinimal.h"
#include "Math/IntVector.h"
#include "VoxelMapTypes.h"
#include "VoxelMapGenerator.generated.h"

/** 体素化生成参数（CPU，M1） */
struct FVoxelMapConfig
{
	// 默认比原始 128³ 数据增加 4 倍水平面积，便于进行基础性能测试。
	FIntVector GridSize = FIntVector(256, 128, 256); // (X, Y=高度, Z)
	int32 Seed = 1337;
	float TerrainHeight = 48.0f;
	float TerrainAmplitude = 32.0f;
	float NoiseFrequency = 0.03f;
	int32 NoiseOctaves = 4;
	bool bFlatTerrain = false;
};

/** 可序列化的体素地图数据，可直接上传 GPU。 */
USTRUCT()
struct FVoxelMapData
{
	GENERATED_BODY()

	UPROPERTY()
	FIntVector BlockGridSize = FIntVector::ZeroValue;

	UPROPERTY()
	int32 BlockCount = 0;

	UPROPERTY()
	int32 OccupiedVoxelCount = 0;

	UPROPERTY()
	TArray<uint32> BlockData;

	UPROPERTY()
	TArray<uint32> VoxelData;

	void Reset()
	{
		BlockGridSize = FIntVector::ZeroValue;
		BlockCount = 0;
		OccupiedVoxelCount = 0;
		BlockData.Reset();
		VoxelData.Reset();
	}
};

/** 纯 CPU 体素化生成器（M1）。 */
class FVoxelMapGenerator
{
public:
	static void Generate(const FVoxelMapConfig& Config, FVoxelMapData& OutData);
	static bool Validate(const FVoxelMapData& Data);

private:
	static float SampleSurfaceHeight(int32 X, int32 Z, const FVoxelMapConfig& Config);
	static void SampleColor(float HeightT, uint8& OutR, uint8& OutG, uint8& OutB, uint8& OutExtra);
	static uint32 Hash(int32 X, int32 Y, int32 Seed);
	static float ValueNoise(float X, float Y, int32 Seed);
	static float Fbm(float X, float Y, int32 Seed, int32 Octaves);
};
