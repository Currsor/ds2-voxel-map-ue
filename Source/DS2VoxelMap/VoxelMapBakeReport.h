#pragma once

#include "CoreMinimal.h"
#include "VoxelMapBakeReport.generated.h"

/** M7 烘焙质量与性能报告。覆盖率基于已观测候选体素，不代表未被相机看到的场景真值。 */
USTRUCT(BlueprintType)
struct DS2VOXELMAP_API FVoxelMapBakeReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Identity")
	FString BakeLabel;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Identity")
	FString GeneratedAtUtc;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Identity")
	FString DataHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Identity")
	FString PreviousDataHash;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Identity")
	bool bComparedWithPreviousBake = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Identity")
	bool bDeterministicHashMatch = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	double TotalSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	double CaptureAndReadbackSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	double FusionSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	double FilterSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timing")
	double BuildAndCommitSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	FIntVector VoxelGridSize = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	int32 CaptureViewCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	int32 CandidateVoxelCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	int32 OccupiedVoxelCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	int32 NonEmptyBlockCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	int32 RegionCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	int64 PackedDataBytes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Data")
	double PackedDataMiB = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	double ObservedCandidateRetentionPercent = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	double MeanQuantizationErrorCm = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	double MaximumQuantizationErrorCm = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	double MeanQuantizationErrorInVoxels = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	int64 EvaluatedSampleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	int32 RejectedByConfidence = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	int32 RemovedIsolated = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quality")
	int32 FilledHoles = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Validation")
	bool bDataValid = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Validation")
	bool bCoveragePass = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Validation")
	bool bPositionErrorPass = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Validation")
	bool bOverallPass = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Output")
	FString ReportFilePath;
};
