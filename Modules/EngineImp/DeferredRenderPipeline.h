#pragma once
#include "../EngineImp/CBaseRenderPipeline.h"
#include "../Skin/SkinnedData.h"
#include "../FrameResource/FrameResource.h"
#include "../Common/Camera.h"
#include "../Common/RenderItems.h"
#include <queue>
#include "../Predefines/ScenePredefines.h"
#include "../Predefines/BufferPredefines.h"

class ShadowMap;
class Ssao;

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gGbufferCount = 2;
class CDeferredRenderPipeline : public CBaseRenderPipeline
{
public:
	CDeferredRenderPipeline(HINSTANCE hInstance, HWND wnd);
	CDeferredRenderPipeline(const CDeferredRenderPipeline& rhs) = delete;
	CDeferredRenderPipeline& operator=(const CDeferredRenderPipeline& rhs) = delete;
	~CDeferredRenderPipeline();

	virtual bool Initialize()override;
	virtual void PushMats(std::vector<RenderItem*>& render_items) override;

	virtual void PitchCamera(float rad);
	virtual void RotateCameraY(float rad);
	virtual void MoveCamera(float dis);
	virtual void StrafeCamera(float dis);
private:
	virtual void CreateRtvAndDsvDescriptorHeaps()override;
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;
	virtual void UpdateCamera(const GameTimer& gt) override;
	void DrawWithDeferredTexturing(const GameTimer& gt);
	virtual std::vector<RenderItem*>& GetRenderItems(int layer);
	virtual DirectX::XMFLOAT3 GetCameraPos();
	virtual BoundingFrustum GetCameraFrustum() override;
	virtual DirectX::XMFLOAT3 GetCameraDir();
	virtual void ClearVisibleRenderItems();
	virtual void PushVisibleModels(std::map<int,  std::vector<RenderItem*>>& render_items, bool add = false) override;
	virtual bool InitDirect3D() override;
	virtual bool IsCameraDirty() override;

	void BuildRootSignature();
	void BuildDescriptorHeaps();
	void BuildShadersAndInputLayout();
	void BuildPSOs();
	void BuildDeferredPSO();
	void BuildFrameResources();
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems, int layer);
	void PushRenderItems(std::vector<RenderItem*>& render_item);
	CD3DX12_CPU_DESCRIPTOR_HANDLE GetCpuSrv(int index)const;
	CD3DX12_GPU_DESCRIPTOR_HANDLE GetGpuSrv(int index)const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE GetDsv(int index)const;


	CD3DX12_CPU_DESCRIPTOR_HANDLE GetRtv(int index)const;

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GetStaticSamplers();

	void CreateGBufferRTV();
	UINT GBufferSize() const;
	void DeferredDrawFillGBufferPass();
	void DeferredDrawShadingPass();

	void BuildDeferredRootSignature();
	void BuildDeferredGSRootSignature();
	void BuildDeferredShadingRootSignature();
private:

	std::unique_ptr<FrameResource> mFrameResources;
	FrameResourceOffset* mCurrFrameResource = nullptr;
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
	std::vector<RenderItem*> mAllRitems;

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

	Microsoft::WRL::ComPtr<ID3D12Resource> m_g_buffer[gGbufferCount];
	DXGI_FORMAT m_g_buffer_format[gGbufferCount];

	std::queue<FrameResourceOffset> m_frame_res_offset;
	void UpdateFrameResource(const GameTimer& gt);
	bool CanFillFrameRes(FrameResComponentSize& size, FrameResourceOffset& offset);
	void FreeMemToCompletedFrame(UINT64 frame_index);
	void CopyFrameRescourceData(const GameTimer& gt, const FrameResourceOffset& offset);
	void CopyObjectCBAndVertexData(const FrameResourceOffset& offset);
	void CopyMatCBData(const FrameResourceOffset& offset);
	void CopyPassCBData(const GameTimer& gt, const FrameResourceOffset& offset);
	FrameResComponentSize CalCurFrameContantsSize();

	//hi-z pass
	void HiZPass();
	void CreateHiZBuffer();
	Microsoft::WRL::ComPtr<ID3D12Resource> m_hiz_buffer;
	DXGI_FORMAT m_hiz_buffer_format = DXGI_FORMAT_R32_FLOAT;
	void GenerateFullResDepthPass();
	void GenerateHiZBufferChainPass();
	void BuildHiZRootSignature();
	void BuildFullResDepthPassRootSignature();
	void BuildHiZBufferChainPassRootSignature();
	void BuildHiZPSO();

	ComPtr<ID3D12RootSignature> m_hiz_fullres_depth_pass_root_signature = nullptr;
	ComPtr<ID3D12RootSignature> m_hiz_buffer_chain_pass_root_signature = nullptr;
	int GetRenderLayerObjectOffset(int layer);
	UINT GetHiZMipmapLevels() const;
	FrameResComponentSize m_contants_size;

	//instance culling
	void InstanceHiZCullingPass();
	void BuildHiZInstanceCullingRootSignature();
	void BuildHiZInstanceCullingPSO();
	ComPtr<ID3D12RootSignature> m_hiz_instance_culling_pass_root_signature = nullptr;

	//instance culling result
	ComPtr<ID3D12Resource> m_instance_culling_result_buffer;
	ComPtr<ID3D12Resource> m_counter_reset_buffer;
	UINT AlignForUavCounter(UINT bufferSize);
	const int ObjectConstantsBufferOffset = AlignForUavCounter(ScenePredefine::MaxObjectNumPerScene * sizeof(ObjectConstants));
	const UINT CullingResBufferMaxElementNum = ScenePredefine::MaxMeshVertexNumPerScene / (VertexPerCluster * ClusterPerChunk) + ((ScenePredefine::MaxMeshVertexNumPerScene % (VertexPerCluster * ClusterPerChunk)) ? 1 : 0);
	const UINT CullingResMaxObjSize = AlignForUavCounter(sizeof(InstanceChunk) * CullingResBufferMaxElementNum);
	UINT64 AlignForCrvAddress(const D3D12_GPU_VIRTUAL_ADDRESS& address, const UINT& offset);
	UINT Align(const UINT& size, const UINT& alignment);

	std::vector<RenderItem*> GetVisibleRenderItems();

	//Chunk expan
	void ChunkExpanPass();
	void BuildChunkExpanRootSignature();
	void BuildChunkExpanPSO();
	void CreateChunExpanBuffer();
	ComPtr<ID3D12Resource> m_chunk_expan_result_buffer;
	ComPtr<ID3D12RootSignature> m_chunk_expan_pass_root_signature = nullptr;
	const UINT ChunkExpanBufferMaxElementNum = ScenePredefine::MaxMeshVertexNumPerScene / VertexPerCluster + ((ScenePredefine::MaxMeshVertexNumPerScene % VertexPerCluster) ? 1 : 0);
	const UINT ChunkExpanSize = AlignForUavCounter(sizeof(ClusterChunk) * ChunkExpanBufferMaxElementNum);

	int m_descriptor_end = 0;
	CD3DX12_GPU_DESCRIPTOR_HANDLE m_obj_handle;

	//Cluster Culling
	void ClusterHiZCullingPass();
	void BuildClusterHiZCullingPSO();
	ComPtr<ID3D12Resource> m_cluster_culling_result_buffer;
	ComPtr<ID3D12RootSignature> m_hiz_cluster_culling_pass_root_signature = nullptr;

};

