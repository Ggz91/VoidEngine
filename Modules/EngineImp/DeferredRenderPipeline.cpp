#include "DeferredRenderPipeline.h"
#include <map>
#include <iostream>

#include "CBaseRenderPipeline.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "../SSAO/Ssao.h"
#include "../ShadowMap/ShadowMap.h"
#include "../RenderItemUtil/RenderItemUtil.h"
#include "../Predefines/BufferPredefines.h"
#include "../Logger/LoggerWrapper.h"

const int gNumFrameResources = 3;

const int BufferThreadSize = 128;

CDeferredRenderPipeline::CDeferredRenderPipeline(HINSTANCE hInstance, HWND wnd)
	: CBaseRenderPipeline(hInstance, wnd)
{
	// Estimate the scene bounding sphere manually since we know how the scene was constructed.
	// The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
	// the world space origin.  In general, you need to loop over every world space vertex
	// position and compute the bounding sphere.
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(10.0f * 10.0f + 15.0f * 15.0f);
}

CDeferredRenderPipeline::~CDeferredRenderPipeline()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool CDeferredRenderPipeline::Initialize()
{
	
	if (!CBaseRenderPipeline::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	BuildDescriptorHeaps();
	BuildFrameResources();


	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}



void CDeferredRenderPipeline::PitchCamera(float rad)
{
	mCamera.Pitch(rad);
}

void CDeferredRenderPipeline::RotateCameraY(float rad)
{
	mCamera.RotateY(rad);
}

void CDeferredRenderPipeline::MoveCamera(float dis)
{
	mCamera.Walk(dis);
}

void CDeferredRenderPipeline::StrafeCamera(float dis)
{
	mCamera.Strafe(dis);
}

void CDeferredRenderPipeline::CreateRtvAndDsvDescriptorHeaps()
{
	//+2 for G-Buffers
	//+1 for Hi-Z buffers array
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + GBufferSize() + 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));


	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));

	//G-buffer
	CreateGBufferRTV();

	//Hi-Z
	CreateHiZBuffer();

	//Instance Culling
	CreateHiZInstanceCullingBuffers();

	//Chunk Expan
	CreateChunExpanBuffer();

	//Cluster Culling
	CreateHIZClusterCullingBuffers();
}

void CDeferredRenderPipeline::OnResize()
{
	CBaseRenderPipeline::OnResize();

	mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1000.0f, 10000.0f);
	mCamera.SetPosition(0.0f, 500.0f, 1500.0f);
	mCamera.LookAt(mCamera.GetPosition3f(), XMFLOAT3(0, 0, 0), XMFLOAT3(0, 1, 0));
}

void CDeferredRenderPipeline::Update(const GameTimer& gt)
{
	
	mLightRotationAngle += 0.1f * gt.DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

	UpdateFrameResource(gt);
}

void CDeferredRenderPipeline::Draw(const GameTimer& gt)
{
	DrawWithDeferredTexturing(gt);
}

void CDeferredRenderPipeline::UpdateCamera(const GameTimer& gt)
{
	mCamera.UpdateViewMatrix();
}

void CDeferredRenderPipeline::DrawWithDeferredTexturing(const GameTimer& gt)
{
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % MaxCommandAllocNum;
	auto cmdListAlloc = mFrameResources->CmdListAlloc[mCurrFrameResourceIndex];

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	
	HiZPass();
	InstanceHiZCullingPass();
	ChunkExpanPass();
	ClusterHiZCullingPass();
	//DeferredDrawFillGBufferPass();
	//DeferredDrawShadingPass();

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Swap the back and front buffers
	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	// Advance the fence value to mark commands up to this fence point.
	m_frame_res_offset.back().Fence = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point. 
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

std::vector<RenderItem*>& CDeferredRenderPipeline::GetRenderItems(int layer)
{
	return mRitemLayer[layer];
}

DirectX::XMFLOAT3 CDeferredRenderPipeline::GetCameraPos()
{
	DirectX::XMFLOAT3 pos;
	XMStoreFloat3(&pos, mCamera.GetPosition());
	return pos;
}

BoundingFrustum CDeferredRenderPipeline::GetCameraFrustum()
{
	BoundingFrustum cam_frustum;
	//view空间的视锥
	BoundingFrustum::CreateFromMatrix(cam_frustum, mCamera.GetProj());
	//需要转换到世界空间
	XMMATRIX inv_view = XMMatrixInverse(&XMMatrixDeterminant(mCamera.GetView()), mCamera.GetView());
	BoundingFrustum res;
	cam_frustum.Transform(res, inv_view);
	return res;
}

DirectX::XMFLOAT3 CDeferredRenderPipeline::GetCameraDir()
{
	DirectX::XMFLOAT3 dir;
	auto vec_dir = mCamera.GetLook() - mCamera.GetPosition();
	XMStoreFloat3(&dir, vec_dir);
	return dir;
}

void CDeferredRenderPipeline::ClearVisibleRenderItems()
{
	for (int i=0; i<(int)RenderLayer::Count; ++i)
	{
		mRitemLayer[i].clear();
	}
	mAllRitems.clear();
}

void CDeferredRenderPipeline::PushVisibleModels(std::map<int, std::vector<RenderItem*>>& render_items, bool add /*= false*/)
{
	auto itr = render_items.begin();
	if (add)
	{
		while (itr != render_items.end())
		{
			mRitemLayer[itr->first].insert(mRitemLayer[itr->first].end(), itr->second.begin(), itr->second.end());
			mAllRitems.insert(mAllRitems.end(), itr->second.begin(), itr->second.end());
			itr++;
		}
	}
	else
	{
		while (itr != render_items.end())
		{
			mRitemLayer[itr->first] = itr->second;
			mAllRitems.insert(mAllRitems.end(), itr->second.begin(), itr->second.end());
			itr++;
		}
	}
}

bool CDeferredRenderPipeline::InitDirect3D()
{
	if (!CBaseRenderPipeline::InitDirect3D())
	{
		return false;
	}

	

	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
	BuildRootSignature();
	BuildShadersAndInputLayout();
	BuildPSOs();

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	FlushCommandQueue();
	return true;
}

bool CDeferredRenderPipeline::IsCameraDirty()
{
	return mCamera.Dirty();
}

void CDeferredRenderPipeline::BuildRootSignature()
{
	BuildDeferredRootSignature();
	BuildHiZRootSignature();
	BuildHiZInstanceCullingRootSignature();
	BuildChunkExpanRootSignature();
	BuildClusterHiZCullingRootSignature();
}



void CDeferredRenderPipeline::BuildDescriptorHeaps()
{
	//+2 for g-buffers
	//+1 for hi z buffer
	//+GetHiZMipmapLevels() for hi z mipmaps
	//+1 for instance culling res
	//+1 for instance culling object buffer 
	//+1 for chunk expan buffer
	//+1 for cluster culling buffer
	//+1 for vertex buffer / dynamic
	//+1 for index buffer /dynamic
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = mTextures.size() +GBufferSize() + 1 + GetHiZMipmapLevels() + 1 + 1 + 1 + 1 + 1 + 1;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	auto  itr = mTextures.begin();
	while (itr != mTextures.end())
	{
		srvDesc.Format = itr->second->Resource->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = itr->second->Resource->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(itr->second->Resource.Get(), &srvDesc, hDescriptor);

		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
		itr++;
	}

	

	for (int i=0; i<GBufferSize(); ++i)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC gbuffer_srv_desc = {};
		gbuffer_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		gbuffer_srv_desc.Format = m_g_buffer_format[i];
		gbuffer_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		gbuffer_srv_desc.Texture2D.MipLevels = 1;
		gbuffer_srv_desc.Texture2D.MostDetailedMip = 0;
		gbuffer_srv_desc.Texture2D.PlaneSlice = 0;
		gbuffer_srv_desc.Texture2D.ResourceMinLODClamp = 0;
		md3dDevice->CreateShaderResourceView(m_g_buffer[i].Get(), &gbuffer_srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mTextures.size()+i, mCbvSrvUavDescriptorSize));
	}

	//+1 for Hi-Z
	D3D12_SHADER_RESOURCE_VIEW_DESC hiz_srv_desc = {};
	hiz_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	hiz_srv_desc.Format = m_hiz_buffer_format;
	hiz_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	hiz_srv_desc.Texture2D.MipLevels = GetHiZMipmapLevels();
	hiz_srv_desc.Texture2D.MostDetailedMip = 0;
	hiz_srv_desc.Texture2D.PlaneSlice = 0;
	hiz_srv_desc.Texture2D.ResourceMinLODClamp = 0;
	md3dDevice->CreateShaderResourceView(m_hiz_buffer.Get(), &hiz_srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mTextures.size() + GBufferSize(), mCbvSrvUavDescriptorSize));

	//for hi-z uav
	D3D12_UNORDERED_ACCESS_VIEW_DESC hiz_uav = {};
	hiz_uav.Format = m_hiz_buffer_format;
	hiz_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	hiz_uav.Texture2D.MipSlice = 0;
	for (int i=0; i<hiz_srv_desc.Texture2D.MipLevels; ++i)
	{
		md3dDevice->CreateUnorderedAccessView(m_hiz_buffer.Get(), NULL, &hiz_uav, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mTextures.size() + GBufferSize() + 1 + i, mCbvSrvUavDescriptorSize));
		hiz_uav.Texture2D.MipSlice++;
	}

	//instance culling res buffer
	D3D12_UNORDERED_ACCESS_VIEW_DESC instance_culling_uav = {};
	instance_culling_uav.Format = DXGI_FORMAT_UNKNOWN;
	instance_culling_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	instance_culling_uav.Buffer.CounterOffsetInBytes = CullingResMaxObjSize;
	instance_culling_uav.Buffer.FirstElement = 0;
	instance_culling_uav.Buffer.StructureByteStride = sizeof(InstanceChunk);
	instance_culling_uav.Buffer.NumElements = CullingResBufferMaxElementNum;
	instance_culling_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	md3dDevice->CreateUnorderedAccessView(m_instance_culling_result_buffer.Get(), m_instance_culling_result_buffer.Get(), &instance_culling_uav, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mTextures.size() + GBufferSize() + 1 + hiz_srv_desc.Texture2D.MipLevels, mCbvSrvUavDescriptorSize));

	// chunk expan res buffer
	D3D12_UNORDERED_ACCESS_VIEW_DESC chunk_expan_uav = {};
	chunk_expan_uav.Format = DXGI_FORMAT_UNKNOWN;
	chunk_expan_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	chunk_expan_uav.Buffer.CounterOffsetInBytes = ChunkExpanMaxSize;
	chunk_expan_uav.Buffer.FirstElement = 0;
	chunk_expan_uav.Buffer.StructureByteStride = sizeof(ClusterChunk);
	chunk_expan_uav.Buffer.NumElements = ChunkExpanBufferMaxElementNum;
	chunk_expan_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	md3dDevice->CreateUnorderedAccessView(m_chunk_expan_result_buffer.Get(), m_chunk_expan_result_buffer.Get(), &chunk_expan_uav, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mTextures.size() + GBufferSize() + 1 + hiz_srv_desc.Texture2D.MipLevels + 1, mCbvSrvUavDescriptorSize));

	//cluster culling res buffer
	D3D12_UNORDERED_ACCESS_VIEW_DESC cluster_culling_uav = {};
	cluster_culling_uav.Format = DXGI_FORMAT_UNKNOWN;
	cluster_culling_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	cluster_culling_uav.Buffer.CounterOffsetInBytes = ClusterCullingResMaxSize;
	cluster_culling_uav.Buffer.FirstElement = 0;
	cluster_culling_uav.Buffer.StructureByteStride = sizeof(IndirectCommand);
	cluster_culling_uav.Buffer.NumElements = ChunkExpanBufferMaxElementNum;
	cluster_culling_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	md3dDevice->CreateUnorderedAccessView(m_cluster_culling_result_buffer.Get(), m_cluster_culling_result_buffer.Get(), &cluster_culling_uav, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), mTextures.size() + GBufferSize() + 1 + hiz_srv_desc.Texture2D.MipLevels + 1 + 1, mCbvSrvUavDescriptorSize));

	m_descriptor_end = mTextures.size() + GBufferSize() + 1 + hiz_srv_desc.Texture2D.MipLevels + 1 + 1;
}

