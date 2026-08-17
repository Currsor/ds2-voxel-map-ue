#include "VoxelMapCollector.h"

#include "Camera/CameraTypes.h"
#include "Components/BoxComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "DS2VoxelMapRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/SlowTask.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "VoxelMapDataAsset.h"
#include "VoxelMapDataBuilder.h"
#include "VoxelMapTypes.h"
#include "VoxelMapWorldAsset.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#endif


struct FAsyncVoxelCaptureReadback
{
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	TArray<FLinearColor> Pixels;
	FThreadSafeBool bCopySubmitted = false;
	FThreadSafeBool bFinishQueued = false;
	FThreadSafeBool bCopyFinished = false;
};

namespace
{
	TArray<FVector> BuildHemisphereDirections(int32 ViewCount)

	{
		TArray<FVector> Directions;
		ViewCount = FMath::Max(1, ViewCount);
		Directions.Reserve(ViewCount);
		const double GoldenAngle = PI * (3.0 - FMath::Sqrt(5.0));
		for (int32 Index = 0; Index < ViewCount; ++Index)
		{
			const double Z = (static_cast<double>(Index) + 0.5) / static_cast<double>(ViewCount);
			const double Radius = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
			const double Azimuth = GoldenAngle * static_cast<double>(Index);
			Directions.Add(FVector(Radius * FMath::Cos(Azimuth), Radius * FMath::Sin(Azimuth), Z));
		}
		return Directions;
	}

	double GetAxisValue(const FVector& Vector, int32 Axis)
	{
		return Axis == 0 ? Vector.X : Axis == 1 ? Vector.Y : Vector.Z;
	}

	void SetAxisValue(FVector& Vector, int32 Axis, double Value)
	{
		if (Axis == 0) Vector.X = Value;
		else if (Axis == 1) Vector.Y = Value;
		else Vector.Z = Value;
	}

	FVector GetAxisVector(int32 Axis)
	{
		return Axis == 0 ? FVector::XAxisVector : Axis == 1 ? FVector::YAxisVector : FVector::ZAxisVector;
	}
}

AVoxelMapCollector::AVoxelMapCollector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	CaptureBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("CaptureBounds"));

	SetRootComponent(CaptureBounds);
	CaptureBounds->SetBoxExtent(FVector(1280.0, 1280.0, 640.0));
	CaptureBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureComponent"));
	CaptureComponent->SetupAttachment(CaptureBounds);
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->bAlwaysPersistRenderingState = false;
	CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;

#if WITH_EDITORONLY_DATA
	bIsEditorOnlyActor = true;
#endif
}

AVoxelMapCollector::~AVoxelMapCollector() = default;

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

FIntVector AVoxelMapCollector::CalculateCaptureGridSize() const
{
	const FBox Bounds = CaptureBounds->CalcBounds(CaptureBounds->GetComponentTransform()).GetBox();
	const FVector Size = Bounds.GetSize();
	auto ToAlignedVoxels = [this](double WorldSize)
	{
		const int32 VoxelCount = FMath::Max(1, FMath::CeilToInt(WorldSize / VoxelSize));
		return FMath::DivideAndRoundUp(VoxelCount, VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE;
	};
	return FIntVector(ToAlignedVoxels(Size.X), ToAlignedVoxels(Size.Z), ToAlignedVoxels(Size.Y));
}

FVector AVoxelMapCollector::CalculateCaptureWorldOrigin(const FIntVector& GridSize) const
{
	const FBox Bounds = CaptureBounds->CalcBounds(CaptureBounds->GetComponentTransform()).GetBox();
	const FVector AlignedWorldSize(
		static_cast<double>(GridSize.X) * VoxelSize,
		static_cast<double>(GridSize.Z) * VoxelSize,
		static_cast<double>(GridSize.Y) * VoxelSize);
	return Bounds.GetCenter() - AlignedWorldSize * 0.5;
}

void AVoxelMapCollector::BakeVoxelMap()
{
	BakeVoxelMapInternal(VoxelGridSize, TEXT("Standard"));
}

void AVoxelMapCollector::BakePerformanceTest()
{
	BakeVoxelMapInternal(PerformanceTestGridSize, TEXT("Performance Test"));
}

void AVoxelMapCollector::BakeSingleViewCapture()
{
	TArray<FVector> Directions;
	Directions.Add(SingleViewDirection.IsNearlyZero()
		? FVector(1.0, -1.0, 1.0).GetSafeNormal()
		: SingleViewDirection.GetSafeNormal());
	if (bUseAsyncGPUReadback)
	{
		StartAsyncCapture(Directions, false, TEXT("Single View"));
	}
	else
	{
		BakeWorldCapture(Directions, TEXT("Single View"));
	}
}

void AVoxelMapCollector::BakeHemisphereCapture()
{
	const TArray<FVector> Directions = BuildHemisphereDirections(HemisphereViewCount);
	if (bUseAsyncGPUReadback)
	{
		StartAsyncCapture(Directions, false, TEXT("Hemisphere"));
	}
	else
	{
		BakeWorldCapture(Directions, TEXT("Hemisphere"));
	}
}

void AVoxelMapCollector::BakeXYZSliceCapture()
{
	if (bUseAsyncGPUReadback)
	{
		StartAsyncCapture(TArray<FVector>(), true, TEXT("XYZ Slice"));
	}
	else
	{
		BakeCapturePasses(TArray<FVector>(), true, TEXT("XYZ Slice"));
	}
}

void AVoxelMapCollector::BakeCombinedWorldCapture()
{
	const TArray<FVector> Directions = BuildHemisphereDirections(HemisphereViewCount);
	if (bUseAsyncGPUReadback)
	{
		StartAsyncCapture(Directions, true, TEXT("Hemisphere + XYZ Slice"));
	}
	else
	{
		BakeCapturePasses(Directions, true, TEXT("Hemisphere + XYZ Slice"));
	}
}

void AVoxelMapCollector::CancelCapture()
{
	if (bCaptureInProgress)
	{
		bAsyncCancelRequested = true;
		CaptureStatus = TEXT("Cancel requested; waiting for current GPU readback");
	}
}

bool AVoxelMapCollector::ShouldTickIfViewportsOnly() const
{
	return bCaptureInProgress;
}

void AVoxelMapCollector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bCaptureInProgress)
	{
		AdvanceAsyncCapture();
	}
}

void AVoxelMapCollector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bAsyncCancelRequested = true;
	if (AsyncReadback)
	{
		FlushRenderingCommands();
		AsyncReadback.Reset();
	}
	AsyncProgressTask.Reset();
	bCaptureInProgress = false;
	Super::EndPlay(EndPlayReason);
}


void AVoxelMapCollector::BakeVoxelMapInternal(const FIntVector& GridSize, const TCHAR* BakeLabel)
{
	if (!OutputDataAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s bake failed: assign an OutputDataAsset first"), BakeLabel);
		return;
	}

	const FIntVector AlignedGrid(
		FMath::DivideAndRoundUp(FMath::Max(GridSize.X, VOXELMAP_BLOCK_SIZE), VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE,
		FMath::DivideAndRoundUp(FMath::Max(GridSize.Y, VOXELMAP_BLOCK_SIZE), VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE,
		FMath::DivideAndRoundUp(FMath::Max(GridSize.Z, VOXELMAP_BLOCK_SIZE), VOXELMAP_BLOCK_SIZE) * VOXELMAP_BLOCK_SIZE);

	FVoxelMapData NewData;
	const double GenerateStart = FPlatformTime::Seconds();
	FVoxelMapGenerator::Generate(BuildVoxelConfig(AlignedGrid), NewData);
	if (!FVoxelMapGenerator::Validate(NewData))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s bake failed: generated data is invalid"), BakeLabel);
		return;
	}

	const FVector WorldOrigin = CalculateWorldOrigin(NewData);
	const FVector WorldSize(
		static_cast<double>(AlignedGrid.X) * VoxelSize,
		static_cast<double>(AlignedGrid.Z) * VoxelSize,
		static_cast<double>(AlignedGrid.Y) * VoxelSize);
	CommitDataAsset(MoveTemp(NewData), WorldOrigin, AlignedGrid,
		FBox(WorldOrigin, WorldOrigin + WorldSize), 0, BakeLabel);
	UE_LOG(LogTemp, Log, TEXT("[VoxelMap] %s generation completed in %.3fs"),
		BakeLabel, FPlatformTime::Seconds() - GenerateStart);
}

bool AVoxelMapCollector::StartAsyncCapture(const TArray<FVector>& ViewDirections, bool bIncludeXYZSlices,
	const TCHAR* BakeLabel)
{
	if (bCaptureInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] A capture is already running"));
		return false;
	}
	if ((!bWritePartitionedWorldAsset && !OutputDataAsset)
		|| (bWritePartitionedWorldAsset && !OutputWorldAsset)
		|| !CaptureBounds || !CaptureComponent || (ViewDirections.IsEmpty() && !bIncludeXYZSlices)
		|| VoxelSize <= UE_SMALL_NUMBER || CaptureResolution < 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s async capture failed: invalid output, components, passes, or settings"), BakeLabel);
		return false;
	}

	AsyncBounds = CaptureBounds->CalcBounds(CaptureBounds->GetComponentTransform()).GetBox();
	AsyncGridSize = CalculateCaptureGridSize();
	AsyncWorldOrigin = CalculateCaptureWorldOrigin(AsyncGridSize);
	const int64 TotalBlocks = static_cast<int64>(AsyncGridSize.X / VOXELMAP_BLOCK_SIZE)
		* (AsyncGridSize.Y / VOXELMAP_BLOCK_SIZE) * (AsyncGridSize.Z / VOXELMAP_BLOCK_SIZE);
	if (TotalBlocks <= 0 || TotalBlocks > 0x00FFFFFFll)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s async capture failed: %lld addressable blocks"), BakeLabel, TotalBlocks);
		return false;
	}

	BuildCaptureJobs(ViewDirections, bIncludeXYZSlices, AsyncBounds, AsyncCaptureJobs);
	if (AsyncCaptureJobs.IsEmpty() || !EnsureCaptureTarget())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s async capture failed: no capture jobs or render target"), BakeLabel);
		return false;
	}

	AsyncCapturedVoxels.Reset();
	AsyncDepthPixels.Reset();
	AsyncReadback.Reset();
	AsyncBakeLabel = BakeLabel;
	AsyncJobIndex = 0;
	AsyncStartTime = FPlatformTime::Seconds();
	AsyncFusionSeconds = 0.0;
	QualityErrorSumCm = 0.0;
	QualityMaximumErrorCm = 0.0;
	QualityEvaluatedSampleCount = 0;
	bAsyncCancelRequested = false;
	bCaptureInProgress = true;
	CaptureProgress = 0.0f;
	CaptureStatus = FString::Printf(TEXT("%s: starting 0/%d"), BakeLabel, AsyncCaptureJobs.Num());
	AsyncCapturePhase = EAsyncCapturePhase::SubmitDepth;

	AsyncProgressTask = MakeUnique<FSlowTask>(static_cast<float>(AsyncCaptureJobs.Num()), FText::FromString(CaptureStatus));
	AsyncProgressTask->Initialize();
	AsyncProgressTask->MakeDialogDelayed(0.25f, true, false);
	UE_LOG(LogTemp, Log, TEXT("[VoxelMap] %s asynchronous capture started with %d views"), BakeLabel, AsyncCaptureJobs.Num());
	return true;
}

