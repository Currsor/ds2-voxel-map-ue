#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelMapBakeReport.h"
#include "VoxelMapWorldAsset.generated.h"

class UVoxelMapDataAsset;

USTRUCT(BlueprintType)
struct FVoxelMapRegionReference
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Region")
	FIntVector RegionCoordinate = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Region")
	FBox WorldBounds = FBox(EForceInit::ForceInit);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Region")
	TObjectPtr<UVoxelMapDataAsset> Asset;
};

/** 大范围体素地图的轻量清单；实际 Block/Voxel 数据分散保存在独立 Region DataAsset 中。 */
UCLASS(BlueprintType)
class DS2VOXELMAP_API UVoxelMapWorldAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	float VoxelSize = 10.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	FVector WorldOrigin = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	FIntVector VoxelGridSize = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	FIntVector RegionSizeInVoxels = FIntVector(128, 128, 128);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	FBox CaptureBounds = FBox(EForceInit::ForceInit);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	FString SourceMapPackage;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	TArray<FVoxelMapRegionReference> Regions;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map")
	int32 DataVersion = 5;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|M7 Report")
	FVoxelMapBakeReport LastBakeReport;
};