void CDeferredRenderPipeline::BuildShadersAndInputLayout()
{
	mShaders["DeferredGSVS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredGSShader.hlsl", nullptr, "DeferredGSVS", "vs_5_1");
	mShaders["DeferredGSPS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredGSShader.hlsl", nullptr, "DeferredGSPS", "ps_5_1");
	mShaders["DeferredShadingVS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredShadingShader.hlsl", nullptr, "ShadingVS", "vs_5_1");
	mShaders["DeferredShadingPS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredShadingShader.hlsl", nullptr, "ShadingPS", "ps_5_1");
	
	//hi-z generate
	mShaders["HiZVS"] = d3dUtil::CompileShader(L".\\Shaders\\Depth.hlsl", nullptr, "DepthVS", "vs_5_1");
	mShaders["HiZPS"] = d3dUtil::CompileShader(L".\\Shaders\\Depth.hlsl", nullptr, "DepthPS", "ps_5_1");
	mShaders["HiZCS"] = d3dUtil::CompileShader(L".\\Shaders\\HiZMipmap.hlsl", nullptr, "GenerateHiZMipmaps", "cs_5_1");
	
	const D3D_SHADER_MACRO compute_macros[] =
	{
		"BufferThreadSize",  "128",
		NULL, NULL
	};

	//hi-z instance culling
	mShaders["HiZInstanceCulling"] = d3dUtil::CompileShader(L".\\Shaders\\HiZInstanceCulling.hlsl", compute_macros, "HiZInstanceCulling", "cs_5_1");
	

	//chunk expan
	mShaders["ChunkExpan"] = d3dUtil::CompileShader(L".\\Shaders\\ChunkExpan.hlsl", compute_macros, "ChunkExpan", "cs_5_1");

	//cluster culling
	mShaders["HiZClusterCulling"] = d3dUtil::CompileShader(L".\\Shaders\\HiZClusterCulling.hlsl", nullptr, "HiZClusterCulling", "cs_5_1");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}


void CDeferredRenderPipeline::BuildPSOs()
{
	BuildDeferredPSO();
	BuildHiZPSO();
	BuildHiZInstanceCullingPSO();
	BuildChunkExpanPSO();
	BuildClusterHiZCullingPSO();
}

void CDeferredRenderPipeline::BuildDeferredPSO()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gs_pso_desc;
	ZeroMemory(&gs_pso_desc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	gs_pso_desc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	gs_pso_desc.pRootSignature = m_deferred_gs_root_signature.Get();
	gs_pso_desc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredGSVS"]->GetBufferPointer()),
		mShaders["DeferredGSVS"]->GetBufferSize()
	};
	gs_pso_desc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredGSPS"]->GetBufferPointer()),
		mShaders["DeferredGSPS"]->GetBufferSize()
	};
	gs_pso_desc.NumRenderTargets = 2;
	gs_pso_desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
	gs_pso_desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;

	gs_pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	gs_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	gs_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	gs_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	gs_pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	gs_pso_desc.DepthStencilState.StencilEnable = true;
	gs_pso_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
	gs_pso_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	gs_pso_desc.SampleMask = UINT_MAX;
	gs_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	gs_pso_desc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	gs_pso_desc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	gs_pso_desc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gs_pso_desc, IID_PPV_ARGS(&mPSOs["DeferredGS"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC shading_pso_desc = gs_pso_desc;
	shading_pso_desc.pRootSignature = m_deferred_shading_root_signature.Get();
	shading_pso_desc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredShadingVS"]->GetBufferPointer()),
		mShaders["DeferredShadingVS"]->GetBufferSize()
	};
	shading_pso_desc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredShadingPS"]->GetBufferPointer()),
		mShaders["DeferredShadingPS"]->GetBufferSize()
	};
	shading_pso_desc.DepthStencilState.DepthEnable = false;
	shading_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	shading_pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	shading_pso_desc.DepthStencilState.StencilEnable = true;
	shading_pso_desc.DepthStencilState.StencilWriteMask = 0x0;
	shading_pso_desc.DepthStencilState.StencilReadMask = 0xFF;
	shading_pso_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
	shading_pso_desc.NumRenderTargets = 1;
	shading_pso_desc.RTVFormats[0] = mBackBufferFormat;
	shading_pso_desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
	shading_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	shading_pso_desc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&shading_pso_desc, IID_PPV_ARGS(&mPSOs["DeferredShading"])));
}

