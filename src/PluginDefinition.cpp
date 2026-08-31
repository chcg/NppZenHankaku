#include "PluginDefinition.h"
#include "DbcsCodec.h"
#include "ToolbarIcons.h"
#include "converter.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <vector>

FuncItem funcItem[nbFunc];
NppData nppData;

namespace
{
	constexpr Sci_Position kMaxConvertBytes = 64 * 1024 * 1024; // 64MB ガード

	// これ以下しか離れていない編集は 1 回の置換にまとめる（間の未変更バイトは元のまま持ち越す）。
	// 連続する変換単位は変換器の側で既に 1 件になっているので、ここでの結合は
	// 「変換されなかった文字をまたいで、置換範囲を広げる」ことを意味する。
	// 広げるほどマーカーやインジケータを巻き込むため、既定では広げない。
	constexpr size_t kEditMergeGap = 0;
	// 結合後もこの件数を超えたら、次の段階へ移る（converter.h の coalesceEdits）。
	// 開発環境での計測では、置換 1 件あたり約 46µs。上限に張り付いたときの
	// 上乗せは約 190ms に収まる。
	//
	// この上限があるため、製品ビルドで実際に走る置換は 4096 件以下に収まる。
#ifndef NPPZH_MAX_EDIT_COUNT
#define NPPZH_MAX_EDIT_COUNT 4096
#endif
	constexpr size_t kMaxEditCount = NPPZH_MAX_EDIT_COUNT;

	HWND getCurrentScintilla()
	{
		int which = -1;
		::SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
		if (which == -1)
			return nullptr;
		return (which == 0) ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle;
	}

	Sci_Position sciGet(HWND scintilla, UINT message, Sci_Position wParam = 0)
	{
		return static_cast<Sci_Position>(::SendMessage(scintilla, message, static_cast<WPARAM>(wParam), 0));
	}

	// 文書の [begin, end) をバイト列で取り出す。
	// stringresult 系なので、終端ヌルの分を足したバッファが必要。
	bool readRange(HWND scintilla, Sci_Position begin, Sci_Position end, std::string& out)
	{
		const Sci_Position length = end - begin;
		if (length <= 0)
			return false;

		out.assign(static_cast<size_t>(length) + 1, '\0');
		::SendMessage(scintilla, SCI_SETTARGETRANGE, static_cast<WPARAM>(begin), static_cast<LPARAM>(end));
		::SendMessage(scintilla, SCI_GETTARGETTEXT, 0, reinterpret_cast<LPARAM>(out.data()));
		out.resize(static_cast<size_t>(length));
		return true;
	}

	// 選択 1 つ分の状態。復元まで受け持つので、向き（アンカーとキャレット）と
	// 仮想空白も持っておく。SCI_SETSELECTION / SCI_ADDSELECTION は仮想空白を
	// 0 に戻してしまうため、入れ直すには覚えておく必要がある。
	struct SelectionState
	{
		Sci_Position start = 0;
		Sci_Position end = 0;
		Sci_Position anchor = 0;
		Sci_Position caret = 0;
		Sci_Position anchorSpace = 0;
		Sci_Position caretSpace = 0;
	};

	// 矩形選択の状態。Scintilla は矩形を anchor / caret の 2 点だけで保持し、
	// 行ごとの範囲はそこから導出する。列の計算（全角を 2 列と数える、短い行や
	// 空行を仮想空白で埋める）はすべて Scintilla 側の仕事なので、こちらは
	// 行ごとの範囲を読んで変換し、2 点を戻すだけでよい。
	struct RectangleState
	{
		bool active = false;
		Sci_Position anchor = 0;
		Sci_Position caret = 0;
		Sci_Position anchorSpace = 0;
		Sci_Position caretSpace = 0;
	};

