#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelMapGenerator.h"
#include "VoxelMapCollector.generated.h"

class UVoxelMapDataAsset;

/** 编辑器采集器：生成一次体素数据并写入 DataAsset，运行时无需保留。 */
UCLASS()
class DS2VOXELMAP_API AVoxelMapCollector : public AActor
{
	GENERATED_BODY()

public:
	AVoxelMapCollector();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Output")
	TObjectPtr<UVoxelMapDataAsset> OutputDataAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Output", meta = (ClampMin = "0.001"))
	float VoxelSize = 10.0f;

	/** 普通烘焙规模。XYZ 会自动向上对齐到 4；建议逐级测试 256×128×256、512×128×512。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation", meta = (ClampMin = "4", UIMin = "4", UIMax = "1024"))
	FIntVector VoxelGridSize = FIntVector(256, 128, 256);

	/** 一键压力测试使用的规模，不修改普通 VoxelGridSize。默认约为原 128³ 数据的 16 倍 block 数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Performance Test", meta = (ClampMin = "4", UIMin = "4", UIMax = "2048"))
	FIntVector PerformanceTestGridSize = FIntVector(512, 128, 512);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation")
	int32 VoxelSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation")
	float TerrainHeight = 48.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation")
	float TerrainAmplitude = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation")
	float NoiseFrequency = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation")
	int32 NoiseOctaves = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Map|Generation")
	bool bFlatTerrain = false;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map")
	void BakeVoxelMap();

	/** 使用 PerformanceTestGridSize 烘焙，不覆盖普通 VoxelGridSize。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Performance Test")
	void BakePerformanceTest();

private:
	FVoxelMapConfig BuildVoxelConfig(const FIntVector& GridSize) const;
	void BakeVoxelMapInternal(const FIntVector& GridSize, const TCHAR* BakeLabel);
	FVector CalculateWorldOrigin(const FVoxelMapData& Data) const;
};
