#include "FrameResource.h"
#include "../Predefines/ScenePredefines.h"
#include "../Predefines/BufferPredefines.h"

FrameResource::FrameResource(ID3D12Device* device)
{
	CmdListAlloc.resize(MaxCommandAllocNum);
	for (int i=0; i< MaxCommandAllocNum; ++i)
	{
		ThrowIfFailed(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(CmdListAlloc[i].GetAddressOf())));
	}
	
	/*
		| ObjectContents | MatBuffer | PassContents | VertexBuffer | IndexBuffer |
	*/


	//根据场景内实体顶点上限的buffer来计算size
	UINT pass_size = sizeof(PassConstants);
	UINT object_max_size = sizeof(ObjectConstants)* ScenePredefine::MaxObjectNumPerScene;
	UINT vertex_max_size = sizeof(VertexData)* ScenePredefine::MaxMeshVertexNumPerScene;
	UINT mat_max_size = sizeof(MatData) * ScenePredefine::MaxObjectNumPerScene;
	UINT index_max_size = sizeof(std::uint16_t) * ScenePredefine::MaxMeshVertexNumPerScene * 3;
	m_total_size = (pass_size + object_max_size + vertex_max_size + index_max_size + mat_max_size) * gNumFrameResources;

	FrameResCB = std::make_unique<UploadBuffer>(device, m_total_size , sizeof(char), false);
	FrameResCB->Resource()->SetName(L"FrameResrource CB");
}

FrameResource::~FrameResource()
{

}

UINT FrameResource::Size()
{
	return m_total_size;
}
