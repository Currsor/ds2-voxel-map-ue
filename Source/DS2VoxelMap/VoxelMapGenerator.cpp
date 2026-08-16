#include "VoxelMapGenerator.h"

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
	// 平滑插值（Hermite smoothstep），避免格点处出现明显的区块边界
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
	return Value / Total; // 归一化到 [0,1]
}

float FVoxelMapGenerator::SampleSurfaceHeight(int32 X, int32 Z, const FVoxelMapConfig& Config)
{
	if (Config.bFlatTerrain)
	{
		// 调试：完全平面地形
		return FMath::Clamp(Config.TerrainHeight, 0.0f, (float)(Config.GridSize.Y - 1));
	}
	const float N = Fbm((float)X * Config.NoiseFrequency, (float)Z * Config.NoiseFrequency, Config.Seed, Config.NoiseOctaves);
	const float H = Config.TerrainHeight + (N * 2.0f - 1.0f) * Config.TerrainAmplitude;
	return FMath::Clamp(H, 0.0f, (float)(Config.GridSize.Y - 1));
}

void FVoxelMapGenerator::SampleColor(float HeightT, uint8& OutR, uint8& OutG, uint8& OutB, uint8& OutExtra)
{
	// 按归一化高度分带：沙 -> 草 -> 岩 -> 雪
	const float T = FMath::Clamp(HeightT, 0.0f, 1.0f);
	if (T < 0.30f)      { OutR = 0x8A; OutG = 0x7A; OutB = 0x57; OutExtra = 0; }
	else if (T < 0.50f) { OutR = 0x3E; OutG = 0x63; OutB = 0x2E; OutExtra = 1; }
	else if (T < 0.72f) { OutR = 0x5A; OutG = 0x5A; OutB = 0x58; OutExtra = 2; }
	else                { OutR = 0xE8; OutG = 0xEA; OutB = 0xEF; OutExtra = 3; }
}

void FVoxelMapGenerator::Generate(const FVoxelMapConfig& InConfig, FVoxelMapData& OutData)
{
	OutData.Reset();

	// 网格对齐到 4 的倍数，保证所有 block 都是完整 4×4×4
	FVoxelMapConfig Config = InConfig;
	auto Align4 = [](int32 V)
	{
		return FMath::Max(VOXELMAP_BLOCK_SIZE, ((V + VOXELMAP_BLOCK_SIZE - 1) / VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE);
	};
	Config.GridSize.X = Align4(Config.GridSize.X);
	Config.GridSize.Y = Align4(Config.GridSize.Y);
	Config.GridSize.Z = Align4(Config.GridSize.Z);

	const FIntVector Grid = Config.GridSize;
	const FIntVector BlockGrid(Grid.X / VOXELMAP_BLOCK_SIZE, Grid.Y / VOXELMAP_BLOCK_SIZE, Grid.Z / VOXELMAP_BLOCK_SIZE);
	const int32 TotalBlocks = BlockGrid.X * BlockGrid.Y * BlockGrid.Z;

	OutData.BlockGridSize = BlockGrid;
	OutData.BlockCount = TotalBlocks;
	OutData.BlockData.SetNumUninitialized(TotalBlocks * 4);
	OutData.VoxelData.Reserve((int64)TotalBlocks * VOXELMAP_BLOCK_VOXELS / 2);

	const int32 BV = VOXELMAP_BLOCK_SIZE;

	for (int32 Bz = 0; Bz < BlockGrid.Z; ++Bz)
	{
		for (int32 By = 0; By < BlockGrid.Y; ++By)
		{
			for (int32 Bx = 0; Bx < BlockGrid.X; ++Bx)
			{
				const int32 BlockIndex = Bx + By * BlockGrid.X + Bz * BlockGrid.X * BlockGrid.Y;
				// 前缀和：该 block 之前已写入的占用体素数
				const int32 VoxelStart = OutData.VoxelData.Num();

				uint64 Mask = 0;
				uint8 CoarseMask = 0;

				// 遍历顺序 = 掩码位升序（Lx 最快 -> Ly -> Lz），保证 VoxelData 顺序与掩码位序一致，
				// M4 里「统计命中位之前的占用数」才能直接用做 VoxelData 的 offset。
				for (int32 Lz = 0; Lz < BV; ++Lz)
				{
					for (int32 Ly = 0; Ly < BV; ++Ly)
					{
						for (int32 Lx = 0; Lx < BV; ++Lx)
						{
							const int32 X = Bx * BV + Lx;
							const int32 Y = By * BV + Ly;
							const int32 Z = Bz * BV + Lz;

							const float H = SampleSurfaceHeight(X, Z, Config);
							if (Y > (int32)H)
							{
								continue;
							}

							Mask |= (uint64)1 << VoxelMapBits::LocalBitIndex(Lx, Ly, Lz);
							CoarseMask |= (uint8)(1 << VoxelMapBits::CoarseBitIndex(Lx >> 1, Ly >> 1, Lz >> 1));

							uint8 R, G, B, Extra;
							SampleColor(H / (float)Grid.Y, R, G, B, Extra);
							OutData.VoxelData.Add(VoxelMapBits::PackColor(R, G, B, Extra));
						}
					}
				}

				const uint32 Base = (uint32)BlockIndex * 4;
				OutData.BlockData[Base + 0] = (uint32)CoarseMask | ((uint32)(BlockIndex & 0x00FFFFFF) << 8);
				OutData.BlockData[Base + 1] = (uint32)(Mask & 0xFFFFFFFFu);
				OutData.BlockData[Base + 2] = (uint32)(Mask >> 32);
				OutData.BlockData[Base + 3] = (uint32)VoxelStart;
			}
		}
	}

	OutData.OccupiedVoxelCount = OutData.VoxelData.Num();
}

bool FVoxelMapGenerator::Validate(const FVoxelMapData& Data)
{
	if (Data.BlockData.Num() != Data.BlockCount * 4)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: BlockData length %d != %d*4"),
			Data.BlockData.Num(), Data.BlockCount);
		return false;
	}

	int32 ExpectedVoxels = 0;
	for (int32 BlockIndex = 0; BlockIndex < Data.BlockCount; ++BlockIndex)
	{
		const uint32* B = &Data.BlockData[BlockIndex * 4];
		const uint8 Coarse = (uint8)(B[0] & 0xFFu);
		const uint64 Mask = (uint64)B[1] | ((uint64)B[2] << 32);
		const uint32 Start = B[3];

		if ((int32)Start != ExpectedVoxels)
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Validate failed: block %d RenderStartIndex %u != expected %d"),
				BlockIndex, Start, ExpectedVoxels);
			return false;
		}

		// 重建粗掩码并与存储值比对（粗 cell 占用 = 其 8 个体素掩码的 OR）
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
				if (Mask & ((uint64)1 << Bit))
				{
					bAny = true;
				}
			}
			if (bAny)
			{
				Rebuilt |= (uint8)(1 << VoxelMapBits::CoarseBitIndex(Cx, Cy, Cz));
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
