#include "EngineImp.h"
#include "DeferredRenderPipeline.h"
#include "ZBufferRenderPipeline.h"

CEngine::CEngine(EngineInitParam& init_param)
{
	if (init_param.UseDeferredRendering)
	{
		m_render_pipeline = std::make_unique<CDeferredRenderPipeline>(init_param.HInstance, init_param.HWnd);
	}
	else
	{
		m_render_pipeline = std::make_unique<CZBufferRenderPipeline>(init_param.HInstance, init_param.HWnd);
	}
}

CEngine::~CEngine()
{
	if (NULL != m_render_pipeline)
	{
		m_render_pipeline.release();
		m_render_pipeline = NULL;
	}
}

bool CEngine::Initialize()
{
	return m_render_pipeline->Initialize();
}

void CEngine::OnResize()
{
	m_render_pipeline->OnResize();
}

void CEngine::Update(const GameTimer& gt)
{
	m_render_pipeline->Update(gt);
}

void CEngine::Draw(const GameTimer& gt)
{
	m_render_pipeline->Draw(gt);
}

void CEngine::PushModels(std::vector<RenderItem*>& render_items)
{
	m_render_pipeline->PushModels(render_items);
}

bool CEngine::InitDirect3D()
{
	return m_render_pipeline->InitDirect3D();
}

void CEngine::Debug()
{
	m_render_pipeline->Debug();
}

void CEngine::PitchCamera(float rad)
{
	m_render_pipeline->PitchCamera(rad);
}

void CEngine::RotateCameraY(float rad)
{
	m_render_pipeline->RotateCameraY(rad);
}

void CEngine::MoveCamera(float dis)
{
	m_render_pipeline->MoveCamera(dis);
}

void CEngine::StrafeCamera(float dis)
{
	m_render_pipeline->StrafeCamera(dis);
}




