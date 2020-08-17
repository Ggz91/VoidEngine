#pragma once
#include <iostream>
#include "../../VoidEngineInterface.h"

class IEngine;

class CEngineWrapper : public IEngineWrapper
{
public:
	CEngineWrapper(HINSTANCE h_instance, HWND h_wnd);
	~CEngineWrapper();
	void Init() override;
	void Update(const GameTimer& gt) override;
	void Draw(const GameTimer& gt) override;
	void PushModels(std::vector<RenderItem*>& render_items) override;
private:
	std::unique_ptr<IEngine> m_ptr_engine;
};
