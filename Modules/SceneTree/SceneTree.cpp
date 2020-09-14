#include "SceneTree.h"
#include "SceneTreeNode.h"
#include "../Common/RenderItems.h"
#include "../Logger/LoggerWrapper.h"

namespace QuadTree
{

	CQuadTree::CQuadTree()
	{
		m_tree = std::make_unique<TreeNode>();
	}

	void CQuadTree::Init(std::vector<RenderItem*>& render_items)
	{
		//1、划分好各层级节点
		InitSceneTreeLayers();
		
		//2、插入各节点
		InsertRenderItems(render_items);

		//3、从上往下合并子节点
		CombineTreeNodes();
	}

	void CQuadTree::Load(std::string& file)
	{

	}

	void CQuadTree::Save(std::string& file)
	{

	}

	std::map<int, std::vector<RenderItem*>> CQuadTree::Culling(const DirectX::BoundingFrustum& frustum)
{
		std::map<int, std::vector<RenderItem*>> res;
		
		//深度遍历把没剔除的子节点加入到最后要渲染的队列中
		TreeNode* node = m_tree.get();
		CullingNode(node, frustum, res);
		return res;
	}

	void CQuadTree::InitSceneTreeLayers()
	{
		if (0 != m_tree_layers.size())
		{
			m_tree_layers.clear();
		}
		
		for (int i = 0; i<SceneTreeDepth; ++i)
		{
			SceneTreeLayer layer;
			layer.GridSize = XMFLOAT2(SceneSize / (pow(2, i)), SceneSize / (pow(2, i)));
			if (0 == i)
			{
				SceneTreeGrid grid;
				grid.Node = m_tree.get();
				grid.Node->aabb.Center = XMFLOAT3(0, 0, 0);
				grid.Node->aabb.Extents = XMFLOAT3(SceneSize/2, SceneSize/2, SceneSize/2);
				layer.Grids[std::pair<int, int>(0, 0)] = grid;

			}
			m_tree_layers[i] = layer;
		
		}
	}

	void CQuadTree::InsertRenderItems(std::vector<RenderItem*>& render_items)
	{
		//因为是四叉树，所以在XZ平面上进行插入
		for (int i = 0; i<render_items.size(); ++i)
		{
			int depth = CalLayerDepth(render_items[i]->Bounds);
			GridIndex index = CalGridIndex(render_items[i]->World, depth);
			m_tree_layers[depth].Grids[index].RenderItemsList[(int)render_items[i]->Layer].push_back(render_items[i]);
		}

	}

	void CQuadTree::CombineTreeNodes()
	{
		//如果高层的节点中包含AABB，说明这个节点划分的后续节点要合并到这个高层节点中
		for (int i = 0; i<SceneTreeDepth; ++i)
		{
			if (0 == m_tree_layers[i].Grids.size())
			{
				continue;
			}

			auto layer_grids_itr = m_tree_layers[i].Grids.begin();

			while (layer_grids_itr != m_tree_layers[i].Grids.end())
			{
				if (0 == layer_grids_itr->second.RenderItemsList.size())
				{
					//已经被合并过了
					layer_grids_itr++;
					continue;
				}

				//从上往下合并高层节点覆盖的低层节点
				//获取parent,确定根节点到该节点（叶子节点）的父子关系
				layer_grids_itr->second.Node = CreateNode(layer_grids_itr->first, i);
				//遍历child，把子节点的render items全部合并过来
				CollectRenderItems(layer_grids_itr->first, i, layer_grids_itr->second.Node->RenderItemsList);
				layer_grids_itr++;
			}
		}
	}

	int CQuadTree::CalLayerDepth(const AABB& bound)
	{
		//判断依据，包围盒大小属于被包围的最大节点
		//因为场景节点大小可以直接计算出来，因此可以直接计算得出当前层
		float width = bound.MaxVertex.x - bound.MinVertex.x;
		float length = bound.MaxVertex.z - bound.MinVertex.z;
		float size = max(width, length);
		int max_2_power_exp = log2f(size) + ((0 == size / 2) ? 0 : 1 );
		float grid_size = 2 ^ max_2_power_exp;
		int depth = log2f(SceneSize / grid_size);
		return (depth < 0 || depth >= SceneTreeDepth) ? (SceneTreeDepth - 1) : depth;
	}

