#include "VoidEngineImp.h"
#include <iostream>

#include "CBaseEngine.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "../SSAO/Ssao.h"
#include "../ShadowMap/ShadowMap.h"
#include "../RenderItemUtil/RenderItemUtil.h"

const int gNumFrameResources = 3;

CVoidEgine::CVoidEgine(HINSTANCE hInstance, HWND wnd)
	: CBaseEngine(hInstance, wnd)
{
	// Estimate the scene bounding sphere manually since we know how the scene was constructed.
	// The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
	// the world space origin.  In general, you need to loop over every world space vertex
	// position and compute the bounding sphere.
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(10.0f * 10.0f + 15.0f * 15.0f);
	m_use_deferred_texturing = true;
}

CVoidEgine::~CVoidEgine()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

bool CVoidEgine::Initialize()
{
	if (!CBaseEngine::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mCamera.SetPosition(0.0f, 500.0f, 1500.0f);
	mCamera.LookAt(mCamera.GetPosition3f(), XMFLOAT3(0, 0, 0), XMFLOAT3(0, 1, 0));
	mCamera.UpdateViewMatrix();

	BuildRootSignature();
	BuildDescriptorHeaps();
	BuildShadersAndInputLayout();
	BuildFrameResources();
	BuildPSOs();


	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}



void CVoidEgine::PushModels(std::vector<RenderItem*>& render_items)
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



void CVoidEgine::CreateRtvAndDsvDescriptorHeaps()
{
	if (m_use_deferred_texturing)
	{
		//+2 for G-Buffers
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
		rtvHeapDesc.NumDescriptors = SwapChainBufferCount + GBufferSize();
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
	}
	else
	{
		CBaseEngine::CreateRtvAndDsvDescriptorHeaps();
	}
	
	
}

void CVoidEgine::OnResize()
{
	CBaseEngine::OnResize();

	mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 10000.0f);

}

void CVoidEgine::Update(const GameTimer& gt)
{
	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	//
	// Animate the lights (and hence shadows).
	//

	mLightRotationAngle += 0.1f * gt.DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateMainPassCB(gt);
}

void CVoidEgine::Draw(const GameTimer& gt)
{
	if (m_use_deferred_texturing)
	{
		DrawWithDeferredTexturing(gt);
	}
	else
	{
		DrawWithZBuffer(gt);
	}
	
}

