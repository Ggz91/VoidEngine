#pragma once
#include "../EngineImp/CBaseRenderPipeline.h"
#include "../Skin/SkinnedData.h"
#include "../FrameResource/FrameResource.h"
#include "../Common/Camera.h"

class ShadowMap;
class Ssao;

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

class CZBufferRenderPipeline : public CBaseRenderPipeline
{
public:
	CZBufferRenderPipeline(HINSTANCE hInstance, HWND wnd);
	CZBufferRenderPipeline(const CZBufferRenderPipeline& rhs) = delete;
	CZBufferRenderPipeline& operator=(const CZBufferRenderPipeline& rhs) = delete;
	~CZBufferRenderPipeline();

	virtual bool Initialize()override;
	virtual void PushModels(std::vector<RenderItem*>& render_items) override;
private:
	virtual void CreateRtvAndDsvDescriptorHeaps()override;
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	void DrawWithZBuffer(const GameTimer& gt);

	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMaterialBuffer(const GameTimer& gt);
	void UpdateShadowTransform(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);
	void UpdateShadowPassCB(const GameTimer& gt);
	void UpdateSsaoCB(const GameTimer& gt);

	void BuildRootSignature();
	void BuildDescriptorHeaps();
	void BuildShadersAndInputLayout();
	void BuildPSOs();
	void BuildZBufferPSO();
	void BuildDeferredPSO();
	void BuildFrameResources();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
	void DrawSceneToShadowMap();
	void DrawNormalsAndDepth();
	void PushRenderItems(std::vector<RenderItem*>& render_item);
	void PushMats(std::vector<RenderItem*>& render_item);
	CD3DX12_CPU_DESCRIPTOR_HANDLE GetCpuSrv(int index)const;
	CD3DX12_GPU_DESCRIPTOR_HANDLE GetGpuSrv(int index)const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE GetDsv(int index)const;


	CD3DX12_CPU_DESCRIPTOR_HANDLE GetRtv(int index)const;

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GetStaticSamplers();

	void BuildZbufferRootSignature();
private:

	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
	ComPtr<ID3D12RootSignature> m_deferred_gs_root_signature = nullptr;
	ComPtr<ID3D12RootSignature> m_deferred_shading_root_signature = nullptr;
	ComPtr<ID3D12RootSignature> mSsaoRootSignature = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, Material*> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

	std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	UINT mSkyTexHeapIndex = 0;
	UINT mShadowMapHeapIndex = 0;
	UINT mSsaoHeapIndexStart = 0;
	UINT mSsaoAmbientMapIndex = 0;

	UINT mNullCubeSrvIndex = 0;
	UINT mNullTexSrvIndex1 = 0;
	UINT mNullTexSrvIndex2 = 0;

	CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

	PassConstants mMainPassCB;  // index 0 of pass cbuffer.
	PassConstants mShadowPassCB;// index 1 of pass cbuffer.


	Camera mCamera;

	std::unique_ptr<ShadowMap> mShadowMap;

	std::unique_ptr<Ssao> mSsao;

	DirectX::BoundingSphere mSceneBounds;

	float mLightNearZ = 0.0f;
	float mLightFarZ = 0.0f;
	XMFLOAT3 mLightPosW;
	XMFLOAT4X4 mLightView = MathHelper::Identity4x4();
	XMFLOAT4X4 mLightProj = MathHelper::Identity4x4();
	XMFLOAT4X4 mShadowTransform = MathHelper::Identity4x4();

	float mLightRotationAngle = 0.0f;
	XMFLOAT3 mBaseLightDirections[3] = {
		XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
		XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
		XMFLOAT3(0.0f, -0.707f, -0.707f)
	};
	XMFLOAT3 mRotatedLightDirections[3];

};