void CDeferredRenderPipeline::BuildFrameResources()
{
	mFrameResources = std::make_unique<FrameResource>(md3dDevice.Get());
}

void CDeferredRenderPipeline::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems, int layer)
{
	if (ritems.empty())
	{
		return;
	}
	UINT objCBByteSize = sizeof(ObjectConstants);
	UINT vertexCBByteSize = sizeof(VertexData);
	UINT indexCBByteSize = sizeof(std::uint16_t);

	auto objectCB = mFrameResources->FrameResCB->Resource();
	UINT offset = m_frame_res_offset.back().ObjectBeginOffset + GetRenderLayerObjectOffset(layer);

	UINT vertex_offset = m_frame_res_offset.back().VertexBeginOffset;
	UINT index_offset = m_frame_res_offset.back().IndexBeginOffset;

	D3D12_VERTEX_BUFFER_VIEW vbv;
	vbv.BufferLocation = objectCB->GetGPUVirtualAddress() + vertex_offset;
	vbv.StrideInBytes = vertexCBByteSize;
	vbv.SizeInBytes = m_contants_size.VertexCBSize;
	cmdList->IASetVertexBuffers(0, 1, &vbv);

	D3D12_INDEX_BUFFER_VIEW ibv;
	ibv.BufferLocation = objectCB->GetGPUVirtualAddress() + index_offset;
	ibv.Format = DXGI_FORMAT_R16_UINT;
	ibv.SizeInBytes = m_contants_size.IndexCBSize;
	cmdList->IASetIndexBuffer(&ibv);

	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		UINT object_offset = (offset + i * objCBByteSize) % mFrameResources->Size();
		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + object_offset;

		cmdList->SetGraphicsRootShaderResourceView(0, objCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

void CDeferredRenderPipeline::PushRenderItems(std::vector<RenderItem*>& render_items)
{
	RenderItemUtil::FillGeoData(render_items, md3dDevice.Get(), mCommandList.Get());
	
// 	auto& opaque_items = mRitemLayer[(int)RenderLayer::Opaque];
// 	opaque_items.insert(opaque_items.end(), render_items.begin(), render_items.end());

	mAllRitems.insert(mAllRitems.end(), render_items.begin(), render_items.end());
}

void CDeferredRenderPipeline::PushMats(std::vector<RenderItem*>& render_items)
{
	FlushCommandQueue();
	ThrowIfFailed(mDirectCmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	

	//LoadTexture
	int tex_index = 0;
	std::unordered_map<std::string, int> tex_indices;
	for (int i=0; i<render_items.size(); ++i)
	{
		//DiffuseMap
		auto diffuse_tex_itr = tex_indices.find(render_items[i]->Mat->Name + "_diffuse");
		if (tex_indices.end() != diffuse_tex_itr)
		{
			render_items[i]->Mat->DiffuseSrvHeapIndex = diffuse_tex_itr->second;
		}
		else
		{
			auto diffuse_map = std::make_unique<Texture>();
			diffuse_map->Name = render_items[i]->Mat->Name + "_diffuse";
			diffuse_map->Filename = AnsiToWString(render_items[i]->Mat->DiffuseMapPath.c_str());
			ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
				mCommandList.Get(), diffuse_map->Filename.c_str(),
				diffuse_map->Resource, diffuse_map->UploadHeap));
			render_items[i]->Mat->DiffuseSrvHeapIndex = tex_index++;
			tex_indices[diffuse_map->Name] = render_items[i]->Mat->DiffuseSrvHeapIndex;
			mTextures[diffuse_map->Name] = std::move(diffuse_map);
		}
		

		//NormalMap
		auto normal_tex_itr = tex_indices.find(render_items[i]->Mat->Name + "_normal");
		if (tex_indices.end() != normal_tex_itr)
		{
			render_items[i]->Mat->NormalSrvHeapIndex = normal_tex_itr->second;
		}
		else
		{
			auto normal_map = std::make_unique<Texture>();
			normal_map->Name = render_items[i]->Mat->Name + "_normal";
			normal_map->Filename = AnsiToWString(render_items[i]->Mat->NormalMapPath.c_str());
			ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
				mCommandList.Get(), normal_map->Filename.c_str(),
				normal_map->Resource, normal_map->UploadHeap));
			render_items[i]->Mat->NormalSrvHeapIndex = tex_index++;
			tex_indices[normal_map->Name] = render_items[i]->Mat->NormalSrvHeapIndex;
			mTextures[normal_map->Name] = std::move(normal_map);
		}
		

		mMaterials[render_items[i]->Mat->Name] = std::move(render_items[i]->Mat);
	}

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmd_lists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmd_lists), cmd_lists);
	FlushCommandQueue();
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CDeferredRenderPipeline::GetCpuSrv(int index)const
{
	auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE CDeferredRenderPipeline::GetGpuSrv(int index)const
{
	auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CDeferredRenderPipeline::GetDsv(int index)const
{
	auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
	dsv.Offset(index, mDsvDescriptorSize);
	return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CDeferredRenderPipeline::GetRtv(int index)const
{
	auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtv.Offset(index, mRtvDescriptorSize);
	return rtv;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> CDeferredRenderPipeline::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow
	};
}

void CDeferredRenderPipeline::CreateGBufferRTV()
{
	m_g_buffer_format[0] = DXGI_FORMAT_R32G32B32A32_UINT;
	m_g_buffer_format[1] = DXGI_FORMAT_R32_UINT;

	for (int i=0; i<GBufferSize(); ++i)
	{
		auto clear_values = CD3DX12_CLEAR_VALUE(m_g_buffer_format[i], Colors::LightSteelBlue);
		ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Tex2D(m_g_buffer_format[i], mClientWidth, mClientHeight, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			&clear_values,
			IID_PPV_ARGS(&m_g_buffer[i])));
		std::wstring buffer_name = L"GBuffer RT " + std::to_wstring(i);
		m_g_buffer[i]->SetName(buffer_name.c_str());

		D3D12_RENDER_TARGET_VIEW_DESC rt_desc;
		rt_desc.Format = m_g_buffer_format[i];
		rt_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rt_desc.Texture2D.MipSlice = 0;
		rt_desc.Texture2D.PlaneSlice = 0;
		CD3DX12_CPU_DESCRIPTOR_HANDLE h(CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(), SwapChainBufferCount+i, mRtvDescriptorSize));
		md3dDevice->CreateRenderTargetView(m_g_buffer[i].Get(), &rt_desc, h);
	}
}

UINT CDeferredRenderPipeline::GBufferSize() const
{
	return sizeof(m_g_buffer) / sizeof(m_g_buffer[0]);
}

