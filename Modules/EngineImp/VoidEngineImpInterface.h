#pragma once
#include "../Common/GameTimer.h"
#include <unordered_map>
#include "../Common/d3dUtil.h"

using Microsoft::WRL::ComPtr;

class RenderPipelineInterface
{
public:
	virtual void CreateRtvAndDsvDescriptorHeaps(Microsoft::WRL::ComPtr<ID3D12Device>& device, Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& rtv_heap, Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& dsv_heap) = 0;
	virtual void Draw(const GameTimer& gt) = 0;
	virtual void BuildRootSignature() = 0;
	virtual void BuildShadersAndInputLayout(std::unordered_map<std::string, ComPtr<ID3DBlob>>& shaders, std::vector<D3D12_INPUT_ELEMENT_DESC>& layouts) = 0;
};