void AVoxelMapCollector::BuildCaptureJobs(const TArray<FVector>& ViewDirections, bool bIncludeXYZSlices,
	const FBox& Bounds, TArray<FCaptureJob>& OutJobs) const
{
	OutJobs.Reset();
	const FVector Center = Bounds.GetCenter();
	const double HalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(CaptureFieldOfView, 5.0f, 150.0f) * 0.5f);
	const double SphereRadius = Bounds.GetExtent().Size();
	const double CameraDistance = SphereRadius / FMath::Max(FMath::Sin(HalfFovRadians), 0.05) + VoxelSize * 2.0;
	for (const FVector& InputDirection : ViewDirections)
	{
		const FVector ViewDirection = InputDirection.GetSafeNormal();
		if (ViewDirection.IsNearlyZero())
		{
			continue;
		}
		FCaptureJob& Job = OutJobs.AddDefaulted_GetRef();
		Job.CameraPosition = Center + ViewDirection * CameraDistance;
		Job.CameraRotation = (Center - Job.CameraPosition).Rotation();
	}

	if (!bIncludeXYZSlices)
	{
		return;
	}

	const bool AxisEnabled[3] = { bCaptureSliceAxisX, bCaptureSliceAxisY, bCaptureSliceAxisZ };
	const double SlabThickness = FMath::Max(1, SliceThicknessInVoxels) * static_cast<double>(VoxelSize);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (!AxisEnabled[Axis])
		{
			continue;
		}
		const double AxisMin = GetAxisValue(Bounds.Min, Axis);
		const double AxisMax = GetAxisValue(Bounds.Max, Axis);
		const int32 SliceCount = FMath::CeilToInt((AxisMax - AxisMin) / SlabThickness);
		for (int32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
		{
			const double SlabMin = AxisMin + SliceIndex * SlabThickness;
			const double SlabMax = FMath::Min(SlabMin + SlabThickness, AxisMax);
			const int32 DirectionCount = bCaptureSlicesFromBothDirections ? 2 : 1;
			for (int32 DirectionIndex = 0; DirectionIndex < DirectionCount; ++DirectionIndex)
			{
				const int32 DirectionSign = DirectionIndex == 0 ? 1 : -1;
				FCaptureJob& Job = OutJobs.AddDefaulted_GetRef();
				Job.bOrthographic = true;
				Job.SlabAxis = Axis;
				Job.SlabMin = SlabMin;
				Job.SlabMax = SlabMax;
				Job.CameraPosition = Bounds.GetCenter();
				const double CameraOffset = FMath::Max(1.0, static_cast<double>(MinimumCaptureDepth)) + 1.0;
				SetAxisValue(Job.CameraPosition, Axis, DirectionSign > 0 ? SlabMin - CameraOffset : SlabMax + CameraOffset);
				Job.CameraRotation = (GetAxisVector(Axis) * static_cast<double>(DirectionSign)).Rotation();
				const FVector Size = Bounds.GetSize();
				Job.OrthoWidth = FMath::Max(Axis == 0 ? Size.Y : Size.X, Axis == 2 ? Size.Y : Size.Z) + VoxelSize * 2.0;
			}
		}
	}
}

bool AVoxelMapCollector::ConfigureCaptureJob(const FCaptureJob& Job, bool bCaptureDepth)
{
	if (!EnsureCaptureTarget())
	{
		return false;
	}
	CaptureComponent->TextureTarget = CaptureTarget;
	CaptureComponent->ProjectionType = Job.bOrthographic
		? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
	CaptureComponent->FOVAngle = CaptureFieldOfView;
	CaptureComponent->OrthoWidth = Job.OrthoWidth;
	CaptureComponent->bAutoCalculateOrthoPlanes = Job.bOrthographic;
	CaptureComponent->bUpdateOrthoPlanes = false;
	CaptureComponent->bOverride_CustomNearClippingPlane = Job.bOrthographic;
	CaptureComponent->CustomNearClippingPlane = FMath::Max(0.1f, MinimumCaptureDepth * 0.5f);
	CaptureComponent->bEnableClipPlane = false;
	CaptureComponent->SetWorldLocationAndRotation(Job.CameraPosition, Job.CameraRotation);
	CaptureComponent->CaptureSource = bCaptureDepth
		? ESceneCaptureSource::SCS_SceneDepth : ESceneCaptureSource::SCS_BaseColor;
	CaptureComponent->CaptureScene();
	return true;
}

bool AVoxelMapCollector::QueueAsyncReadback()
{
	if (!CaptureTarget || AsyncReadback)
	{
		return false;
	}
	FTextureRenderTargetResource* Resource = CaptureTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return false;
	}
	FTextureRHIRef TextureRHI = Resource->GetRenderTargetTexture();
	if (!TextureRHI.IsValid())
	{
		return false;
	}

	AsyncReadback = MakeShared<FAsyncVoxelCaptureReadback, ESPMode::ThreadSafe>();
	TSharedPtr<FAsyncVoxelCaptureReadback, ESPMode::ThreadSafe> State = AsyncReadback;
	const int32 Width = CaptureResolution;
	const int32 Height = CaptureResolution;
	ENQUEUE_RENDER_COMMAND(VoxelMapQueueTextureReadback)(
		[State, TextureRHI, Width, Height](FRHICommandListImmediate& RHICmdList)
		{
			State->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("VoxelMapCaptureReadback"));
			State->Readback->EnqueueCopy(RHICmdList, TextureRHI.GetReference(), FResolveRect(0, 0, Width, Height));
			State->bCopySubmitted = true;
		});
	return true;
}

bool AVoxelMapCollector::PollAsyncReadback(TArray<FLinearColor>& OutPixels)
{
	if (!AsyncReadback || !AsyncReadback->bCopySubmitted)
	{
		return false;
	}
	if (AsyncReadback->bCopyFinished)
	{
		OutPixels = MoveTemp(AsyncReadback->Pixels);
		AsyncReadback.Reset();
		return OutPixels.Num() == CaptureResolution * CaptureResolution;
	}
	if (AsyncReadback->bFinishQueued || !AsyncReadback->Readback || !AsyncReadback->Readback->IsReady())
	{
		return false;
	}

	TSharedPtr<FAsyncVoxelCaptureReadback, ESPMode::ThreadSafe> State = AsyncReadback;
	State->bFinishQueued = true;
	const int32 Width = CaptureResolution;
	const int32 Height = CaptureResolution;
	ENQUEUE_RENDER_COMMAND(VoxelMapFinishTextureReadback)(
		[State, Width, Height](FRHICommandListImmediate& RHICmdList)
		{
			int32 RowPitchInPixels = 0;
			int32 BufferHeight = 0;
			void* SourceData = State->Readback->Lock(RowPitchInPixels, &BufferHeight);
			if (SourceData && RowPitchInPixels >= Width && BufferHeight >= Height)
			{
				State->Pixels.SetNumUninitialized(Width * Height);
				const FLinearColor* Source = static_cast<const FLinearColor*>(SourceData);
				FLinearColor* Destination = State->Pixels.GetData();
				for (int32 Y = 0; Y < Height; ++Y)
				{
					FMemory::Memcpy(Destination + Y * Width, Source + Y * RowPitchInPixels,
						static_cast<SIZE_T>(Width) * sizeof(FLinearColor));
				}
			}
			State->Readback->Unlock();
			State->Readback.Reset();
			State->bCopyFinished = true;
		});
	return false;
}

