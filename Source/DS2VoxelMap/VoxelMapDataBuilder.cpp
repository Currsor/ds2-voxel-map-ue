#include "VoxelMapDataBuilder.h"

#include "VoxelMapTypes.h"

namespace
{
	struct FPendingVoxel
	{
		uint8 LocalBit = 0;
		uint32 PackedData = 0;
	};

	FIntVector AlignGrid(const FIntVector& GridSize)
	{
		auto AlignDimension = [](int32 Value)
		{
			return FMath::Max(VOXELMAP_BLOCK_SIZE,
				FMath::DivideAndRoundUp(Value, VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE);
		};
		return FIntVector(AlignDimension(GridSize.X), AlignDimension(GridSize.Y), AlignDimension(GridSize.Z));
	}
}

bool FVoxelMapDataBuilder::Build(const FIntVector& InGridSize, const TArray<FVoxelMapSourceVoxel>& SourceVoxels, FVoxelMapData& OutData)
{
	OutData.Reset();
	const FIntVector GridSize = AlignGrid(InGridSize);
	const FIntVector BlockGrid = GridSize / VOXELMAP_BLOCK_SIZE;
	const int64 TotalBlockCount64 = static_cast<int64>(BlockGrid.X) * BlockGrid.Y * BlockGrid.Z;
	if (TotalBlockCount64 <= 0 || TotalBlockCount64 > 0x00FFFFFFll)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Builder failed: %lld addressable blocks exceed the 24-bit shader limit"), TotalBlockCount64);
		return false;
	}

	TMap<int32, TArray<FPendingVoxel>> Blocks;
	Blocks.Reserve(FMath::Min<int32>(SourceVoxels.Num(), static_cast<int32>(TotalBlockCount64)));
	for (const FVoxelMapSourceVoxel& SourceVoxel : SourceVoxels)
	{
		const FIntVector& Voxel = SourceVoxel.Coordinate;
		if (Voxel.X < 0 || Voxel.Y < 0 || Voxel.Z < 0 || Voxel.X >= GridSize.X || Voxel.Y >= GridSize.Y || Voxel.Z >= GridSize.Z)
		{
			continue;
		}

		const FIntVector Block(Voxel.X / VOXELMAP_BLOCK_SIZE, Voxel.Y / VOXELMAP_BLOCK_SIZE, Voxel.Z / VOXELMAP_BLOCK_SIZE);
		const int32 BlockIndex = Block.X + Block.Y * BlockGrid.X + Block.Z * BlockGrid.X * BlockGrid.Y;
		FPendingVoxel Pending;
		Pending.LocalBit = static_cast<uint8>(VoxelMapBits::LocalBitIndex(
			Voxel.X % VOXELMAP_BLOCK_SIZE,
			Voxel.Y % VOXELMAP_BLOCK_SIZE,
			Voxel.Z % VOXELMAP_BLOCK_SIZE));
		Pending.PackedData = SourceVoxel.PackedData;
		Blocks.FindOrAdd(BlockIndex).Add(Pending);
	}

	TArray<int32> SortedBlockIndices;
	Blocks.GetKeys(SortedBlockIndices);
	SortedBlockIndices.Sort();

	OutData.BlockGridSize = BlockGrid;
	OutData.BlockData.Reserve(SortedBlockIndices.Num() * 4);
	OutData.VoxelData.Reserve(SourceVoxels.Num());

	for (const int32 BlockIndex : SortedBlockIndices)
	{
		TArray<FPendingVoxel>& PendingVoxels = Blocks.FindChecked(BlockIndex);
		PendingVoxels.Sort([](const FPendingVoxel& A, const FPendingVoxel& B)
		{
			return A.LocalBit < B.LocalBit;
		});

		uint64 Mask = 0;
		uint8 CoarseMask = 0;
		const uint32 RenderStartIndex = static_cast<uint32>(OutData.VoxelData.Num());
		int32 LastBit = INDEX_NONE;
		for (const FPendingVoxel& Pending : PendingVoxels)
		{
			if (Pending.LocalBit == LastBit)
			{
				OutData.VoxelData.Last() = Pending.PackedData;
				continue;
			}

			LastBit = Pending.LocalBit;
			Mask |= static_cast<uint64>(1) << Pending.LocalBit;
			const int32 LocalX = Pending.LocalBit & 3;
			const int32 LocalY = (Pending.LocalBit >> 2) & 3;
			const int32 LocalZ = (Pending.LocalBit >> 4) & 3;
			CoarseMask |= static_cast<uint8>(1 << VoxelMapBits::CoarseBitIndex(LocalX >> 1, LocalY >> 1, LocalZ >> 1));
			OutData.VoxelData.Add(Pending.PackedData);
		}

		OutData.BlockData.Add(static_cast<uint32>(CoarseMask) | (static_cast<uint32>(BlockIndex) << 8));
		OutData.BlockData.Add(static_cast<uint32>(Mask & 0xFFFFFFFFull));
		OutData.BlockData.Add(static_cast<uint32>(Mask >> 32));
		OutData.BlockData.Add(RenderStartIndex);
	}