void CVoidEgine::DrawWithZBuffer(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

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

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	mCommandList->SetPipelineState(mPSOs["opaque"].Get());
	// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
	// set as a root descriptor.
	if (NULL != mCurrFrameResource->MaterialBuffer)
	{
		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
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
	mCurrFrameResource->Fence = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point. 
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void CVoidEgine::DrawWithDeferredTexturing(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

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

	DeferredDrawFillGBufferPass();
	DeferredDrawShadingPass();

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Swap the back and front buffers
	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	// Advance the fence value to mark commands up to this fence point.
	mCurrFrameResource->Fence = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point. 
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void CVoidEgine::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if (e->NumFramesDirty > 0)
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

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void CVoidEgine::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for (auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
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

			currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void CVoidEgine::UpdateShadowTransform(const GameTimer& gt)
{
	// Only the first "main" light casts a shadow.
	XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
	XMVECTOR lightPos = -2.0f * mSceneBounds.Radius * lightDir;
	XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

	XMStoreFloat3(&mLightPosW, lightPos);

	// Transform bounding sphere to light space.
	XMFLOAT3 sphereCenterLS;
	XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

	// Ortho frustum in light space encloses scene.
	float l = sphereCenterLS.x - mSceneBounds.Radius;
	float b = sphereCenterLS.y - mSceneBounds.Radius;
	float n = sphereCenterLS.z - mSceneBounds.Radius;
	float r = sphereCenterLS.x + mSceneBounds.Radius;
	float t = sphereCenterLS.y + mSceneBounds.Radius;
	float f = sphereCenterLS.z + mSceneBounds.Radius;

	mLightNearZ = n;
	mLightFarZ = f;
	XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX S = lightView * lightProj * T;
	XMStoreFloat4x4(&mLightView, lightView);
	XMStoreFloat4x4(&mLightProj, lightProj);
	XMStoreFloat4x4(&mShadowTransform, S);
}

void CVoidEgine::UpdateMainPassCB(const GameTimer& gt)
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

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void CVoidEgine::UpdateShadowPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mLightView);
	XMMATRIX proj = XMLoadFloat4x4(&mLightProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	UINT w = mShadowMap->Width();
	UINT h = mShadowMap->Height();

	XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mShadowPassCB.EyePosW = mLightPosW;
	mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
	mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
	mShadowPassCB.NearZ = mLightNearZ;
	mShadowPassCB.FarZ = mLightFarZ;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(1, mShadowPassCB);
}

void CVoidEgine::UpdateSsaoCB(const GameTimer& gt)
{
	SsaoConstants ssaoCB;

	XMMATRIX P = mCamera.GetProj();

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	ssaoCB.Proj = mMainPassCB.Proj;
	ssaoCB.InvProj = mMainPassCB.InvProj;
	XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

	mSsao->GetOffsetVectors(ssaoCB.OffsetVectors);

	auto blurWeights = mSsao->CalcGaussWeights(2.5f);
	ssaoCB.BlurWeights[0] = XMFLOAT4(&blurWeights[0]);
	ssaoCB.BlurWeights[1] = XMFLOAT4(&blurWeights[4]);
	ssaoCB.BlurWeights[2] = XMFLOAT4(&blurWeights[8]);

	ssaoCB.InvRenderTargetSize = XMFLOAT2(1.0f / mSsao->SsaoMapWidth(), 1.0f / mSsao->SsaoMapHeight());

	// Coordinates given in view space.
	ssaoCB.OcclusionRadius = 0.5f;
	ssaoCB.OcclusionFadeStart = 0.2f;
	ssaoCB.OcclusionFadeEnd = 2.0f;
	ssaoCB.SurfaceEpsilon = 0.05f;

	auto currSsaoCB = mCurrFrameResource->SsaoCB.get();
	currSsaoCB->CopyData(0, ssaoCB);
}

void CVoidEgine::BuildRootSignature()
{
	if (m_use_deferred_texturing)
	{
		BuildDeferredRootSignature();
	}
	else
	{
		BuildZbufferRootSignature();
	}
}

void CVoidEgine::BuildDescriptorHeaps()
{
	//+2 for g-buffers
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = mTextures.size()+GBufferSize();
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
}

void CVoidEgine::BuildShadersAndInputLayout()
{
	if (m_use_deferred_texturing)
	{
		mShaders["DeferredVS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredGSShader.hlsl", nullptr, "DeferredVS", "vs_5_1");
		mShaders["DeferredPS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredGSShader.hlsl", nullptr, "DeferredPS", "ps_5_1");
		mShaders["DeferredCS"] = d3dUtil::CompileShader(L".\\Shaders\\DeferredCSShader.hlsl", nullptr, "Shading", "cs_5_1");
	}
	else
	{
		mShaders["standardVS"] = d3dUtil::CompileShader(L".\\Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
		mShaders["opaquePS"] = d3dUtil::CompileShader(L".\\Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");
	}
	
	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

}


void CVoidEgine::BuildPSOs()
{
	if (m_deferred_gs_root_signature)
	{
		BuildDeferredPSO();
	}
	else
	{
		BuildZBufferPSO();
	}

}

void CVoidEgine::BuildZBufferPSO()
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
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	pso_desc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	pso_desc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	pso_desc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&mPSOs["opaque"])));
}

