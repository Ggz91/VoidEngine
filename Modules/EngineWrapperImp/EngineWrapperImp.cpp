#include "EngineWrapperImp.h"
#include "../EngineImp/VoidEngineImp.h"

CEngineWrapper::CEngineWrapper(HINSTANCE h_instance, HWND h_wnd) : m_ptr_engine(std::make_unique<CVoidEgine>(h_instance, h_wnd))
{

}

CEngineWrapper::~CEngineWrapper()
{
	m_ptr_engine.release();
}

bool CEngineWrapper::Init3D()
{
	return m_ptr_engine->InitDirect3D();
}

bool CEngineWrapper::Init()
{
	return m_ptr_engine->Initialize();
}

void CEngineWrapper::Update(const GameTimer& gt)
{
	m_ptr_engine->Update(gt);
}

void CEngineWrapper::Draw(const GameTimer& gt)
{
	m_ptr_engine->Draw(gt);
}

void CEngineWrapper::PushModels(std::vector<RenderItem*>& render_items)
{
	m_ptr_engine->PushModels(render_items);
}

void CEngineWrapper::OnResize()
{
	m_ptr_engine->OnResize();
}

void CEngineWrapper::Debug()
{
	m_ptr_engine->Debug();
}

