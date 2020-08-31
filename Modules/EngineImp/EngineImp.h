#pragma once

#include "EngineInterface.h"
#include "CBaseRenderPipeline.h"

class IRenderPipeline;

struct EngineInitParam
{
	HINSTANCE HInstance; 
	HWND HWnd;
	bool UseDeferredRendering;
};

class CEngine : public IEngine
{
public:
	CEngine(EngineInitParam& init_param);
	~CEngine();
	virtual bool Initialize() override;
	virtual void OnResize() override;
	virtual void Update(const GameTimer& gt) override;
	virtual void Draw(const GameTimer& gt) override;
	virtual void PushModels(std::vector<RenderItem*>& render_items) override;
	virtual bool InitDirect3D() override;
	virtual void Debug() override;
	virtual void PitchCamera(float rad);
	virtual void RotateCameraY(float rad);
	virtual void MoveCamera(float dis);
	virtual void StrafeCamera(float dis);
private:
	std::unique_ptr<IRenderPipeline> m_render_pipeline;
};
