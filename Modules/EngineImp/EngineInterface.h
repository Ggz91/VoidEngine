#pragma once
#include "../Common/GameTimer.h"
#include <vector>

struct RenderItem;

class IEngine
{
public:
	virtual bool Initialize() = 0;
	virtual void OnResize() = 0;
	virtual void Update(const GameTimer& gt) = 0;
	virtual void Draw(const GameTimer& gt) = 0;
	virtual void PushModels(std::vector<RenderItem*>& render_items) = 0;
	virtual bool InitDirect3D() = 0;
	virtual void Debug() = 0;
	virtual void PitchCamera(float rad) = 0;
	virtual void RotateCameraY(float rad) = 0;
	virtual void MoveCamera(float dis) = 0;
	virtual void StrafeCamera(float dis) = 0;
};