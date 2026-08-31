// Minimal Notepad++ message declarations for NppZenHankaku.

#pragma once

#include <windows.h>

#define NPPMSG (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_ADDTOOLBARICON_FORDARKMODE (NPPMSG + 101)

#define NPPN_FIRST 1000
#define NPPN_TBMODIFICATION (NPPN_FIRST + 2)
#define NPPN_SHUTDOWN (NPPN_FIRST + 9)

struct toolbarIconsWithDarkMode
{
	HBITMAP hToolbarBmp;
	HICON hToolbarIcon;
	HICON hToolbarIconDarkMode;
};