void CDeferredRenderPipeline::DeferredDrawFillGBufferPass()
{
	//第一个pass，先填充G-Buffers
	mCommandList->SetGraphicsRootSignature(m_deferred_gs_root_signature.Get());

	for (int i = 0; i < GBufferSize(); ++i)
	{
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[i].Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET));
	}

	// Clear the back buffer and depth buffer.
	for (int i = 0; i < GBufferSize(); ++i)
	{
		auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
			SwapChainBufferCount + i,
			mRtvDescriptorSize);
		mCommandList->ClearRenderTargetView(handle, Colors::LightSteelBlue, 0, nullptr);
	}
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	CD3DX12_CPU_DESCRIPTOR_HANDLE g_buffer_handle(mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		SwapChainBufferCount,
		mRtvDescriptorSize);
	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(GBufferSize(),
		&g_buffer_handle,
		true,
		&DepthStencilView());
	mCommandList->OMSetStencilRef(1);
	auto passCB = mFrameResources->FrameResCB->Resource();
	UINT pass_offset = m_frame_res_offset.back().PassBeginOffset;
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress() + pass_offset);

	mCommandList->SetPipelineState(mPSOs["DeferredGS"].Get());
	// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
	// set as a root descriptor.

	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque], (int) RenderLayer::Opaque);

	for (int i = 0; i < GBufferSize(); ++i)
	{
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[i].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
	}
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
// 		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[0].Get(),
// 		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));
// 	mCommandList->CopyResource(CurrentBackBuffer(), m_g_buffer[0].Get());
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
// 		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[0].Get(),
// 		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void CDeferredRenderPipeline::DeferredDrawShadingPass()
{

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
	mCommandList->SetGraphicsRootSignature(m_deferred_shading_root_signature.Get());
	mCommandList->SetPipelineState(mPSOs["DeferredShading"].Get());
	UINT pass_offset = m_frame_res_offset.back().PassBeginOffset;
	mCommandList->SetGraphicsRootConstantBufferView(0, mFrameResources->FrameResCB->Resource()->GetGPUVirtualAddress() + pass_offset);
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_des(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	mCommandList->SetGraphicsRootDescriptorTable(1, h_des.Offset(mTextures.size(), mCbvSrvUavDescriptorSize));
	mCommandList->SetGraphicsRootDescriptorTable(2, h_des.Offset(1, mCbvSrvUavDescriptorSize));
	auto matBuffer = mFrameResources->FrameResCB->Resource();
	mCommandList->SetGraphicsRootShaderResourceView(3, matBuffer->GetGPUVirtualAddress() + m_frame_res_offset.back().MatBeginOffset);
	
	if (0 != mTextures.size())
	{
		mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	}
	mCommandList->OMSetStencilRef(1);
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
	mCommandList->IASetVertexBuffers(0, 1, nullptr);
	mCommandList->IASetIndexBuffer(nullptr);
	mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	mCommandList->DrawInstanced(6, 1, 0, 0);
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT));

}

void CDeferredRenderPipeline::BuildDeferredRootSignature()
{
	BuildDeferredGSRootSignature();
	BuildDeferredShadingRootSignature();
}

void CDeferredRenderPipeline::BuildDeferredGSRootSignature()
{
	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[2];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstantBufferView(0);
	slotRootParameter[1].InitAsConstantBufferView(1);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_deferred_gs_root_signature.GetAddressOf())));
}

void CDeferredRenderPipeline::BuildDeferredShadingRootSignature()
{
	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];

	CD3DX12_DESCRIPTOR_RANGE gbuffer0_table;
	gbuffer0_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE gbuffer1_table;
	gbuffer1_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

	CD3DX12_DESCRIPTOR_RANGE tex_table;
	tex_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 2, 1);


	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstantBufferView(1);
	slotRootParameter[1].InitAsDescriptorTable(1, &gbuffer0_table);
	slotRootParameter[2].InitAsDescriptorTable(1, &gbuffer1_table);
	slotRootParameter[3].InitAsShaderResourceView(0, 1);
	slotRootParameter[4].InitAsDescriptorTable(1, &tex_table, D3D12_SHADER_VISIBILITY_ALL);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_deferred_shading_root_signature.GetAddressOf())));
}

void CDeferredRenderPipeline::UpdateFrameResource(const GameTimer& gt)
{
	//填充数据到frame res offset queue中
	m_contants_size = CalCurFrameContantsSize();
	FrameResourceOffset  offset;
	
	//初始值
	offset.ObjectBeginOffset = m_frame_res_offset.empty() ? 0 : Align(m_frame_res_offset.back().EndResOffset, sizeof(ObjectConstants));
	offset.MatBeginOffset = offset.ObjectBeginOffset + m_contants_size.ObjectCBSize;
	offset.PassBeginOffset = AlignForCrvAddress(mFrameResources->FrameResCB->Resource()->GetGPUVirtualAddress(), offset.MatBeginOffset + m_contants_size.MatCBSize);
	offset.VertexBeginOffset = offset.PassBeginOffset + m_contants_size.PassCBSize;
	offset.IndexBeginOffset = offset.VertexBeginOffset + m_contants_size.VertexCBSize;

	if (!CanFillFrameRes(m_contants_size, offset) || (m_frame_res_offset.size() >= MaxCommandAllocNum))
	{
		//不能填充数据或者命令队列不够用
		UINT64 completed_frame_index = mFence->GetCompletedValue();
		FreeMemToCompletedFrame(completed_frame_index);
		if (!m_frame_res_offset.empty())
		{
			auto cur_frame_resource = &m_frame_res_offset.back();
			while (!CanFillFrameRes(m_contants_size, offset) || (m_frame_res_offset.size() >= MaxCommandAllocNum))
			{
				if (cur_frame_resource->Fence != 0 && mFence->GetCompletedValue() < cur_frame_resource->Fence)
				{
					HANDLE eventHandle = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
					ThrowIfFailed(mFence->SetEventOnCompletion(cur_frame_resource->Fence, eventHandle));
					WaitForSingleObject(eventHandle, INFINITE);
					CloseHandle(eventHandle);
				}
				FreeMemToCompletedFrame(mFence->GetCompletedValue());
			}
		}
	}
	//LogDebug(" [Fill Frame Resource] size {} ", m_frame_res_offset.size());
	//压入队列
	
	offset.Fence = mCurrentFence;

	//copy data
	CopyFrameRescourceData(gt, offset);

	offset.EndResOffset = offset.IndexBeginOffset + m_contants_size.IndexCBSize;
	offset.EndResOffset %= mFrameResources->Size();
	m_frame_res_offset.push(offset);

	
}

bool CDeferredRenderPipeline::CanFillFrameRes(FrameResComponentSize& size, FrameResourceOffset& offset)
{
	if (m_frame_res_offset.empty())
	{
		return true;
	}

	if (m_frame_res_offset.back().EndResOffset + size.TotalSize <= mFrameResources->Size())
	{
		return true;
	}
	
	//现在object buffer也要是连续的, vertex buffer和index buffer必须是连续的,因为frame buffer实际上是一个松散的结构
	UINT tail_index = offset.ObjectBeginOffset + size.ObjectCBSize;
	if ( tail_index <= mFrameResources->Size())
	{
		//在Object区后还有位置
		offset.MatBeginOffset = tail_index;
		tail_index += size.MatCBSize;
		if (tail_index <= mFrameResources->Size())
		{
			tail_index = AlignForCrvAddress(mFrameResources->FrameResCB->Resource()->GetGPUVirtualAddress(), tail_index);
			offset.PassBeginOffset = tail_index;
			tail_index += size.PassCBSize;
			if (tail_index <= mFrameResources->Size())
			{
				offset.VertexBeginOffset = tail_index;
				tail_index += size.VertexCBSize;
				//Pass区后还有位置
				if (tail_index <= mFrameResources->Size())
				{
					offset.IndexBeginOffset = tail_index;
					tail_index += size.IndexCBSize;
					//Vertex之后还有位置
					if (tail_index <= mFrameResources->Size())
					{
						return true;
					}
					offset.IndexBeginOffset = 0;
					if (offset.IndexBeginOffset + size.IndexCBSize < m_frame_res_offset.front().ObjectBeginOffset)
					{
						return true;
					}
					return false;
				}
				else
				{
					offset.VertexBeginOffset = 0;
					offset.IndexBeginOffset = offset.VertexBeginOffset + size.VertexCBSize;
					if (offset.VertexBeginOffset + size.VertexCBSize + size.IndexCBSize < m_frame_res_offset.front().ObjectBeginOffset)
					{
						return true;
					}
					return false;
				}
			}
			else
			{
				//在Pass区换到头
				offset.PassBeginOffset = AlignForCrvAddress(mFrameResources->FrameResCB->Resource()->GetGPUVirtualAddress(), 0);
				if (offset.PassBeginOffset + size.PassCBSize + size.VertexCBSize + size.IndexCBSize >= m_frame_res_offset.front().ObjectBeginOffset)
				{
					return false;
				}
				offset.VertexBeginOffset = offset.PassBeginOffset + size.PassCBSize;
				offset.IndexBeginOffset = offset.VertexBeginOffset + size.VertexCBSize; 
				return true;
			}
		}
		else
		{
			offset.MatBeginOffset = 0;
			offset.PassBeginOffset = AlignForCrvAddress(mFrameResources->FrameResCB->Resource()->GetGPUVirtualAddress(), size.MatCBSize);
			if (offset.PassBeginOffset + size.PassCBSize + size.VertexCBSize + size.IndexCBSize >= m_frame_res_offset.front().ObjectBeginOffset)
			{
				return false;
			}
			offset.VertexBeginOffset = offset.PassBeginOffset + size.PassCBSize;
			offset.IndexBeginOffset = offset.VertexBeginOffset + size.VertexCBSize;
			return true;
		}
	}
	else
	{
		//object buffer也要是连续的
		offset.ObjectBeginOffset = 0;
		offset.MatBeginOffset = offset.ObjectBeginOffset + size.MatCBSize;
		offset.PassBeginOffset = AlignForCrvAddress(mFrameResources->FrameResCB->Resource()->GetGPUVirtualAddress(), m_frame_res_offset.back().EndResOffset + size.ObjectCBSize + size.MatCBSize);
		if (offset.PassBeginOffset + size.PassCBSize + size.VertexCBSize + size.IndexCBSize >= m_frame_res_offset.front().ObjectBeginOffset)
		{
			return false;
		}
		offset.VertexBeginOffset = offset.PassBeginOffset + size.PassCBSize;
		offset.IndexBeginOffset = offset.VertexBeginOffset + size.VertexCBSize; 
		return true;
	}


}

