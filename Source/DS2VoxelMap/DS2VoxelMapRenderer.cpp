#include "DS2VoxelMapRenderer.h"

#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawnMovement.h"
#include "GameFramework/PlayerInput.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "SceneViewExtension.h"
#include "VoxelMapDataAsset.h"
#include "VoxelMapDataBuilder.h"
#include "VoxelMapRendering.h"
#include "VoxelMapTypes.h"
#include "VoxelMapWorldAsset.h"
#include "Misc/FileHelper.h"


ADS2VoxelMapRenderer::ADS2VoxelMapRenderer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer
		.SetDefaultSubobjectClass<USpectatorPawnMovement>(ADefaultPawn::MovementComponentName)
		.DoNotCreateDefaultSubobject(ADefaultPawn::MeshComponentName))
{
	PrimaryActorTick.bCanEverTick = false;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	bAddDefaultMovementBindings = true;
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	if (USphereComponent* Collision = GetCollisionComponent())
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Collision->SetGenerateOverlapEvents(false);
	}

	DisplayCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("DisplayCamera"));
	DisplayCamera->SetupAttachment(GetRootComponent());
	DisplayCamera->SetAutoActivate(true);
	DisplayCamera->bUsePawnControlRotation = true;
	DisplayCamera->FieldOfView = DisplayFieldOfView;
	DisplayCamera->bConstrainAspectRatio = false;
	DisplayCamera->PostProcessBlendWeight = 1.0f;

	if (UFloatingPawnMovement* FloatingMovement = Cast<UFloatingPawnMovement>(GetMovementComponent()))
	{
		FloatingMovement->MaxSpeed = 2400.0f;
		FloatingMovement->Acceleration = 8000.0f;
		FloatingMovement->Deceleration = 8000.0f;
		FloatingMovement->TurningBoost = 8.0f;
	}
}

void ADS2VoxelMapRenderer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (DisplayCamera)
	{
		DisplayCamera->SetFieldOfView(DisplayFieldOfView);
	}
	CreateRenderTarget();
}

void ADS2VoxelMapRenderer::BeginPlay()
{
	Super::BeginPlay();
	CreateRenderTarget();
	ReloadVoxelMapAsset();
	EnsureViewExtension();
	SetupDisplayCamera();
}

void ADS2VoxelMapRenderer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (DisplayCamera && DisplayMaterialInstance)
	{
		DisplayCamera->RemoveBlendable(DisplayMaterialInstance);
	}
	DisplayMaterialInstance = nullptr;
	ViewExtension.Reset();
	Super::EndPlay(EndPlayReason);
}

void ADS2VoxelMapRenderer::CreateRenderTarget()
{
	if (RenderTarget)
	{
		const bool bIsRuntimeRenderTarget = RenderTarget->GetOuter() == this && RenderTarget->HasAnyFlags(RF_Transient);
		if (!bIsRuntimeRenderTarget || (RenderTarget->SizeX == Resolution && RenderTarget->SizeY == Resolution))
		{
			return;
		}
	}

	RenderTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
	RenderTarget->InitCustomFormat(static_cast<uint32>(Resolution), static_cast<uint32>(Resolution), PF_B8G8R8A8, false);
	RenderTarget->ClearColor = FLinearColor::Black;
#if WITH_EDITORONLY_DATA
	RenderTarget->AssetImportData = nullptr;
#endif
	RenderTarget->UpdateResource();
}

