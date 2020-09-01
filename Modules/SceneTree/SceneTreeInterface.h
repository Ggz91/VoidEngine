#pragma once
#include <vector>
#include <string>
#include "../Common/GeometryDefines.h"

struct RenderItem;

class ISceneTree
{
public:
	virtual void Init(std::vector<RenderItem*>& render_items) = 0;
	virtual void Load(std::string& file) = 0;
	virtual void Save(std::string& file) = 0;
	virtual void Culling(const DirectX::XMFLOAT3& camera_pos, const DirectX::XMFLOAT3& camera_dir,const Frustum& frustum) = 0;
};