	GridIndex CQuadTree::CalGridIndex(const XMFLOAT4X4& pos, int layer_depth)
	{
		//只考虑XZ平面
		//直接根据中心点与网格的位置关系来确定最终的Index
		XMFLOAT2 world_pos = XMFLOAT2(pos.m[3][0], pos.m[3][2]);
		XMFLOAT2 offset = XMFLOAT2(world_pos.x - -SceneSize/2, world_pos.y - -SceneSize/2);
		GridIndex index;
		index.first = offset.x / m_tree_layers[layer_depth].GridSize.x;
		index.second = offset.y / m_tree_layers[layer_depth].GridSize.y;
		return index;
	}

	QuadTree::TreeNode* CQuadTree::GetParentTreeNode(const GridIndex& index, int depth)
	{
		if (0 == depth)
		{
			return m_tree.get();
		}

		auto& grid = m_tree_layers[depth].Grids[index];
		if (NULL == grid.Node)
		{
			grid.Node = CreateNode(index, depth);
		}
		return grid.Node;
	}

	void CQuadTree::CollectRenderItems(const GridIndex& index, int depth, std::map< int, std::list<RenderItem*>>& render_items)
	{
		//把自己的节点加入
		if (0 != m_tree_layers[depth].Grids[index].RenderItemsList.size())
		{
			auto& cur_render_items = m_tree_layers[depth].Grids[index].RenderItemsList;
			auto itr = cur_render_items.begin();
			while (itr != cur_render_items.end())
			{
				render_items[itr->first].insert(render_items[itr->first].end(), cur_render_items[itr->first].begin(), cur_render_items[itr->first].end());
				++itr;
			}
		}

		if ((SceneTreeDepth - 1) == depth)
		{
			return;
		}

		//把子节点的加入
		for (int i=0; i<2; ++i)
		{
			for (int j=0; j<2; ++j)
			{
				GridIndex child_index = std::pair<int, int>(index.first + i,index.second + j);
				CollectRenderItems(child_index, depth + 1, render_items);
			}
		}
	}

	TreeNode* CQuadTree::CreateNode(const GridIndex& index, int depth)
	{
		if (0 == depth)
		{
			m_tree_layers[depth].Grids[index].Node = m_tree.get();
			return m_tree.get();
		}

		//场景树节点相关
		int parent_depth = depth - 1;
		GridIndex parent_index = std::pair<int, int>(index.first / 2, index.second / 2);
		TreeNode* node = new TreeNode();
		m_tree_layers[depth].Grids[index].Node = node;
		node->Parent = GetParentTreeNode(parent_index, parent_depth);
		node->Parent->ChildNodes.push_back(node);

		//空间位置相关
		const auto& grid_size = m_tree_layers[depth].GridSize;
		node->aabb.Center = XMFLOAT3(index.first *  grid_size.x + grid_size.x/2 + -SceneSize/2, SceneSize/2,index.second * grid_size.y + grid_size.y/2 + -SceneSize/2);
		node->aabb.Extents = XMFLOAT3(grid_size.x/2, SceneSize / 2, grid_size.y/2 );
		return node;
	}

	void CQuadTree::ReleaseNode(TreeNode* node)
	{
		if (NULL == node)
		{
			return;
		}


		//深度遍历释放内存
		auto itr = node->ChildNodes.begin();
		while (itr != node->ChildNodes.end())
		{
			ReleaseNode(*itr);
			itr++;
		}
		delete node;
		node = NULL;
	}

	void CQuadTree::CullingNode(TreeNode* node, const DirectX::BoundingFrustum& frustum, std::map<int, std::vector<RenderItem*>>& render_items)
	{
		//判断当前是否被剔除
		auto status = frustum.Contains(node->aabb);
		//LogDebug(" [Scene Tree Culling] culling res : {}", status);
		if (DirectX::DISJOINT == status)
		{
			//LogDebug(" [Scene tree Culling] aabb : center - {} {} {}, extents - {} {} {}", node->aabb.Center.x, node->aabb.Center.y, node->aabb.Center.z, node->aabb.Extents.x, node->aabb.Extents.y, node->aabb.Extents.z);
			return;
		}

		if (0 == node->ChildNodes.size())
		{
			//已经到了叶子节点
			auto itr = node->RenderItemsList.begin();
			while (itr != node->RenderItemsList.end())
			{
				render_items[itr->first].insert(render_items[itr->first].end(), node->RenderItemsList[itr->first].begin(), node->RenderItemsList[itr->first].end());
				++itr;
			}
			return;
		}

		auto itr = node->ChildNodes.begin();
		while (itr!=node->ChildNodes.end())
		{
			CullingNode(*itr, frustum, render_items);
			itr++;
		}
	}

	CQuadTree::~CQuadTree()
	{
		if (NULL != m_tree)
		{
			ReleaseNode(m_tree.release());
		}
	}

}

