#pragma once

#include "../Common/d3dUtil.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryDefines.h"

//#pragma pack(1)

struct IndirectCommand
{
	D3D12_GPU_VIRTUAL_ADDRESS ObjCbv;
	D3D12_GPU_VIRTUAL_ADDRESS PassCbv;
	D3D12_DRAW_INDEXED_ARGUMENTS drawArguments;
	float padding[3];
};

struct InstanceChunk
{
	UINT InstanceID;
	UINT ChunkID;
};

struct ClusterChunk
{
	UINT InstanceID;
	UINT ClusterID;
};

struct ObjectConstants
{
	IndirectCommand DrawCommand;
	float padding0[9];
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
	AABB    Bounds;
	float padding1[6];
	UINT    MaterialIndex;
};

struct SkinnedConstants
{
	DirectX::XMFLOAT4X4 BoneTransforms[96];
};

struct PassConstants
{
	DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 InvView = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 InvProj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 ViewProjTex = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 ShadowTransform = MathHelper::Identity4x4();
	DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
	float cbPerObjectPad1 = 0.0f;
	DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 InvRenderTargetSize = { 0.0f, 0.0f };
	float NearZ = 0.0f;
	float FarZ = 0.0f;
	float TotalTime = 0.0f;
	float DeltaTime = 0.0f;

	DirectX::XMFLOAT4 AmbientLight = { 0.0f, 0.0f, 0.0f, 1.0f };
	UINT ObjectNum = 0;
	// Indices [0, NUM_DIR_LIGHTS) are directional lights;
	// indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
	// indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
	// are spot lights for a maximum of MaxLights per object.
	Light Lights[MaxLights];
};

struct SsaoConstants
{
	DirectX::XMFLOAT4X4 Proj;
	DirectX::XMFLOAT4X4 InvProj;
	DirectX::XMFLOAT4X4 ProjTex;
	DirectX::XMFLOAT4   OffsetVectors[14];

	// For SsaoBlur.hlsl
	DirectX::XMFLOAT4 BlurWeights[3];

	DirectX::XMFLOAT2 InvRenderTargetSize = { 0.0f, 0.0f };

	// Coordinates given in view space.
	float OcclusionRadius = 0.5f;
	float OcclusionFadeStart = 0.2f;
	float OcclusionFadeEnd = 2.0f;
	float SurfaceEpsilon = 0.05f;
};

struct FrameResComponentSize
{
	UINT TotalSize;
	UINT VertexCBSize;
	UINT IndexCBSize;
	UINT ObjectCBSize;
	UINT PassCBSize;
	UINT MatCBSize;
};

struct FrameResourceOffset
{
	UINT64 Fence;
	UINT64 EndResOffset;
	UINT64 VertexBeginOffset;
	UINT64 IndexBeginOffset;
	UINT64 ObjectBeginOffset;
	UINT64 PassBeginOffset;
	UINT64 MatBeginOffset;
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:

	FrameResource(ID3D12Device* device);
	FrameResource(const FrameResource& rhs) = delete;
	FrameResource& operator=(const FrameResource& rhs) = delete;
	~FrameResource();

	// We cannot reset the allocator until the GPU is done processing the commands.
	// So each frame needs their own allocator.
	std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> CmdListAlloc;

	// We cannot update a cbuffer until the GPU is done processing the commands
	// that reference it.  So each frame needs their own cbuffers.
	std::unique_ptr<UploadBuffer> FrameResCB = nullptr;

	// Fence value to mark commands up to this fence point.  This lets us
	// check if these frame resources are still in use by the GPU.
	UINT64 Fence = 0;

	UINT64 Size();
private:
	UINT64 m_total_size;
};
