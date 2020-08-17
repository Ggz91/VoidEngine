#include "VoidEngineInterface.h"
#include "Modules/EngineWrapperImp/EngineWrapperImp.h"

static IEngineWrapper* singleton_engine_ptr = NULL;

IEngineWrapper* GetEngineWrapper(HINSTANCE h_instance, HWND h_wnd)
{
	if (NULL == singleton_engine_ptr)
	{
		singleton_engine_ptr = new CEngineWrapper(h_instance, h_wnd);
	}
	return singleton_engine_ptr;
}
