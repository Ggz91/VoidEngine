#pragma once
#include <vector>
#include <string>

struct RenderItem;

class ISceneTree
{
public:
	virtual void Init(std::vector<RenderItem*>& render_items) = 0;
	virtual void Load(std::string& file) = 0;
	virtual void Save(std::string& file) = 0;
};