void CDeferredRenderPipeline::FreeMemToCompletedFrame(UINT64 frame_index)
{
	while (!m_frame_res_offset.empty() && m_frame_res_offset.front().Fence <= frame_index)
	{
		m_frame_res_offset.pop();
	}
}

void CDeferredRenderPipeline::CopyFrameRescourceData(const GameTimer& gt, const FrameResourceOffset& offset)
{
	CopyObjectCBAndVertexData(offset);
	CopyMatCBData(offset);
	CopyPassCBData(gt, offset);
}

void CDeferredRenderPipeline::CopyObjectCBAndVertexData(const FrameResourceOffset& offset)
{
	UINT objCBByteSize = sizeof(ObjectConstants);
	UINT vertexCBByteSize = sizeof(VertexData);
	UINT indexCBByteSize = sizeof(std::uint16_t);
	auto curr_cb = mFrameResources->FrameResCB.get();
	std::vector<RenderItem*> all_visible_objects;
	all_visible_objects.insert(all_visible_objects.end(), mRitemLayer[(int)RenderLayer::Occluder].begin(), mRitemLayer[(int)RenderLayer::Occluder].end());
	all_visible_objects.insert(all_visible_objects.end(), mRitemLayer[(int)RenderLayer::Opaque].begin(), mRitemLayer[(int)RenderLayer::Opaque].end());
	UINT64 object_offset = offset.ObjectBeginOffset;
	UINT64 vertex_offset = offset.VertexBeginOffset;
	UINT64 index_offset = offset.IndexBeginOffset;
	UINT start_vertex_index = 0;
	UINT start_index_index = 0;
	//LogDebug("Cur Fence : {} , Completed Fence : {}", offset.Fence, mFence->GetCompletedValue());
	for (int i = 0; i < all_visible_objects.size(); ++i)
	{
		auto& e = all_visible_objects[i];
		XMMATRIX world = XMLoadFloat4x4(&e->World);
		XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

		//copy vertex
		curr_cb->CopyData(vertex_offset, e->Data.Mesh.Vertices.data(), vertexCBByteSize * e->Data.Mesh.Vertices.size());
		vertex_offset += vertexCBByteSize * e->Data.Mesh.Vertices.size();
		e->BaseVertexLocation = start_vertex_index;
		start_vertex_index += e->Data.Mesh.Vertices.size();

		//copy index
		curr_cb->CopyData(index_offset, e->Data.Mesh.Indices.data(), indexCBByteSize * e->Data.Mesh.Indices.size());
		index_offset += indexCBByteSize * e->Data.Mesh.Indices.size();
		e->StartIndexLocation = start_index_index;
		start_index_index += e->Data.Mesh.Indices.size();
		e->IndexCount = e->Data.Mesh.Indices.size();

		//copy object data
		ObjectConstants objConstants;
		objConstants.Bounds.MaxVertex = e->Bounds.MaxVertex;
		objConstants.Bounds.MinVertex = e->Bounds.MinVertex;
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
		D3D12_GPU_VIRTUAL_ADDRESS address = curr_cb->Resource()->GetGPUVirtualAddress() + vertex_offset;
		objConstants.DrawCommand.drawArguments.InstanceCount = 1;
		objConstants.DrawCommand.drawArguments.StartInstanceLocation = 0;
		objConstants.DrawCommand.drawArguments.StartIndexLocation = e->StartIndexLocation;
		objConstants.DrawCommand.drawArguments.InstanceCount = e->Data.Mesh.Indices.size();
		objConstants.DrawCommand.drawArguments.BaseVertexLocation = e->BaseVertexLocation;
		if (NULL != e->Mat)
		{
			objConstants.MaterialIndex = e->Mat->MatCBIndex;
		}
			
		curr_cb->CopyData(object_offset , &objConstants, objCBByteSize );
		object_offset += objCBByteSize;

		
				
	}
}

void CDeferredRenderPipeline::CopyMatCBData(const FrameResourceOffset& offset)
{
	UINT matCBByteSize = sizeof(MatData);
	auto currMaterialBuffer = mFrameResources->FrameResCB.get() ;
	for (auto& e : mMaterials)
	{
		Material* mat = e.second;
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MatData matData;
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
			matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
			matData.NormalMapIndex = mat->NormalSrvHeapIndex;
			
			
			currMaterialBuffer->CopyData(mat->MatCBIndex * sizeof(MatData) + offset.MatBeginOffset, &matData, sizeof(MatData));

			mat->NumFramesDirty--;
		}
	}
}

void CDeferredRenderPipeline::CopyPassCBData(const GameTimer& gt, const FrameResourceOffset& offset)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX viewProjTex = XMMatrixMultiply(viewProj, T);
	XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProjTex, XMMatrixTranspose(viewProjTex));
	XMStoreFloat4x4(&mMainPassCB.ShadowTransform, XMMatrixTranspose(shadowTransform));
	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = mCamera.GetNearZ();
	mMainPassCB.FarZ = mCamera.GetFarZ();
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
	mMainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
	mMainPassCB.Lights[0].Strength = { 0.9f, 0.9f, 0.7f };
	mMainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
	mMainPassCB.Lights[1].Strength = { 0.4f, 0.4f, 0.4f };
	mMainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
	mMainPassCB.Lights[2].Strength = { 0.2f, 0.2f, 0.2f };
	mMainPassCB.ObjectNum = GetVisibleRenderItems().size();

	auto currPassCB = mFrameResources->FrameResCB.get();
	currPassCB->CopyData(offset.PassBeginOffset , &mMainPassCB, sizeof(PassConstants));
}

FrameResComponentSize CDeferredRenderPipeline::CalCurFrameContantsSize()
{
	FrameResComponentSize res;
	res.ObjectCBSize= mAllRitems.size() * sizeof(ObjectConstants);
	res.PassCBSize = sizeof(PassConstants);
	res.VertexCBSize = 0;
	res.IndexCBSize = 0;
	res.MatCBSize = mMaterials.size() * sizeof(MatData);
	for (int i=0; i<mAllRitems.size(); ++i)
	{
		res.VertexCBSize += mAllRitems[i]->Data.Mesh.Vertices.size() * sizeof(VertexData);
		res.IndexCBSize += mAllRitems[i]->Data.Mesh.Indices.size() * sizeof(std::uint16_t);
	}
	res.TotalSize = res.ObjectCBSize + res.PassCBSize + res.VertexCBSize + res.IndexCBSize + res.MatCBSize;
	return res;
}

