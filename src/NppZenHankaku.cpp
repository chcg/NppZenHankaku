#include "PluginDefinition.h"

#include <windows.h>

extern FuncItem funcItem[nbFunc];
extern NppData nppData;

BOOL APIENTRY DllMain(HANDLE hModule, DWORD reasonForCall, LPVOID /*lpReserved*/)
{
	switch (reasonForCall)
	{
	case DLL_PROCESS_ATTACH:
		::DisableThreadLibraryCalls(static_cast<HMODULE>(hModule));
		pluginInit(hModule);
		break;
	default:
		break;
	}
	return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData notpadPlusData)
{
	nppData = notpadPlusData;
	commandMenuInit();
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
	return NPP_PLUGIN_NAME;
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nbF)
{
	*nbF = nbFunc;
	return funcItem;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notifyCode)
{
	if (!notifyCode)
		return;

	switch (notifyCode->nmhdr.code)
	{
	case NPPN_TBMODIFICATION:
		// ツールバーアイコンは、この通知のときだけ登録する
		addToolbarButtons();
		break;
	case NPPN_SHUTDOWN:
		commandMenuCleanUp();
		// GDI オブジェクトの破棄は、DllMain のローダーロック下ではなくここで行う
		pluginCleanUp();
		break;
	default:
		break;
	}
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*Message*/, WPARAM /*wParam*/, LPARAM /*lParam*/)
{
	return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
	return TRUE;
}