void AVoxelMapCollector::AdvanceAsyncCapture()
{
	if (AsyncProgressTask)
	{
		AsyncProgressTask->TickProgress();
		if (AsyncProgressTask->ShouldCancel())
		{
			bAsyncCancelRequested = true;
		}
	}
	if (bAsyncCancelRequested && !AsyncReadback)
	{
		FinishAsyncCapture(false, TEXT("cancelled"));
		return;
	}
	if (!AsyncCaptureJobs.IsValidIndex(AsyncJobIndex) && AsyncCapturePhase != EAsyncCapturePhase::Finalize)
	{
		AsyncCapturePhase = EAsyncCapturePhase::Finalize;
	}

	const FCaptureJob* Job = AsyncCaptureJobs.IsValidIndex(AsyncJobIndex) ? &AsyncCaptureJobs[AsyncJobIndex] : nullptr;
	switch (AsyncCapturePhase)
	{
	case EAsyncCapturePhase::SubmitDepth:
		if (!Job || !ConfigureCaptureJob(*Job, true) || !QueueAsyncReadback())
		{
			FinishAsyncCapture(false, TEXT("failed to submit depth readback"));
			return;
		}
		CaptureStatus = FString::Printf(TEXT("%s: depth %d/%d"), *AsyncBakeLabel, AsyncJobIndex + 1, AsyncCaptureJobs.Num());
		AsyncCapturePhase = EAsyncCapturePhase::WaitDepth;
		break;

	case EAsyncCapturePhase::WaitDepth:
		if (PollAsyncReadback(AsyncDepthPixels))
		{
			if (bAsyncCancelRequested)
			{
				FinishAsyncCapture(false, TEXT("cancelled"));
				return;
			}
			AsyncCapturePhase = EAsyncCapturePhase::SubmitColor;
		}
		break;

	case EAsyncCapturePhase::SubmitColor:
		if (!Job || !ConfigureCaptureJob(*Job, false) || !QueueAsyncReadback())
		{
			FinishAsyncCapture(false, TEXT("failed to submit color readback"));
			return;
		}
		CaptureStatus = FString::Printf(TEXT("%s: color %d/%d"), *AsyncBakeLabel, AsyncJobIndex + 1, AsyncCaptureJobs.Num());
		AsyncCapturePhase = EAsyncCapturePhase::WaitColor;
		break;

	case EAsyncCapturePhase::WaitColor:
		{
			TArray<FLinearColor> ColorPixels;
			if (!PollAsyncReadback(ColorPixels))
			{
				break;
			}
			if (bAsyncCancelRequested || !Job)
			{
				FinishAsyncCapture(false, TEXT("cancelled"));
				return;
			}
			if (!AccumulateCapturePixels(AsyncDepthPixels, ColorPixels, Job->CameraPosition, Job->CameraRotation,
				Job->bOrthographic, Job->OrthoWidth, AsyncBounds, AsyncWorldOrigin, AsyncGridSize,
				AsyncCapturedVoxels, Job->SlabAxis, Job->SlabMin, Job->SlabMax))
			{
				FinishAsyncCapture(false, TEXT("pixel accumulation failed"));
				return;
			}
			++AsyncJobIndex;
			CaptureProgress = static_cast<float>(AsyncJobIndex) / static_cast<float>(AsyncCaptureJobs.Num());
			CaptureStatus = FString::Printf(TEXT("%s: completed %d/%d, voxels=%d"),
				*AsyncBakeLabel, AsyncJobIndex, AsyncCaptureJobs.Num(), AsyncCapturedVoxels.Num());
			if (AsyncProgressTask)
			{
				AsyncProgressTask->EnterProgressFrame(1.0f, FText::FromString(CaptureStatus));
			}
			AsyncDepthPixels.Reset();
			AsyncCapturePhase = AsyncJobIndex < AsyncCaptureJobs.Num()
				? EAsyncCapturePhase::SubmitDepth : EAsyncCapturePhase::Finalize;
		}
		break;

	case EAsyncCapturePhase::Finalize:
		{
			const FString BakeLabel = AsyncBakeLabel;
			const bool bSucceeded = FinalizeCapturedVoxels(AsyncCapturedVoxels, AsyncBounds, AsyncWorldOrigin,
				AsyncGridSize, AsyncCaptureJobs.Num(), *BakeLabel, AsyncStartTime);
			FinishAsyncCapture(bSucceeded, bSucceeded ? TEXT("completed") : TEXT("finalization failed"));
		}
		break;

	default:
		break;
	}
}

void AVoxelMapCollector::FinishAsyncCapture(bool bSucceeded, const TCHAR* Reason)
{
	if (AsyncProgressTask)
	{
		AsyncProgressTask->Destroy();
		AsyncProgressTask.Reset();
	}
	bCaptureInProgress = false;
	CaptureProgress = bSucceeded ? 1.0f : CaptureProgress;
	CaptureStatus = FString::Printf(TEXT("%s: %s"), *AsyncBakeLabel, Reason);
	AsyncCapturePhase = EAsyncCapturePhase::Idle;
	AsyncCaptureJobs.Reset();
	AsyncCapturedVoxels.Reset();
	AsyncDepthPixels.Reset();
	AsyncReadback.Reset();
	bAsyncCancelRequested = false;
	if (bSucceeded)
	{
		UE_LOG(LogTemp, Log, TEXT("[VoxelMap] %s"), *CaptureStatus);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s"), *CaptureStatus);
	}
}

bool AVoxelMapCollector::BakeWorldCapture(const TArray<FVector>& ViewDirections, const TCHAR* BakeLabel)
{
	return BakeCapturePasses(ViewDirections, false, BakeLabel);
}

bool AVoxelMapCollector::BakeCapturePasses(const TArray<FVector>& ViewDirections, bool bIncludeXYZSlices, const TCHAR* BakeLabel)

{
	if ((!bWritePartitionedWorldAsset && !OutputDataAsset)
		|| (bWritePartitionedWorldAsset && !OutputWorldAsset)
		|| !CaptureBounds || !CaptureComponent || (ViewDirections.IsEmpty() && !bIncludeXYZSlices))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s capture failed: missing selected output asset, capture components, or passes"), BakeLabel);
		return false;
	}

	if (VoxelSize <= UE_SMALL_NUMBER || CaptureResolution < 64)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s capture failed: invalid voxel size or capture resolution"), BakeLabel);
		return false;
	}

	const FBox Bounds = CaptureBounds->CalcBounds(CaptureBounds->GetComponentTransform()).GetBox();
	const FIntVector GridSize = CalculateCaptureGridSize();
	const FVector WorldOrigin = CalculateCaptureWorldOrigin(GridSize);
	const int64 TotalBlocks = static_cast<int64>(GridSize.X / VOXELMAP_BLOCK_SIZE)
		* (GridSize.Y / VOXELMAP_BLOCK_SIZE) * (GridSize.Z / VOXELMAP_BLOCK_SIZE);
	if (TotalBlocks > 0x00FFFFFFll)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s capture failed: %lld blocks exceed the 24-bit shader limit"), BakeLabel, TotalBlocks);
		return false;
	}

	TMap<int32, FCaptureAccumulator> CapturedVoxels;
	int32 CaptureViews = 0;
	AsyncFusionSeconds = 0.0;
	QualityErrorSumCm = 0.0;
	QualityMaximumErrorCm = 0.0;
	QualityEvaluatedSampleCount = 0;
	const double StartTime = FPlatformTime::Seconds();

	for (int32 ViewIndex = 0; ViewIndex < ViewDirections.Num(); ++ViewIndex)
	{
		if (!CaptureView(ViewDirections[ViewIndex], Bounds, WorldOrigin, GridSize, CapturedVoxels))
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s failed at hemisphere view %d/%d"), BakeLabel, ViewIndex + 1, ViewDirections.Num());
			return false;
		}
		++CaptureViews;
		UE_LOG(LogTemp, Log, TEXT("[VoxelMap] %s view %d/%d captured, unique voxels=%d"),
			BakeLabel, ViewIndex + 1, ViewDirections.Num(), CapturedVoxels.Num());
	}

	if (bIncludeXYZSlices && !CaptureXYZSlices(Bounds, WorldOrigin, GridSize, CapturedVoxels, CaptureViews))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s failed during XYZ Slice pass"), BakeLabel);
		return false;
	}

	return FinalizeCapturedVoxels(CapturedVoxels, Bounds, WorldOrigin, GridSize, CaptureViews, BakeLabel, StartTime);
}