void CDeferredRenderPipeline::HiZPass()
{
	if (mRitemLayer[(int)RenderLayer::Occluder].empty())
	{
		return;
	}

	//1、生成全屏的depth
	GenerateFullResDepthPass();
	//2、通过depth downsample采样得到hi-Z
	GenerateHiZBufferChainPass();
}

void CDeferredRenderPipeline::CreateHiZBuffer()
{
	auto clear_values = CD3DX12_CLEAR_VALUE(m_hiz_buffer_format, Colors::White);
	ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Tex2D(m_hiz_buffer_format, mClientWidth, mClientHeight, 1, GetHiZMipmapLevels(), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		&clear_values,
		IID_PPV_ARGS(&m_hiz_buffer)));

	m_hiz_buffer->SetName(L"HiZ Buffer");

	D3D12_RENDER_TARGET_VIEW_DESC rt_desc;
	rt_desc.Format = m_hiz_buffer_format;
	rt_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	rt_desc.Texture2D.MipSlice = 0;
	rt_desc.Texture2D.PlaneSlice = 0;
	CD3DX12_CPU_DESCRIPTOR_HANDLE h(CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(), SwapChainBufferCount + GBufferSize(), mRtvDescriptorSize));
	md3dDevice->CreateRenderTargetView(m_hiz_buffer.Get(), &rt_desc, h);

	

}

void CDeferredRenderPipeline::GenerateFullResDepthPass()
{
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_hiz_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET));
	mCommandList->SetGraphicsRootSignature(m_hiz_fullres_depth_pass_root_signature.Get());
	CD3DX12_CPU_DESCRIPTOR_HANDLE h_hiz(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	h_hiz.Offset(SwapChainBufferCount + GBufferSize(), mRtvDescriptorSize);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	mCommandList->ClearRenderTargetView(h_hiz, Colors::White, 0, nullptr);
	mCommandList->OMSetRenderTargets(1, &h_hiz, true, &DepthStencilView());

	auto passCB = mFrameResources->FrameResCB->Resource();
	UINT pass_offset = m_frame_res_offset.back().PassBeginOffset;
	mCommandList->SetGraphicsRootShaderResourceView(1, passCB->GetGPUVirtualAddress() + pass_offset);

	mCommandList->SetPipelineState(mPSOs["HiZFullRes"].Get());

 	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Occluder], (int)RenderLayer::Occluder);
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_hiz_buffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

}

void CDeferredRenderPipeline::GenerateHiZBufferChainPass()
{
	mCommandList->SetPipelineState(mPSOs["HiZChainBuffer"].Get());
	mCommandList->SetComputeRootSignature(m_hiz_buffer_chain_pass_root_signature.Get());
	
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_full_res(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_mipmap(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	h_full_res.Offset(mTextures.size() + GBufferSize(), mCbvSrvUavDescriptorSize);
	h_mipmap.Offset(mTextures.size() + GBufferSize() + 1, mCbvSrvUavDescriptorSize);

	struct TexelParam
	{
		TexelParam(FLOAT f) : Float(f) {}
		TexelParam(UINT u) : Uint(u) {}

		void operator= (FLOAT f) { Float = f; }
		void operator= (UINT u) { Uint = u; }

		union
		{
			FLOAT Float;
			UINT Uint;
		};
	};

	for (int i=0; i<GetHiZMipmapLevels() - 1; ++i)
	{
		mCommandList->SetComputeRootDescriptorTable(0, h_full_res);
		h_mipmap.Offset(1, mCbvSrvUavDescriptorSize);
		mCommandList->SetComputeRootDescriptorTable(1, h_mipmap);

		UINT width = mClientWidth >> (i+1);
		UINT height = mClientHeight >> (i+1);

		mCommandList->SetComputeRoot32BitConstant(2, TexelParam(1.0f / width).Uint, 0);
		mCommandList->SetComputeRoot32BitConstant(2, TexelParam(1.0f / height).Uint, 1);
		mCommandList->SetComputeRoot32BitConstant(2, i, 2);

		mCommandList->Dispatch(mClientWidth/8, mClientHeight/8, 1);
	}
}

void CDeferredRenderPipeline::BuildHiZRootSignature()
{
	BuildFullResDepthPassRootSignature();
	BuildHiZBufferChainPassRootSignature();
}

void CDeferredRenderPipeline::BuildFullResDepthPassRootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[2];

	slotRootParameter[0].InitAsShaderResourceView(0);
	slotRootParameter[1].InitAsShaderResourceView(1);

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter,
		NULL, NULL,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_hiz_fullres_depth_pass_root_signature.GetAddressOf())));
}

void CDeferredRenderPipeline::BuildHiZBufferChainPassRootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];
	CD3DX12_DESCRIPTOR_RANGE input_buffer_table;
	input_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE output_buffer_table;
	output_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

	slotRootParameter[0].InitAsDescriptorTable(1, &input_buffer_table);
	slotRootParameter[1].InitAsDescriptorTable(1, &output_buffer_table);
	slotRootParameter[2].InitAsConstants(3, 0);

	const CD3DX12_STATIC_SAMPLER_DESC sampler_desc(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
		1, &sampler_desc,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	
	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_hiz_buffer_chain_pass_root_signature.GetAddressOf())));
}

void CDeferredRenderPipeline::BuildHiZPSO()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC fullres_pso_desc;
	ZeroMemory(&fullres_pso_desc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	fullres_pso_desc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	fullres_pso_desc.pRootSignature = m_hiz_fullres_depth_pass_root_signature.Get();
	fullres_pso_desc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["HiZVS"]->GetBufferPointer()),
		mShaders["HiZVS"]->GetBufferSize()
	};
	fullres_pso_desc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["HiZPS"]->GetBufferPointer()),
		mShaders["HiZPS"]->GetBufferSize()
	};
	fullres_pso_desc.NumRenderTargets = 1;
	fullres_pso_desc.RTVFormats[0] = m_hiz_buffer_format;

	fullres_pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	fullres_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	fullres_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	fullres_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	fullres_pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	fullres_pso_desc.SampleMask = UINT_MAX;
	fullres_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	fullres_pso_desc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	fullres_pso_desc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	fullres_pso_desc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&fullres_pso_desc, IID_PPV_ARGS(&mPSOs["HiZFullRes"])));

	D3D12_COMPUTE_PIPELINE_STATE_DESC chainbuffer_pso_desc;
	ZeroMemory(&chainbuffer_pso_desc, sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC));
	chainbuffer_pso_desc.pRootSignature = m_hiz_buffer_chain_pass_root_signature.Get();
	chainbuffer_pso_desc.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["HiZCS"]->GetBufferPointer()),
		mShaders["HiZCS"]->GetBufferSize()
	};
	chainbuffer_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&chainbuffer_pso_desc, IID_PPV_ARGS(&mPSOs["HiZChainBuffer"])));
}

int CDeferredRenderPipeline::GetRenderLayerObjectOffset(int layer)
{
	switch (layer)
	{
	case (int)RenderLayer::Occluder:
		return 0;
	case (int)RenderLayer::Opaque:
		return mRitemLayer[(int)RenderLayer::Occluder].size();
	default:
		break;
	}
	return 0;
}

UINT CDeferredRenderPipeline::GetHiZMipmapLevels() const
{
	return log2(mClientWidth / HiZBufferMinSize) + 1;
}