	// SCI_GETCODEPAGE が CP_ACP (0) を返した場合は GetACP() で実効値へ解決する。
	// UTF-8 ロケールでは実効値が 65001 になり、codePage == 0 のままだと UTF-8 判定に失敗する。
	// 以降の UTF-8 判定とコーデック生成には、必ずここで解決した値を使う。
	int resolveCodePage(int codePage)
	{
		return (codePage == CP_ACP) ? static_cast<int>(::GetACP()) : codePage;
	}

	bool isUtf8CodePage(int codePage)
	{
		return codePage == SC_CP_UTF8 || codePage == CP_UTF8;
	}

	// 編集は文書座標の昇順で来る。後ろから適用すると、まだ処理していない編集の
	// バイト位置がずれない。全体を 1 つの Undo アクションにまとめるので、
	// 複数選択でも Ctrl+Z 一回で元に戻る。
	void applyEdits(HWND scintilla, const std::vector<ConvertEdit>& edits)
	{
		::SendMessage(scintilla, SCI_BEGINUNDOACTION, 0, 0);
		for (auto it = edits.rbegin(); it != edits.rend(); ++it)
		{
			::SendMessage(scintilla, SCI_SETTARGETRANGE,
				static_cast<WPARAM>(it->begin), static_cast<LPARAM>(it->end));
			::SendMessage(scintilla, SCI_REPLACETARGET,
				static_cast<WPARAM>(it->text.size()), reinterpret_cast<LPARAM>(it->text.c_str()));
		}
		::SendMessage(scintilla, SCI_ENDUNDOACTION, 0, 0);
	}