bool AVoxelMapCollector::FinalizeCapturedVoxels(TMap<int32, FCaptureAccumulator>& CapturedVoxels,
	const FBox& Bounds, const FVector& WorldOrigin, const FIntVector& GridSize, int32 CaptureViews,
	const TCHAR* BakeLabel, double StartTime)
{
	const int32 CandidateVoxelCount = CapturedVoxels.Num();
	const FString PreviousHash = bWritePartitionedWorldAsset && OutputWorldAsset
		? OutputWorldAsset->LastBakeReport.DataHash
		: OutputDataAsset ? OutputDataAsset->LastBakeReport.DataHash : FString();
	const double FilterStartTime = FPlatformTime::Seconds();
	int32 RejectedByConfidence = 0;
	int32 RemovedIsolated = 0;
	int32 FilledHoles = 0;
	FilterCapturedVoxels(CapturedVoxels, GridSize, RejectedByConfidence, RemovedIsolated, FilledHoles);
	const double FilterSeconds = FPlatformTime::Seconds() - FilterStartTime;
	UE_LOG(LogTemp, Log,
		TEXT("[VoxelMap] %s M4 filter: rejected=%d, isolated=%d, holes filled=%d, remaining=%d"),
		BakeLabel, RejectedByConfidence, RemovedIsolated, FilledHoles, CapturedVoxels.Num());

	TArray<FVoxelMapSourceVoxel> SourceVoxels;
	SourceVoxels.Reserve(CapturedVoxels.Num());
	for (const TPair<int32, FCaptureAccumulator>& Pair : CapturedVoxels)
	{
		const FLinearColor Average = Pair.Value.WeightedLinearColorSum
			/ FMath::Max(Pair.Value.TotalWeight, UE_SMALL_NUMBER);
		const FColor Color = Average.GetClamped().ToFColorSRGB();
		FVoxelMapSourceVoxel& SourceVoxel = SourceVoxels.AddDefaulted_GetRef();
		SourceVoxel.Coordinate = FromLinearIndex(Pair.Key, GridSize);
		SourceVoxel.PackedData = VoxelMapBits::PackColor(Color.R, Color.G, Color.B, 0);
	}

	const double BuildAndCommitStartTime = FPlatformTime::Seconds();
	if (bWritePartitionedWorldAsset)
	{
		if (!CommitPartitionedWorldAsset(SourceVoxels, WorldOrigin, GridSize, Bounds, CaptureViews, BakeLabel))
		{
			return false;
		}
	}
	else
	{
		FVoxelMapData NewData;
		if (!FVoxelMapDataBuilder::Build(GridSize, SourceVoxels, NewData) || !FVoxelMapGenerator::Validate(NewData))
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s capture failed: packed data is invalid"), BakeLabel);
			return false;
		}
		CommitDataAsset(MoveTemp(NewData), WorldOrigin, GridSize, Bounds, CaptureViews, BakeLabel);
	}
	const double BuildAndCommitSeconds = FPlatformTime::Seconds() - BuildAndCommitStartTime;
	CompleteM7Report(BakeLabel, GridSize, CaptureViews, CandidateVoxelCount, RejectedByConfidence,
		RemovedIsolated, FilledHoles, StartTime, FilterSeconds, BuildAndCommitSeconds, PreviousHash);

	UE_LOG(LogTemp, Log, TEXT("[VoxelMap] %s capture completed in %.3fs with %d views"),
		BakeLabel, FPlatformTime::Seconds() - StartTime, CaptureViews);
	return true;
}

FString AVoxelMapCollector::ComputeDataHash(const FVoxelMapData& Data, const FVector& WorldOrigin,
	const FIntVector& GridSize) const
{
	FSHA1 Hasher;
	auto Update = [&Hasher](const void* DataPointer, SIZE_T DataSize)
	{
		const uint8* Bytes = static_cast<const uint8*>(DataPointer);
		while (DataSize > 0)
		{
			const uint32 ChunkSize = static_cast<uint32>(FMath::Min<SIZE_T>(DataSize, MAX_uint32));
			Hasher.Update(Bytes, ChunkSize);
			Bytes += ChunkSize;
			DataSize -= ChunkSize;
		}
	};
	Update(&GridSize, sizeof(GridSize));
	Update(&Data.BlockGridSize, sizeof(Data.BlockGridSize));
	Update(&WorldOrigin, sizeof(WorldOrigin));
	Update(&VoxelSize, sizeof(VoxelSize));
	if (!Data.BlockData.IsEmpty())
	{
		Update(Data.BlockData.GetData(), Data.BlockData.Num() * sizeof(uint32));
	}
	if (!Data.VoxelData.IsEmpty())
	{
		Update(Data.VoxelData.GetData(), Data.VoxelData.Num() * sizeof(uint32));
	}
	Hasher.Final();
	uint8 Digest[20];
	Hasher.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
}