void CDeferredRenderPipeline::InstanceHiZCullingPass()
{
	
	//根据instance的包围盒结合HiZ进行剔除
	mCommandList->SetPipelineState(mPSOs["HiZInstanceCulling"].Get());
	mCommandList->SetComputeRootSignature(m_hiz_instance_culling_pass_root_signature.Get());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_instance_culling_result_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));
	//重置数据
	mCommandList->CopyBufferRegion(m_instance_culling_result_buffer.Get(), CullingResMaxObjSize, m_counter_reset_buffer.Get(), 0, sizeof(UINT));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_instance_culling_result_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	//绑定描述符
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_input_hiz(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_output_culling(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	h_input_hiz.Offset(mTextures.size() + GBufferSize() + 1, mCbvSrvUavDescriptorSize);
	h_output_culling.Offset(mTextures.size() + GBufferSize() + 1 + GetHiZMipmapLevels(), mCbvSrvUavDescriptorSize);

	mCommandList->SetComputeRootDescriptorTable(1, h_input_hiz);
	mCommandList->SetComputeRootDescriptorTable(3, h_output_culling);

	auto cur_cb = mFrameResources->FrameResCB->Resource();
	auto cur_offset = m_frame_res_offset.back();

	//动态绑定ring buffer中的资源
	m_obj_handle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	m_obj_handle.Offset(m_descriptor_end + HO_Object, mCbvSrvUavDescriptorSize);
	D3D12_SHADER_RESOURCE_VIEW_DESC obj_srv_desc = {};
	obj_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	obj_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
	obj_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	obj_srv_desc.Buffer.FirstElement = cur_offset.ObjectBeginOffset / sizeof(ObjectConstants) + GetRenderLayerObjectOffset((int)RenderLayer::Opaque);
	obj_srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	obj_srv_desc.Buffer.NumElements = ScenePredefine::MaxObjectNumPerScene;
	obj_srv_desc.Buffer.StructureByteStride = sizeof(ObjectConstants);
	md3dDevice->CreateShaderResourceView(mFrameResources->FrameResCB->Resource(), &obj_srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_descriptor_end + HO_Object, mCbvSrvUavDescriptorSize));

	mCommandList->SetComputeRootConstantBufferView(0, cur_cb->GetGPUVirtualAddress() + cur_offset.PassBeginOffset);
	mCommandList->SetComputeRootDescriptorTable(2, m_obj_handle);
	UINT size = GetVisibleRenderItems().size() / BufferThreadSize;
	size += (GetVisibleRenderItems().size() % BufferThreadSize == 0) ? 0 : 1;
	mCommandList->Dispatch(max(1, size), 1, 1);
}

void CDeferredRenderPipeline::ClusterHiZCullingPass()
{
	//根据instance的包围盒结合HiZ进行剔除
	mCommandList->SetPipelineState(mPSOs["HiZClusterCulling"].Get());
	mCommandList->SetComputeRootSignature(m_hiz_cluster_culling_pass_root_signature.Get());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_cluster_culling_result_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));
	//重置数据
	mCommandList->CopyBufferRegion(m_cluster_culling_result_buffer.Get(), ClusterCullingResMaxSize, m_counter_reset_buffer.Get(), 0, sizeof(UINT));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_cluster_culling_result_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	//绑定描述符
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_input_cluster(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_output_culling(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	h_input_cluster.Offset(mTextures.size() + GBufferSize() + 1 + GetHiZMipmapLevels() + 1, mCbvSrvUavDescriptorSize);
	h_output_culling.Offset(mTextures.size() + GBufferSize() + 1 + GetHiZMipmapLevels() + 1 + 1, mCbvSrvUavDescriptorSize);

	mCommandList->SetComputeRootDescriptorTable(0, h_input_cluster);
	mCommandList->SetComputeRootDescriptorTable(6, h_output_culling);

	auto cur_cb = mFrameResources->FrameResCB->Resource();
	auto cur_offset = m_frame_res_offset.back();
	
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_hiz(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	h_hiz.Offset(mTextures.size() + GBufferSize()+1, mCbvSrvUavDescriptorSize);
	mCommandList->SetComputeRootDescriptorTable(1, m_obj_handle);
	mCommandList->SetComputeRootDescriptorTable(2, h_hiz);
	mCommandList->SetComputeRootConstantBufferView(3, cur_cb->GetGPUVirtualAddress() + cur_offset.PassBeginOffset);
	
	//动态绑定vertex buffer 和index buffer
	D3D12_SHADER_RESOURCE_VIEW_DESC vertex_srv_desc = {};
	vertex_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	vertex_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
	vertex_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	vertex_srv_desc.Buffer.FirstElement = cur_offset.VertexBeginOffset / sizeof(VertexData) + GetRenderLayerObjectOffset((int)RenderLayer::Opaque);
	vertex_srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	vertex_srv_desc.Buffer.NumElements = m_contants_size.VertexCBSize / sizeof(VertexData);
	vertex_srv_desc.Buffer.StructureByteStride = sizeof(VertexData);
	md3dDevice->CreateShaderResourceView(mFrameResources->FrameResCB->Resource(), &vertex_srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_descriptor_end + HO_Vertex, mCbvSrvUavDescriptorSize));

	D3D12_SHADER_RESOURCE_VIEW_DESC index_srv_desc = {};
	index_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	index_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
	index_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	index_srv_desc.Buffer.FirstElement = cur_offset.IndexBeginOffset / sizeof(std::uint16_t) + GetRenderLayerObjectOffset((int)RenderLayer::Opaque);
	index_srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	index_srv_desc.Buffer.NumElements = m_contants_size.IndexCBSize / sizeof(std::uint16_t);
	index_srv_desc.Buffer.StructureByteStride = sizeof(std::uint16_t);
	md3dDevice->CreateShaderResourceView(mFrameResources->FrameResCB->Resource(), &index_srv_desc, CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_descriptor_end + HO_Index, mCbvSrvUavDescriptorSize));

	CD3DX12_GPU_DESCRIPTOR_HANDLE h_vertex_index(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	h_vertex_index.Offset(m_descriptor_end + HO_Vertex, mCbvSrvUavDescriptorSize);
	mCommandList->SetComputeRootDescriptorTable(4, h_vertex_index);
	mCommandList->SetComputeRootDescriptorTable(5, h_vertex_index.Offset(1, mCbvSrvUavDescriptorSize));

	mCommandList->SetComputeRootConstantBufferView(7, m_chunk_expan_result_buffer->GetGPUVirtualAddress() + ChunkExpanMaxSize);

	UINT size = ClusterCullingResMaxSize / BufferThreadSize;
	size += (ClusterCullingResMaxSize % BufferThreadSize == 0) ? 0 : 1;
	mCommandList->Dispatch(max(1, size), 1, 1);
}

void CDeferredRenderPipeline::BuildClusterHiZCullingPSO()
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC hiz_cluster_culling_pso_desc;
	ZeroMemory(&hiz_cluster_culling_pso_desc, sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC));
	hiz_cluster_culling_pso_desc.pRootSignature = m_hiz_cluster_culling_pass_root_signature.Get();
	hiz_cluster_culling_pso_desc.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["HiZClusterCulling"]->GetBufferPointer()),
		mShaders["HiZClusterCulling"]->GetBufferSize()
	};
	hiz_cluster_culling_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&hiz_cluster_culling_pso_desc, IID_PPV_ARGS(&mPSOs["HiZClusterCulling"])));
}

void CDeferredRenderPipeline::BuildClusterHiZCullingRootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[8];
	//cluster chunk buffer (consume buffer)
	CD3DX12_DESCRIPTOR_RANGE cluster_chunk_buffer;
	cluster_chunk_buffer.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	
	//object buffer
	CD3DX12_DESCRIPTOR_RANGE object_buffer;
	object_buffer.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	//hi z
	CD3DX12_DESCRIPTOR_RANGE hiz_buffer;
	hiz_buffer.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

	//pass buffer,constant buffer

	//vertex buffer
	CD3DX12_DESCRIPTOR_RANGE vertex_buffer_table;
	vertex_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
	//index buffer
	CD3DX12_DESCRIPTOR_RANGE index_buffer_table;
	index_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
	
	//out put buffer
	CD3DX12_DESCRIPTOR_RANGE output_buffer;
	output_buffer.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);

	//counter buffer

	slotRootParameter[0].InitAsDescriptorTable(1, &cluster_chunk_buffer);
	slotRootParameter[1].InitAsDescriptorTable(1, &object_buffer);
	slotRootParameter[2].InitAsDescriptorTable(1, &hiz_buffer);
	slotRootParameter[3].InitAsConstantBufferView(0);
	slotRootParameter[4].InitAsDescriptorTable(1, &vertex_buffer_table);
	slotRootParameter[5].InitAsDescriptorTable(1, &index_buffer_table);
	slotRootParameter[6].InitAsDescriptorTable(1, &output_buffer);
	slotRootParameter[7].InitAsConstantBufferView(1);

	const CD3DX12_STATIC_SAMPLER_DESC sampler_desc(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(8, slotRootParameter,
		1, &sampler_desc,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_hiz_cluster_culling_pass_root_signature.GetAddressOf())));
}