	void applyConversion(ConvertMode mode, ConvertCategory category)
	{
		HWND scintilla = getCurrentScintilla();
		if (!scintilla)
			return;

		if (::SendMessage(scintilla, SCI_GETREADONLY, 0, 0) != 0)
		{
			::MessageBoxW(nppData._nppHandle, L"読み取り専用の文書は変換できません。", L"NppZenHankaku", MB_OK | MB_ICONINFORMATION);
			return;
		}

		RectangleState rect;
		rect.active = (::SendMessage(scintilla, SCI_SELECTIONISRECTANGLE, 0, 0) != 0);
		if (rect.active)
		{
			rect.anchor = sciGet(scintilla, SCI_GETRECTANGULARSELECTIONANCHOR);
			rect.caret = sciGet(scintilla, SCI_GETRECTANGULARSELECTIONCARET);
			rect.anchorSpace = sciGet(scintilla, SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE);
			rect.caretSpace = sciGet(scintilla, SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE);
		}

		const size_t selectionCount = static_cast<size_t>(::SendMessage(scintilla, SCI_GETSELECTIONS, 0, 0));
		if (selectionCount == 0)
			return;

		const Sci_Position mainSelection = sciGet(scintilla, SCI_GETMAINSELECTION);

		std::vector<SelectionState> selections(selectionCount);
		for (size_t i = 0; i < selectionCount; ++i)
		{
			const Sci_Position n = static_cast<Sci_Position>(i);
			SelectionState& selection = selections[i];
			selection.start = sciGet(scintilla, SCI_GETSELECTIONNSTART, n);
			selection.end = sciGet(scintilla, SCI_GETSELECTIONNEND, n);
			selection.anchor = sciGet(scintilla, SCI_GETSELECTIONNANCHOR, n);
			selection.caret = sciGet(scintilla, SCI_GETSELECTIONNCARET, n);
			selection.anchorSpace = sciGet(scintilla, SCI_GETSELECTIONNANCHORVIRTUALSPACE, n);
			selection.caretSpace = sciGet(scintilla, SCI_GETSELECTIONNCARETVIRTUALSPACE, n);
		}

		// 変換する範囲を決める。選択が 1 つで空なら文書全体、それ以外は非空の選択すべて。
		// 空のキャレットが複数ある状態で全文変換へ落ちると事故になるので、その場合は何もしない。
		// 矩形選択は幅 0 でも全文変換に落とさない（1 行だけの幅 0 矩形は空の選択 1 つに見える）。
		std::vector<ConvertRange> ranges;
		if (!rect.active && selectionCount == 1 && selections[0].start == selections[0].end)
		{
			const Sci_Position length = sciGet(scintilla, SCI_GETLENGTH);
			if (length > kMaxConvertBytes)
			{
				::MessageBoxW(
					nppData._nppHandle,
					L"変換対象が大きすぎます（上限: 64MB）。",
					L"NppZenHankaku",
					MB_OK | MB_ICONWARNING);
				return;
			}

			ConvertRange range;
			range.begin = 0;
			if (!readRange(scintilla, 0, length, range.bytes))
				return;
			ranges.push_back(std::move(range));
		}
		else
		{
			std::vector<SelectionState> ordered;
			for (const SelectionState& selection : selections)
			{
				if (selection.start != selection.end)
					ordered.push_back(selection);
			}
			if (ordered.empty())
				return;

			std::sort(ordered.begin(), ordered.end(),
				[](const SelectionState& a, const SelectionState& b) { return a.start < b.start; });

			// 重なった選択が来ると位置の対応付けが崩れる。Scintilla は通常
			// 重なりを作らないが、他のプラグインやマクロ経由なら起こりうる。
			Sci_Position total = 0;
			for (size_t i = 0; i < ordered.size(); ++i)
			{
				if (i > 0 && ordered[i].start < ordered[i - 1].end)
				{
					::MessageBoxW(
						nppData._nppHandle,
						L"選択範囲が重なっています。\n重なりを解いてから実行してください。",
						L"NppZenHankaku",
						MB_OK | MB_ICONINFORMATION);
					return;
				}
				total += ordered[i].end - ordered[i].start;
			}

			if (total > kMaxConvertBytes)
			{
				::MessageBoxW(
					nppData._nppHandle,
					L"変換対象が大きすぎます（上限: 64MB）。",
					L"NppZenHankaku",
					MB_OK | MB_ICONWARNING);
				return;
			}

			for (const SelectionState& selection : ordered)
			{
				ConvertRange range;
				range.begin = static_cast<size_t>(selection.start);
				if (!readRange(scintilla, selection.start, selection.end, range.bytes))
					continue;
				ranges.push_back(std::move(range));
			}
		}

		if (ranges.empty())
			return;

		const int codePage = resolveCodePage(static_cast<int>(::SendMessage(scintilla, SCI_GETCODEPAGE, 0, 0)));

		// 文書の文字コードのまま変換する。コーデックを差し替えるだけで、UTF-8 でも ANSI でも
		// 文書のバイト位置がそのまま ConvertEdit のオフセットになるので、以降の処理は共通でよい。
		std::unique_ptr<DbcsCodec> ansiCodec;
		const TextCodec* codec = &utf8Codec();
		if (!isUtf8CodePage(codePage))
		{
			ansiCodec = DbcsCodec::create(codePage);
			if (!ansiCodec)
			{
				::MessageBoxW(
					nppData._nppHandle,
					L"この文書の文字コードには対応していません。\n文書をUTF-8へ変換してから実行してください。",
					L"NppZenHankaku",
					MB_OK | MB_ICONERROR);
				return;
			}
			codec = ansiCodec.get();
		}

		// 折り返し表示では表示行と文書行がずれる。変換で行数は変わらないので、
		// 文書行に直して覚えておけば折り返し幅が変わっても同じ行へ戻せる。
		const LRESULT firstVisibleLine = ::SendMessage(scintilla, SCI_GETFIRSTVISIBLELINE, 0, 0);
		const LRESULT firstVisibleDocLine = ::SendMessage(scintilla, SCI_DOCLINEFROMVISIBLE, static_cast<WPARAM>(firstVisibleLine), 0);
		const LRESULT xOffset = ::SendMessage(scintilla, SCI_GETXOFFSET, 0, 0);

		// すべての選択のアンカーとキャレットを変換と同時に対応付ける。置換をまとめた後では
		// 変換単位の境界が分からず、連続変換の内側にある位置がその末尾まで飛んでしまう。
		// 選択の外にある位置も、手前の選択で増減したバイト数だけ converter 側でずらされる。
		//
		// 矩形選択では行ごとのアンカー・キャレットを追跡しない。Scintilla が矩形の
		// 2 点から導出し直すので、追跡しても捨てられる。
		std::vector<size_t> positions;
		if (rect.active)
		{
			positions.push_back(static_cast<size_t>(rect.anchor));
			positions.push_back(static_cast<size_t>(rect.caret));
		}
		else
		{
			positions.reserve(selectionCount * 2);
			for (const SelectionState& selection : selections)
			{
				positions.push_back(static_cast<size_t>(selection.anchor));
				positions.push_back(static_cast<size_t>(selection.caret));
			}
		}

		const std::vector<ConvertEdit> edits = convertZenHankakuRanges(
			ranges, mode, category, *codec, kEditMergeGap, kMaxEditCount, &positions);
		if (edits.empty())
			return;

		applyEdits(scintilla, edits);

		if (rect.active)
		{
			// 矩形は 2 点を戻すだけ。行ごとの範囲は Scintilla が列から導出し直す。
			// 変換で文字幅が変わるので、戻る矩形は元と同じ列にはならない。2 点が
			// それぞれの文字に付いて動いた結果であり、キャレット追跡と同じ考え方。
			::SendMessage(scintilla, SCI_SETRECTANGULARSELECTIONANCHOR, static_cast<WPARAM>(positions[0]), 0);
			::SendMessage(scintilla, SCI_SETRECTANGULARSELECTIONANCHORVIRTUALSPACE, static_cast<WPARAM>(rect.anchorSpace), 0);
			::SendMessage(scintilla, SCI_SETRECTANGULARSELECTIONCARET, static_cast<WPARAM>(positions[1]), 0);
			::SendMessage(scintilla, SCI_SETRECTANGULARSELECTIONCARETVIRTUALSPACE, static_cast<WPARAM>(rect.caretSpace), 0);
		}
		else
		{
			// 選択は元の並び順で入れ直す。並びが変わるとメイン選択の番号が別の選択を指してしまう。
			for (size_t i = 0; i < selectionCount; ++i)
			{
				const Sci_Position newAnchor = static_cast<Sci_Position>(positions[i * 2]);
				const Sci_Position newCaret = static_cast<Sci_Position>(positions[i * 2 + 1]);
				::SendMessage(scintilla, (i == 0) ? SCI_SETSELECTION : SCI_ADDSELECTION,
					static_cast<WPARAM>(newCaret), static_cast<LPARAM>(newAnchor));
			}

			// 仮想空白とメイン選択は入れ直した後に戻す。Scintilla は追加した選択と重なる
			// 既存の選択を畳むことがあり、そのぶん件数が減りうるので、実際の件数の中だけ触る。
			const size_t restoredCount = static_cast<size_t>(::SendMessage(scintilla, SCI_GETSELECTIONS, 0, 0));
			for (size_t i = 0; i < restoredCount && i < selectionCount; ++i)
			{
				if (selections[i].anchorSpace == 0 && selections[i].caretSpace == 0)
					continue;
				::SendMessage(scintilla, SCI_SETSELECTIONNANCHORVIRTUALSPACE,
					static_cast<WPARAM>(i), static_cast<LPARAM>(selections[i].anchorSpace));
				::SendMessage(scintilla, SCI_SETSELECTIONNCARETVIRTUALSPACE,
					static_cast<WPARAM>(i), static_cast<LPARAM>(selections[i].caretSpace));
			}
			if (mainSelection < static_cast<Sci_Position>(restoredCount))
				::SendMessage(scintilla, SCI_SETMAINSELECTION, static_cast<WPARAM>(mainSelection), 0);
		}

		const LRESULT restoredLine = ::SendMessage(scintilla, SCI_VISIBLEFROMDOCLINE, static_cast<WPARAM>(firstVisibleDocLine), 0);
		::SendMessage(scintilla, SCI_SETFIRSTVISIBLELINE, static_cast<WPARAM>(restoredLine), 0);
		::SendMessage(scintilla, SCI_SETXOFFSET, static_cast<WPARAM>(xOffset), 0);
	}

