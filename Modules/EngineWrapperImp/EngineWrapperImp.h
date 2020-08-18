#pragma once
#include <iostream>
#include "../../VoidEngineInterface.h"

class IEngine;

class CEngineWrapper : public IEngineWrapper
{
public:
	CEngineWrapper(HINSTANCE h_instance, HWND h_wnd);
	~CEngineWrapper();
	bool Init3D() override;
	bool Init() override;
	void Update(const GameTimer& gt) override;
	void Draw(const GameTimer& gt) override;
	void PushModels(std::vector<RenderItem*>& render_items) override;
	void OnResize() override;
	void Debug() override;
private:
	std::unique_ptr<IEngine> m_ptr_engine;
};