void CDeferredRenderPipeline::CreateHIZClusterCullingBuffers()
{
	//instance culling result
	//buffer layout : N * ObjectConstants + Counter
	ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(ClusterCullingResMaxSize + sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&m_cluster_culling_result_buffer)));

	m_cluster_culling_result_buffer->SetName(L"Cluster-Culling-Result-Buffer");
}

void CDeferredRenderPipeline::BuildHiZInstanceCullingRootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];
	CD3DX12_DESCRIPTOR_RANGE hiz_buffer_table;
	hiz_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0); 
	CD3DX12_DESCRIPTOR_RANGE obj_buffer_table;
	obj_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0);
	CD3DX12_DESCRIPTOR_RANGE output_buffer_table;
	output_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0);
	
	//pass buffer
	slotRootParameter[0].InitAsConstantBufferView(0);

	//hi z buffer
	slotRootParameter[1].InitAsDescriptorTable(1, &hiz_buffer_table);
	//object buffer
	slotRootParameter[2].InitAsDescriptorTable(1, &obj_buffer_table);
	//output buffer
	slotRootParameter[3].InitAsDescriptorTable(1, &output_buffer_table);

	const CD3DX12_STATIC_SAMPLER_DESC sampler_desc(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
		1, &sampler_desc,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_hiz_instance_culling_pass_root_signature.GetAddressOf())));

}

void CDeferredRenderPipeline::BuildHiZInstanceCullingPSO()
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC hiz_instance_culling_pso_desc = {};
	hiz_instance_culling_pso_desc.pRootSignature = m_hiz_instance_culling_pass_root_signature.Get();
	hiz_instance_culling_pso_desc.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["HiZInstanceCulling"]->GetBufferPointer()),
		mShaders["HiZInstanceCulling"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&hiz_instance_culling_pso_desc, IID_PPV_ARGS(&mPSOs["HiZInstanceCulling"])));

	
}

void CDeferredRenderPipeline::CreateHiZInstanceCullingBuffers()
{
	//instance culling result
	//buffer layout : N * ObjectConstants + Counter
	ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(CullingResMaxObjSize + sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&m_instance_culling_result_buffer)));

	m_instance_culling_result_buffer->SetName(L"HiZ-Instance-Culling-Result-Buffer");

	//count null的buffer
	ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT)),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_counter_reset_buffer)));

	m_counter_reset_buffer->SetName(L"HiZ result reset buffer");

	D3D12_RANGE zero_range = { 0, 0 };
	UINT8* null_data = nullptr;
	m_counter_reset_buffer->Map(0, &zero_range, reinterpret_cast<void**> (&null_data));
	ZeroMemory(&null_data, sizeof(UINT));
	m_counter_reset_buffer->Unmap(0, nullptr);
}

UINT  CDeferredRenderPipeline::AlignForUavCounter(UINT bufferSize)
{
	const UINT alignment = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
	return (bufferSize + (alignment - 1)) & ~(alignment - 1);
}

UINT64 CDeferredRenderPipeline::AlignForCrvAddress(const D3D12_GPU_VIRTUAL_ADDRESS& address, const UINT& offset)
{
	auto real_address = address + offset;
	const UINT alignment = 256;
	real_address = (real_address + (alignment - 1)) & ~(alignment - 1);
	return real_address - address;
}

UINT CDeferredRenderPipeline::Align(const UINT& size, const UINT& alignment)
{
	UINT count = size / alignment;
	count += (size % alignment == 0) ? 0 : 1;
	
	return count * alignment;
}

std::vector<RenderItem*> CDeferredRenderPipeline::GetVisibleRenderItems()
{
	return mRitemLayer[(int)RenderLayer::Opaque];
}

void CDeferredRenderPipeline::ChunkExpanPass()
{
	//主要是根据instance culling的chunk 扩展出cluster culling pass的输入数据
	//根据instance的包围盒结合HiZ进行剔除
	mCommandList->SetPipelineState(mPSOs["ChunkExpan"].Get());
	mCommandList->SetComputeRootSignature(m_chunk_expan_pass_root_signature.Get());

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_chunk_expan_result_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));
	//重置数据
	mCommandList->CopyBufferRegion(m_chunk_expan_result_buffer.Get(), ChunkExpanMaxSize, m_counter_reset_buffer.Get(), 0, sizeof(UINT));
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_chunk_expan_result_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	//绑定描述符
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_input_instance_culling(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	CD3DX12_GPU_DESCRIPTOR_HANDLE h_output_culling(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	h_input_instance_culling.Offset(mTextures.size() + GBufferSize() + 1 + GetHiZMipmapLevels(), mCbvSrvUavDescriptorSize);
	h_output_culling.Offset(mTextures.size() + GBufferSize() + 1 + GetHiZMipmapLevels() + 1, mCbvSrvUavDescriptorSize);

	mCommandList->SetComputeRootDescriptorTable(1, h_input_instance_culling);
	mCommandList->SetComputeRootDescriptorTable(3, h_output_culling);

	auto cur_cb = mFrameResources->FrameResCB->Resource();
	auto cur_offset = m_frame_res_offset.back();

	mCommandList->SetComputeRootConstantBufferView(0, m_instance_culling_result_buffer->GetGPUVirtualAddress() + CullingResMaxObjSize);
	mCommandList->SetComputeRootDescriptorTable(2, m_obj_handle);


	UINT size = CullingResMaxObjSize / BufferThreadSize;
	size += (CullingResMaxObjSize % BufferThreadSize == 0) ? 0 : 1;
	mCommandList->Dispatch(max(1, size), 1, 1);
}

void CDeferredRenderPipeline::BuildChunkExpanRootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];
	
	CD3DX12_DESCRIPTOR_RANGE instance_culling_res_buffer_table;
	instance_culling_res_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
	CD3DX12_DESCRIPTOR_RANGE obj_buffer_table;
	obj_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0);
	CD3DX12_DESCRIPTOR_RANGE output_buffer_table;
	output_buffer_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0);

	//count buffer
	slotRootParameter[0].InitAsConstantBufferView(0);
	//instance culling res buffer
	slotRootParameter[1].InitAsDescriptorTable(1, &instance_culling_res_buffer_table);
	//object buffer
	slotRootParameter[2].InitAsDescriptorTable(1, &obj_buffer_table);
	//output buffer
	slotRootParameter[3].InitAsDescriptorTable(1, &output_buffer_table);


	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
		0, NULL,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_chunk_expan_pass_root_signature.GetAddressOf())));
}

void CDeferredRenderPipeline::BuildChunkExpanPSO()
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC chunk_expan_pso_desc = {};
	chunk_expan_pso_desc.pRootSignature = m_chunk_expan_pass_root_signature.Get();
	chunk_expan_pso_desc.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["ChunkExpan"]->GetBufferPointer()),
		mShaders["ChunkExpan"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&chunk_expan_pso_desc, IID_PPV_ARGS(&mPSOs["ChunkExpan"])));
}

void CDeferredRenderPipeline::CreateChunExpanBuffer()
{
	ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(ChunkExpanMaxSize + sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&m_chunk_expan_result_buffer)));

	m_chunk_expan_result_buffer->SetName(L"Chunk-Expan-Result-Buffer");
}

