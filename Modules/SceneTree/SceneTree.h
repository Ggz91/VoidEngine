#pragma once
#include "SceneTreeInterface.h"
#include <iostream>
#include "../Common/GeometryDefines.h"
#include <list>
#include <map>

namespace QuadTree
{
	using namespace DirectX;

	//限制场景大小
	const float SceneSize = pow(2,15);
	const int SceneTreeDepth = 5;

	struct TreeNode;

	struct SceneTreeGrid
	{
		TreeNode* Node;
		std::list<RenderItem*> RenderItemsList;
	};

	typedef std::pair<int, int> GridIndex;
	typedef std::map<GridIndex, SceneTreeGrid> SceneTreeLayerGrids;

	struct SceneTreeLayer
	{
		XMFLOAT2 GridSize;
		SceneTreeLayerGrids Grids;
	};

	class CQuadTree : public ISceneTree
	{
	public:
		CQuadTree();
		~CQuadTree();
		virtual void Init(std::vector<RenderItem*>& render_items) override;
		virtual void Load(std::string& file) override;
		virtual void Save(std::string& file) override;
		virtual std::vector<RenderItem*> Culling(const DirectX::BoundingFrustum& frustum) override;
	private:
		std::unique_ptr<TreeNode> m_tree;

		std::map<int, SceneTreeLayer> m_tree_layers;
		void InitSceneTreeLayers();
		void InsertRenderItems(std::vector<RenderItem*>& render_items);
		void CombineTreeNodes();
		int CalLayerDepth(const AABB& bound);
		GridIndex CalGridIndex(const XMFLOAT4X4& pos, int layer_depth);
		TreeNode* GetParentTreeNode(const GridIndex& index, int depth);
		void CollectRenderItems(const GridIndex& index, int depth , std::list<RenderItem*>& render_items);
		TreeNode* CreateNode(const GridIndex& index, int depth);
		void ReleaseNode(TreeNode* node);
		void CullingNode(TreeNode* node, const DirectX::BoundingFrustum& frustum, std::vector<RenderItem*>& render_items);
	};
}