	OutData.BlockCount = SortedBlockIndices.Num();
	OutData.OccupiedVoxelCount = OutData.VoxelData.Num();
	return true;
}

bool FVoxelMapDataBuilder::BuildFromSampler(const FIntVector& InGridSize, FVoxelSampler Sampler, FVoxelMapData& OutData)
{
	OutData.Reset();
	const FIntVector GridSize = AlignGrid(InGridSize);
	const FIntVector BlockGrid = GridSize / VOXELMAP_BLOCK_SIZE;
	const int64 TotalBlockCount64 = static_cast<int64>(BlockGrid.X) * BlockGrid.Y * BlockGrid.Z;
	if (TotalBlockCount64 <= 0 || TotalBlockCount64 > 0x00FFFFFFll)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Builder failed: %lld addressable blocks exceed the 24-bit shader limit"), TotalBlockCount64);
		return false;
	}

	OutData.BlockGridSize = BlockGrid;
	for (int32 BlockZ = 0; BlockZ < BlockGrid.Z; ++BlockZ)
	{
		for (int32 BlockY = 0; BlockY < BlockGrid.Y; ++BlockY)
		{
			for (int32 BlockX = 0; BlockX < BlockGrid.X; ++BlockX)
			{
				const int32 BlockIndex = BlockX + BlockY * BlockGrid.X + BlockZ * BlockGrid.X * BlockGrid.Y;
				const uint32 RenderStartIndex = static_cast<uint32>(OutData.VoxelData.Num());
				uint64 Mask = 0;
				uint8 CoarseMask = 0;

				for (int32 LocalZ = 0; LocalZ < VOXELMAP_BLOCK_SIZE; ++LocalZ)
				{
					for (int32 LocalY = 0; LocalY < VOXELMAP_BLOCK_SIZE; ++LocalY)
					{
						for (int32 LocalX = 0; LocalX < VOXELMAP_BLOCK_SIZE; ++LocalX)
						{
							const FIntVector Coordinate(
								BlockX * VOXELMAP_BLOCK_SIZE + LocalX,
								BlockY * VOXELMAP_BLOCK_SIZE + LocalY,
								BlockZ * VOXELMAP_BLOCK_SIZE + LocalZ);
							uint32 PackedData = 0;
							if (!Sampler(Coordinate, PackedData))
							{
								continue;
							}

							const uint32 LocalBit = VoxelMapBits::LocalBitIndex(LocalX, LocalY, LocalZ);
							Mask |= static_cast<uint64>(1) << LocalBit;
							CoarseMask |= static_cast<uint8>(1 << VoxelMapBits::CoarseBitIndex(LocalX >> 1, LocalY >> 1, LocalZ >> 1));
							OutData.VoxelData.Add(PackedData);
						}
					}
				}

				if (Mask == 0)
				{
					continue;
				}

				OutData.BlockData.Add(static_cast<uint32>(CoarseMask) | (static_cast<uint32>(BlockIndex) << 8));
				OutData.BlockData.Add(static_cast<uint32>(Mask & 0xFFFFFFFFull));
				OutData.BlockData.Add(static_cast<uint32>(Mask >> 32));
				OutData.BlockData.Add(RenderStartIndex);
			}
		}
	}

	OutData.BlockCount = OutData.BlockData.Num() / 4;
	OutData.OccupiedVoxelCount = OutData.VoxelData.Num();
	return true;
}
