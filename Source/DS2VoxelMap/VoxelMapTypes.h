#pragma once

#include "CoreMinimal.h"

// 体素块尺寸：4×4×4，占用状态恰好用一个 64-bit 掩码表示
constexpr int32 VOXELMAP_BLOCK_SIZE = 4;
constexpr int32 VOXELMAP_BLOCK_VOXELS = VOXELMAP_BLOCK_SIZE * VOXELMAP_BLOCK_SIZE * VOXELMAP_BLOCK_SIZE; // 64

/**
 * 每 block 4 个 uint32，与 GPU StructuredBuffer 布局严格一致（M2 上传 / M4 着色器依赖此布局）。
 */
struct FBlockRenderData
{
	uint32 ShapeLevelAndLocation; // [0..7] = 2×2×2 粗掩码, [8..31] = block 线性索引
	uint32 ShapeMaskLow;          // 4×4×4 64-bit 占用掩码 低 32
	uint32 ShapeMaskHigh;         // 高 32
	uint32 RenderStartIndex;      // 该 block 在 voxel render buffer 的起始索引
};
static_assert(sizeof(FBlockRenderData) == 16, "FBlockRenderData must be 16 bytes (4x uint32)");

/** 每占用 voxel 1 个 uint32 */
struct FVoxelRenderData
{
	uint32 PackedData; // [0..23] = RGB, [24..31] = material/extra
};
static_assert(sizeof(FVoxelRenderData) == 4, "FVoxelRenderData must be 4 bytes");

namespace VoxelMapBits
{
	/** 4×4×4 局部坐标 (Lx,Ly,Lz) -> 64-bit 掩码位 */
	FORCEINLINE uint32 LocalBitIndex(int32 Lx, int32 Ly, int32 Lz)
	{
		return (uint32)((Lx & 3) | ((Ly & 3) << 2) | ((Lz & 3) << 4));
	}

	/** 2×2×2 粗 cell 坐标 (Cx,Cy,Cz) -> 8-bit 粗掩码位 */
	FORCEINLINE uint32 CoarseBitIndex(int32 Cx, int32 Cy, int32 Cz)
	{
		return (uint32)((Cx & 1) | ((Cy & 1) << 1) | ((Cz & 1) << 2));
	}

	/** RGB(24bit) + extra(8bit) -> uint32 */
	FORCEINLINE uint32 PackColor(uint8 R, uint8 G, uint8 B, uint8 Extra)
	{
		return (uint32)R | ((uint32)G << 8) | ((uint32)B << 16) | ((uint32)Extra << 24);
	}
}
