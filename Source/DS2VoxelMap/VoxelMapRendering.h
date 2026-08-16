#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "SceneViewExtension.h"

class ADS2VoxelMapRenderer;

/** M3 顶点着色器：解码 block 位置 + 展开 billboard */
class FVoxelMapVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMapVS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMapVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, BlockDataBuffer)
		SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
		SHADER_PARAMETER(FVector3f, CameraRight)
		SHADER_PARAMETER(FVector3f, CameraUp)
		SHADER_PARAMETER(FVector3f, CameraPos)
		SHADER_PARAMETER(FVector4f, BlockGrid)
		SHADER_PARAMETER(FVector3f, WorldOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
		SHADER_PARAMETER(float, BillboardHalfSizeScale)
	END_SHADER_PARAMETER_STRUCT()
};

/** M4 片元着色器：ray-box + 3D DDA + 逐体素颜色 + LOD + 深度覆写 */
class FVoxelMapPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMapPS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMapPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RENDER_TARGET_BINDING_SLOTS()
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, BlockDataBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, VoxelDataBuffer)
		SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
		SHADER_PARAMETER(FVector3f, CameraPos)
		SHADER_PARAMETER(FVector4f, BlockGrid)
		SHADER_PARAMETER(FVector3f, WorldOrigin)
		SHADER_PARAMETER(float, VoxelSize)
		SHADER_PARAMETER(FVector2f, LODDistances)
		SHADER_PARAMETER(float, bDebugDepth)
	END_SHADER_PARAMETER_STRUCT()
};

/** 每帧在后处理前把体素 billboard 渲染进独立 RT */
class FVoxelMapSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FVoxelMapSceneViewExtension(const FAutoRegister& AutoRegister, ADS2VoxelMapRenderer* InRenderer);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) override;

private:
	ADS2VoxelMapRenderer* Renderer = nullptr;
};
