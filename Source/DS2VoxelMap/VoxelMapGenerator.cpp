#include "VoxelMapGenerator.h"

#include "VoxelMapDataBuilder.h"

namespace
{
	/** 64-bit 分支友好 popcount（自检用） */
	int32 PopCount64(uint64 X)
	{
		X = X - ((X >> 1) & 0x5555555555555555ull);
		X = (X & 0x3333333333333333ull) + ((X >> 2) & 0x3333333333333333ull);
		X = (X + (X >> 4)) & 0x0F0F0F0F0F0F0F0Full;
		return (int32)((X * 0x0101010101010101ull) >> 56);
	}
}

uint32 FVoxelMapGenerator::Hash(int32 X, int32 Y, int32 Seed)
{
	uint32 H = (uint32)X * 374761393u + (uint32)Y * 668265263u + (uint32)Seed * 2246822519u;
	H = (H ^ (H >> 13)) * 1274126177u;
	H ^= H >> 16;
	return H;
}

float FVoxelMapGenerator::ValueNoise(float X, float Y, int32 Seed)
{
	const int32 X0 = FMath::FloorToInt(X);
	const int32 Y0 = FMath::FloorToInt(Y);
	float Fx = X - (float)X0;
	float Fy = Y - (float)Y0;
	Fx = Fx * Fx * (3.0f - 2.0f * Fx);
	Fy = Fy * Fy * (3.0f - 2.0f * Fy);

	const float A = (float)(Hash(X0, Y0, Seed) & 0x00FFFFFFu) / (float)0x01000000;
	const float B = (float)(Hash(X0 + 1, Y0, Seed) & 0x00FFFFFFu) / (float)0x01000000;
	const float C = (float)(Hash(X0, Y0 + 1, Seed) & 0x00FFFFFFu) / (float)0x01000000;
	const float D = (float)(Hash(X0 + 1, Y0 + 1, Seed) & 0x00FFFFFFu) / (float)0x01000000;
	return FMath::Lerp(FMath::Lerp(A, B, Fx), FMath::Lerp(C, D, Fx), Fy);
}

float FVoxelMapGenerator::Fbm(float X, float Y, int32 Seed, int32 Octaves)
{
	float Value = 0.0f;
	float Amplitude = 1.0f;
	float Frequency = 1.0f;
	float Total = 0.0f;
	for (int32 i = 0; i < Octaves; ++i)
	{
		Value += Amplitude * ValueNoise(X * Frequency, Y * Frequency, Seed + i * 1013);
		Total += Amplitude;
		Amplitude *= 0.5f;
		Frequency *= 2.0f;
	}
	return Value / Total;
}

float FVoxelMapGenerator::SampleSurfaceHeight(int32 X, int32 Z, const FVoxelMapConfig& Config)
{
	if (Config.bFlatTerrain)
	{
		return FMath::Clamp(Config.TerrainHeight, 0.0f, (float)(Config.GridSize.Y - 1));
	}
	const float N = Fbm((float)X * Config.NoiseFrequency, (float)Z * Config.NoiseFrequency, Config.Seed, Config.NoiseOctaves);
	const float H = Config.TerrainHeight + (N * 2.0f - 1.0f) * Config.TerrainAmplitude;
	return FMath::Clamp(H, 0.0f, (float)(Config.GridSize.Y - 1));
}

void FVoxelMapGenerator::SampleColor(float HeightT, uint8& OutR, uint8& OutG, uint8& OutB, uint8& OutExtra)
{
	const float T = FMath::Clamp(HeightT, 0.0f, 1.0f);
	if (T < 0.30f)      { OutR = 0x8A; OutG = 0x7A; OutB = 0x57; OutExtra = 0; }
	else if (T < 0.50f) { OutR = 0x3E; OutG = 0x63; OutB = 0x2E; OutExtra = 1; }
	else if (T < 0.72f) { OutR = 0x5A; OutG = 0x5A; OutB = 0x58; OutExtra = 2; }
	else                { OutR = 0xE8; OutG = 0xEA; OutB = 0xEF; OutExtra = 3; }
}

