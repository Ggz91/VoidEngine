#pragma once
#include <vector>
#include "../Common/d3dUtil.h"
#include "../Common/GeometryDefines.h"

struct RenderItem;

typedef  void (*FlushCmdsFunc)();

struct RIUAccParam
{
	int IndexStartOffset;
	int BaseVertexOffset;
	std::vector<VertexData> TotalVertices;
	std::vector<std::uint16_t> TotalIndices;
};

class RenderItemUtil
{
public:
	static void FillGeoData(std::vector<RenderItem*>& render_items, ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList);
	
	static void FillSingleGeoData(RenderItem* render_item, ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList, std::unique_ptr <RIUAccParam>&& acc_param, MeshGeometry* geo);
};