void CVoidEgine::BuildDeferredPSO()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gs_pso_desc;
	ZeroMemory(&gs_pso_desc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	gs_pso_desc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	gs_pso_desc.pRootSignature = m_deferred_gs_root_signature.Get();
	gs_pso_desc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredVS"]->GetBufferPointer()),
		mShaders["DeferredVS"]->GetBufferSize()
	};
	gs_pso_desc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredPS"]->GetBufferPointer()),
		mShaders["DeferredPS"]->GetBufferSize()
	};
	gs_pso_desc.NumRenderTargets = 2;
	gs_pso_desc.RTVFormats[0] = mBackBufferFormat;
	gs_pso_desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;

	gs_pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	gs_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	gs_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	gs_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	gs_pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	gs_pso_desc.SampleMask = UINT_MAX;
	gs_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	gs_pso_desc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	gs_pso_desc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	gs_pso_desc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gs_pso_desc, IID_PPV_ARGS(&mPSOs["DeferredGS"])));

	D3D12_COMPUTE_PIPELINE_STATE_DESC shading_pso_desc = {};
	shading_pso_desc.pRootSignature = m_deferred_shading_root_signature.Get();
	shading_pso_desc.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["DeferredCS"]->GetBufferPointer()),
		mShaders["DeferredCS"]->GetBufferSize()
	};
	shading_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	shading_pso_desc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&shading_pso_desc, IID_PPV_ARGS(&mPSOs["DeferredCS"])));
}

void CVoidEgine::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			2, (UINT)mAllRitems.size(),
			1,
			(UINT)mMaterials.size()));
	}
}

void CVoidEgine::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	if (NULL == mCurrFrameResource->ObjectCB)
	{
		return;
	}
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT skinnedCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(SkinnedConstants));

	auto objectCB = mCurrFrameResource->ObjectCB->Resource();

	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;

		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

