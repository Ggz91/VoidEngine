#pragma once
#include <vector>
#include "../Common/d3dUtil.h"

struct RenderItem;

class RenderItemUtil
{
public:
	static void FillGeoData(std::vector<RenderItem*>& render_items, ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList);
private:
	static void FillSingleGoeData(RenderItem* render_item, ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList);
};