	void runConversion(ConvertMode mode, ConvertCategory category)
	{
		try
		{
			applyConversion(mode, category);
		}
		catch (const std::exception& ex)
		{
			::MessageBoxA(nppData._nppHandle, ex.what(), "NppZenHankaku", MB_OK | MB_ICONERROR);
		}
		catch (...)
		{
			::MessageBoxW(nppData._nppHandle, L"変換中に不明なエラーが発生しました。", L"NppZenHankaku", MB_OK | MB_ICONERROR);
		}
	}
}

void pluginInit(HANDLE hModule)
{
	setPluginInstance(static_cast<HINSTANCE>(hModule));
}

void pluginCleanUp()
{
	destroyToolbarIcons();
}

void addToolbarButtons()
{
	registerToolbarIcons();
}

void commandMenuInit()
{
	setCommand(0, L"全角 → 半角（すべて）", convertToHankakuAll, nullptr, false);
	setCommand(1, L"全角 → 半角（英数字）", convertToHankakuAlphaNum, nullptr, false);
	setCommand(2, L"全角 → 半角（カタカナ）", convertToHankakuKatakana, nullptr, false);
	setCommand(3, L"全角 → 半角（記号）", convertToHankakuSymbol, nullptr, false);
	setCommand(4, L"全角 → 半角（スペース）", convertToHankakuSpace, nullptr, false);
	setCommand(5, L"---", nullptr, nullptr, false); // 区切り線
	setCommand(6, L"半角 → 全角（すべて）", convertToZenkakuAll, nullptr, false);
	setCommand(7, L"半角 → 全角（英数字）", convertToZenkakuAlphaNum, nullptr, false);
	setCommand(8, L"半角 → 全角（カタカナ）", convertToZenkakuKatakana, nullptr, false);
	setCommand(9, L"半角 → 全角（記号）", convertToZenkakuSymbol, nullptr, false);
	setCommand(10, L"半角 → 全角（スペース）", convertToZenkakuSpace, nullptr, false);
}

