#include "ZBufferRenderPipeline.h"
#include <iostream>

#include "CBaseRenderPipeline.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "../SSAO/Ssao.h"
#include "../ShadowMap/ShadowMap.h"
#include "../RenderItemUtil/RenderItemUtil.h"
#include "../Predefines/BufferPredefines.h"

extern const int gNumFrameResources;

CZBufferRenderPipeline::CZBufferRenderPipeline(HINSTANCE hInstance, HWND wnd)
	: CBaseRenderPipeline(hInstance, wnd)
{
	// Estimate the scene bounding sphere manually since we know how the scene was constructed.
	// The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
	// the world space origin.  In general, you need to loop over every world space vertex
	// position and compute the bounding sphere.
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(10.0f * 10.0f + 15.0f * 15.0f);
}

CZBufferRenderPipeline::~CZBufferRenderPipeline()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool CZBufferRenderPipeline::Initialize()
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



void CZBufferRenderPipeline::PushModels(std::vector<RenderItem*>& render_items)
{
	FlushCommandQueue();
	ThrowIfFailed(mDirectCmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	PushRenderItems(render_items);
	PushMats(render_items);

	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmd_lists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmd_lists), cmd_lists);
	FlushCommandQueue();
}



void CZBufferRenderPipeline::PitchCamera(float rad)
{
	mCamera.Pitch(rad);
}

void CZBufferRenderPipeline::RotateCameraY(float rad)
{
	mCamera.RotateY(rad);
}

void CZBufferRenderPipeline::MoveCamera(float dis)
{
	mCamera.Walk(dis);
}

void CZBufferRenderPipeline::StrafeCamera(float dis)
{
	mCamera.Strafe(dis);
}

void CZBufferRenderPipeline::CreateRtvAndDsvDescriptorHeaps()
{
	CBaseRenderPipeline::CreateRtvAndDsvDescriptorHeaps();
}

void CZBufferRenderPipeline::OnResize()
{
	CBaseRenderPipeline::OnResize();

	mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 10000.0f);
	mCamera.SetPosition(0.0f, 500.0f, 1500.0f);
	mCamera.LookAt(mCamera.GetPosition3f(), XMFLOAT3(0, 0, 0), XMFLOAT3(0, 1, 0));
}

void CZBufferRenderPipeline::Update(const GameTimer& gt)
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

void CZBufferRenderPipeline::Draw(const GameTimer& gt)
{
	DrawWithZBuffer(gt);
}

void CZBufferRenderPipeline::UpdateCamera(const GameTimer& gt)
{
	mCamera.UpdateViewMatrix();
}

void CZBufferRenderPipeline::DrawWithZBuffer(const GameTimer& gt)
{
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % MaxCommandAllocNum;
	auto cmdListAlloc = mFrameResources->CmdListAlloc[mCurrFrameResourceIndex];

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());


	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear the back buffer and depth buffer.
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	auto passCB = mFrameResources->FrameResCB->Resource();
	UINT pass_offset = m_frame_res_offset.back().PassBeginOffset;
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress() + pass_offset);

	mCommandList->SetPipelineState(mPSOs["opaque"].Get());
	// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
	// set as a root descriptor.
	if (NULL != mFrameResources->MatCB)
	{
		auto matBuffer = mFrameResources->MatCB->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());
	}
	if (0 != mTextures.size())
	{
		mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	}
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
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

std::vector<RenderItem*>& CZBufferRenderPipeline::GetRenderItems(int layer)
{
	return mRitemLayer[layer];
}

DirectX::XMFLOAT3 CZBufferRenderPipeline::GetCameraPos()
{
	DirectX::XMFLOAT3 pos;
	XMStoreFloat3(&pos, mCamera.GetPosition());
	return pos;
}

BoundingFrustum CZBufferRenderPipeline::GetCameraFrustum()
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

DirectX::XMFLOAT3 CZBufferRenderPipeline::GetCameraDir()
{
	DirectX::XMFLOAT3 dir;
	auto vec_dir = mCamera.GetLook() - mCamera.GetPosition();
	XMStoreFloat3(&dir, vec_dir);
	return dir;
}

void CZBufferRenderPipeline::ClearVisibleRenderItems()
{
	for (int i = 0; i < (int)RenderLayer::Count; ++i)
	{
		mRitemLayer[i].clear();
	}
}

