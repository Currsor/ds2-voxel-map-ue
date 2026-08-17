#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelMapBakeReport.h"
#include "VoxelMapGenerator.h"
#include "VoxelMapCollector.generated.h"

class ADS2VoxelMapRenderer;
class UBoxComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UVoxelMapDataAsset;
class UVoxelMapWorldAsset;
struct FAsyncVoxelCaptureReadback;
struct FSlowTask;
struct FVoxelMapSourceVoxel;

/** 编辑器离线采集器：可生成模拟数据，也可从当前真实关卡采集表面并写入 DataAsset。 */
UCLASS(PrioritizeCategories = ("可调参数"))
class DS2VOXELMAP_API AVoxelMapCollector : public AActor
{
	GENERATED_BODY()

public:
	AVoxelMapCollector();
	virtual ~AVoxelMapCollector() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Capture",
		meta = (DisplayName = "采集范围", ToolTip = "定义需要体素化的世界空间范围；调整盒体尺寸和位置即可改变采集区域。"))
	TObjectPtr<UBoxComponent> CaptureBounds;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Capture",
		meta = (DisplayName = "场景采集组件", ToolTip = "内部复用的场景采集组件，负责渲染深度和基础颜色；通常无需手动修改。"))
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "输出体素资产", ToolTip = "非分区模式下写入的 VoxelMap DataAsset。"))
	TObjectPtr<UVoxelMapDataAsset> OutputDataAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "输出世界资产", ToolTip = "分区模式下写入的世界清单资产；区域资产会自动创建在同目录的 Regions 子目录。"))
	TObjectPtr<UVoxelMapWorldAsset> OutputWorldAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "启用分区输出", ToolTip = "启用后把采集结果拆分为多个区域资产，并更新输出世界资产。"))
	bool bWritePartitionedWorldAsset = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "区域体素尺寸", ToolTip = "每个分区在 X、Y、Z 三轴包含的体素数量；运行时会向上对齐到 4 的倍数。", ClampMin = "4", UIMin = "32", UIMax = "512"))
	FIntVector RegionSizeInVoxels = FIntVector(128, 128, 128);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "预览渲染器", ToolTip = "可选；烘焙完成后自动让该渲染器重新加载最新的单资产或世界资产。"))
	TObjectPtr<ADS2VoxelMapRenderer> PreviewRenderer;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "体素边长（厘米）", ToolTip = "单个体素在世界空间中的边长，单位为厘米；越小精度越高，但采集和存储成本越大。", ClampMin = "0.001"))
	float VoxelSize = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "单视图方向", ToolTip = "单视图采集时相机相对采集中心的方向；输入会自动归一化。"))
	FVector SingleViewDirection = FVector(1.0, -1.0, 1.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "透视视场角", ToolTip = "单视图和半球视图使用的垂直视场角，单位为度。", ClampMin = "5.0", ClampMax = "150.0"))
	float CaptureFieldOfView = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "采集分辨率", ToolTip = "深度和颜色 RenderTarget 的宽高像素数；提高分辨率会增加 GPU 渲染、读回和 CPU 融合成本。", ClampMin = "64", UIMin = "128", UIMax = "2048"))
	int32 CaptureResolution = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "半球视图数量", ToolTip = "Fibonacci 半球采集生成的视角数量；越多越能减少遮挡缺失，但耗时近似线性增加。", ClampMin = "1", UIMin = "4", UIMax = "128"))
	int32 HemisphereViewCount = 24;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "切片厚度（体素）", ToolTip = "XYZ 切片薄层的厚度；数值越小遮挡越少，但需要的采集视图越多。", ClampMin = "1", UIMin = "1", UIMax = "16"))
	int32 SliceThicknessInVoxels = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "双向采集切片", ToolTip = "每个薄层同时从正向和反向采集，以覆盖朝向相反的表面。"))
	bool bCaptureSlicesFromBothDirections = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "采集 X 轴切片", ToolTip = "启用沿世界 X 轴分层的 YZ 截面采集。"))
	bool bCaptureSliceAxisX = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "采集 Y 轴切片", ToolTip = "启用沿世界 Y 轴分层的 XZ 截面采集。"))
	bool bCaptureSliceAxisY = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "采集 Z 轴切片", ToolTip = "启用沿世界 Z 轴分层的 XY 截面采集。"))
	bool bCaptureSliceAxisZ = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "最小有效深度（厘米）", ToolTip = "深度小于该值的像素不会生成世界空间采样点。", ClampMin = "0.0"))
	float MinimumCaptureDepth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "深度突变阈值（体素）", ToolTip = "相邻像素允许的最大深度跳变；超过阈值的邻域不参与法线估计，以抑制轮廓边缘错误。", ClampMin = "0.1", UIMin = "0.5", UIMax = "16.0"))
	float DepthDiscontinuityThresholdInVoxels = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "最少独立观测数", ToolTip = "普通体素被接受所需的独立视图数量；同一视图内多个像素只计一次。", ClampMin = "1", UIMin = "1", UIMax = "16"))
	int32 MinimumObservationCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "高置信度阈值", ToolTip = "单次观测达到该值时，可绕过最少独立观测数限制。", ClampMin = "0.0", ClampMax = "1.0"))
	float HighConfidenceThreshold = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "最低样本置信度", ToolTip = "低于该正视置信度的掠射样本会被丢弃。", ClampMin = "0.0", ClampMax = "1.0"))
	float MinimumSampleConfidence = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "移除孤立体素", ToolTip = "启用后根据 26 邻域删除缺少足够邻居的飞点。"))
	bool bRemoveIsolatedVoxels = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "最少邻居数量", ToolTip = "26 邻域内至少具有该数量的占用邻居才保留体素；设为 1 只删除完全孤立的点。", ClampMin = "0", ClampMax = "26"))
	int32 MinimumOccupiedNeighborCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "填充单体素孔洞", ToolTip = "保守填充被相对体素夹住的一体素孔洞；默认关闭以优先保持原始拓扑。"))
	bool bFillSingleVoxelHoles = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "补洞所需对向轴数", ToolTip = "至少有多少个轴同时存在一对相对邻居时才补洞；2 通常适合修补表面小孔。", ClampMin = "1", ClampMax = "3"))
	int32 FillHoleRequiredOpposingAxes = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "使用异步 GPU 读回", ToolTip = "启用后深度和颜色 RenderTarget 会跨 Tick 异步读回，避免长时间阻塞编辑器。"))
	bool bUseAsyncGPUReadback = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Async",
		meta = (DisplayName = "采集进行中", ToolTip = "表示当前是否存在尚未完成或取消的异步采集任务。"))
	bool bCaptureInProgress = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Async",
		meta = (DisplayName = "采集进度", ToolTip = "当前异步采集进度，范围为 0 到 1。"))
	float CaptureProgress = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|Async",
		meta = (DisplayName = "采集状态", ToolTip = "显示当前采集阶段、视图进度、完成结果或错误原因。"))
	FString CaptureStatus;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "最低观测保留率（%）", ToolTip = "置信度过滤和去噪后至少应保留的已观测候选体素比例；该指标不是全场景几何真值覆盖率。", ClampMin = "0.0", ClampMax = "100.0"))
	float MinimumObservedCoveragePercent = 98.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "最大平均量化误差（体素）", ToolTip = "允许的采样点到体素中心平均距离，以 VoxelSize 的倍数表示。", ClampMin = "0.0", ClampMax = "2.0"))
	float MaximumMeanQuantizationErrorInVoxels = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "写入 M7 报告文件", ToolTip = "启用后把质量与性能报告写入 Saved/VoxelMapReports 目录中的 JSON 文件。"))
	bool bWriteM7ReportFile = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Voxel Map|M7 Validation",
		meta = (DisplayName = "最近一次烘焙报告", ToolTip = "最近一次成功完成的烘焙所生成的质量、性能、数据规模和 Hash 验收报告。"))
	FVoxelMapBakeReport LastBakeReport;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "模拟体素网格尺寸", ToolTip = "普通程序化模拟烘焙的 X、Y、Z 体素数量；各轴会向上对齐到 4。", ClampMin = "4", UIMin = "4", UIMax = "1024"))
	FIntVector VoxelGridSize = FIntVector(256, 128, 256);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "性能测试网格尺寸", ToolTip = "性能测试烘焙使用的 X、Y、Z 体素数量；应根据可用内存逐步增大。", ClampMin = "4", UIMin = "4", UIMax = "2048"))
	FIntVector PerformanceTestGridSize = FIntVector(512, 128, 512);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "地形随机种子", ToolTip = "程序化地形噪声使用的随机种子；相同参数和种子应生成相同数据。"))
	int32 VoxelSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "基础地形高度", ToolTip = "程序化地形的平均高度，以体素层数表示。"))
	float TerrainHeight = 48.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "地形高度振幅", ToolTip = "程序化噪声在基础高度上下变化的最大幅度，以体素层数表示。"))
	float TerrainAmplitude = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "噪声频率", ToolTip = "程序化地形噪声的空间频率；越大地形变化越密集。"))
	float NoiseFrequency = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "噪声八度数", ToolTip = "分形噪声叠加的层数；越多细节越丰富，生成成本也越高。"))
	int32 NoiseOctaves = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "可调参数",
		meta = (DisplayName = "生成平坦地形", ToolTip = "启用后忽略噪声和高度振幅，只按基础高度生成水平地形。"))
	bool bFlatTerrain = false;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Simulation")
	void BakeVoxelMap();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Performance Test")
	void BakePerformanceTest();

	/** M1：从 SingleViewDirection 执行一组 Depth + BaseColor 采集。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Real World Capture")
	void BakeSingleViewCapture();

	/** M2：用 Fibonacci 半球视角融合真实关卡表面。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Real World Capture")
	void BakeHemisphereCapture();

	/** M3：沿世界 XYZ 三轴执行正交薄层采集。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|XYZ Slice")
	void BakeXYZSliceCapture();

	/** 联合 Hemisphere 与 XYZ Slice，在一次烘焙中融合两种采集结果。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Real World Capture")
	void BakeCombinedWorldCapture();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel Map|Async")
	void CancelCapture();

	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 跨视图融合后的单体素累计数据。 */
	struct FCaptureAccumulator
	{
		FLinearColor WeightedLinearColorSum = FLinearColor::Black; // 线性空间加权颜色总和。
		float TotalWeight = 0.0f; // 所有有效观测的权重总和。
		float MaximumConfidence = 0.0f; // 所有观测中的最高置信度。
		int32 ObservationCount = 0; // 独立视图观测数量。
	};

	/** 单个采集视图内的体素累计数据，同一视图只贡献一次观测计数。 */
	struct FViewVoxelAccumulator
	{
		FLinearColor WeightedLinearColorSum = FLinearColor::Black; // 当前视图内的加权颜色总和。
		float TotalWeight = 0.0f; // 当前视图内的样本权重总和。
		float MaximumConfidence = 0.0f; // 当前视图内的最高样本置信度。
	};

	/** 一个待执行的透视或正交采集视图。 */
	struct FCaptureJob
	{
		FVector CameraPosition = FVector::ZeroVector; // 相机世界位置。
		FRotator CameraRotation = FRotator::ZeroRotator; // 相机世界旋转。
		bool bOrthographic = false; // 是否使用正交投影。
		double OrthoWidth = 0.0; // 正交投影宽度，透视视图忽略。
		int32 SlabAxis = INDEX_NONE; // 切片所属世界轴；非切片视图为 INDEX_NONE。
		double SlabMin = 0.0; // 当前薄层在指定轴上的最小世界坐标。
		double SlabMax = 0.0; // 当前薄层在指定轴上的最大世界坐标。
	};

	/** 异步采集状态机阶段。 */
	enum class EAsyncCapturePhase : uint8
	{
		Idle,
		SubmitDepth,
		WaitDepth,
		SubmitColor,
		WaitColor,
		Finalize
	};

	FVoxelMapConfig BuildVoxelConfig(const FIntVector& GridSize) const;
	void BakeVoxelMapInternal(const FIntVector& GridSize, const TCHAR* BakeLabel);
	bool BakeWorldCapture(const TArray<FVector>& ViewDirections, const TCHAR* BakeLabel);
	bool BakeCapturePasses(const TArray<FVector>& ViewDirections, bool bIncludeXYZSlices, const TCHAR* BakeLabel);
	bool StartAsyncCapture(const TArray<FVector>& ViewDirections, bool bIncludeXYZSlices, const TCHAR* BakeLabel);
	void BuildCaptureJobs(const TArray<FVector>& ViewDirections, bool bIncludeXYZSlices, const FBox& Bounds,
		TArray<FCaptureJob>& OutJobs) const;
	bool ConfigureCaptureJob(const FCaptureJob& Job, bool bCaptureDepth);
	bool QueueAsyncReadback();
	bool PollAsyncReadback(TArray<FLinearColor>& OutPixels);
	void AdvanceAsyncCapture();
	bool FinalizeCapturedVoxels(TMap<int32, FCaptureAccumulator>& CapturedVoxels, const FBox& Bounds,
		const FVector& WorldOrigin, const FIntVector& GridSize, int32 CaptureViews, const TCHAR* BakeLabel,
		double StartTime);
	FString ComputeDataHash(const FVoxelMapData& Data, const FVector& WorldOrigin,
		const FIntVector& GridSize) const;
	FString ComputeWorldDataHash() const;
	void CompleteM7Report(const TCHAR* BakeLabel, const FIntVector& GridSize, int32 CaptureViews,
		int32 CandidateVoxelCount, int32 RejectedByConfidence, int32 RemovedIsolated, int32 FilledHoles,
		double StartTime, double FilterSeconds, double BuildAndCommitSeconds, const FString& PreviousHash);
	bool WriteM7ReportFile(FVoxelMapBakeReport& Report) const;
	void FinishAsyncCapture(bool bSucceeded, const TCHAR* Reason);
	bool CommitPartitionedWorldAsset(const TArray<FVoxelMapSourceVoxel>& SourceVoxels, const FVector& WorldOrigin,
		const FIntVector& GridSize, const FBox& Bounds, int32 CaptureViews, const TCHAR* BakeLabel);
	bool CaptureView(const FVector& ViewDirection, const FBox& Bounds, const FVector& WorldOrigin,
		const FIntVector& GridSize, TMap<int32, FCaptureAccumulator>& InOutVoxels);
	bool CaptureXYZSlices(const FBox& Bounds, const FVector& WorldOrigin, const FIntVector& GridSize,
		TMap<int32, FCaptureAccumulator>& InOutVoxels, int32& InOutCaptureViews);
	bool CaptureSliceView(int32 WorldAxis, int32 DirectionSign, double SlabMin, double SlabMax,
		const FBox& Bounds, const FVector& WorldOrigin, const FIntVector& GridSize,
		TMap<int32, FCaptureAccumulator>& InOutVoxels);
	bool ReadCapturePixels(TArray<FLinearColor>& OutDepthPixels, TArray<FLinearColor>& OutColorPixels);
	bool EnsureCaptureTarget();
	bool AccumulateCapturePixels(const TArray<FLinearColor>& DepthPixels, const TArray<FLinearColor>& ColorPixels,
		const FVector& CameraPosition, const FRotator& CameraRotation, bool bOrthographic, double OrthoWidth,
		const FBox& Bounds, const FVector& WorldOrigin, const FIntVector& GridSize,
		TMap<int32, FCaptureAccumulator>& InOutVoxels, int32 SlabAxis = INDEX_NONE,
		double SlabMin = 0.0, double SlabMax = 0.0);
	void FilterCapturedVoxels(TMap<int32, FCaptureAccumulator>& InOutVoxels, const FIntVector& GridSize,
		int32& OutRejectedByConfidence, int32& OutRemovedIsolated, int32& OutFilledHoles) const;
	bool IsValidCoordinate(const FIntVector& Coordinate, const FIntVector& GridSize) const;
	int32 ToLinearIndex(const FIntVector& Coordinate, const FIntVector& GridSize) const;
	FIntVector FromLinearIndex(int32 LinearIndex, const FIntVector& GridSize) const;
	FIntVector CalculateCaptureGridSize() const;
	FVector CalculateCaptureWorldOrigin(const FIntVector& GridSize) const;
	FVector CalculateWorldOrigin(const FVoxelMapData& Data) const;
	void CommitDataAsset(FVoxelMapData&& NewData, const FVector& WorldOrigin, const FIntVector& GridSize,
		const FBox& Bounds, int32 CaptureViews, const TCHAR* BakeLabel);

	UPROPERTY(Transient, meta = (DisplayName = "临时采集纹理", ToolTip = "运行时复用的临时 RenderTarget，不会保存到关卡或资产。"))
	TObjectPtr<UTextureRenderTarget2D> CaptureTarget;

	TArray<FCaptureJob> AsyncCaptureJobs; // 当前异步烘焙的全部采集任务。
	TMap<int32, FCaptureAccumulator> AsyncCapturedVoxels; // 异步任务已融合的体素累计数据。
	TArray<FLinearColor> AsyncDepthPixels; // 当前任务暂存的深度像素。
	TSharedPtr<FAsyncVoxelCaptureReadback, ESPMode::ThreadSafe> AsyncReadback; // 当前 GPU 纹理读回对象。
	TUniquePtr<FSlowTask> AsyncProgressTask; // 编辑器进度对话框任务。

	FBox AsyncBounds = FBox(EForceInit::ForceInit); // 当前异步采集的世界范围。
	FVector AsyncWorldOrigin = FVector::ZeroVector; // 当前体素网格的世界原点。
	FIntVector AsyncGridSize = FIntVector::ZeroValue; // 当前体素网格尺寸。
	FString AsyncBakeLabel; // 当前烘焙任务的显示名称。
	EAsyncCapturePhase AsyncCapturePhase = EAsyncCapturePhase::Idle; // 当前异步状态机阶段。
	int32 AsyncJobIndex = 0; // 当前正在处理的采集任务索引。
	double AsyncStartTime = 0.0; // 当前烘焙开始的高精度时间。
	double AsyncFusionSeconds = 0.0; // CPU 重建和融合累计耗时。
	double QualityErrorSumCm = 0.0; // 有效采样点到体素中心的距离总和，单位厘米。
	double QualityMaximumErrorCm = 0.0; // 最大采样量化误差，单位厘米。
	int64 QualityEvaluatedSampleCount = 0; // 参与量化误差统计的有效采样点数量。
	bool bAsyncCancelRequested = false; // 是否已请求在安全点取消当前异步任务。
};
