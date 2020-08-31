#pragma once
#include <vector>
#include <DirectXMath.h>
#include <list>
/*
	四叉树
	AABB 使用对角线上的两点来表示
*/

using namespace DirectX;

namespace QuadTree
{
	const int AABB_Vertex_Count = 2;
	const int Child_Node_Count = 4;

	struct TreeNode
	{
		XMFLOAT3 Pos;
		XMFLOAT3 AABB[AABB_Vertex_Count];
		std::list<TreeNode*> ChildNodes;
		TreeNode* Parent;
	};
}