FString AVoxelMapCollector::ComputeWorldDataHash() const
{
	if (!OutputWorldAsset)
	{
		return FString();
	}
	FSHA1 Hasher;
	auto Update = [&Hasher](const void* DataPointer, SIZE_T DataSize)
	{
		const uint8* Bytes = static_cast<const uint8*>(DataPointer);
		while (DataSize > 0)
		{
			const uint32 ChunkSize = static_cast<uint32>(FMath::Min<SIZE_T>(DataSize, MAX_uint32));
			Hasher.Update(Bytes, ChunkSize);
			Bytes += ChunkSize;
			DataSize -= ChunkSize;
		}
	};
	Update(&OutputWorldAsset->VoxelGridSize, sizeof(OutputWorldAsset->VoxelGridSize));
	Update(&OutputWorldAsset->RegionSizeInVoxels, sizeof(OutputWorldAsset->RegionSizeInVoxels));
	Update(&OutputWorldAsset->WorldOrigin, sizeof(OutputWorldAsset->WorldOrigin));
	Update(&OutputWorldAsset->VoxelSize, sizeof(OutputWorldAsset->VoxelSize));
	for (const FVoxelMapRegionReference& Reference : OutputWorldAsset->Regions)
	{
		Update(&Reference.RegionCoordinate, sizeof(Reference.RegionCoordinate));
		if (const UVoxelMapDataAsset* RegionAsset = Reference.Asset)
		{
			const FString RegionHash = ComputeDataHash(
				RegionAsset->Data, RegionAsset->WorldOrigin, RegionAsset->VoxelGridSize);
			FTCHARToUTF8 Utf8Hash(*RegionHash);
			Update(Utf8Hash.Get(), Utf8Hash.Length());
		}
	}
	Hasher.Final();
	uint8 Digest[20];
	Hasher.GetHash(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
}

void AVoxelMapCollector::CompleteM7Report(const TCHAR* BakeLabel, const FIntVector& GridSize,
	int32 CaptureViews, int32 CandidateVoxelCount, int32 RejectedByConfidence, int32 RemovedIsolated,
	int32 FilledHoles, double StartTime, double FilterSeconds, double BuildAndCommitSeconds,
	const FString& PreviousHash)
{
	FVoxelMapBakeReport Report;
	Report.BakeLabel = BakeLabel;
	Report.GeneratedAtUtc = FDateTime::UtcNow().ToIso8601();
	Report.TotalSeconds = FPlatformTime::Seconds() - StartTime;
	Report.FusionSeconds = AsyncFusionSeconds;
	Report.FilterSeconds = FilterSeconds;
	Report.BuildAndCommitSeconds = BuildAndCommitSeconds;
	Report.CaptureAndReadbackSeconds = FMath::Max(0.0,
		Report.TotalSeconds - Report.FusionSeconds - Report.FilterSeconds - Report.BuildAndCommitSeconds);
	Report.VoxelGridSize = GridSize;
	Report.CaptureViewCount = CaptureViews;
	Report.CandidateVoxelCount = CandidateVoxelCount;
	Report.RejectedByConfidence = RejectedByConfidence;
	Report.RemovedIsolated = RemovedIsolated;
	Report.FilledHoles = FilledHoles;
	Report.PreviousDataHash = PreviousHash;
	Report.bComparedWithPreviousBake = !PreviousHash.IsEmpty();

	if (bWritePartitionedWorldAsset && OutputWorldAsset)
	{
		Report.RegionCount = OutputWorldAsset->Regions.Num();
		Report.DataHash = ComputeWorldDataHash();
		Report.bDataValid = Report.RegionCount > 0;
		for (const FVoxelMapRegionReference& Reference : OutputWorldAsset->Regions)
		{
			if (!Reference.Asset || !FVoxelMapGenerator::Validate(Reference.Asset->Data))
			{
				Report.bDataValid = false;
				continue;
			}
			Report.NonEmptyBlockCount += Reference.Asset->Data.BlockCount;
			Report.OccupiedVoxelCount += Reference.Asset->Data.OccupiedVoxelCount;
			Report.PackedDataBytes += static_cast<int64>(Reference.Asset->Data.BlockData.Num()
				+ Reference.Asset->Data.VoxelData.Num()) * sizeof(uint32);
		}
	}
	else if (OutputDataAsset)
	{
		Report.DataHash = ComputeDataHash(OutputDataAsset->Data,
			OutputDataAsset->WorldOrigin, OutputDataAsset->VoxelGridSize);
		Report.bDataValid = FVoxelMapGenerator::Validate(OutputDataAsset->Data);
		Report.NonEmptyBlockCount = OutputDataAsset->Data.BlockCount;
		Report.OccupiedVoxelCount = OutputDataAsset->Data.OccupiedVoxelCount;
		Report.PackedDataBytes = static_cast<int64>(OutputDataAsset->Data.BlockData.Num()
			+ OutputDataAsset->Data.VoxelData.Num()) * sizeof(uint32);
	}

	Report.PackedDataMiB = static_cast<double>(Report.PackedDataBytes) / (1024.0 * 1024.0);
	Report.ObservedCandidateRetentionPercent = CandidateVoxelCount > 0
		? static_cast<double>(Report.OccupiedVoxelCount) * 100.0 / static_cast<double>(CandidateVoxelCount) : 0.0;
	Report.EvaluatedSampleCount = QualityEvaluatedSampleCount;
	Report.MeanQuantizationErrorCm = QualityEvaluatedSampleCount > 0
		? QualityErrorSumCm / static_cast<double>(QualityEvaluatedSampleCount) : 0.0;
	Report.MaximumQuantizationErrorCm = QualityMaximumErrorCm;
	Report.MeanQuantizationErrorInVoxels = VoxelSize > UE_SMALL_NUMBER
		? Report.MeanQuantizationErrorCm / VoxelSize : 0.0;
	Report.bDeterministicHashMatch = Report.bComparedWithPreviousBake
		&& Report.DataHash.Equals(PreviousHash, ESearchCase::CaseSensitive);
	Report.bCoveragePass = Report.ObservedCandidateRetentionPercent >= MinimumObservedCoveragePercent;
	Report.bPositionErrorPass = QualityEvaluatedSampleCount > 0
		&& Report.MeanQuantizationErrorInVoxels <= MaximumMeanQuantizationErrorInVoxels;
	Report.bOverallPass = Report.bDataValid && Report.bCoveragePass && Report.bPositionErrorPass
		&& (!Report.bComparedWithPreviousBake || Report.bDeterministicHashMatch);

	if (bWriteM7ReportFile)
	{
		WriteM7ReportFile(Report);
	}
	LastBakeReport = Report;
	if (bWritePartitionedWorldAsset && OutputWorldAsset)
	{
		OutputWorldAsset->LastBakeReport = Report;
		OutputWorldAsset->MarkPackageDirty();
	}
	else if (OutputDataAsset)
	{
		OutputDataAsset->LastBakeReport = Report;
		OutputDataAsset->MarkPackageDirty();
	}

	UE_LOG(LogTemp, Log,
		TEXT("[VoxelMap][M7] pass=%s hash=%s repeat=%s coverage=%.2f%% meanError=%.3f voxels data=%.2f MiB total=%.3fs capture=%.3fs fusion=%.3fs filter=%.3fs build=%.3fs report=%s"),
		Report.bOverallPass ? TEXT("true") : TEXT("false"), *Report.DataHash,
		Report.bComparedWithPreviousBake ? (Report.bDeterministicHashMatch ? TEXT("match") : TEXT("mismatch")) : TEXT("no-baseline"),
		Report.ObservedCandidateRetentionPercent, Report.MeanQuantizationErrorInVoxels,
		Report.PackedDataMiB, Report.TotalSeconds, Report.CaptureAndReadbackSeconds,
		Report.FusionSeconds, Report.FilterSeconds, Report.BuildAndCommitSeconds, *Report.ReportFilePath);
}

bool AVoxelMapCollector::WriteM7ReportFile(FVoxelMapBakeReport& Report) const
{
	FString SafeLabel = Report.BakeLabel;
	for (TCHAR& Character : SafeLabel)
	{
		if (!FChar::IsAlnum(Character))
		{
			Character = TEXT('_');
		}
	}
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VoxelMapReports"));
	IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ"));
	Report.ReportFilePath = FPaths::Combine(ReportDirectory,
		FString::Printf(TEXT("%s_%s.json"), *SafeLabel, *Timestamp));
	const FString Json = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schemaVersion\": 1,\n")
		TEXT("  \"bakeLabel\": \"%s\",\n")
		TEXT("  \"generatedAtUtc\": \"%s\",\n")
		TEXT("  \"dataHash\": \"%s\",\n")
		TEXT("  \"previousDataHash\": \"%s\",\n")
		TEXT("  \"comparedWithPreviousBake\": %s,\n")
		TEXT("  \"deterministicHashMatch\": %s,\n")
		TEXT("  \"overallPass\": %s,\n")
		TEXT("  \"timingSeconds\": {\"total\": %.6f, \"captureAndReadback\": %.6f, \"fusion\": %.6f, \"filter\": %.6f, \"buildAndCommit\": %.6f},\n")
		TEXT("  \"gridSize\": [%d, %d, %d],\n")
		TEXT("  \"captureViewCount\": %d,\n")
		TEXT("  \"candidateVoxelCount\": %d,\n")
		TEXT("  \"occupiedVoxelCount\": %d,\n")
		TEXT("  \"nonEmptyBlockCount\": %d,\n")
		TEXT("  \"regionCount\": %d,\n")
		TEXT("  \"packedDataBytes\": %lld,\n")
		TEXT("  \"packedDataMiB\": %.6f,\n")
		TEXT("  \"quality\": {\"observedCandidateRetentionPercent\": %.6f, \"meanQuantizationErrorCm\": %.6f, \"maximumQuantizationErrorCm\": %.6f, \"meanQuantizationErrorInVoxels\": %.6f, \"evaluatedSampleCount\": %lld},\n")
		TEXT("  \"filtering\": {\"rejectedByConfidence\": %d, \"removedIsolated\": %d, \"filledHoles\": %d},\n")
		TEXT("  \"validation\": {\"dataValid\": %s, \"coveragePass\": %s, \"positionErrorPass\": %s},\n")
		TEXT("  \"coverageMetricNote\": \"Observed candidate retention; not full-scene ground-truth geometry coverage.\"\n")
		TEXT("}\n"),
		*Report.BakeLabel.ReplaceCharWithEscapedChar(), *Report.GeneratedAtUtc, *Report.DataHash,
		*Report.PreviousDataHash, Report.bComparedWithPreviousBake ? TEXT("true") : TEXT("false"),
		Report.bDeterministicHashMatch ? TEXT("true") : TEXT("false"), Report.bOverallPass ? TEXT("true") : TEXT("false"),
		Report.TotalSeconds, Report.CaptureAndReadbackSeconds, Report.FusionSeconds, Report.FilterSeconds,
		Report.BuildAndCommitSeconds, Report.VoxelGridSize.X, Report.VoxelGridSize.Y, Report.VoxelGridSize.Z,
		Report.CaptureViewCount, Report.CandidateVoxelCount, Report.OccupiedVoxelCount,
		Report.NonEmptyBlockCount, Report.RegionCount, Report.PackedDataBytes, Report.PackedDataMiB,
		Report.ObservedCandidateRetentionPercent, Report.MeanQuantizationErrorCm,
		Report.MaximumQuantizationErrorCm, Report.MeanQuantizationErrorInVoxels, Report.EvaluatedSampleCount,
		Report.RejectedByConfidence, Report.RemovedIsolated, Report.FilledHoles,
		Report.bDataValid ? TEXT("true") : TEXT("false"), Report.bCoveragePass ? TEXT("true") : TEXT("false"),
		Report.bPositionErrorPass ? TEXT("true") : TEXT("false"));
	return FFileHelper::SaveStringToFile(Json, *Report.ReportFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool AVoxelMapCollector::EnsureCaptureTarget()

{
	if (CaptureTarget && CaptureTarget->SizeX == CaptureResolution && CaptureTarget->SizeY == CaptureResolution)
	{
		return true;
	}

	CaptureTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
	CaptureTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA32f;
	CaptureTarget->ClearColor = FLinearColor::Black;
#if WITH_EDITORONLY_DATA
	CaptureTarget->AssetImportData = nullptr;
#endif
	CaptureTarget->InitAutoFormat(CaptureResolution, CaptureResolution);
	CaptureTarget->UpdateResourceImmediate(true);
	return CaptureTarget->GetResource() != nullptr;
}

bool AVoxelMapCollector::CaptureView(const FVector& InViewDirection, const FBox& Bounds, const FVector& WorldOrigin,
	const FIntVector& GridSize, TMap<int32, FCaptureAccumulator>& InOutVoxels)
{
	const FVector ViewDirection = InViewDirection.GetSafeNormal();
	if (ViewDirection.IsNearlyZero() || !EnsureCaptureTarget())
	{
		return false;
	}

	const FVector Center = Bounds.GetCenter();
	const double HalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(CaptureFieldOfView, 5.0f, 150.0f) * 0.5f);
	const double SphereRadius = Bounds.GetExtent().Size();
	const double CameraDistance = SphereRadius / FMath::Max(FMath::Sin(HalfFovRadians), 0.05) + VoxelSize * 2.0;
	const FVector CameraPosition = Center + ViewDirection * CameraDistance;
	const FRotator CameraRotation = (Center - CameraPosition).Rotation();

	CaptureComponent->TextureTarget = CaptureTarget;
	CaptureComponent->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComponent->FOVAngle = CaptureFieldOfView;
	CaptureComponent->bOverride_CustomNearClippingPlane = false;
	CaptureComponent->bEnableClipPlane = false;
	CaptureComponent->SetWorldLocationAndRotation(CameraPosition, CameraRotation);
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	CaptureComponent->CaptureScene();

	TArray<FLinearColor> DepthPixels;
	TArray<FLinearColor> ColorPixels;
	return ReadCapturePixels(DepthPixels, ColorPixels)
		&& AccumulateCapturePixels(DepthPixels, ColorPixels, CameraPosition, CameraRotation, false, 0.0,
			Bounds, WorldOrigin, GridSize, InOutVoxels);
}