bool ADS2VoxelMapRenderer::ReloadVoxelMapAsset()
{
	if (VoxelMapWorldAsset)
	{
		TArray<FVoxelMapSourceVoxel> SourceVoxels;
		RuntimeVoxelSize = VoxelMapWorldAsset->VoxelSize;
		VoxelWorldOrigin = VoxelMapWorldAsset->WorldOrigin;
		if (RuntimeVoxelSize <= UE_SMALL_NUMBER || VoxelMapWorldAsset->VoxelGridSize.GetMin() <= 0)
		{
			VoxelMapData.Reset();
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Assigned WorldAsset has invalid global metadata"));
			return false;
		}

		for (const FVoxelMapRegionReference& RegionReference : VoxelMapWorldAsset->Regions)
		{
			const UVoxelMapDataAsset* RegionAsset = RegionReference.Asset;
			if (!RegionAsset || !FVoxelMapGenerator::Validate(RegionAsset->Data)
				|| !FMath::IsNearlyEqual(RegionAsset->VoxelSize, RuntimeVoxelSize))
			{
				UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Skipping invalid or incompatible region (%d,%d,%d)"),
					RegionReference.RegionCoordinate.X, RegionReference.RegionCoordinate.Y, RegionReference.RegionCoordinate.Z);
				continue;
			}

			const FVoxelMapData& RegionData = RegionAsset->Data;
			for (int32 StorageIndex = 0; StorageIndex < RegionData.BlockCount; ++StorageIndex)
			{
				const uint32* Block = &RegionData.BlockData[StorageIndex * 4];
				const int32 BlockIndex = static_cast<int32>((Block[0] >> 8) & 0x00FFFFFFu);
				const uint64 Mask = static_cast<uint64>(Block[1]) | (static_cast<uint64>(Block[2]) << 32);
				const int32 BlockX = BlockIndex % RegionData.BlockGridSize.X;
				const int32 BlockY = (BlockIndex / RegionData.BlockGridSize.X) % RegionData.BlockGridSize.Y;
				const int32 BlockZ = BlockIndex / (RegionData.BlockGridSize.X * RegionData.BlockGridSize.Y);
				int32 PackedOffset = static_cast<int32>(Block[3]);

				for (int32 LocalBit = 0; LocalBit < VOXELMAP_BLOCK_VOXELS; ++LocalBit)

				{
					if ((Mask & (static_cast<uint64>(1) << LocalBit)) == 0)
					{
						continue;
					}
					if (!RegionData.VoxelData.IsValidIndex(PackedOffset))
					{
						VoxelMapData.Reset();
						UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Region voxel stream is truncated"));
						return false;
					}

					const FIntVector LocalCoordinate(
						BlockX * VOXELMAP_BLOCK_SIZE + (LocalBit & 3),
						BlockY * VOXELMAP_BLOCK_SIZE + ((LocalBit >> 2) & 3),
						BlockZ * VOXELMAP_BLOCK_SIZE + ((LocalBit >> 4) & 3));
					const FVector WorldPosition = RegionAsset->WorldOrigin + FVector(
						static_cast<double>(LocalCoordinate.X) * RuntimeVoxelSize,
						static_cast<double>(LocalCoordinate.Z) * RuntimeVoxelSize,
						static_cast<double>(LocalCoordinate.Y) * RuntimeVoxelSize);
					const FVector Relative = (WorldPosition - VoxelWorldOrigin) / RuntimeVoxelSize;

					FVoxelMapSourceVoxel& SourceVoxel = SourceVoxels.AddDefaulted_GetRef();
					SourceVoxel.Coordinate = FIntVector(
						FMath::RoundToInt(Relative.X),
						FMath::RoundToInt(Relative.Z),
						FMath::RoundToInt(Relative.Y));
					SourceVoxel.PackedData = RegionData.VoxelData[PackedOffset++];
				}
			}
		}

		if (!FVoxelMapDataBuilder::Build(VoxelMapWorldAsset->VoxelGridSize, SourceVoxels, VoxelMapData)
			|| !FVoxelMapGenerator::Validate(VoxelMapData))
		{
			VoxelMapData.Reset();
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Failed to merge WorldAsset regions"));
			return false;
		}
	}
	else
	{
		if (!VoxelMapAsset)
		{
			VoxelMapData.Reset();
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] No VoxelMapAsset or VoxelMapWorldAsset assigned"));
			return false;
		}
		if (!FVoxelMapGenerator::Validate(VoxelMapAsset->Data))
		{
			VoxelMapData.Reset();
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Assigned VoxelMapAsset contains invalid data"));
			return false;
		}

		VoxelMapData = VoxelMapAsset->Data;
		RuntimeVoxelSize = VoxelMapAsset->VoxelSize;
		VoxelWorldOrigin = VoxelMapAsset->WorldOrigin;
	}

	UploadVoxelMapToGPU();
	UE_LOG(LogTemp, Log, TEXT("[VoxelMap] Loaded %s: %d blocks, %d occupied voxels"),
		VoxelMapWorldAsset ? TEXT("world asset") : TEXT("asset"),
		VoxelMapData.BlockCount, VoxelMapData.OccupiedVoxelCount);
	return true;
}