void CZBufferRenderPipeline::PushVisibleModels(int layer, std::vector<RenderItem*>& render_items, bool add )
{
	if (add)
	{
		mRitemLayer[layer].insert(mRitemLayer[layer].end(), render_items.begin(), render_items.end());
	}
	else
	{
		mRitemLayer[layer] = render_items;
	}
}

bool CZBufferRenderPipeline::IsCameraDirty()
{
	return mCamera.Dirty();
}

bool CZBufferRenderPipeline::InitDirect3D()
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

void CZBufferRenderPipeline::BuildRootSignature()
{
	BuildZbufferRootSignature();
}

void CZBufferRenderPipeline::BuildDescriptorHeaps()
{
	//+2 for g-buffers
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = mTextures.size();
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

}

void CZBufferRenderPipeline::BuildShadersAndInputLayout()
{
	mShaders["standardVS"] = d3dUtil::CompileShader(L".\\Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L".\\Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

}


void CZBufferRenderPipeline::BuildPSOs()
{
	BuildZBufferPSO();
}

void CZBufferRenderPipeline::BuildZBufferPSO()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
	ZeroMemory(&pso_desc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	pso_desc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	pso_desc.pRootSignature = mRootSignature.Get();
	pso_desc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	pso_desc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = mBackBufferFormat;

	pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	pso_desc.RasterizerState.FrontCounterClockwise = false;
	pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	pso_desc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	pso_desc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	pso_desc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&mPSOs["opaque"])));
}

void CZBufferRenderPipeline::BuildFrameResources()
{
	mFrameResources = std::make_unique<FrameResource>(md3dDevice.Get(), mMaterials.size());
}

void CZBufferRenderPipeline::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	if (ritems.empty())
	{
		return;
	}
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT skinnedCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(SkinnedConstants));

	auto objectCB = mFrameResources->FrameResCB->Resource();
	UINT offset = m_frame_res_offset.back().ObjectBeginOffset;
	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		UINT object_offset = (offset + i * objCBByteSize) % mFrameResources->Size();
		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + object_offset;

		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

void CZBufferRenderPipeline::PushRenderItems(std::vector<RenderItem*>& render_items)
{

	RenderItemUtil::FillGeoData(render_items, md3dDevice.Get(), mCommandList.Get());

// 	auto& opaque_items = mRitemLayer[(int)RenderLayer::Opaque];
// 	opaque_items.insert(opaque_items.end(), render_items.begin(), render_items.end());

	mAllRitems.insert(mAllRitems.end(), render_items.begin(), render_items.end());
}

void CZBufferRenderPipeline::PushMats(std::vector<RenderItem*>& render_items)
{
	//LoadTexture
	int tex_index = 0;
	std::unordered_map<std::string, int> tex_indices;
	for (int i = 0; i < render_items.size(); ++i)
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

}