void FVoxelMapGenerator::Generate(const FVoxelMapConfig& InConfig, FVoxelMapData& OutData)
{
	FVoxelMapConfig Config = InConfig;
	auto Align4 = [](int32 Value)
	{
		return FMath::Max(VOXELMAP_BLOCK_SIZE,
			FMath::DivideAndRoundUp(Value, VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE);
	};
	Config.GridSize = FIntVector(Align4(Config.GridSize.X), Align4(Config.GridSize.Y), Align4(Config.GridSize.Z));

	FVoxelMapDataBuilder::BuildFromSampler(Config.GridSize,
		[&Config](const FIntVector& Coordinate, uint32& OutPackedData)
		{
			const float Height = SampleSurfaceHeight(Coordinate.X, Coordinate.Z, Config);
			if (Coordinate.Y > static_cast<int32>(Height))
			{
				return false;
			}

			uint8 R, G, B, Extra;
			SampleColor(Height / static_cast<float>(Config.GridSize.Y), R, G, B, Extra);
			OutPackedData = VoxelMapBits::PackColor(R, G, B, Extra);
			return true;
		}, OutData);
}

bool FVoxelMapGenerator::Validate(const FVoxelMapData& Data)
{
	if (Data.BlockCount < 0 || Data.BlockData.Num() != Data.BlockCount * 4)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: BlockData length %d != %d*4"),
			Data.BlockData.Num(), Data.BlockCount);
		return false;
	}

	const int64 AddressableBlocks = static_cast<int64>(Data.BlockGridSize.X) * Data.BlockGridSize.Y * Data.BlockGridSize.Z;
	if (AddressableBlocks <= 0 || AddressableBlocks > 0x00FFFFFFll)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: invalid BlockGridSize %dx%dx%d"),
			Data.BlockGridSize.X, Data.BlockGridSize.Y, Data.BlockGridSize.Z);
		return false;
	}

	int32 ExpectedVoxels = 0;
	int32 PreviousBlockIndex = INDEX_NONE;
	for (int32 StorageIndex = 0; StorageIndex < Data.BlockCount; ++StorageIndex)
	{
		const uint32* Block = &Data.BlockData[StorageIndex * 4];
		const uint8 Coarse = static_cast<uint8>(Block[0] & 0xFFu);
		const int32 BlockIndex = static_cast<int32>((Block[0] >> 8) & 0x00FFFFFFu);
		const uint64 Mask = static_cast<uint64>(Block[1]) | (static_cast<uint64>(Block[2]) << 32);
		const uint32 Start = Block[3];

		if (BlockIndex <= PreviousBlockIndex || BlockIndex >= AddressableBlocks)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: invalid block at storage %d, location %d"), StorageIndex, BlockIndex);
			return false;
		}

		PreviousBlockIndex = BlockIndex;

		if (static_cast<int32>(Start) != ExpectedVoxels)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: block %d RenderStartIndex %u != expected %d"),
				BlockIndex, Start, ExpectedVoxels);
			return false;
		}

		uint8 Rebuilt = 0;
		for (int32 Cz = 0; Cz < 2; ++Cz)
		for (int32 Cy = 0; Cy < 2; ++Cy)
		for (int32 Cx = 0; Cx < 2; ++Cx)
		{
			bool bAny = false;
			for (int32 Dz = 0; Dz < 2 && !bAny; ++Dz)
			for (int32 Dy = 0; Dy < 2 && !bAny; ++Dy)
			for (int32 Dx = 0; Dx < 2 && !bAny; ++Dx)
			{
				const uint32 Bit = VoxelMapBits::LocalBitIndex(Cx * 2 + Dx, Cy * 2 + Dy, Cz * 2 + Dz);
				bAny = (Mask & (static_cast<uint64>(1) << Bit)) != 0;
			}
			if (bAny)
			{
				Rebuilt |= static_cast<uint8>(1 << VoxelMapBits::CoarseBitIndex(Cx, Cy, Cz));
			}
		}

		if (Rebuilt != Coarse)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: block %d coarse mask 0x%02X != rebuilt 0x%02X"),
				BlockIndex, Coarse, Rebuilt);
			return false;
		}
		ExpectedVoxels += PopCount64(Mask);
	}

	if (ExpectedVoxels != Data.OccupiedVoxelCount || ExpectedVoxels != Data.VoxelData.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: voxel count expected %d, got %d/%d"),
			ExpectedVoxels, Data.OccupiedVoxelCount, Data.VoxelData.Num());
		return false;
	}
	return true;
}