void ADS2VoxelMapRenderer::SetupDisplayCamera()
{
	if (!DisplayCamera || !DisplayPostProcessMaterial || !RenderTarget)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VoxelMap] Display camera disabled: Camera=%s, Material=%s, RenderTarget=%s"),
			DisplayCamera ? TEXT("OK") : TEXT("Missing"),
			DisplayPostProcessMaterial ? TEXT("OK") : TEXT("Missing"),
			RenderTarget ? TEXT("OK") : TEXT("Missing"));
		return;
	}

	if (DisplayMaterialInstance)
	{
		DisplayCamera->RemoveBlendable(DisplayMaterialInstance);
	}

	DisplayMaterialInstance = UMaterialInstanceDynamic::Create(DisplayPostProcessMaterial, this);
	if (!DisplayMaterialInstance)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Failed to create display material instance"));
		return;
	}

	DisplayMaterialInstance->SetTextureParameterValue(TEXT("VoxelMapRT"), RenderTarget);
	DisplayCamera->AddOrUpdateBlendable(DisplayMaterialInstance, 1.0f);

	if (bAutoPossessDisplayCamera)
	{
		if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
		{
			DisplayCamera->SetFieldOfView(DisplayFieldOfView);
			DisplayCamera->Activate(true);
			PlayerController->Possess(this);
			PlayerController->SetControlRotation(GetActorRotation());
			PlayerController->SetInputMode(FInputModeGameOnly());
			PlayerController->bShowMouseCursor = false;
			UE_LOG(LogTemp, Log, TEXT("[VoxelMap] Display pawn possessed: %s, InputComponent=%s"),
				PlayerController->GetPawn() == this ? TEXT("yes") : TEXT("no"),
				InputComponent ? TEXT("ready") : TEXT("missing"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] No player controller found for display camera"));
		}
	}
}

void ADS2VoxelMapRenderer::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	bAddDefaultMovementBindings = true;
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}

void ADS2VoxelMapRenderer::EnsureViewExtension()
{
	if (!ViewExtension.IsValid())
	{
		ViewExtension = FSceneViewExtensions::NewExtension<FVoxelMapSceneViewExtension>(this);
	}
}

void ADS2VoxelMapRenderer::UploadVoxelMapToGPU()
{
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
				Arr.Append(BlockDataCopy);
				FRHIResourceCreateInfo Info(TEXT("VoxelMapBlockData"), &Arr);
				BlockBuf = RHICmdList.CreateStructuredBuffer(sizeof(uint32), static_cast<uint32>(BlockDataCopy.Num() * sizeof(uint32)), BUF_ShaderResource | BUF_Static, Info);
			}

			FBufferRHIRef VoxelBuf;
			if (VoxelDataCopy.Num() > 0)
			{
				TResourceArray<uint32> Arr;
				Arr.Append(VoxelDataCopy);
				FRHIResourceCreateInfo Info(TEXT("VoxelMapVoxelData"), &Arr);
				VoxelBuf = RHICmdList.CreateStructuredBuffer(sizeof(uint32), static_cast<uint32>(VoxelDataCopy.Num() * sizeof(uint32)), BUF_ShaderResource | BUF_Static, Info);
			}

			ThisPtr->BlockDataBuffer = BlockBuf;
			ThisPtr->VoxelDataBuffer = VoxelBuf;
			ThisPtr->BlockDataSRV = BlockBuf.IsValid() ? RHICmdList.CreateShaderResourceView(BlockBuf.GetReference()) : nullptr;
			ThisPtr->VoxelDataSRV = VoxelBuf.IsValid() ? RHICmdList.CreateShaderResourceView(VoxelBuf.GetReference()) : nullptr;

			UE_LOG(LogTemp, Log, TEXT("[VoxelMap] GPU upload OK: %d blocks, %d voxels"), BlockCount, VoxelCount);
		});
}

void ADS2VoxelMapRenderer::ExportRenderTargetToCSV()
{
	if (!RenderTarget)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: no render target"));
		return;
	}

	FlushRenderingCommands();
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: render target resource is unavailable"));
		return;
	}

	TArray<FColor> Pixels;
	if (!RTResource->ReadPixels(Pixels))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Export failed: ReadPixels"));
		return;
	}

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;
	FString Csv;
	Csv.Reserve(static_cast<int64>(Pixels.Num()) * 16);
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		const FColor& Pixel = Pixels[Index];
		Csv += FString::Printf(TEXT("%d,%d,%d,%d,%d\n"), Index % Width, Index / Width, Pixel.R, Pixel.G, Pixel.B);
	}

	const FString Path = FPaths::ProjectDir() / TEXT("rt_dump.csv");
	if (FFileHelper::SaveStringToFile(Csv, *Path))
	{
		UE_LOG(LogTemp, Log, TEXT("[VoxelMap] Exported RT: %s (%dx%d)"), *Path, Width, Height);
	}
}
