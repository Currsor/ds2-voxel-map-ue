#include "VoxelMapRendering.h"
#include "DS2VoxelMapRenderer.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "PipelineStateCache.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Camera/CameraComponent.h"
#include "ConvexVolume.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "Math/InverseRotationMatrix.h"
#include "Math/PerspectiveMatrix.h"
#include "Math/TranslationMatrix.h"

IMPLEMENT_GLOBAL_SHADER(FVoxelMapVS, "/Project/VoxelMap/VoxelMap.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVoxelMapPS, "/Project/VoxelMap/VoxelMap.usf", "MainPS", SF_Pixel);

namespace
{
	// unit quad（4 顶点 / 6 索引），渲染线程首次使用时创建
	FBufferRHIRef GQuadVertexBuffer;
	FBufferRHIRef GQuadIndexBuffer;
	FVertexDeclarationRHIRef GQuadVertexDeclaration;

	void EnsureQuadResources(FRHICommandListImmediate& RHICmdList)
	{
		if (GQuadVertexBuffer.IsValid())
		{
			return;
		}

		const FVector2f Corners[4] =
		{
			FVector2f(-1.0f, -1.0f),
			FVector2f( 1.0f, -1.0f),
			FVector2f(-1.0f,  1.0f),
			FVector2f( 1.0f,  1.0f),
		};
		TResourceArray<FVector2f> VBData;
		VBData.Append(Corners, 4);
		FRHIResourceCreateInfo VBInfo(TEXT("VoxelMapQuadVB"), &VBData);
		GQuadVertexBuffer = RHICmdList.CreateVertexBuffer(sizeof(FVector2f) * 4, BUF_Static, VBInfo);

		const uint16 Indices[6] = { 0, 1, 2, 2, 1, 3 };
		TResourceArray<uint16> IBData;
		IBData.Append(Indices, 6);
		FRHIResourceCreateInfo IBInfo(TEXT("VoxelMapQuadIB"), &IBData);
		GQuadIndexBuffer = RHICmdList.CreateIndexBuffer(sizeof(uint16), sizeof(uint16) * 6, BUF_Static, IBInfo);

		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0, 0, VET_Float2, 0, sizeof(FVector2f), false));
		GQuadVertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}
}

FVoxelMapSceneViewExtension::FVoxelMapSceneViewExtension(const FAutoRegister& AutoRegister, ADS2VoxelMapRenderer* InRenderer)
	: FSceneViewExtensionBase(AutoRegister)
	, Renderer(InRenderer)
{
}

void FVoxelMapSceneViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	(void)View;
	(void)Inputs;

	if (!Renderer)
	{
		return;
	}

	const FBufferRHIRef& BlockDataBuffer = Renderer->BlockDataBuffer;
	const FShaderResourceViewRHIRef& BlockDataSRV = Renderer->BlockDataSRV;
	const FShaderResourceViewRHIRef& VoxelDataSRV = Renderer->VoxelDataSRV;
	UTextureRenderTarget2D* RT = Renderer->RenderTarget;
	const FVoxelMapData& Data = Renderer->GetVoxelMapData();

	if (!BlockDataBuffer.IsValid() || !BlockDataSRV.IsValid() || !VoxelDataSRV.IsValid() || Data.BlockCount == 0 || !RT || !RT->GetRenderTargetResource() || !Renderer->DisplayCamera)
	{
		return;
	}

	FRDGTextureRef OutputTexture = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(RT->GetRenderTargetResource()->GetRenderTargetTexture(), TEXT("VoxelMapRT")));

	FRDGTextureRef DepthTexture = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(RT->SizeX, RT->SizeY), PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_DepthStencilTargetable),
		TEXT("VoxelMapDepth"));

	// DisplayCamera 同时是玩家视角与体素渲染视角，移动后会在下一帧生成对应的新 RT。
	UCameraComponent* Cam = Renderer->DisplayCamera;
	const FVector Eye = Cam->GetComponentLocation();
	const FRotator CamRot = Cam->GetComponentRotation();
	const float FOV = FMath::Clamp(Cam->FieldOfView, 5.0f, 170.0f);
	const int32 ViewWidth = FMath::Max(View.UnscaledViewRect.Width(), 1);
	const int32 ViewHeight = FMath::Max(View.UnscaledViewRect.Height(), 1);
	const float DisplayAspectRatio = static_cast<float>(ViewWidth) / static_cast<float>(ViewHeight);
	const FVector Right = CamRot.RotateVector(FVector::RightVector);
	const FVector Up = CamRot.RotateVector(FVector::UpVector);

	// UE 相机的 FieldOfView 是水平 FOV。使用最终玩家视口的宽高比预补偿方形 RT，
	// 这样 RT 在后处理材质中铺满宽屏时不会把画面横向拉宽。
	FMatrix ViewPlanesMatrix(
		FPlane(0.0f, 0.0f, 1.0f, 0.0f),
		FPlane(1.0f, 0.0f, 0.0f, 0.0f),
		FPlane(0.0f, 1.0f, 0.0f, 0.0f),
		FPlane(0.0f, 0.0f, 0.0f, 1.0f));
	FMatrix ViewMatrix = FTranslationMatrix(-Eye) * FInverseRotationMatrix(CamRot) * ViewPlanesMatrix;
	FMatrix ProjMatrix = FReversedZPerspectiveMatrix(
		FMath::DegreesToRadians(FOV) * 0.5f,
		DisplayAspectRatio, 1.0f, 1.0f, 100000.0f);
	FMatrix ViewProjD = ViewMatrix * ProjMatrix;
	const FMatrix44f ViewProj(ViewProjD);

	// M6：视锥平面（法线朝内）
	FConvexVolume Frustum;
	GetViewFrustumBounds(Frustum, ViewProjD, true);

	FVoxelMapVS::FParameters* VSParams = GraphBuilder.AllocParameters<FVoxelMapVS::FParameters>();
	VSParams->BlockDataBuffer = BlockDataSRV.GetReference();
	VSParams->ViewProjectionMatrix = ViewProj;
	VSParams->CameraRight = FVector3f(Right.X, Right.Y, Right.Z);
	VSParams->CameraUp = FVector3f(Up.X, Up.Y, Up.Z);
	VSParams->CameraPos = FVector3f(Eye.X, Eye.Y, Eye.Z);
	VSParams->BlockGrid = FVector4f((float)Data.BlockGridSize.X, (float)Data.BlockGridSize.Y, (float)Data.BlockGridSize.Z, 0.0f);
	VSParams->WorldOrigin = FVector3f(Renderer->GetVoxelWorldOrigin());
	VSParams->VoxelSize = Renderer->GetVoxelSize();
	VSParams->BillboardHalfSizeScale = Renderer->BillboardScale;
	for (int32 i = 0; i < FMath::Min(6, Frustum.Planes.Num()); ++i)
	{
		const FPlane& P = Frustum.Planes[i];
		VSParams->FrustumPlanes[i] = FVector4f(P.X, P.Y, P.Z, P.W);
	}

	FVoxelMapPS::FParameters* PSParams = GraphBuilder.AllocParameters<FVoxelMapPS::FParameters>();
	PSParams->BlockDataBuffer = BlockDataSRV.GetReference();
	PSParams->VoxelDataBuffer = VoxelDataSRV.GetReference();
	PSParams->ViewProjectionMatrix = ViewProj;
	PSParams->CameraPos = FVector3f(Eye.X, Eye.Y, Eye.Z);
	PSParams->BlockGrid = FVector4f((float)Data.BlockGridSize.X, (float)Data.BlockGridSize.Y, (float)Data.BlockGridSize.Z, 0.0f);
	PSParams->WorldOrigin = FVector3f(Renderer->GetVoxelWorldOrigin());
	PSParams->VoxelSize = Renderer->GetVoxelSize();
	PSParams->LODDistances = FVector2f(Renderer->LOD2Distance, Renderer->LOD1Distance);
	PSParams->bDebugDepth = Renderer->bDebugDepth ? 1.0f : 0.0f;
	PSParams->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::EClear);
	PSParams->RenderTargets.DepthStencil = FDepthStencilBinding(
		DepthTexture,
		ERenderTargetLoadAction::EClear,
		ERenderTargetLoadAction::EClear,
		FExclusiveDepthStencil::DepthWrite_StencilWrite);

	const uint32 NumInstances = (uint32)Data.BlockCount;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("VoxelMap"),
		PSParams,
		ERDGPassFlags::Raster,
		[VSParams, PSParams, NumInstances](FRHICommandListImmediate& RHICmdList)
		{
			EnsureQuadResources(RHICmdList);

			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FVoxelMapVS> VertexShader(ShaderMap);
			TShaderMapRef<FVoxelMapPS> PixelShader(ShaderMap);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GQuadVertexDeclaration.GetReference();
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *VSParams);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PSParams);

			RHICmdList.SetStreamSource(0, GQuadVertexBuffer.GetReference(), 0);
			RHICmdList.DrawIndexedPrimitive(GQuadIndexBuffer.GetReference(), 0, 0, 4, 0, 2, NumInstances);
		});
}
