#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelMapGenerator.h"
#include "VoxelMapDataAsset.generated.h"

/** 采集阶段生成、运行时只读的体素地图资产。 */
UCLASS(BlueprintType)
class DS2VOXELMAP_API UVoxelMapDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Voxel Map")
	FVoxelMapData Data;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voxel Map", meta = (ClampMin = "0.001"))
	float VoxelSize = 10.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	FVector WorldOrigin = FVector::ZeroVector;
};
