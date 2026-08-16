#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RHIFwd.h"
#include "VoxelMapGenerator.h"
#include "DS2VoxelMapRenderer.generated.h"

class UTextureRenderTarget2D;
class UCameraComponent;
class FVoxelMapSceneViewExtension;

/**
 * 渲染骨架 Actor：
 *  - M0：独立 RT + 独立相机。
 *  - M1：CPU 体素化数据（FVoxelMapData）。
 *  - M2：数据上传 GPU（StructuredBuffer + SRV）。
 *  - M3：自定义全局 shader（billboard 顶点着色器）。
 *  - M4：片元着色器（ray-box + DDA + LOD + 深度覆写）。
 *  - M5：独立地图相机（轨道相机）。
 */
UCLASS()
class DS2VOXELMAP_API ADS2VoxelMapRenderer : public AActor
{
	GENERATED_BODY()

public:
	ADS2VoxelMapRenderer();

	// ---- M0：渲染目标（独立输出表面） ----
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoxelMap")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap")
	int32 Resolution = 1024;

	// ---- M1：体素生成参数 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	FIntVector VoxelGridSize = FIntVector(128, 128, 128);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	int32 VoxelSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	float TerrainHeight = 48.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	float TerrainAmplitude = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	float NoiseFrequency = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	int32 NoiseOctaves = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Generation")
	bool bFlatTerrain = false; // 调试：完全平面地形（表面高度 = TerrainHeight）

	// ---- M3：渲染参数 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap")
	float VoxelSize = 10.0f;

	// ---- M4：LOD 距离阈值（世界单位） ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|LOD")
	float LOD2Distance = 600.0f;  // 小于此距离：完整 4×4×4 DDA

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|LOD")
	float LOD1Distance = 1200.0f; // 小于此距离：2×2×2 粗网格；更远：整个 block

	// ---- M6：性能参数 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Performance")
	float BillboardScale = 1.1f; // billboard 尺寸安全余量（距离自适应后乘此值，>1 留余量）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Performance")
	bool bDebugDepth = false; // 调试：把深度渲染成灰度（近=白，远=黑），验证遮挡

	// ---- M5：独立地图相机 + 轨道 ----
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoxelMap")
	TObjectPtr<UCameraComponent> MapCamera;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Camera")
	bool bAutoOrbit = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Camera")
	float OrbitSpeed = 15.0f;   // 度/秒

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Camera")
	float OrbitDistance = 0.0f; // 0 = 按地形自动

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Camera")
	float OrbitHeight = 0.0f;   // 0 = 按地形自动

	UFUNCTION(BlueprintCallable, Category = "VoxelMap")
	UTextureRenderTarget2D* GetRenderTarget() const { return RenderTarget.Get(); }

	UFUNCTION(BlueprintCallable, Category = "VoxelMap")
	void RegenerateVoxelMap();

	/** 把 RT 当前内容导出为 CSV（x,y,R,G,B），用于诊断 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "VoxelMap")
	void ExportRenderTargetToCSV();

	const FVoxelMapData& GetVoxelMapData() const { return VoxelMapData; }
	FVector GetVoxelWorldOrigin() const;

	// ---- GPU 缓冲 + SRV（渲染线程读取） ----
	FBufferRHIRef BlockDataBuffer;
	FBufferRHIRef VoxelDataBuffer;
	FShaderResourceViewRHIRef BlockDataSRV;
	FShaderResourceViewRHIRef VoxelDataSRV;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void CreateRenderTarget();
	FVoxelMapConfig BuildVoxelConfig() const;
	void UploadVoxelMapToGPU();
	void EnsureViewExtension();
	void UpdateMapCamera();

	FVoxelMapData VoxelMapData;
	TSharedPtr<FVoxelMapSceneViewExtension, ESPMode::ThreadSafe> ViewExtension;
	float OrbitAngle = 0.0f;
};
