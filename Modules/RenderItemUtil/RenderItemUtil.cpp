#include "RenderItemUtil.h"
#include "../Common/RenderItems.h"
#include "../Common/GeometryDefines.h"


void RenderItemUtil::FillGeoData(std::vector<RenderItem*>& render_items, ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList)
{
	for (int i=0; i<render_items.size(); ++i )
	{
		FillSingleGoeData(render_items[i], device, cmdList);
	}
}

void RenderItemUtil::FillSingleGoeData(RenderItem* render_item, ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList)
{
	render_item->Geo = new MeshGeometry();
	auto data = render_item->Data;
	auto& geo = render_item->Geo;
	geo->Name = "architecture";
	const UINT v_byte_size = (UINT)data->Mesh.Vertices.size() * sizeof(VertexData);
	const UINT i_byte_size = (UINT)data->Mesh.Indices.size() * sizeof(int);

	ThrowIfFailed(D3DCreateBlob(v_byte_size, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), data->Mesh.Vertices.data(), v_byte_size);

	ThrowIfFailed(D3DCreateBlob(i_byte_size, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), data->Mesh.Indices.data(), i_byte_size);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, data->Mesh.Vertices.data(), v_byte_size, geo->VertexBufferGPU);
	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, data->Mesh.Indices.data(), i_byte_size, geo->IndexBufferGPU);
	
	geo->VertexByteStride = sizeof(VertexData);
	geo->VertexBufferByteSize = v_byte_size;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = i_byte_size;

	SubmeshGeometry mesh;
	mesh.IndexCount = data->Mesh.Indices.size();
	mesh.StartIndexLocation = 0;
	mesh.BaseVertexLocation = 0;

	geo->DrawArgs["arch"] = mesh;
}

