#include "FrameResource.h"
#include "../Predefines/ScenePredefines.h"
#include "../Predefines/BufferPredefines.h"

FrameResource::FrameResource(ID3D12Device* device, UINT mat_size)
{
	CmdListAlloc.resize(MaxCommandAllocNum);
	for (int i=0; i< MaxCommandAllocNum; ++i)
	{
		ThrowIfFailed(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(CmdListAlloc[i].GetAddressOf())));
	}
	

	//根据场景内实体顶点上限的buffer来计算size
	UINT mat_max_size = sizeof(MatData)* mat_size;
	UINT pass_size = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
	UINT object_max_size = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants))* ScenePredefine::MaxObjectNumPerScene;
	m_total_size = (pass_size + object_max_size) * gNumFrameResources;

	FrameResCB = std::make_unique<UploadBuffer>(device, m_total_size / UploadBufferChunkSize, UploadBufferChunkSize, true);

	MatCB = std::make_unique<UploadBuffer>(device, mat_max_size , sizeof(MatData), false);
}

FrameResource::~FrameResource()
{

}

UINT FrameResource::Size()
{
	return m_total_size;
}
