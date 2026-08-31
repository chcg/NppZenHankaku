// Minimal Scintilla declarations for Notepad++ plugins.
// Subset of Scintilla API used by NppZenHankaku.

#pragma once

#include <windows.h>
#include <cstdint>

using Sci_Position = intptr_t;
using Sci_PositionU = uintptr_t;
using Sci_PositionCR = long;

struct Sci_CharacterRange
{
	Sci_PositionCR cpMin;
	Sci_PositionCR cpMax;
};

struct Sci_TextRange
{
	Sci_CharacterRange chrg;
	char* lpstrText;
};

struct SCNotification
{
	NMHDR nmhdr;
	Sci_Position position;
	int ch;
	int modifiers;
	int modificationType;
	const char* text;
	Sci_Position length;
	Sci_Position linesAdded;
	int message;
	Sci_Position wParam;
	Sci_Position lParam;
	Sci_Position line;
	int foldLevelNow;
	int foldLevelPrev;
	int margin;
	int listType;
	int x;
	int y;
	int token;
	Sci_Position annotationLinesAdded;
	int updated;
	int listCompletionMethod;
	int characterSource;
};

#define SC_CP_UTF8 65001

#define SCI_GETLENGTH 2006
#define SCI_GETCURRENTPOS 2008
#define SCI_GETANCHOR 2009
#define SCI_GETFIRSTVISIBLELINE 2152
#define SCI_SETFIRSTVISIBLELINE 2613
#define SCI_VISIBLEFROMDOCLINE 2220
#define SCI_DOCLINEFROMVISIBLE 2221
#define SCI_SETXOFFSET 2397
#define SCI_GETXOFFSET 2398
#define SCI_GETTEXT 2182
#define SCI_SETTEXT 2181
#define SCI_GETCODEPAGE 2137
#define SCI_GETREADONLY 2140
#define SCI_SETSELECTIONSTART 2142
#define SCI_GETSELECTIONSTART 2143
#define SCI_SETSELECTIONEND 2144
#define SCI_GETSELECTIONEND 2145
#define SCI_GETSELTEXT 2161
#define SCI_SETSEL 2160
#define SCI_SELECTIONISRECTANGLE 2372
#define SCI_REPLACETARGET 2194
#define SCI_SETTARGETSTART 2190
#define SCI_SETTARGETEND 2192
#define SCI_SETTARGETRANGE 2686
#define SCI_GETTARGETTEXT 2687
#define SCI_BEGINUNDOACTION 2078
#define SCI_ENDUNDOACTION 2079
#define SCI_GETTEXTRANGE 2162

// 複数選択。SCI_SETSELECTION と SCI_ADDSELECTION の引数は (caret, anchor) の順で、
// start / end の順ではない。取り違えると選択の向きが反転する。
#define SCI_GETSELECTIONS 2570
#define SCI_SETSELECTION 2572
#define SCI_ADDSELECTION 2573
#define SCI_SETMAINSELECTION 2574
#define SCI_GETMAINSELECTION 2575
#define SCI_GETSELECTIONNCARET 2577
#define SCI_GETSELECTIONNANCHOR 2579
#define SCI_SETSELECTIONNCARETVIRTUALSPACE 2580
#define SCI_GETSELECTIONNCARETVIRTUALSPACE 2581
#define SCI_SETSELECTIONNANCHORVIRTUALSPACE 2582
#define SCI_GETSELECTIONNANCHORVIRTUALSPACE 2583
#define SCI_GETSELECTIONNSTART 2585
#define SCI_GETSELECTIONNEND 2587

// 矩形選択。Scintilla は矩形を anchor / caret の 2 点だけで保持し、行ごとの範囲は
// そこから導出する（列の計算、全角を 2 列と数えること、短い行や空行の仮想空白も
// すべて Scintilla 側の仕事）。行ごとの範囲は上の SCI_GETSELECTIONN* で読める。
#define SCI_SETRECTANGULARSELECTIONCARET 2588
#define SCI_GETRECTANGULARSELECTIONCARET 2589
#define SCI_SETRECTANGULARSELECTIONANCHOR 2590
#define SCI_GETRECTANGULARSELECTIONANCHOR 2591
#define SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE 2592
#define SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE 2593
#define SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE 2594
#define SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE 2595
