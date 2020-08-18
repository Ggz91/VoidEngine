#include "RenderItemUtil.h"
#include "../Common/RenderItems.h"
#include "../Common/GeometryDefines.h"


void RenderItemUtil::FillGeoData(std::vector<RenderItem*>& render_items, ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList)
{
	auto geo = new MeshGeometry;
	auto acc_param = std::make_unique<RIUAccParam>();

	for (int i=0; i<render_items.size(); ++i)
	{
		FillSingleGeoData(render_items[i], device, cmdList, std::move(acc_param), geo);
	}
	
	//建立CPU和GPU的映射
	const UINT v_byte_size = (UINT)acc_param->TotalVertices.size() * sizeof(VertexData);
	const UINT i_byte_size = (UINT)acc_param->TotalIndices.size() * sizeof(std::uint16_t);

	ThrowIfFailed(D3DCreateBlob(v_byte_size, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), acc_param->TotalVertices.data(), v_byte_size);

	ThrowIfFailed(D3DCreateBlob(i_byte_size, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), acc_param->TotalIndices.data(), i_byte_size);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, acc_param->TotalVertices.data(), v_byte_size, geo->VertexBufferUploader);
	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, acc_param->TotalIndices.data(), i_byte_size, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(VertexData);
	geo->VertexBufferByteSize = v_byte_size;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = i_byte_size;
}

void RenderItemUtil::FillSingleGeoData(RenderItem* render_item, ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList, std::unique_ptr <RIUAccParam>&& acc_param, MeshGeometry* geo)
{
	render_item->Geo = geo;
	auto data = render_item->Data;
	
	acc_param->TotalVertices.insert(acc_param->TotalVertices.end(), data->Mesh.Vertices.begin(), data->Mesh.Vertices.end());
	acc_param->TotalIndices.insert(acc_param->TotalIndices.end(), data->Mesh.Indices.begin(), data->Mesh.Indices.end());

	SubmeshGeometry mesh;
	mesh.IndexCount = data->Mesh.Indices.size();
	mesh.StartIndexLocation = acc_param->IndexStartOffset;
	mesh.BaseVertexLocation = acc_param->BaseVertexOffset;
	render_item->StartIndexLocation = mesh.StartIndexLocation;
	render_item->BaseVertexLocation = mesh.BaseVertexLocation;
	render_item->IndexCount = mesh.IndexCount;

	acc_param->IndexStartOffset += data->Mesh.Vertices.size();
	acc_param->BaseVertexOffset += data->Mesh.Indices.size();

	geo->DrawArgs["architecture" + std::to_string(render_item->ObjCBIndex)] = mesh;
}

