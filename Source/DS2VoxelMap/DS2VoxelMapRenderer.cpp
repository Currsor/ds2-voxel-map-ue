#include "DS2VoxelMapRenderer.h"

#include "Camera/CameraComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "SceneViewExtension.h"
#include "VoxelMapRendering.h"
#include "Misc/FileHelper.h"

ADS2VoxelMapRenderer::ADS2VoxelMapRenderer()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// 独立地图相机（M5）
	MapCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("MapCamera"));
	MapCamera->FieldOfView = 60.0f;
	MapCamera->SetupAttachment(Root);

	CreateRenderTarget();
}

void ADS2VoxelMapRenderer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RegenerateVoxelMap();
	UpdateMapCamera();
	EnsureViewExtension();
}

void ADS2VoxelMapRenderer::BeginPlay()
{
	Super::BeginPlay();
	RegenerateVoxelMap();
	UpdateMapCamera();
	EnsureViewExtension();
}

void ADS2VoxelMapRenderer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ViewExtension.Reset();
	Super::EndPlay(EndPlayReason);
}

void ADS2VoxelMapRenderer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bAutoOrbit)
	{
		OrbitAngle = FMath::Fmod(OrbitAngle + OrbitSpeed * DeltaSeconds, 360.0f);
		UpdateMapCamera();
	}
}

void ADS2VoxelMapRenderer::CreateRenderTarget()
{
	// 运行时创建、每次构造重建的 RT 用 RF_Transient，避免它（连同继承自 UTexture 的
	// AssetImportData 编辑器子对象）被序列化进关卡，导致保存 .umap 报错。
	RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("VoxelMapRenderTarget"), RF_Transient);
	RenderTarget->InitCustomFormat((uint32)Resolution, (uint32)Resolution, PF_B8G8R8A8, /*bInForceLinearGamma=*/false);
	RenderTarget->ClearColor = FLinearColor::Black;
#if WITH_EDITORONLY_DATA
	RenderTarget->AssetImportData = nullptr;
#endif
	RenderTarget->UpdateResource();
}

FVoxelMapConfig ADS2VoxelMapRenderer::BuildVoxelConfig() const
{
	FVoxelMapConfig Config;
	Config.GridSize = VoxelGridSize;
	Config.Seed = VoxelSeed;
	Config.TerrainHeight = TerrainHeight;
	Config.TerrainAmplitude = TerrainAmplitude;
	Config.NoiseFrequency = NoiseFrequency;
	Config.NoiseOctaves = NoiseOctaves;
	Config.bFlatTerrain = bFlatTerrain;
	return Config;
}

void ADS2VoxelMapRenderer::EnsureViewExtension()
{
	if (!ViewExtension.IsValid())
	{
		ViewExtension = FSceneViewExtensions::NewExtension<FVoxelMapSceneViewExtension>(this);
	}
}

FVector ADS2VoxelMapRenderer::GetVoxelWorldOrigin() const
{
	const FIntVector VoxelGrid = VoxelMapData.BlockGridSize * VOXELMAP_BLOCK_SIZE;
	const FVector HalfWorld(
		(float)VoxelGrid.X * VoxelSize * 0.5f,
		(float)VoxelGrid.Z * VoxelSize * 0.5f,
		(float)VoxelGrid.Y * VoxelSize * 0.5f);
	return GetActorLocation() - HalfWorld;
}

void ADS2VoxelMapRenderer::UpdateMapCamera()
{
	if (bAutoOrbit && MapCamera)
	{
		// 轨道相机：绕地形中心旋转，俯视
		const FIntVector VoxelGrid = VoxelMapData.BlockGridSize * VOXELMAP_BLOCK_SIZE;
		const FVector HalfWorld(
			(float)VoxelGrid.X * VoxelSize * 0.5f,
			(float)VoxelGrid.Z * VoxelSize * 0.5f,
			(float)VoxelGrid.Y * VoxelSize * 0.5f);
		const FVector Center = GetActorLocation();
		const float MaxExtent = FMath::Max(HalfWorld.X, HalfWorld.Y);
		const float Dist = OrbitDistance > 0.0f ? OrbitDistance : MaxExtent * 1.6f;
		const float Height = OrbitHeight > 0.0f ? OrbitHeight : MaxExtent * 1.4f;
		const float AngleRad = FMath::DegreesToRadians(OrbitAngle);
		const FVector Eye = Center + FVector(FMath::Cos(AngleRad) * Dist, FMath::Sin(AngleRad) * Dist, Height);
		const FRotator LookRot = (Center - Eye).Rotation();

		MapCamera->SetWorldLocation(Eye);
		MapCamera->SetWorldRotation(LookRot);
	}
}