CD3DX12_CPU_DESCRIPTOR_HANDLE CZBufferRenderPipeline::GetCpuSrv(int index)const
{
	auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE CZBufferRenderPipeline::GetGpuSrv(int index)const
{
	auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CZBufferRenderPipeline::GetDsv(int index)const
{
	auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
	dsv.Offset(index, mDsvDescriptorSize);
	return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CZBufferRenderPipeline::GetRtv(int index)const
{
	auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtv.Offset(index, mRtvDescriptorSize);
	return rtv;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> CZBufferRenderPipeline::GetStaticSamplers()
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

void CZBufferRenderPipeline::BuildZbufferRootSignature()
{
	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];

	CD3DX12_DESCRIPTOR_RANGE tex_table;
	tex_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 48, 0, 0);

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstantBufferView(0);
	slotRootParameter[1].InitAsConstantBufferView(1);
	slotRootParameter[2].InitAsShaderResourceView(0, 1);
	slotRootParameter[3].InitAsDescriptorTable(1, &tex_table, D3D12_SHADER_VISIBILITY_PIXEL);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
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
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void CZBufferRenderPipeline::UpdateFrameResource(const GameTimer& gt)
{

	//填充数据到frame res offset queue中
	UINT contants_size = CalCurFrameContantsSize();
	if (!CanFillFrameRes(contants_size) || (m_frame_res_offset.size() >= MaxCommandAllocNum))
	{
		//不能填充数据或者命令队列不够用
		UINT64 completed_frame_index = mFence->GetCompletedValue();
		FreeMemToCompletedFrame(completed_frame_index);
		if (!m_frame_res_offset.empty())
		{
			auto cur_frame_resource = &m_frame_res_offset.back();
			while (!CanFillFrameRes(contants_size) || (m_frame_res_offset.size() >= MaxCommandAllocNum))
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
	FrameResourceOffset  offset;
	offset.Fence = mCurrentFence;

	//copy data
	CopyFrameRescourceData(gt, offset);

	offset.EndResOffset = m_frame_res_offset.empty() ? contants_size : m_frame_res_offset.back().EndResOffset + contants_size;
	offset.EndResOffset %= mFrameResources->Size();
	offset.Size = contants_size;
	m_frame_res_offset.push(offset);


}

bool CZBufferRenderPipeline::CanFillFrameRes(UINT size)
{
	if (m_frame_res_offset.empty())
	{
		return true;
	}

	if (m_frame_res_offset.back().EndResOffset + size < mFrameResources->Size())
	{
		return true;
	}

	int extra_size = size - m_frame_res_offset.back().EndResOffset;

	if (extra_size <= m_frame_res_offset.front().ObjectBeginOffset)
	{
		return true;
	}
	return false;
}

void CZBufferRenderPipeline::FreeMemToCompletedFrame(UINT64 frame_index)
{
	while (!m_frame_res_offset.empty() && m_frame_res_offset.front().Fence <= frame_index)
	{
		m_frame_res_offset.pop();
	}
}

void CZBufferRenderPipeline::CopyFrameRescourceData(const GameTimer& gt, FrameResourceOffset& offset)
{
	CopyMatCBData();

	UINT begin_index = m_frame_res_offset.empty() ? 0 : m_frame_res_offset.back().EndResOffset;

	offset.ObjectBeginOffset = begin_index;
	CopyObjectCBData(begin_index);

	offset.PassBeginOffset = begin_index;
	CopyPassCBData(gt, begin_index);
}

void CZBufferRenderPipeline::CopyObjectCBData(UINT& begin_index)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	auto currObjectCB = mFrameResources->FrameResCB.get();
	for (int i = 0; i < mRitemLayer[(int)RenderLayer::Opaque].size(); ++i)
	{
		auto& e = mRitemLayer[(int)RenderLayer::Opaque][i];
		//if (e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			if (NULL != e->Mat)
			{
				objConstants.MaterialIndex = e->Mat->MatCBIndex;
			}

			currObjectCB->CopyData(begin_index / UploadBufferChunkSize, &objConstants, objCBByteSize / UploadBufferChunkSize);

			e->NumFramesDirty--;
		}
		if ((mAllRitems.size() - 1) != i)
		{
			begin_index += objCBByteSize;
			begin_index %= mFrameResources->Size();
		}
	}
}

void CZBufferRenderPipeline::CopyMatCBData()
{
	UINT matCBByteSize = sizeof(MatData);
	auto currMaterialBuffer = mFrameResources->MatCB.get();
	const auto& end_itr = *mMaterials.cbegin();
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


			currMaterialBuffer->CopyData(mat->MatCBIndex, &matData, 1);

			mat->NumFramesDirty--;
		}
	}
}

void CZBufferRenderPipeline::CopyPassCBData(const GameTimer& gt, UINT& begin_index)
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
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
	mMainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
	mMainPassCB.Lights[0].Strength = { 0.9f, 0.9f, 0.7f };
	mMainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
	mMainPassCB.Lights[1].Strength = { 0.4f, 0.4f, 0.4f };
	mMainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
	mMainPassCB.Lights[2].Strength = { 0.2f, 0.2f, 0.2f };

	auto currPassCB = mFrameResources->FrameResCB.get();
	currPassCB->CopyData(begin_index / UploadBufferChunkSize, &mMainPassCB, d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants)) / UploadBufferChunkSize);
}

UINT CZBufferRenderPipeline::CalCurFrameContantsSize()
{
	UINT object_size = mRitemLayer[(int)RenderLayer::Opaque].size() * d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT pass_size = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

	return object_size + pass_size;
}