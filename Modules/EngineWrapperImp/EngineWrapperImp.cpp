#include "EngineWrapperImp.h"
#include "../EngineImp/DeferredRenderPipeline.h"
#include "../EngineImp/ZBufferRenderPipeline.h"

CEngineWrapper::CEngineWrapper(HINSTANCE h_instance, HWND h_wnd) 
{
	EngineInitParam param;
	param.HInstance = h_instance;
	param.HWnd = h_wnd;
#ifdef __ZBuffer_Rendering
	param.UseDeferredRendering = false;
#else
	param.UseDeferredRendering = true;
#endif
	m_ptr_engine = std::make_unique<CEngine>(param);
}

CEngineWrapper::~CEngineWrapper()
{

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

void CEngineWrapper::PitchCamera(float rad)
{
	m_ptr_engine->PitchCamera(rad);
}

void CEngineWrapper::RotateCameraY(float rad)
{
	m_ptr_engine->RotateCameraY(rad);
}

void CEngineWrapper::MoveCamera(float dis)
{
	m_ptr_engine->MoveCamera(dis);
}

void CEngineWrapper::StrafeCamera(float dis)
{
	m_ptr_engine->StrafeCamera(dis);
}