void commandMenuCleanUp()
{
}

bool setCommand(size_t index, const wchar_t* cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey* sk, bool check0nInit)
{
	if (index >= nbFunc || !cmdName)
		return false;

	// pFunc == nullptr はメニュー区切り用
	wcsncpy_s(funcItem[index]._itemName, menuItemSize, cmdName, _TRUNCATE);
	funcItem[index]._pFunc = pFunc;
	funcItem[index]._init2Check = check0nInit;
	funcItem[index]._pShKey = sk;
	return true;
}

void convertToHankakuAll() { runConversion(ConvertMode::ToHankaku, ConvertCategory::All); }
void convertToHankakuAlphaNum() { runConversion(ConvertMode::ToHankaku, ConvertCategory::AlphaNum); }
void convertToHankakuKatakana() { runConversion(ConvertMode::ToHankaku, ConvertCategory::Katakana); }
void convertToHankakuSymbol() { runConversion(ConvertMode::ToHankaku, ConvertCategory::Symbol); }
void convertToHankakuSpace() { runConversion(ConvertMode::ToHankaku, ConvertCategory::Space); }

void convertToZenkakuAll() { runConversion(ConvertMode::ToZenkaku, ConvertCategory::All); }
void convertToZenkakuAlphaNum() { runConversion(ConvertMode::ToZenkaku, ConvertCategory::AlphaNum); }
void convertToZenkakuKatakana() { runConversion(ConvertMode::ToZenkaku, ConvertCategory::Katakana); }
void convertToZenkakuSymbol() { runConversion(ConvertMode::ToZenkaku, ConvertCategory::Symbol); }
void convertToZenkakuSpace() { runConversion(ConvertMode::ToZenkaku, ConvertCategory::Space); }