bool AVoxelMapCollector::CaptureXYZSlices(const FBox& Bounds, const FVector& WorldOrigin, const FIntVector& GridSize,
	TMap<int32, FCaptureAccumulator>& InOutVoxels, int32& InOutCaptureViews)
{
	const bool AxisEnabled[3] = { bCaptureSliceAxisX, bCaptureSliceAxisY, bCaptureSliceAxisZ };
	const double SlabThickness = FMath::Max(1, SliceThicknessInVoxels) * static_cast<double>(VoxelSize);
	int32 TotalSlices = 0;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (AxisEnabled[Axis])
		{
			TotalSlices += FMath::CeilToInt((GetAxisValue(Bounds.Max, Axis) - GetAxisValue(Bounds.Min, Axis)) / SlabThickness);
		}
	}
	const int32 DirectionCount = bCaptureSlicesFromBothDirections ? 2 : 1;
	int32 CompletedViews = 0;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (!AxisEnabled[Axis])
		{
			continue;
		}

		const double AxisMin = GetAxisValue(Bounds.Min, Axis);
		const double AxisMax = GetAxisValue(Bounds.Max, Axis);
		const int32 SliceCount = FMath::CeilToInt((AxisMax - AxisMin) / SlabThickness);
		for (int32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
		{
			const double SlabMin = AxisMin + SliceIndex * SlabThickness;
			const double SlabMax = FMath::Min(SlabMin + SlabThickness, AxisMax);
			if (!CaptureSliceView(Axis, 1, SlabMin, SlabMax, Bounds, WorldOrigin, GridSize, InOutVoxels))
			{
				return false;
			}
			++CompletedViews;
			++InOutCaptureViews;

			if (bCaptureSlicesFromBothDirections)
			{
				if (!CaptureSliceView(Axis, -1, SlabMin, SlabMax, Bounds, WorldOrigin, GridSize, InOutVoxels))
				{
					return false;
				}
				++CompletedViews;
				++InOutCaptureViews;
			}

			UE_LOG(LogTemp, Log, TEXT("[VoxelMap] XYZ Slice %d/%d views captured, unique voxels=%d"),
				CompletedViews, TotalSlices * DirectionCount, InOutVoxels.Num());
		}
	}
	return CompletedViews > 0;
}

bool AVoxelMapCollector::CaptureSliceView(int32 WorldAxis, int32 DirectionSign, double SlabMin, double SlabMax,
	const FBox& Bounds, const FVector& WorldOrigin, const FIntVector& GridSize,
	TMap<int32, FCaptureAccumulator>& InOutVoxels)
{
	if (!EnsureCaptureTarget())
	{
		return false;
	}

	const FVector Direction = GetAxisVector(WorldAxis) * static_cast<double>(DirectionSign);
	const double CameraOffset = FMath::Max(1.0, static_cast<double>(MinimumCaptureDepth)) + 1.0;
	FVector CameraPosition = Bounds.GetCenter();
	SetAxisValue(CameraPosition, WorldAxis, DirectionSign > 0 ? SlabMin - CameraOffset : SlabMax + CameraOffset);
	const FRotator CameraRotation = Direction.Rotation();
	const FVector Size = Bounds.GetSize();
	const double OrthoWidth = FMath::Max(
		WorldAxis == 0 ? Size.Y : Size.X,
		WorldAxis == 2 ? Size.Y : Size.Z) + VoxelSize * 2.0;

	CaptureComponent->TextureTarget = CaptureTarget;
	CaptureComponent->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureComponent->OrthoWidth = OrthoWidth;
	CaptureComponent->bAutoCalculateOrthoPlanes = true;
	CaptureComponent->bUpdateOrthoPlanes = false;
	CaptureComponent->bOverride_CustomNearClippingPlane = true;

	CaptureComponent->CustomNearClippingPlane = FMath::Max(0.1f, MinimumCaptureDepth * 0.5f);
	CaptureComponent->bEnableClipPlane = false;
	CaptureComponent->SetWorldLocationAndRotation(CameraPosition, CameraRotation);
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	CaptureComponent->CaptureScene();

	TArray<FLinearColor> DepthPixels;
	TArray<FLinearColor> ColorPixels;
	return ReadCapturePixels(DepthPixels, ColorPixels)
		&& AccumulateCapturePixels(DepthPixels, ColorPixels, CameraPosition, CameraRotation, true, OrthoWidth,
			Bounds, WorldOrigin, GridSize, InOutVoxels, WorldAxis, SlabMin, SlabMax);
}

bool AVoxelMapCollector::AccumulateCapturePixels(const TArray<FLinearColor>& DepthPixels,
	const TArray<FLinearColor>& ColorPixels, const FVector& CameraPosition, const FRotator& CameraRotation,
	bool bOrthographic, double OrthoWidth, const FBox& Bounds, const FVector& WorldOrigin,
	const FIntVector& GridSize, TMap<int32, FCaptureAccumulator>& InOutVoxels,
	int32 SlabAxis, double SlabMin, double SlabMax)
{
	if (DepthPixels.Num() != CaptureResolution * CaptureResolution || ColorPixels.Num() != DepthPixels.Num())
	{
		return false;
	}

	const double FusionStartTime = FPlatformTime::Seconds();
	const FVector Forward = CameraRotation.Vector();
	const FVector Right = FRotationMatrix(CameraRotation).GetScaledAxis(EAxis::Y);
	const FVector Up = FRotationMatrix(CameraRotation).GetScaledAxis(EAxis::Z);
	const double TanHalfFov = FMath::Tan(
		FMath::DegreesToRadians(FMath::Clamp(CaptureFieldOfView, 5.0f, 150.0f) * 0.5f));
	const double MaximumDepth = Bounds.GetExtent().Size() * 8.0 + FMath::Max(OrthoWidth, 0.0);
	const double DepthThreshold = FMath::Max(0.1f, DepthDiscontinuityThresholdInVoxels) * VoxelSize;

	auto ReconstructWorldPosition = [&](int32 PixelX, int32 PixelY, FVector& OutPosition, FVector& OutViewDirection)
	{
		if (PixelX < 0 || PixelY < 0 || PixelX >= CaptureResolution || PixelY >= CaptureResolution)
		{
			return false;
		}
		const float SceneDepth = DepthPixels[PixelX + PixelY * CaptureResolution].R;
		if (!FMath::IsFinite(SceneDepth) || SceneDepth < MinimumCaptureDepth || SceneDepth > MaximumDepth)
		{
			return false;
		}

		const double NormalizedX = (static_cast<double>(PixelX) + 0.5) / CaptureResolution * 2.0 - 1.0;
		const double NormalizedY = 1.0 - (static_cast<double>(PixelY) + 0.5) / CaptureResolution * 2.0;
		if (bOrthographic)
		{
			OutViewDirection = Forward;
			OutPosition = CameraPosition + Forward * SceneDepth
				+ Right * (NormalizedX * OrthoWidth * 0.5) + Up * (NormalizedY * OrthoWidth * 0.5);
			return true;
		}

		OutViewDirection = (Forward + Right * (NormalizedX * TanHalfFov)
			+ Up * (NormalizedY * TanHalfFov)).GetSafeNormal();
		const double ForwardProjection = FVector::DotProduct(OutViewDirection, Forward);
		if (ForwardProjection <= UE_SMALL_NUMBER)
		{
			return false;
		}
		OutPosition = CameraPosition + OutViewDirection * (SceneDepth / ForwardProjection);
		return true;
	};

	TMap<int32, FViewVoxelAccumulator> ViewVoxels;
	for (int32 PixelY = 0; PixelY < CaptureResolution; ++PixelY)
	{
		for (int32 PixelX = 0; PixelX < CaptureResolution; ++PixelX)
		{
			FVector WorldPosition;
			FVector ViewDirection;
			if (!ReconstructWorldPosition(PixelX, PixelY, WorldPosition, ViewDirection)
				|| !Bounds.IsInsideOrOn(WorldPosition))
			{
				continue;
			}
			if (SlabAxis != INDEX_NONE)
		{
				const double AxisValue = GetAxisValue(WorldPosition, SlabAxis);
				if (AxisValue < SlabMin - UE_KINDA_SMALL_NUMBER || AxisValue > SlabMax + UE_KINDA_SMALL_NUMBER)
				{
					continue;
				}
			}

			FVector HorizontalPosition;
			FVector HorizontalViewDirection;
			FVector VerticalPosition;
			FVector VerticalViewDirection;
			bool bHasHorizontal = ReconstructWorldPosition(PixelX + 1, PixelY, HorizontalPosition, HorizontalViewDirection);
			bool bHasVertical = ReconstructWorldPosition(PixelX, PixelY + 1, VerticalPosition, VerticalViewDirection);
			if (!bHasHorizontal)
			{
				bHasHorizontal = ReconstructWorldPosition(PixelX - 1, PixelY, HorizontalPosition, HorizontalViewDirection);
			}
			if (!bHasVertical)
			{
				bHasVertical = ReconstructWorldPosition(PixelX, PixelY - 1, VerticalPosition, VerticalViewDirection);
			}

			float Confidence = 0.25f;
			if (bHasHorizontal && bHasVertical)
			{
				const double CenterDepth = DepthPixels[PixelX + PixelY * CaptureResolution].R;
				const double HorizontalDepth = FVector::DotProduct(HorizontalPosition - CameraPosition, Forward);
				const double VerticalDepth = FVector::DotProduct(VerticalPosition - CameraPosition, Forward);
				const double HorizontalDepthDelta = FMath::Abs(CenterDepth - HorizontalDepth);
				const double VerticalDepthDelta = FMath::Abs(CenterDepth - VerticalDepth);
				if (HorizontalDepthDelta <= DepthThreshold && VerticalDepthDelta <= DepthThreshold)
				{
					const FVector Normal = FVector::CrossProduct(
						HorizontalPosition - WorldPosition, VerticalPosition - WorldPosition).GetSafeNormal();
					Confidence = FMath::Abs(FVector::DotProduct(Normal, -ViewDirection));
					const float EdgePenalty = 1.0f - static_cast<float>(
						FMath::Max(HorizontalDepthDelta, VerticalDepthDelta) / DepthThreshold);
					Confidence *= FMath::Clamp(EdgePenalty, 0.0f, 1.0f);
				}
			}
			if (Confidence < MinimumSampleConfidence)
			{
				continue;
			}

			const FVector Relative = (WorldPosition - WorldOrigin) / VoxelSize;
			const FIntVector Coordinate(
				FMath::FloorToInt(Relative.X),
				FMath::FloorToInt(Relative.Z),
					FMath::FloorToInt(Relative.Y));
			if (!IsValidCoordinate(Coordinate, GridSize))
			{
				continue;
			}

			const FVector VoxelCenter = WorldOrigin + FVector(
				(static_cast<double>(Coordinate.X) + 0.5) * VoxelSize,
				(static_cast<double>(Coordinate.Z) + 0.5) * VoxelSize,
				(static_cast<double>(Coordinate.Y) + 0.5) * VoxelSize);
			const double QuantizationErrorCm = FVector::Distance(WorldPosition, VoxelCenter);
			QualityErrorSumCm += QuantizationErrorCm;
			QualityMaximumErrorCm = FMath::Max(QualityMaximumErrorCm, QuantizationErrorCm);
			++QualityEvaluatedSampleCount;

			FViewVoxelAccumulator& ViewAccumulator = ViewVoxels.FindOrAdd(ToLinearIndex(Coordinate, GridSize));
			ViewAccumulator.WeightedLinearColorSum += ColorPixels[PixelX + PixelY * CaptureResolution] * Confidence;
			ViewAccumulator.TotalWeight += Confidence;
			ViewAccumulator.MaximumConfidence = FMath::Max(ViewAccumulator.MaximumConfidence, Confidence);
		}
	}

	for (const TPair<int32, FViewVoxelAccumulator>& Pair : ViewVoxels)
	{
		if (Pair.Value.TotalWeight <= UE_SMALL_NUMBER)
		{
			continue;
		}
		const FLinearColor ViewAverage = Pair.Value.WeightedLinearColorSum / Pair.Value.TotalWeight;
		const float ViewWeight = FMath::Max(Pair.Value.MaximumConfidence, MinimumSampleConfidence);
		FCaptureAccumulator& Accumulator = InOutVoxels.FindOrAdd(Pair.Key);
		Accumulator.WeightedLinearColorSum += ViewAverage * ViewWeight;
		Accumulator.TotalWeight += ViewWeight;
		Accumulator.MaximumConfidence = FMath::Max(Accumulator.MaximumConfidence, Pair.Value.MaximumConfidence);
		++Accumulator.ObservationCount;
	}
	AsyncFusionSeconds += FPlatformTime::Seconds() - FusionStartTime;
	return true;
}

