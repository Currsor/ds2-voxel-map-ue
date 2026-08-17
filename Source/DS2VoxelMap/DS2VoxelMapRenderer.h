#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "RHIFwd.h"
#include "VoxelMapGenerator.h"
#include "DS2VoxelMapRenderer.generated.h"

class UTextureRenderTarget2D;
class UCameraComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UVoxelMapDataAsset;
class UVoxelMapWorldAsset;
class FVoxelMapSceneViewExtension;


/**
 * 运行时体素地图观察者：加载已烘焙的数据资产，使用引擎原生 DefaultPawn 输入和
 * SpectatorPawnMovement 驱动 DisplayCamera，并通过后处理材质全屏显示体素 RenderTarget。
 */
UCLASS()
class DS2VOXELMAP_API ADS2VoxelMapRenderer : public ADefaultPawn
{
	GENERATED_BODY()

public:
	ADS2VoxelMapRenderer(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Data")
	TObjectPtr<UVoxelMapDataAsset> VoxelMapAsset;

	/** 可选的分区地图清单。设置后优先于单一 VoxelMapAsset，并在加载时合并为一个稀疏 GPU 缓冲。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Data")
	TObjectPtr<UVoxelMapWorldAsset> VoxelMapWorldAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Render")

	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Render", meta = (ClampMin = "1", UIMin = "1"))
	int32 Resolution = 1024;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VoxelMap|Display")
	TObjectPtr<UCameraComponent> DisplayCamera;

	/** Post Process 材质，须包含名为 VoxelMapRT 的 Texture Parameter。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VoxelMap|Display")
	TObjectPtr<UMaterialInterface> DisplayPostProcessMaterial;

	/** 水平视场角。较小的值可减少画面边缘的透视夸张。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Display", meta = (ClampMin = "5.0", ClampMax = "170.0", UIMin = "30.0", UIMax = "120.0"))
	float DisplayFieldOfView = 60.0f;

	/** 开始游戏时让第一个玩家控制器直接控制这个观察者 Pawn。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Display")
	bool bAutoPossessDisplayCamera = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|LOD")
	float LOD2Distance = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|LOD")
	float LOD1Distance = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Performance")
	float BillboardScale = 1.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VoxelMap|Performance")
	bool bDebugDepth = false;

	UFUNCTION(BlueprintCallable, Category = "VoxelMap")
	UTextureRenderTarget2D* GetRenderTarget() const { return RenderTarget.Get(); }

	UFUNCTION(BlueprintCallable, Category = "VoxelMap")
	bool ReloadVoxelMapAsset();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "VoxelMap")
	void ExportRenderTargetToCSV();

	const FVoxelMapData& GetVoxelMapData() const { return VoxelMapData; }
	FVector GetVoxelWorldOrigin() const { return VoxelWorldOrigin; }
	float GetVoxelSize() const { return RuntimeVoxelSize; }

	FBufferRHIRef BlockDataBuffer;
	FBufferRHIRef VoxelDataBuffer;
	FShaderResourceViewRHIRef BlockDataSRV;
	FShaderResourceViewRHIRef VoxelDataSRV;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void CreateRenderTarget();
	void UploadVoxelMapToGPU();
	void EnsureViewExtension();
	void SetupDisplayCamera();

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DisplayMaterialInstance;

	FVoxelMapData VoxelMapData;
	FVector VoxelWorldOrigin = FVector::ZeroVector;
	float RuntimeVoxelSize = 10.0f;
	TSharedPtr<FVoxelMapSceneViewExtension, ESPMode::ThreadSafe> ViewExtension;
};
