#include "ToolbarIcons.h"
#include "PluginDefinition.h"
#include "PluginInterface.h"
#include "resource.h"

extern FuncItem funcItem[nbFunc];
extern NppData nppData;

namespace
{
	HINSTANCE g_hInst = nullptr;

	HBITMAP g_bmpToHan = nullptr;
	HICON g_iconToHan = nullptr;
	HICON g_iconToHanDark = nullptr;

	HBITMAP g_bmpToZen = nullptr;
	HICON g_iconToZen = nullptr;
	HICON g_iconToZenDark = nullptr;

	HICON loadIcon(int resId)
	{
		if (!g_hInst)
			return nullptr;

		return static_cast<HICON>(::LoadImageW(
			g_hInst,
			MAKEINTRESOURCEW(resId),
			IMAGE_ICON,
			16,
			16,
			LR_DEFAULTCOLOR));
	}

	// N++ は hToolbarBmp も必須。アイコンからカラービットマップを取り出す。
	HBITMAP bitmapFromIcon(HICON hIcon)
	{
		if (!hIcon)
			return nullptr;

		ICONINFO ii{};
		if (!::GetIconInfo(hIcon, &ii))
			return nullptr;

		if (ii.hbmMask)
			::DeleteObject(ii.hbmMask);

		// hbmColor は呼び出し側（＝本プラグイン）が所有・破棄する
		return ii.hbmColor;
	}

	bool loadIconSet(int lightId, int darkId, HBITMAP& bmp, HICON& iconLight, HICON& iconDark)
	{
		HICON newLight = loadIcon(lightId);
		HICON newDark = loadIcon(darkId);
		if (!newLight || !newDark)
		{
			if (newLight)
				::DestroyIcon(newLight);
			if (newDark)
				::DestroyIcon(newDark);
			return false;
		}

		HBITMAP newBmp = bitmapFromIcon(newLight);
		if (!newBmp)
		{
			::DestroyIcon(newLight);
			::DestroyIcon(newDark);
			return false;
		}

		bmp = newBmp;
		iconLight = newLight;
		iconDark = newDark;
		return true;
	}

	void addToolbarIcon(int funcIndex, HBITMAP bmp, HICON iconLight, HICON iconDark)
	{
		if (funcIndex < 0 || funcIndex >= nbFunc)
			return;
		if (!funcItem[funcIndex]._pFunc || !bmp || !iconLight || !iconDark)
			return;

		toolbarIconsWithDarkMode icons{};
		icons.hToolbarBmp = bmp;
		icons.hToolbarIcon = iconLight;
		icons.hToolbarIconDarkMode = iconDark;

		::SendMessage(
			nppData._nppHandle,
			NPPM_ADDTOOLBARICON_FORDARKMODE,
			static_cast<WPARAM>(funcItem[funcIndex]._cmdID),
			reinterpret_cast<LPARAM>(&icons));
	}
}

void setPluginInstance(HINSTANCE hInst)
{
	g_hInst = hInst;
}

void registerToolbarIcons()
{
	if (!g_hInst)
		return;

	if (!g_bmpToHan)
	{
		if (!loadIconSet(IDI_HAN_LIGHT, IDI_HAN_DARK, g_bmpToHan, g_iconToHan, g_iconToHanDark))
			return;
	}
	if (!g_bmpToZen)
	{
		if (!loadIconSet(IDI_ZEN_LIGHT, IDI_ZEN_DARK, g_bmpToZen, g_iconToZen, g_iconToZenDark))
			return;
	}

	addToolbarIcon(0, g_bmpToHan, g_iconToHan, g_iconToHanDark); // 全角 → 半角
	addToolbarIcon(6, g_bmpToZen, g_iconToZen, g_iconToZenDark); // 半角 → 全角
}

void destroyToolbarIcons()
{
	auto destroyIcon = [](HICON& h)
	{
		if (h)
		{
			::DestroyIcon(h);
			h = nullptr;
		}
	};
	auto destroyBmp = [](HBITMAP& h)
	{
		if (h)
		{
			::DeleteObject(h);
			h = nullptr;
		}
	};

	destroyIcon(g_iconToHan);
	destroyIcon(g_iconToHanDark);
	destroyIcon(g_iconToZen);
	destroyIcon(g_iconToZenDark);
	destroyBmp(g_bmpToHan);
	destroyBmp(g_bmpToZen);
}