bool AVoxelMapCollector::IsValidCoordinate(const FIntVector& Coordinate, const FIntVector& GridSize) const
{
	return Coordinate.X >= 0 && Coordinate.Y >= 0 && Coordinate.Z >= 0
		&& Coordinate.X < GridSize.X && Coordinate.Y < GridSize.Y && Coordinate.Z < GridSize.Z;
}

int32 AVoxelMapCollector::ToLinearIndex(const FIntVector& Coordinate, const FIntVector& GridSize) const
{
	return Coordinate.X + Coordinate.Y * GridSize.X + Coordinate.Z * GridSize.X * GridSize.Y;
}

FIntVector AVoxelMapCollector::FromLinearIndex(int32 LinearIndex, const FIntVector& GridSize) const
{
	return FIntVector(
		LinearIndex % GridSize.X,
		(LinearIndex / GridSize.X) % GridSize.Y,
		LinearIndex / (GridSize.X * GridSize.Y));
}

void AVoxelMapCollector::FilterCapturedVoxels(TMap<int32, FCaptureAccumulator>& InOutVoxels,
	const FIntVector& GridSize, int32& OutRejectedByConfidence, int32& OutRemovedIsolated,
	int32& OutFilledHoles) const
{
	OutRejectedByConfidence = 0;
	OutRemovedIsolated = 0;
	OutFilledHoles = 0;

	TArray<int32> KeysToRemove;
	for (const TPair<int32, FCaptureAccumulator>& Pair : InOutVoxels)
	{
		const bool bEnoughObservations = Pair.Value.ObservationCount >= FMath::Max(1, MinimumObservationCount);
		const bool bHighConfidence = Pair.Value.MaximumConfidence >= FMath::Clamp(HighConfidenceThreshold, 0.0f, 1.0f);
		if ((!bEnoughObservations && !bHighConfidence) || Pair.Value.TotalWeight <= UE_SMALL_NUMBER)
		{
			KeysToRemove.Add(Pair.Key);
		}
	}
	OutRejectedByConfidence = KeysToRemove.Num();
	for (int32 Key : KeysToRemove)
	{
		InOutVoxels.Remove(Key);
	}

	if (bRemoveIsolatedVoxels && MinimumOccupiedNeighborCount > 0)
	{
		KeysToRemove.Reset();
		const int32 RequiredNeighbors = FMath::Clamp(MinimumOccupiedNeighborCount, 1, 26);
		for (const TPair<int32, FCaptureAccumulator>& Pair : InOutVoxels)
		{
			const FIntVector Coordinate = FromLinearIndex(Pair.Key, GridSize);
			int32 NeighborCount = 0;
			for (int32 Z = -1; Z <= 1 && NeighborCount < RequiredNeighbors; ++Z)
			{
				for (int32 Y = -1; Y <= 1 && NeighborCount < RequiredNeighbors; ++Y)
				{
					for (int32 X = -1; X <= 1 && NeighborCount < RequiredNeighbors; ++X)
					{
						if (X == 0 && Y == 0 && Z == 0)
						{
							continue;
						}
						const FIntVector Neighbor = Coordinate + FIntVector(X, Y, Z);
						if (IsValidCoordinate(Neighbor, GridSize)
							&& InOutVoxels.Contains(ToLinearIndex(Neighbor, GridSize)))
						{
							++NeighborCount;
						}
					}
				}
			}
			if (NeighborCount < RequiredNeighbors)
			{
				KeysToRemove.Add(Pair.Key);
			}
		}
		OutRemovedIsolated = KeysToRemove.Num();
		for (int32 Key : KeysToRemove)
		{
			InOutVoxels.Remove(Key);
		}
	}

	if (!bFillSingleVoxelHoles || InOutVoxels.IsEmpty())
	{
		return;
	}

	static const FIntVector AxisOffsets[6] = {
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};
	TSet<int32> CandidateHoles;
	for (const TPair<int32, FCaptureAccumulator>& Pair : InOutVoxels)
	{
		const FIntVector Coordinate = FromLinearIndex(Pair.Key, GridSize);
		for (const FIntVector& Offset : AxisOffsets)
		{
			const FIntVector Candidate = Coordinate + Offset;
			if (IsValidCoordinate(Candidate, GridSize))
			{
				const int32 CandidateIndex = ToLinearIndex(Candidate, GridSize);
				if (!InOutVoxels.Contains(CandidateIndex))
				{
					CandidateHoles.Add(CandidateIndex);
				}
			}
		}
	}

	TMap<int32, FCaptureAccumulator> FilledVoxels;
	const int32 RequiredAxes = FMath::Clamp(FillHoleRequiredOpposingAxes, 1, 3);
	for (int32 CandidateIndex : CandidateHoles)
	{
		const FIntVector Coordinate = FromLinearIndex(CandidateIndex, GridSize);
		int32 OpposingAxes = 0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FIntVector Positive = Coordinate + AxisOffsets[Axis * 2];
			const FIntVector Negative = Coordinate + AxisOffsets[Axis * 2 + 1];
			if (IsValidCoordinate(Positive, GridSize) && IsValidCoordinate(Negative, GridSize)
				&& InOutVoxels.Contains(ToLinearIndex(Positive, GridSize))
				&& InOutVoxels.Contains(ToLinearIndex(Negative, GridSize)))
			{
				++OpposingAxes;
			}
		}
		if (OpposingAxes < RequiredAxes)
		{
			continue;
		}

		FLinearColor ColorSum = FLinearColor::Black;
		int32 ColorCount = 0;
		for (const FIntVector& Offset : AxisOffsets)
		{
			const FIntVector Neighbor = Coordinate + Offset;
			if (!IsValidCoordinate(Neighbor, GridSize))
			{
				continue;
			}
			const FCaptureAccumulator* NeighborVoxel = InOutVoxels.Find(ToLinearIndex(Neighbor, GridSize));
			if (NeighborVoxel && NeighborVoxel->TotalWeight > UE_SMALL_NUMBER)
			{
				ColorSum += NeighborVoxel->WeightedLinearColorSum / NeighborVoxel->TotalWeight;
				++ColorCount;
			}
		}
		if (ColorCount > 0)
		{
			FCaptureAccumulator& Filled = FilledVoxels.Add(CandidateIndex);
			Filled.WeightedLinearColorSum = ColorSum / static_cast<float>(ColorCount);
			Filled.TotalWeight = 1.0f;
			Filled.MaximumConfidence = HighConfidenceThreshold;
			Filled.ObservationCount = MinimumObservationCount;
		}
	}

	OutFilledHoles = FilledVoxels.Num();
	for (const TPair<int32, FCaptureAccumulator>& Pair : FilledVoxels)
	{
		InOutVoxels.Add(Pair.Key, Pair.Value);
	}
}