void CVoidEgine::DrawSceneToShadowMap()
{
	mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
	mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

	// Change to DEPTH_WRITE.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	// Clear the back buffer and depth buffer.
	mCommandList->ClearDepthStencilView(mShadowMap->Dsv(),
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv());

	// Bind the pass constant buffer for the shadow map pass.
	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
	auto passCB = mCurrFrameResource->PassCB->Resource();
	D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + 1 * passCBByteSize;
	mCommandList->SetGraphicsRootConstantBufferView(2, passCBAddress);

	mCommandList->SetPipelineState(mPSOs["shadow_opaque"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	mCommandList->SetPipelineState(mPSOs["skinnedShadow_opaque"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedOpaque]);

	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void CVoidEgine::DrawNormalsAndDepth()
{
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	auto normalMap = mSsao->NormalMap();
	auto normalMapRtv = mSsao->NormalMapRtv();

	// Change to RENDER_TARGET.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear the screen normal map and depth buffer.
	float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
	mCommandList->ClearRenderTargetView(normalMapRtv, clearValue, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(1, &normalMapRtv, true, &DepthStencilView());

	// Bind the constant buffer for this pass.
	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

	mCommandList->SetPipelineState(mPSOs["drawNormals"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	mCommandList->SetPipelineState(mPSOs["skinnedDrawNormals"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedOpaque]);

	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void CVoidEgine::PushRenderItems(std::vector<RenderItem*>& render_items)
{
	
	RenderItemUtil::FillGeoData(render_items, md3dDevice.Get(), mCommandList.Get());
	
	auto& opaque_items = mRitemLayer[(int)RenderLayer::Opaque];
	opaque_items.insert(opaque_items.end(), render_items.begin(), render_items.end());

	mAllRitems.insert(mAllRitems.end(), render_items.begin(), render_items.end());
}

void CVoidEgine::PushMats(std::vector<RenderItem*>& render_items)
{
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
	
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CVoidEgine::GetCpuSrv(int index)const
{
	auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE CVoidEgine::GetGpuSrv(int index)const
{
	auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CVoidEgine::GetDsv(int index)const
{
	auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
	dsv.Offset(index, mDsvDescriptorSize);
	return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CVoidEgine::GetRtv(int index)const
{
	auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtv.Offset(index, mRtvDescriptorSize);
	return rtv;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> CVoidEgine::GetStaticSamplers()
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

void CVoidEgine::CreateGBufferRTV()
{
	m_g_buffer_format[0] = mBackBufferFormat;
	m_g_buffer_format[1] = DXGI_FORMAT_R32_UINT;

	for (int i=0; i<GBufferSize(); ++i)
	{
		D3D12_RESOURCE_DESC tex_desc;
		tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex_desc.Alignment = 0;
		tex_desc.Width = mClientWidth;
		tex_desc.Height = mClientHeight;
		tex_desc.DepthOrArraySize = 1;
		tex_desc.MipLevels = 1;
		tex_desc.Format = m_g_buffer_format[i];
		tex_desc.SampleDesc.Count = 1;
		tex_desc.SampleDesc.Quality = 0;
		tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		auto clear_values = CD3DX12_CLEAR_VALUE(m_g_buffer_format[i], Colors::LightSteelBlue);
		ThrowIfFailed(md3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&tex_desc,
			D3D12_RESOURCE_STATE_COMMON,
			&clear_values,
			IID_PPV_ARGS(&m_g_buffer[i])));

		D3D12_RENDER_TARGET_VIEW_DESC rt_desc;
		rt_desc.Format = m_g_buffer_format[i];
		rt_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rt_desc.Texture2D.MipSlice = 0;
		rt_desc.Texture2D.PlaneSlice = 0;
		CD3DX12_CPU_DESCRIPTOR_HANDLE h(CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(), SwapChainBufferCount+i, mRtvDescriptorSize));
		md3dDevice->CreateRenderTargetView(m_g_buffer[i].Get(), &rt_desc, h);
	}
}

UINT CVoidEgine::GBufferSize() const
{
	return sizeof(m_g_buffer) / sizeof(m_g_buffer[0]);
}

void CVoidEgine::DeferredDrawFillGBufferPass()
{
	//第一个pass，先填充G-Buffers
	mCommandList->SetGraphicsRootSignature(m_deferred_gs_root_signature.Get());

	for (int i = 0; i < GBufferSize(); ++i)
	{
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[i].Get(),
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
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

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	mCommandList->SetPipelineState(mPSOs["DeferredGS"].Get());
	// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
	// set as a root descriptor.

	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	for (int i = 0; i < GBufferSize(); ++i)
	{
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[i].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
	}
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
// 		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[0].Get(),
// 		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
// 	mCommandList->CopyResource(CurrentBackBuffer(), m_g_buffer[0].Get());
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
// 		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
// 	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_g_buffer[0].Get(),
// 		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
}

void CVoidEgine::DeferredDrawShadingPass()
{
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

	mCommandList->SetComputeRootSignature(m_deferred_shading_root_signature.Get());
	mCommandList->SetPipelineState(mPSOs["DeferredCS"].Get());
	mCommandList->SetComputeRootShaderResourceView(0, m_g_buffer[0]->GetGPUVirtualAddress());
	mCommandList->SetComputeRootShaderResourceView(1, m_g_buffer[1]->GetGPUVirtualAddress());
	if (NULL != mCurrFrameResource->MaterialBuffer)
	{
		auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetComputeRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());
	}
	if (0 != mTextures.size())
	{
		mCommandList->SetComputeRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	}
	mCommandList->SetComputeRootDescriptorTable(4, CD3DX12_GPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetGPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize));
	mCommandList->Dispatch(16, 16, 1);
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PRESENT));
}

void CVoidEgine::BuildZbufferRootSignature()
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

void CVoidEgine::BuildDeferredRootSignature()
{
	BuildDeferredGSRootSignature();
	BuildDeferredShadingRootSignature();
}

void CVoidEgine::BuildDeferredGSRootSignature()
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

void CVoidEgine::BuildDeferredShadingRootSignature()
{
	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];

	CD3DX12_DESCRIPTOR_RANGE tex_table;
	tex_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mTextures.size(), 1, 1);

	CD3DX12_DESCRIPTOR_RANGE uav_table;
	uav_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0);

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsShaderResourceView(0);
	slotRootParameter[1].InitAsShaderResourceView(1);
	slotRootParameter[2].InitAsShaderResourceView(0, 1);
	slotRootParameter[3].InitAsDescriptorTable(1, &tex_table, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[4].InitAsDescriptorTable(1, &uav_table);

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

