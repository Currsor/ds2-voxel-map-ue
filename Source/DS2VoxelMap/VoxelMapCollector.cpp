#include "VoxelMapCollector.h"

#include "Components/SceneComponent.h"
#include "HAL/PlatformTime.h"
#include "VoxelMapDataAsset.h"
#include "VoxelMapTypes.h"

AVoxelMapCollector::AVoxelMapCollector()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

#if WITH_EDITORONLY_DATA
	bIsEditorOnlyActor = true;
#endif
}

FVoxelMapConfig AVoxelMapCollector::BuildVoxelConfig(const FIntVector& GridSize) const
{
	FVoxelMapConfig Config;
	Config.GridSize = GridSize;
	Config.Seed = VoxelSeed;
	Config.TerrainHeight = TerrainHeight;
	Config.TerrainAmplitude = TerrainAmplitude;
	Config.NoiseFrequency = NoiseFrequency;
	Config.NoiseOctaves = NoiseOctaves;
	Config.bFlatTerrain = bFlatTerrain;
	return Config;
}

FVector AVoxelMapCollector::CalculateWorldOrigin(const FVoxelMapData& Data) const
{
	const FIntVector VoxelGrid = Data.BlockGridSize * VOXELMAP_BLOCK_SIZE;
	const FVector HalfWorld(
		static_cast<float>(VoxelGrid.X) * VoxelSize * 0.5f,
		static_cast<float>(VoxelGrid.Z) * VoxelSize * 0.5f,
		static_cast<float>(VoxelGrid.Y) * VoxelSize * 0.5f);
	return GetActorLocation() - HalfWorld;
}

void AVoxelMapCollector::BakeVoxelMap()
{
	BakeVoxelMapInternal(VoxelGridSize, TEXT("Standard"));
}

void AVoxelMapCollector::BakePerformanceTest()
{
	BakeVoxelMapInternal(PerformanceTestGridSize, TEXT("Performance Test"));
}

void AVoxelMapCollector::BakeVoxelMapInternal(const FIntVector& GridSize, const TCHAR* BakeLabel)
{
	if (!OutputDataAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s bake failed: assign an OutputDataAsset first"), BakeLabel);
		return;
	}

	if (GridSize.X < VOXELMAP_BLOCK_SIZE || GridSize.Y < VOXELMAP_BLOCK_SIZE || GridSize.Z < VOXELMAP_BLOCK_SIZE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s bake failed: every grid dimension must be at least %d"),
			BakeLabel, VOXELMAP_BLOCK_SIZE);
		return;
	}

	const FIntVector AlignedGrid(
		FMath::DivideAndRoundUp(GridSize.X, VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE,
		FMath::DivideAndRoundUp(GridSize.Y, VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE,
		FMath::DivideAndRoundUp(GridSize.Z, VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE);
	const FIntVector BlockGrid = AlignedGrid / VOXELMAP_BLOCK_SIZE;
	const int64 RequestedBlocks = static_cast<int64>(BlockGrid.X) * BlockGrid.Y * BlockGrid.Z;
	if (RequestedBlocks > 0x00FFFFFFll)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VoxelMap] %s bake failed: %lld blocks exceed the 24-bit shader limit (%d)"),
			BakeLabel, RequestedBlocks, 0x00FFFFFF);
		return;
	}

	UE_LOG(LogTemp, Log,
		TEXT("[VoxelMap] Starting %s bake: grid=%dx%dx%d, blocks=%lld"),
		BakeLabel, AlignedGrid.X, AlignedGrid.Y, AlignedGrid.Z, RequestedBlocks);

	FVoxelMapData NewData;
	const double GenerateStart = FPlatformTime::Seconds();
	FVoxelMapGenerator::Generate(BuildVoxelConfig(AlignedGrid), NewData);
	const double GenerateSeconds = FPlatformTime::Seconds() - GenerateStart;

	const double ValidateStart = FPlatformTime::Seconds();
	if (!FVoxelMapGenerator::Validate(NewData))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s bake failed: generated data is invalid"), BakeLabel);
		return;
	}
	const double ValidateSeconds = FPlatformTime::Seconds() - ValidateStart;

	const int64 BlockBytes = static_cast<int64>(NewData.BlockData.Num()) * sizeof(uint32);
	const int64 VoxelBytes = static_cast<int64>(NewData.VoxelData.Num()) * sizeof(uint32);
	const double DataMiB = static_cast<double>(BlockBytes + VoxelBytes) / (1024.0 * 1024.0);

	OutputDataAsset->Modify();
	OutputDataAsset->Data = MoveTemp(NewData);
	OutputDataAsset->VoxelSize = VoxelSize;
	OutputDataAsset->WorldOrigin = CalculateWorldOrigin(OutputDataAsset->Data);
	OutputDataAsset->MarkPackageDirty();

	UE_LOG(LogTemp, Log,
		TEXT("[VoxelMap] %s baked: %d blocks, %d occupied voxels, %.2f MiB payload, generate %.3fs, validate %.3fs, origin=(%.1f, %.1f, %.1f)"),
		BakeLabel,
		OutputDataAsset->Data.BlockCount,
		OutputDataAsset->Data.OccupiedVoxelCount,
		DataMiB,
		GenerateSeconds,
		ValidateSeconds,
		OutputDataAsset->WorldOrigin.X,
		OutputDataAsset->WorldOrigin.Y,
		OutputDataAsset->WorldOrigin.Z);
}