bool AVoxelMapCollector::CommitPartitionedWorldAsset(const TArray<FVoxelMapSourceVoxel>& SourceVoxels,
	const FVector& WorldOrigin, const FIntVector& GridSize, const FBox& Bounds, int32 CaptureViews,
	const TCHAR* BakeLabel)
{
#if WITH_EDITOR
	if (!OutputWorldAsset)
	{
		return false;
	}

	auto AlignRegionDimension = [](int32 Value)
	{
		return FMath::DivideAndRoundUp(FMath::Max(Value, VOXELMAP_BLOCK_SIZE), VOXELMAP_BLOCK_SIZE)
			* VOXELMAP_BLOCK_SIZE;
	};
	const FIntVector AlignedRegionSize(
		AlignRegionDimension(RegionSizeInVoxels.X),
		AlignRegionDimension(RegionSizeInVoxels.Y),
		AlignRegionDimension(RegionSizeInVoxels.Z));

	TMap<FIntVector, TArray<FVoxelMapSourceVoxel>> RegionVoxels;
	for (const FVoxelMapSourceVoxel& SourceVoxel : SourceVoxels)
	{
		const FIntVector RegionCoordinate(
			SourceVoxel.Coordinate.X / AlignedRegionSize.X,
			SourceVoxel.Coordinate.Y / AlignedRegionSize.Y,
			SourceVoxel.Coordinate.Z / AlignedRegionSize.Z);
		FVoxelMapSourceVoxel LocalVoxel = SourceVoxel;
		LocalVoxel.Coordinate -= RegionCoordinate * AlignedRegionSize;
		RegionVoxels.FindOrAdd(RegionCoordinate).Add(LocalVoxel);
	}

	const FString WorldPackageName = OutputWorldAsset->GetOutermost()->GetName();
	const FString ParentPath = FPackageName::GetLongPackagePath(WorldPackageName);
	const FString WorldAssetName = OutputWorldAsset->GetName();
	const FString RegionFolder = ParentPath / TEXT("Regions");
	TArray<FIntVector> SortedRegionCoordinates;
	RegionVoxels.GetKeys(SortedRegionCoordinates);
	SortedRegionCoordinates.Sort([](const FIntVector& A, const FIntVector& B)
	{
		if (A.Z != B.Z) return A.Z < B.Z;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.X < B.X;
	});

	OutputWorldAsset->Modify();
	OutputWorldAsset->VoxelSize = VoxelSize;
	OutputWorldAsset->WorldOrigin = WorldOrigin;
	OutputWorldAsset->VoxelGridSize = GridSize;
	OutputWorldAsset->RegionSizeInVoxels = AlignedRegionSize;
	OutputWorldAsset->CaptureBounds = Bounds;
	OutputWorldAsset->SourceMapPackage = GetWorld() ? GetWorld()->GetOutermost()->GetName() : FString();
	OutputWorldAsset->Regions.Reset();
	OutputWorldAsset->DataVersion = 5;

	int64 TotalBlocks = 0;
	int64 TotalVoxels = 0;
	for (const FIntVector& RegionCoordinate : SortedRegionCoordinates)
	{
		const FString AssetName = FString::Printf(TEXT("%s_R_%d_%d_%d"), *WorldAssetName,
			RegionCoordinate.X, RegionCoordinate.Y, RegionCoordinate.Z);
		const FString PackageName = RegionFolder / AssetName;
		UPackage* Package = CreatePackage(*PackageName);
		UVoxelMapDataAsset* RegionAsset = FindObject<UVoxelMapDataAsset>(Package, *AssetName);
		if (!RegionAsset)
		{
			RegionAsset = NewObject<UVoxelMapDataAsset>(Package, *AssetName, RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(RegionAsset);
		}

		FVoxelMapData RegionData;
		if (!FVoxelMapDataBuilder::Build(AlignedRegionSize, RegionVoxels.FindChecked(RegionCoordinate), RegionData)
			|| !FVoxelMapGenerator::Validate(RegionData))
		{
			UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] %s failed to build region (%d,%d,%d)"), BakeLabel,
				RegionCoordinate.X, RegionCoordinate.Y, RegionCoordinate.Z);
			return false;
		}

		const FIntVector RegionVoxelOffset = RegionCoordinate * AlignedRegionSize;
		const FVector RegionWorldOrigin = WorldOrigin + FVector(
			static_cast<double>(RegionVoxelOffset.X) * VoxelSize,
			static_cast<double>(RegionVoxelOffset.Z) * VoxelSize,
			static_cast<double>(RegionVoxelOffset.Y) * VoxelSize);
		const FVector RegionWorldSize(
			static_cast<double>(AlignedRegionSize.X) * VoxelSize,
			static_cast<double>(AlignedRegionSize.Z) * VoxelSize,
			static_cast<double>(AlignedRegionSize.Y) * VoxelSize);

		RegionAsset->Modify();
		RegionAsset->Data = MoveTemp(RegionData);
		RegionAsset->VoxelSize = VoxelSize;
		RegionAsset->WorldOrigin = RegionWorldOrigin;
		RegionAsset->VoxelGridSize = AlignedRegionSize;
		RegionAsset->CaptureBounds = FBox(RegionWorldOrigin, RegionWorldOrigin + RegionWorldSize);
		RegionAsset->CaptureViewCount = CaptureViews;
		RegionAsset->DataVersion = 5;
		RegionAsset->SourceMapPackage = OutputWorldAsset->SourceMapPackage;
		RegionAsset->MarkPackageDirty();

		FVoxelMapRegionReference& Reference = OutputWorldAsset->Regions.AddDefaulted_GetRef();
		Reference.RegionCoordinate = RegionCoordinate;
		Reference.WorldBounds = RegionAsset->CaptureBounds;
		Reference.Asset = RegionAsset;
		TotalBlocks += RegionAsset->Data.BlockCount;
		TotalVoxels += RegionAsset->Data.OccupiedVoxelCount;
	}

	OutputWorldAsset->MarkPackageDirty();
	if (PreviewRenderer)
	{
		PreviewRenderer->VoxelMapWorldAsset = OutputWorldAsset;
		PreviewRenderer->VoxelMapAsset = nullptr;
		PreviewRenderer->ReloadVoxelMapAsset();
	}
	UE_LOG(LogTemp, Log, TEXT("[VoxelMap] %s partitioned: %d regions, %lld blocks, %lld voxels"),
		BakeLabel, OutputWorldAsset->Regions.Num(), TotalBlocks, TotalVoxels);
	return true;
#else
	UE_LOG(LogTemp, Warning, TEXT("[VoxelMap] Partitioned asset creation is editor-only"));
	return false;
#endif
}

bool AVoxelMapCollector::ReadCapturePixels(TArray<FLinearColor>& OutDepthPixels, TArray<FLinearColor>& OutColorPixels)
{
	if (!CaptureTarget || !CaptureComponent)
	{

		return false;
	}

	FlushRenderingCommands();
	FTextureRenderTargetResource* Resource = CaptureTarget->GameThread_GetRenderTargetResource();
	if (!Resource || !Resource->ReadLinearColorPixels(OutDepthPixels))
	{
		return false;
	}

	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
	CaptureComponent->CaptureScene();
	FlushRenderingCommands();
	Resource = CaptureTarget->GameThread_GetRenderTargetResource();
	return Resource && Resource->ReadLinearColorPixels(OutColorPixels)
		&& OutDepthPixels.Num() == CaptureResolution * CaptureResolution
		&& OutColorPixels.Num() == OutDepthPixels.Num();
}

void AVoxelMapCollector::CommitDataAsset(FVoxelMapData&& NewData, const FVector& WorldOrigin, const FIntVector& GridSize,
	const FBox& Bounds, int32 CaptureViews, const TCHAR* BakeLabel)
{
	const int64 BlockBytes = static_cast<int64>(NewData.BlockData.Num()) * sizeof(uint32);
	const int64 VoxelBytes = static_cast<int64>(NewData.VoxelData.Num()) * sizeof(uint32);
	const double DataMiB = static_cast<double>(BlockBytes + VoxelBytes) / (1024.0 * 1024.0);

	OutputDataAsset->Modify();
	OutputDataAsset->Data = MoveTemp(NewData);
	OutputDataAsset->VoxelSize = VoxelSize;
	OutputDataAsset->WorldOrigin = WorldOrigin;
	OutputDataAsset->VoxelGridSize = GridSize;
	OutputDataAsset->CaptureBounds = Bounds;
	OutputDataAsset->CaptureViewCount = CaptureViews;
	OutputDataAsset->DataVersion = 5;
	OutputDataAsset->SourceMapPackage = GetWorld() ? GetWorld()->GetOutermost()->GetName() : FString();

	OutputDataAsset->MarkPackageDirty();

	if (PreviewRenderer)
	{
		PreviewRenderer->VoxelMapWorldAsset = nullptr;
		PreviewRenderer->VoxelMapAsset = OutputDataAsset;
		PreviewRenderer->ReloadVoxelMapAsset();
	}


	UE_LOG(LogTemp, Log,
		TEXT("[VoxelMap] %s baked: %d non-empty blocks, %d occupied voxels, %.2f MiB, origin=(%.1f, %.1f, %.1f)"),
		BakeLabel, OutputDataAsset->Data.BlockCount, OutputDataAsset->Data.OccupiedVoxelCount, DataMiB,
		WorldOrigin.X, WorldOrigin.Y, WorldOrigin.Z);
}