void ADS2VoxelMapRenderer::UploadVoxelMapToGPU()
{
	// 拷贝数据进渲染线程 lambda（游戏线程的数组可能随后变化/重分配）
	TArray<uint32> BlockDataCopy = VoxelMapData.BlockData;
	TArray<uint32> VoxelDataCopy = VoxelMapData.VoxelData;
	const int32 BlockCount = VoxelMapData.BlockCount;
	const int32 VoxelCount = VoxelMapData.OccupiedVoxelCount;

	ADS2VoxelMapRenderer* ThisPtr = this;

	ENQUEUE_RENDER_COMMAND(UploadVoxelMap)(
		[ThisPtr, BlockDataCopy = MoveTemp(BlockDataCopy), VoxelDataCopy = MoveTemp(VoxelDataCopy), BlockCount, VoxelCount](FRHICommandListImmediate& RHICmdList)
		{
			FBufferRHIRef BlockBuf;
			if (BlockDataCopy.Num() > 0)
			{
				TResourceArray<uint32> Arr;
				Arr.Reserve(BlockDataCopy.Num());
				for (uint32 V : BlockDataCopy) { Arr.Add(V); }
				FRHIResourceCreateInfo Info(TEXT("VoxelMapBlockData"), &Arr);
				BlockBuf = RHICmdList.CreateStructuredBuffer(sizeof(uint32), (uint32)(BlockDataCopy.Num() * sizeof(uint32)), BUF_ShaderResource | BUF_Static, Info);
			}

			FBufferRHIRef VoxelBuf;
			if (VoxelDataCopy.Num() > 0)
			{
				TResourceArray<uint32> Arr;
				Arr.Reserve(VoxelDataCopy.Num());
				for (uint32 V : VoxelDataCopy) { Arr.Add(V); }
				FRHIResourceCreateInfo Info(TEXT("VoxelMapVoxelData"), &Arr);
				VoxelBuf = RHICmdList.CreateStructuredBuffer(sizeof(uint32), (uint32)(VoxelDataCopy.Num() * sizeof(uint32)), BUF_ShaderResource | BUF_Static, Info);
			}

			ThisPtr->BlockDataBuffer = BlockBuf;
			ThisPtr->VoxelDataBuffer = VoxelBuf;
			ThisPtr->BlockDataSRV = nullptr;
			ThisPtr->VoxelDataSRV = nullptr;
			if (BlockBuf.IsValid())
			{
				ThisPtr->BlockDataSRV = RHICmdList.CreateShaderResourceView(BlockBuf.GetReference());
			}
			if (VoxelBuf.IsValid())
			{
				ThisPtr->VoxelDataSRV = RHICmdList.CreateShaderResourceView(VoxelBuf.GetReference());
			}

			UE_LOG(LogTemp, Log,
				TEXT("[VoxelMap] GPU upload OK: BlockData=%d elems (%d B), VoxelData=%d elems (%d B), BlockCount=%d, VoxelCount=%d"),
				BlockDataCopy.Num(), BlockDataCopy.Num() * (int32)sizeof(uint32),
				VoxelDataCopy.Num(), VoxelDataCopy.Num() * (int32)sizeof(uint32),
				BlockCount, VoxelCount);
		});
}

void ADS2VoxelMapRenderer::ExportRenderTargetToCSV()
{
	if (!RenderTarget)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: 没有 RT"));
		return;
	}

	// 确保渲染线程的 pass 已执行完
	FlushRenderingCommands();

	// 游戏线程安全访问 RT 资源（GetRenderTargetResource 只能在渲染线程调用）
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: RT 资源未初始化"));
		return;
	}

	TArray<FColor> Pixels;
	if (!RTResource->ReadPixels(Pixels))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: ReadPixels"));
		return;
	}

	const int32 W = RenderTarget->SizeX;
	const int32 H = RenderTarget->SizeY;
	FString Csv;
	Csv.Reserve((int64)Pixels.Num() * 16);
	for (int32 i = 0; i < Pixels.Num(); ++i)
	{
		const FColor& P = Pixels[i];
		Csv += FString::Printf(TEXT("%d,%d,%d,%d,%d\n"), i % W, i / W, P.R, P.G, P.B);
	}

	const FString Path = FPaths::ProjectDir() / TEXT("rt_dump.csv");
	if (FFileHelper::SaveStringToFile(Csv, *Path))
	{
		UE_LOG(LogTemp, Log, TEXT("[VoxelMap] RT 已导出: %s (%d×%d)"), *Path, W, H);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: 无法写入 %s"), *Path);
	}
}

void ADS2VoxelMapRenderer::RegenerateVoxelMap()
{
	const FVoxelMapConfig Config = BuildVoxelConfig();
	FVoxelMapGenerator::Generate(Config, VoxelMapData);
	const bool bValid = FVoxelMapGenerator::Validate(VoxelMapData);

	UE_LOG(LogTemp, Log,
		TEXT("[VoxelMap] %dx%dx%d voxels -> %d blocks, %d occupied voxels | BlockData %d B, VoxelData %d B | Validate=%s"),
		Config.GridSize.X, Config.GridSize.Y, Config.GridSize.Z,
		VoxelMapData.BlockCount, VoxelMapData.OccupiedVoxelCount,
		VoxelMapData.BlockData.Num() * (int32)sizeof(uint32),
		VoxelMapData.VoxelData.Num() * (int32)sizeof(uint32),
		bValid ? TEXT("OK") : TEXT("FAILED"));

	if (bValid)
	{
		UploadVoxelMapToGPU();
	}
}
