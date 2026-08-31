#include "converter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include "DbcsCodec.h"
#endif

namespace
{
	int g_failures = 0;

	// 指定した文字だけ「このコーデックでは書けない」とみなすラッパー。
	// 出力コードページに無い文字を含む変換単位が、まるごと据え置かれることを確かめる。
	class RejectingCodec final : public TextCodec
	{
	public:
		RejectingCodec(const TextCodec& inner, std::u32string rejected)
			: inner_(inner)
			, rejected_(std::move(rejected))
		{
		}

		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const override
		{
			return inner_.decode(bytes, pos, out);
		}

		bool encode(std::u32string_view text, std::string& out) const override
		{
			if (text.find_first_of(rejected_) != std::u32string_view::npos)
				return false;
			return inner_.encode(text, out);
		}

	private:
		const TextCodec& inner_;
		std::u32string rejected_;
	};

	RejectingCodec rejectUtf8(std::u32string unsupported)
	{
		return RejectingCodec(utf8Codec(), std::move(unsupported));
	}

	// **契約違反を演じるコーデック。** 「失敗するときは out を変更しない」という
	// encode の契約をわざと破り、**末尾へ追記してから** false を返す。
	//
	// 記録側が変換結果を編集の置換テキストへ直接書くようになった（8.2）ので、
	// 契約が破られたときに汚れるのは使い捨てのバッファではなく**文書へ書き戻す
	// 文字列**になった。追記して失敗する形については、記録側が書いた分を戻す。
	//
	// **戻せるのはこの形だけ。** 呼び出し前からあった部分を消したり書き換えたり
	// するコーデックは、記録側では復元できない（そこまでは守れない）。
	class AppendThenFailCodec final : public TextCodec
	{
	public:
		AppendThenFailCodec(const TextCodec& inner, std::u32string rejected)
			: inner_(inner)
			, rejected_(std::move(rejected))
		{
		}

		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const override
		{
			return inner_.decode(bytes, pos, out);
		}

		bool encode(std::u32string_view text, std::string& out) const override
		{
			if (text.find_first_of(rejected_) != std::u32string_view::npos)
			{
				// 先に書いてしまってから弾く。out は呼び出し前の状態に戻らない。
				out += "!LEAK!";
				return false;
			}

			return inner_.encode(text, out);
		}

	private:
		const TextCodec& inner_;
		std::u32string rejected_;
	};

	// 別のコーデックへ丸投げするだけのラッパー。用途は 2 つある。
	//
	// 1. utf8Codec() をそのまま渡すと非仮想の高速経路（Utf8Access）へ振り分けられる。
	//    包んで別インスタンスにすれば、同じ UTF-8 の処理を仮想経路（CodecAccess）へ
	//    通せる。二重化した実装が食い違っていないかを差分で確かめるのに使う。
	// 2. **1 段積むごとに 1 文字あたりの仮想呼び出しが 1 回増える。** 8.1 の測定で
	//    段数を振るのに使う。**ただし所要時間は段数に比例しない**（1 回で +180ms、
	//    2 回で +580ms、3 回で ±0）。傾きから 0 回を外挿する見積もりには使えない。
	class DelegatingCodec final : public TextCodec
	{
	public:
		explicit DelegatingCodec(const TextCodec& inner)
			: inner_(inner)
		{
		}

		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const override
		{
			return inner_.decode(bytes, pos, out);
		}

		bool encode(std::u32string_view text, std::string& out) const override
		{
			return inner_.encode(text, out);
		}

	private:
		const TextCodec& inner_;
	};

	// CP932 を模した最小の 2 バイトコードページ。Windows API に依存せずに、
	// ANSI 経路の性質（バイトオフセット、重複マッピング、不正バイトの保持）を検証する。
	//
	// 単バイト: 0x00-0x7F は ASCII、0xA1-0xDF は半角カナ (U+FF61-U+FF9F)
	// リードバイト: 0x81-0x9F と 0xE0-0xEF
	// 0x9C40 と 0xE040 はどちらも U+7E8A へ復号される「重複マッピング」。符号化は 0x9C40 を選ぶ。
	class FakeDbcsCodec final : public TextCodec
	{
	public:
		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const override
		{
			const unsigned char b0 = static_cast<unsigned char>(bytes[pos]);

			if (isLeadByte(b0) && pos + 1 < bytes.size())
			{
				const unsigned char b1 = static_cast<unsigned char>(bytes[pos + 1]);
				const unsigned key = (static_cast<unsigned>(b0) << 8) | b1;
				if (const auto found = decodeTable().find(key); found != decodeTable().end())
				{
					out = found->second;
					return 2;
				}
			}

			if (b0 <= 0x7F)
			{
				out = b0;
				return 1;
			}

			if (b0 >= 0xA1 && b0 <= 0xDF)
			{
				out = static_cast<char32_t>(0xFF61 + (b0 - 0xA1));
				return 1;
			}

			out = kInvalidByteBase + b0;
			return 1;
		}

		bool encode(std::u32string_view text, std::string& out) const override
		{
			std::string buffer;
			for (const char32_t cp : text)
			{
				if (cp <= 0x7F)
				{
					buffer.push_back(static_cast<char>(cp));
					continue;
				}

				if (cp >= 0xFF61 && cp <= 0xFF9F)
				{
					buffer.push_back(static_cast<char>(0xA1 + (cp - 0xFF61)));
					continue;
				}

				const auto found = encodeTable().find(cp);
				if (found == encodeTable().end())
					return false;

				buffer.push_back(static_cast<char>(found->second >> 8));
				buffer.push_back(static_cast<char>(found->second & 0xFF));
			}

			out.append(buffer);
			return true;
		}

	private:
		static bool isLeadByte(unsigned char b)
		{
			return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEF);
		}

		static const std::unordered_map<unsigned, char32_t>& decodeTable()
		{
			static const std::unordered_map<unsigned, char32_t> table = {
				{0x8140, U'　'}, {0x8141, U'￥'},
				{0x8260, U'Ａ'}, {0x8261, U'Ｂ'}, {0x8281, U'ａ'}, {0x8282, U'ｂ'},
				{0x8341, U'ア'}, {0x8342, U'ガ'},
				{0x8940, U'漢'},
				{0x9C40, U'纊'}, {0xE040, U'纊'}, // 重複マッピング
			};
			return table;
		}

		static const std::unordered_map<char32_t, unsigned>& encodeTable()
		{
			static const std::unordered_map<char32_t, unsigned> table = {
				{U'　', 0x8140}, {U'￥', 0x8141},
				{U'Ａ', 0x8260}, {U'Ｂ', 0x8261}, {U'ａ', 0x8281}, {U'ｂ', 0x8282},
				{U'ア', 0x8341}, {U'ガ', 0x8342},
				{U'漢', 0x8940},
				{U'纊', 0x9C40}, // 重複のうちこちらを正規の符号化とする
			};
			return table;
		}
	};

	// 2 バイト列・1 バイトをバイト列として書くためのヘルパー
	std::string db(unsigned seq)
	{
		return std::string{ static_cast<char>((seq >> 8) & 0xFF), static_cast<char>(seq & 0xFF) };
	}

	std::string sb(unsigned char b)
	{
		return std::string(1, static_cast<char>(b));
	}

	// ANSI バイト列はそのままでは読めないので、失敗時の表示用に 16 進へ直す
	std::string hex(std::string_view bytes)
	{
		static const char digits[] = "0123456789ABCDEF";
		std::string out;
		for (const char c : bytes)
		{
			const unsigned char b = static_cast<unsigned char>(c);
			if (!out.empty())
				out += ' ';
			out += digits[b >> 4];
			out += digits[b & 0x0F];
		}
		return out;
	}

	std::string enc(const TextCodec& codec, std::u32string_view text)
	{
		std::string out;
		codec.encode(text, out);
		return out;
	}

	// 編集の一覧を "[begin,end)=text|..." の形に整形して比較する
	std::string format(const std::vector<ConvertEdit>& edits)
	{
		std::string out;
		for (const ConvertEdit& edit : edits)
		{
			if (!out.empty())
				out += "|";
			out += "[" + std::to_string(edit.begin) + "," + std::to_string(edit.end) + ")=" + edit.text;
		}
		return out;
	}

	// 同上。ただし置換テキストは 16 進で示す（ANSI 用）
	std::string formatHex(const std::vector<ConvertEdit>& edits)
	{
		std::string out;
		for (const ConvertEdit& edit : edits)
		{
			if (!out.empty())
				out += "|";
			out += "[" + std::to_string(edit.begin) + "," + std::to_string(edit.end) + ")=" + hex(edit.text);
		}
		return out;
	}

	// 位置を変換に通し、対応付いた結果を "0,3,6" の形に整形して比較する
	std::string track(const std::string& bytes, ConvertMode mode, std::vector<size_t> positions, const TextCodec& codec = utf8Codec())
	{
		convertZenHankakuEdits(bytes, mode, ConvertCategory::All, codec, &positions);

		std::string out;
		for (const size_t pos : positions)
		{
			if (!out.empty())
				out += ",";
			out += std::to_string(pos);
		}
		return out;
	}

	// 文書と選択範囲から ConvertRange を組み立てる
	std::vector<ConvertRange> makeRanges(const std::string& document, const std::vector<std::pair<size_t, size_t>>& spans)
	{
		std::vector<ConvertRange> ranges;
		for (const std::pair<size_t, size_t>& span : spans)
		{
			ConvertRange range;
			range.begin = span.first;
			range.bytes = document.substr(span.first, span.second - span.first);
			ranges.push_back(std::move(range));
		}
		return ranges;
	}

	// 返ってきた編集を文書へ当てる。編集は昇順で文書座標なので、前から順に組み直せる。
	std::string rebuild(const std::string& document, const std::vector<ConvertEdit>& edits)
	{
		std::string out;
		size_t pos = 0;
		for (const ConvertEdit& edit : edits)
		{
			out.append(document, pos, edit.begin - pos);
			out.append(edit.text);
			pos = edit.end;
		}
		out.append(document, pos, document.size() - pos);
		return out;
	}

	// 複数範囲の変換を通し、変換後の文書を返す
	std::string convertSpans(const std::string& document, const std::vector<std::pair<size_t, size_t>>& spans,
		ConvertMode mode, size_t maxTotalEditCount = SIZE_MAX, const TextCodec& codec = utf8Codec())
	{
		return rebuild(document, convertZenHankakuRanges(
			makeRanges(document, spans), mode, ConvertCategory::All, codec, 0, maxTotalEditCount, nullptr));
	}

	// 複数範囲の変換で位置を追跡し、"0,3,6" の形に整形して比較する
	std::string trackSpans(const std::string& document, const std::vector<std::pair<size_t, size_t>>& spans,
		ConvertMode mode, std::vector<size_t> positions, const TextCodec& codec = utf8Codec())
	{
		convertZenHankakuRanges(makeRanges(document, spans), mode, ConvertCategory::All, codec, 0, SIZE_MAX, &positions);

		std::string out;
		for (const size_t pos : positions)
		{
			if (!out.empty())
				out += ",";
			out += std::to_string(pos);
		}
		return out;
	}

	void expect(const char* name, const std::string& actual, const std::string& expected)
	{
		if (actual == expected)
		{
			std::cout << "[OK] " << name << "\n";
			return;
		}

		++g_failures;
		std::cout << "[NG] " << name << "\n  expected: " << expected << "\n  actual:   " << actual << "\n";
	}

	void runUtf8Tests()
	{
		expect("zen->han ascii",
			convertZenHankaku(u8"ＡＢＣ１２３！", ConvertMode::ToHankaku),
			"ABC123!");

		expect("han->zen ascii",
			convertZenHankaku("ABC123!", ConvertMode::ToZenkaku),
			u8"ＡＢＣ１２３！");

		expect("zen->han all with space",
			convertZenHankaku(u8"全角　スペース", ConvertMode::ToHankaku),
			u8"全角 ｽﾍﾟｰｽ");

		expect("han->zen space",
			convertZenHankaku(u8"半角 スペース", ConvertMode::ToZenkaku),
			u8"半角　スペース");

		expect("zen->han kana",
			convertZenHankaku(u8"カタカナ", ConvertMode::ToHankaku),
			u8"ｶﾀｶﾅ");

		expect("han->zen kana",
			convertZenHankaku(u8"ｶﾀｶﾅ", ConvertMode::ToZenkaku),
			u8"カタカナ");

		expect("zen->han voiced",
			convertZenHankaku(u8"ガギグゲゴ", ConvertMode::ToHankaku),
			u8"ｶﾞｷﾞｸﾞｹﾞｺﾞ");

		expect("han->zen voiced",
			convertZenHankaku(u8"ｶﾞｷﾞｸﾞｹﾞｺﾞ", ConvertMode::ToZenkaku),
			u8"ガギグゲゴ");

		expect("zen->han wa/wo voiced",
			convertZenHankaku(u8"ヷヺ", ConvertMode::ToHankaku),
			u8"ﾜﾞｦﾞ");

		expect("han->zen wa/wo voiced",
			convertZenHankaku(u8"ﾜﾞｦﾞ", ConvertMode::ToZenkaku),
			u8"ヷヺ");

		// ヸ / ヹ は基底の ヰ / ヱ に半角が無いため変換しない
		expect("zen->han vi/ve unchanged",
			convertZenHankaku(u8"ヸヹ", ConvertMode::ToHankaku),
			u8"ヸヹ");

		// ワ + 合成用濁点は、往復すると正準等価な合成済みの ヷ になる
		expect("wa voiced round trip",
			convertZenHankaku(convertZenHankaku(u8"ワ\u3099", ConvertMode::ToHankaku), ConvertMode::ToZenkaku),
			u8"ヷ");

		expect("zen combining voiced->han",
			convertZenHankaku(u8"カ\u3099ハ\u309Aウ\u3099", ConvertMode::ToHankaku),
			u8"ｶﾞﾊﾟｳﾞ");

		expect("zen combining voiced category",
			convertZenHankaku(u8"Ａカ\u3099！", ConvertMode::ToHankaku, ConvertCategory::Katakana),
			u8"Ａｶﾞ！");

		expect("zen->han yen",
			convertZenHankaku(u8"￥100", ConvertMode::ToHankaku),
			u8"¥100");

		expect("mixed keep hiragana",
			convertZenHankaku(u8"ひらがなカタカナＡＢＣ", ConvertMode::ToHankaku),
			u8"ひらがなｶﾀｶﾅABC");

		expect("category alphanum only",
			convertZenHankaku(u8"ＡＢＣ！カタカナ　", ConvertMode::ToHankaku, ConvertCategory::AlphaNum),
			u8"ABC！カタカナ　");

		expect("category katakana only",
			convertZenHankaku(u8"ＡＢＣ！カタカナ　", ConvertMode::ToHankaku, ConvertCategory::Katakana),
			u8"ＡＢＣ！ｶﾀｶﾅ　");

		expect("category symbol only",
			convertZenHankaku(u8"ＡＢＣ！カタカナ　", ConvertMode::ToHankaku, ConvertCategory::Symbol),
			u8"ＡＢＣ!カタカナ　");

		expect("category space only",
			convertZenHankaku(u8"ＡＢＣ！カタカナ　", ConvertMode::ToHankaku, ConvertCategory::Space),
			u8"ＡＢＣ！カタカナ ");

		// CP932 では半角の ¥ を書けないので、￥ は据え置き、その前後は変換する
		expect("fallback keeps unencodable yen",
			convertZenHankaku(u8"Ａ￥Ｂ", ConvertMode::ToHankaku, ConvertCategory::All, rejectUtf8(U"¥")),
			u8"A￥B");

		// ｶﾞ の ﾞ が書けないなら ｶ も出さず ガ のまま残す（変換単位ごとの差し戻し）
		expect("fallback restores voiced kana as a unit",
			convertZenHankaku(u8"ガＡ", ConvertMode::ToHankaku, ConvertCategory::All, rejectUtf8(U"ﾞ")),
			u8"ガA");

		expect("fallback restores combining voiced as a unit",
			convertZenHankaku(u8"カ\u3099Ａ", ConvertMode::ToHankaku, ConvertCategory::All, rejectUtf8(U"ﾞ")),
			u8"カ\u3099A");

		// 半角→全角でも、差し戻すのは元入力 2 文字分
		expect("fallback restores han->zen voiced pair",
			convertZenHankaku(u8"ｶﾞｷﾞ", ConvertMode::ToZenkaku, ConvertCategory::All, rejectUtf8(U"ガ")),
			u8"ｶﾞギ");

		expect("no rejection converts yen",
			convertZenHankaku(u8"￥", ConvertMode::ToHankaku),
			u8"¥");

		// 隣り合う変換単位は 1 件にまとまる（Ａ と Ｂ はそれぞれ 3 バイト）
		expect("edits: adjacent units merge",
			format(convertZenHankakuEdits(u8"ＡＢ", ConvertMode::ToHankaku)),
			"[0,6)=AB");

		// 変換しない文字を挟むと別の編集になる（漢 は 3 バイト）
		expect("edits: unchanged text splits ranges",
			format(convertZenHankakuEdits(u8"Ａ漢Ｂ", ConvertMode::ToHankaku)),
			"[0,3)=A|[6,9)=B");

		expect("edits: none when nothing changes",
			format(convertZenHankakuEdits("abc", ConvertMode::ToHankaku)),
			"");

		// 濁点付きは元入力 1 文字ぶんの範囲に 2 文字を書き込む
		expect("edits: voiced unit keeps source range",
			format(convertZenHankakuEdits(u8"ガ", ConvertMode::ToHankaku)),
			u8"[0,3)=ｶﾞ");

		expect("edits: fallback records nothing",
			format(convertZenHankakuEdits(u8"ガ", ConvertMode::ToHankaku, ConvertCategory::All, rejectUtf8(U"ﾞ"))),
			"");

		const std::string gapSource = u8"Ａ漢Ｂ";
		expect("coalesce: merges within gap",
			format(coalesceEdits(convertZenHankakuEdits(gapSource, ConvertMode::ToHankaku), gapSource, 3, 1024)),
			u8"[0,9)=A漢B");

		expect("coalesce: keeps ranges apart beyond gap",
			format(coalesceEdits(convertZenHankakuEdits(gapSource, ConvertMode::ToHankaku), gapSource, 2, 1024)),
			"[0,3)=A|[6,9)=B");

		// 改行をまたぐ結合はしない。変換していない行を置換範囲へ巻き込まないため
		const std::string lineSource = u8"Ａ\nＢ";
		expect("coalesce: never merges across a line break",
			format(coalesceEdits(convertZenHankakuEdits(lineSource, ConvertMode::ToHankaku), lineSource, 64, 1024)),
			"[0,3)=A|[4,7)=B");

		const std::string crlfSource = u8"Ａ\r\nＢ";
		expect("coalesce: never merges across crlf",
			format(coalesceEdits(convertZenHankakuEdits(crlfSource, ConvertMode::ToHankaku), crlfSource, 64, 1024)),
			"[0,3)=A|[5,8)=B");

		expect("coalesce: keeps everything apart at gap zero",
			format(coalesceEdits(convertZenHankakuEdits(gapSource, ConvertMode::ToHankaku), gapSource, 0, 1024)),
			"[0,3)=A|[6,9)=B");

		// 1 行しかない文書は、行単位までまとめると 1 件になる
		expect("coalesce: a single line collapses to one over the count limit",
			format(coalesceEdits(convertZenHankakuEdits(gapSource, ConvertMode::ToHankaku), gapSource, 0, 1)),
			u8"[0,9)=A漢B");

		// 上限を超えたときは、まず行単位までまとめる。改行が消えないので、
		// 行に付いたブックマークやマーカーは残る。全体を 1 件へ潰すのは、
		// 行単位にしても上限を超える場合だけ。
		const std::string twoLines = u8"Ａ漢Ｂ\nＣ漢Ｄ"; // 各行 2 件、合計 4 件
		const std::vector<ConvertEdit> twoLineEdits = convertZenHankakuEdits(twoLines, ConvertMode::ToHankaku);

		expect("coalesce: two lines stay split under the limit",
			format(coalesceEdits(twoLineEdits, twoLines, 0, 4)),
			"[0,3)=A|[6,9)=B|[10,13)=C|[16,19)=D");

		expect("coalesce: over the limit merges per line, not into one",
			format(coalesceEdits(twoLineEdits, twoLines, 0, 3)),
			u8"[0,9)=A漢B|[10,19)=C漢D");

		// 行単位が 2 件なので、上限 2 でもまだ行単位で収まる
		expect("coalesce: per line result is kept when it fits the limit",
			format(coalesceEdits(twoLineEdits, twoLines, 0, 2)),
			u8"[0,9)=A漢B|[10,19)=C漢D");

		expect("coalesce: falls back to one only when per line is still too many",
			format(coalesceEdits(twoLineEdits, twoLines, 0, 1)),
			u8"[0,19)=A漢B\nC漢D");

		expect("collapse per line: leaves every line break outside the replacements",
			format(collapseEditsPerLine(twoLineEdits, twoLines)),
			u8"[0,9)=A漢B|[10,19)=C漢D");

		expect("collapse to one: spans the line break",
			format(collapseEditsToOne(twoLineEdits, twoLines)),
			u8"[0,19)=A漢B\nC漢D");

		expect("collapse per line: no edits means no result",
			format(collapseEditsPerLine({}, twoLines)),
			"");

		expect("collapse to one: no edits means no result",
			format(collapseEditsToOne({}, twoLines)),
			"");

		// 変換対象の無い行は置換範囲に入らない。間の行のマーカーが巻き込まれないこと
		const std::string skipLine = u8"Ａ\n漢字\nＢ";
		expect("collapse per line: skips lines without any target",
			format(collapseEditsPerLine(convertZenHankakuEdits(skipLine, ConvertMode::ToHankaku), skipLine)),
			"[0,3)=A|[11,14)=B");

		// Ａ漢Ｂ (9 バイト) は A漢B (5 バイト) になる
		expect("track: positions across separate units",
			track(gapSource, ConvertMode::ToHankaku, { 0, 3, 6, 9 }),
			"0,1,4,5");

		expect("track: inside a unit snaps to its end",
			track(gapSource, ConvertMode::ToHankaku, { 1 }),
			"1");

		// 連続した変換は 1 件の編集にまとまるが、位置はその内側でも単位ごとに追える
		expect("track: inside a run of merged units",
			track("abcd", ConvertMode::ToZenkaku, { 0, 1, 2, 3, 4 }),
			"0,3,6,9,12");

		// 縮む方向と伸びる方向が混ざる例: ｶﾞ漢b (10 バイト) -> ガ漢ｂ (9 バイト)
		expect("track: mixed shrink and growth",
			track(u8"ｶﾞ漢b", ConvertMode::ToZenkaku, { 0, 6, 9, 10 }),
			"0,3,6,9");

		// ｶ と ﾞ の間は分割できないので ガ の末尾へ寄せる
		expect("track: inside a voiced pair snaps to its end",
			track(u8"ｶﾞ", ConvertMode::ToZenkaku, { 3 }),
			"3");

		expect("track: unchanged text keeps positions",
			track("abc", ConvertMode::ToHankaku, { 0, 2 }),
			"0,2");

		expect("track: fallback leaves positions alone",
			track(u8"ガ", ConvertMode::ToHankaku, { 0, 3 }, rejectUtf8(U"ﾞ")),
			"0,3");

		const std::string invalidUtf8("\xFF\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80", 10);
		expect("invalid utf8 preserved",
			convertZenHankaku(invalidUtf8, ConvertMode::ToHankaku),
			invalidUtf8);

		// 不正バイトの直後にある変換対象は、そのまま変換される
		expect("invalid utf8 does not stop conversion",
			convertZenHankaku(std::string("\xFF", 1) + u8"Ａ", ConvertMode::ToHankaku),
			std::string("\xFF", 1) + "A");
	}

	// 変換候補のビット表（converter.cpp）で当たらない文字を早く切り捨てている。
	// 表は分類そのものから作っているので定義上ズレないが、**「候補でないから抜ける」が
	// BMP 全域で結果を変えていないこと**を固定しておく。手で条件を書き写す実装に
	// 変えたり、変換表を増やして表の作り方だけ直し忘れたら、ここが落ちる。
	//
	// BMP 全域の変換結果を 1 つの値へ畳む（FNV-1a 64bit）。
	//
	// **件数ではなく出力そのものを固定する。** 件数だけを見ていると、1 件欠けて別の
	// 1 件が増えても通ってしまう。変換されたコードポイントとその出力の両方を混ぜる。
	// 件数も別に返して、落ちたときにどちらがずれたのか分かるようにしておく。
	struct SweepResult
	{
		size_t count = 0;
		uint64_t digest = 0;
	};

	void mixInto(uint64_t& digest, const std::string& bytes)
	{
		for (const char c : bytes)
		{
			digest ^= static_cast<unsigned char>(c);
			digest *= 0x100000001B3ull;
		}
	}

	std::string format(const SweepResult& result)
	{
		std::ostringstream out;
		out << result.count << " / " << std::hex << std::setw(16) << std::setfill('0') << result.digest;
		return out.str();
	}

	// suffix を渡すと「その文字を後ろに付けた 2 文字」を変換する。2 文字を 1 単位として
	// 扱う経路（濁点付きカナ）はこちらにしか出ない。
	SweepResult sweepBmp(ConvertMode mode, const char32_t* suffix)
	{
		const std::string encodedSuffix = suffix
			? enc(utf8Codec(), std::u32string(1, *suffix))
			: std::string();

		SweepResult result;
		result.digest = 0xCBF29CE484222325ull;

		// U+0000 も含める。単体では変換されないが、2 文字目に半角濁点を置いた
		// ケースでは「U+0000 + 全角濁点」へ変わるので、数に入る
		for (char32_t cp = 0; cp < 0x10000; ++cp)
		{
			// サロゲートは UTF-8 で表せないので、不正バイト扱いになる。ここでは外す
			if (cp >= 0xD800 && cp <= 0xDFFF)
				continue;

			const std::string source = enc(utf8Codec(), std::u32string(1, cp)) + encodedSuffix;
			const std::string converted = convertZenHankaku(source, mode);
			if (converted == source)
				continue;

			++result.count;
			// コードポイントも混ぜる。混ぜないと、変換される文字がずれても
			// 出力の集合が同じなら気づけない
			mixInto(result.digest, std::to_string(static_cast<uint32_t>(cp)));
			mixInto(result.digest, converted);
		}
		return result;
	}

	// 「2 文字を 1 単位として扱った結果」が「各文字を独立に変換してつないだ結果」と
	// 違う数を数える。**組結合の経路だけを見る指標。**
	//
	// ダイジェストだけだと、半角の濁点を後ろに置いたケースは 63,488 件すべてが
	// 変化するため、組結合だけが壊れても数字の見た目で分からない。こちらは
	// 「表に載っている組の数」そのものなので、落ちたときに原因が絞れる。
	size_t countCombiningUnits(ConvertMode mode, char32_t mark)
	{
		const std::string encodedMark = enc(utf8Codec(), std::u32string(1, mark));
		const std::string convertedMark = convertZenHankaku(encodedMark, mode);

		size_t count = 0;
		for (char32_t cp = 0; cp < 0x10000; ++cp)
		{
			if (cp >= 0xD800 && cp <= 0xDFFF)
				continue;

			const std::string alone = enc(utf8Codec(), std::u32string(1, cp));
			if (convertZenHankaku(alone + encodedMark, mode)
				!= convertZenHankaku(alone, mode) + convertedMark)
			{
				++count;
			}
		}
		return count;
	}

	void runCandidateSweepTests()
	{
		// 非 BMP の規則を足すと候補ビット表に載らず、静かに無効化される。表は分類から
		// 作っているが、**その保証は BMP の中だけ**なので別に固定する。
		expect("sweep: no candidate lead character lives outside the bmp",
			std::to_string(nonBmpCandidateLeads()), "0");

		// 「件数 / ダイジェスト」。落ちたら、変換表を意図的に変えたのかを先に確かめる。
		// 意図的なら値を差し替える。そうでなければ候補ビット表の作り方を疑う。
		expect("sweep: zen->han over the whole bmp",
			format(sweepBmp(ConvertMode::ToHankaku, nullptr)), "193 / 9fe2a79e0c6c3445");
		expect("sweep: han->zen over the whole bmp",
			format(sweepBmp(ConvertMode::ToZenkaku, nullptr)), "165 / 57ddac81829caaa3");

		const char32_t zenVoiced = U'\u3099';
		const char32_t zenSemiVoiced = U'\u309A';
		const char32_t hanVoiced = U'ﾞ';
		const char32_t hanSemiVoiced = U'ﾟ';

		// 半角の濁点・半濁点は単体でも変換対象なので、どの文字の後ろに付けても
		// 何かが変わる。件数が 63,488 になるのはそのため（組にならない文字は
		// 「元の文字 + 全角濁点」になっている）
		expect("sweep: zen->han pairs with a combining voiced mark",
			format(sweepBmp(ConvertMode::ToHankaku, &zenVoiced)), "193 / 05e11b0764b7cf96");
		expect("sweep: zen->han pairs with a combining semi-voiced mark",
			format(sweepBmp(ConvertMode::ToHankaku, &zenSemiVoiced)), "193 / 18ad7f8a08885113");
		expect("sweep: han->zen pairs with a half width voiced mark",
			format(sweepBmp(ConvertMode::ToZenkaku, &hanVoiced)), "63488 / f4205a82eeee2386");
		expect("sweep: han->zen pairs with a half width semi-voiced mark",
			format(sweepBmp(ConvertMode::ToZenkaku, &hanSemiVoiced)), "63488 / 299656f5368a3807");

		// 上のダイジェストと併用する。**組結合だけが壊れた場合はこちらの方が読める。**
		// 表の件数そのもので、どちらも濁点 23 件（カ行サ行タ行ハ行の 20 とウ・ワ・ヲ）と
		// 半濁点 5 件（ハ行）
		expect("sweep: this many pairs form a single unit with a combining voiced mark",
			std::to_string(countCombiningUnits(ConvertMode::ToHankaku, zenVoiced)), "23");
		expect("sweep: this many pairs form a single unit with a combining semi-voiced mark",
			std::to_string(countCombiningUnits(ConvertMode::ToHankaku, zenSemiVoiced)), "5");
		expect("sweep: this many pairs form a single unit with a half width voiced mark",
			std::to_string(countCombiningUnits(ConvertMode::ToZenkaku, hanVoiced)), "23");
		expect("sweep: this many pairs form a single unit with a half width semi-voiced mark",
			std::to_string(countCombiningUnits(ConvertMode::ToZenkaku, hanSemiVoiced)), "5");
	}

	// UTF-8 は「非仮想の高速経路」と「TextCodec 経由の仮想経路」の 2 通りで処理できる。
	// 速度のためだけに実装を二重化しているので、結果が一致することを固定しておく。
	// 食い違いが出たサンプルだけを書き出し、全部一致なら空文字列を返す。
	// 8.2 の前の形が、今の製品と完全に一致することを固定する。
	// **速度の比較対象として残しているので、片方だけに手を入れたらここが落ちる。**
	// 直接書き込みは「直前の編集へ続く」経路だけを差し替えるので、隣接して
	// 変換される入力と、間に非対象を挟む入力の両方を通す。
	std::string encodeVariantDifferences()
	{
		const std::vector<std::u32string> samples = {
			U"ＡＢＣ",           // 隣接して続く（直前の編集へ足す経路）
			U"Ａあ Ｂあ Ｃ",     // 間に非対象が入る（新しい編集を作る経路）
			U"ガギグ",           // 1 単位が 2 文字へ分解される
			U"ｶﾞｷﾞ ABC ａｂｃ　", // 2 文字が 1 単位へ結合される
			U"ＡガＢｶﾞ",         // 1 文字と 2 文字の単位が混ざる
			U"",                 // 空
			U"あいう",           // 変換が 1 件も起きない
		};

		std::string differences;
		for (const std::u32string& sample : samples)
		{
			const std::string bytes = enc(utf8Codec(), sample);
			for (const ConvertMode mode : {ConvertMode::ToHankaku, ConvertMode::ToZenkaku})
			{
				// 全バイト位置を追跡させて、位置の対応まで突き合わせる
				std::vector<size_t> productPositions(bytes.size() + 1);
				std::vector<size_t> legacyPositions(bytes.size() + 1);
				for (size_t i = 0; i < productPositions.size(); ++i)
					productPositions[i] = legacyPositions[i] = i;

				const std::string product = formatHex(convertZenHankakuEdits(
					bytes, mode, ConvertCategory::All, utf8Codec(), &productPositions));
				const std::string legacy = formatHex(convertZenHankakuEditsEncode(
					bytes, mode, ConvertCategory::All, EncodeVariant::Legacy, &legacyPositions));

				if (product != legacy || productPositions != legacyPositions)
					differences += hex(bytes) + " ";
			}
		}

		return differences;
	}

	std::string utf8PathDifferences()
	{
		const TextCodec& viaCodec = utf8CodecForcedVirtual();

		const std::vector<std::string> samples = {
			"",
			"abc",
			u8"ＡＢＣ１２３！",
			"ABC123!",
			u8"Ａ漢Ｂ",
			u8"ガギグ",
			u8"ｶﾞｷﾞ",
			u8"カ\u3099ハ\u309Aウ\u3099",
			u8"ヷヺヸヹ",
			u8"ひらがなカタカナＡＢＣ　ｱｲｳ abc ￥100",
			u8"Ａ\r\nＢ\nＣ",
			// 不正 UTF-8: 単独の 0xFF、過剰長、サロゲート、範囲外、途中で切れた列
			std::string("\xFF\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80", 10),
			std::string("\xFF", 1) + u8"Ａ" + std::string("\x80", 1),
			u8"Ａ" + std::string("\xE3\x81", 2),
		};

		const ConvertMode modes[] = { ConvertMode::ToHankaku, ConvertMode::ToZenkaku };
		const ConvertCategory categories[] = {
			ConvertCategory::All,
			ConvertCategory::AlphaNum,
			ConvertCategory::Katakana,
			ConvertCategory::Symbol,
			ConvertCategory::Space,
		};

		std::string differences;
		for (size_t i = 0; i < samples.size(); ++i)
		{
			const std::string& source = samples[i];

			// 位置追跡は 1 バイトごとに全部の位置を突き合わせる
			std::vector<size_t> everyOffset;
			for (size_t pos = 0; pos <= source.size(); ++pos)
				everyOffset.push_back(pos);

			for (const ConvertMode mode : modes)
			{
				for (const ConvertCategory category : categories)
				{
					const std::string tag = "#" + std::to_string(i)
						+ (mode == ConvertMode::ToHankaku ? "h" : "z")
						+ std::to_string(static_cast<int>(category)) + ":";

					const std::string fastText = convertZenHankaku(source, mode, category);
					const std::string codecText = convertZenHankaku(source, mode, category, viaCodec);
					if (fastText != codecText)
						differences += tag + "text(" + hex(fastText) + " / " + hex(codecText) + ") ";

					const std::string fastEdits = formatHex(convertZenHankakuEdits(source, mode, category));
					const std::string codecEdits = formatHex(convertZenHankakuEdits(source, mode, category, viaCodec));
					if (fastEdits != codecEdits)
						differences += tag + "edits(" + fastEdits + " / " + codecEdits + ") ";

					std::vector<size_t> fast = everyOffset;
					std::vector<size_t> viaTextCodec = everyOffset;
					convertZenHankakuEdits(source, mode, category, utf8Codec(), &fast);
					convertZenHankakuEdits(source, mode, category, viaCodec, &viaTextCodec);
					if (fast != viaTextCodec)
						differences += tag + "positions ";
				}
			}
		}
		return differences;
	}

	// encode() の契約: 1 文字でも書けなければ out を一切変更しないこと。
	// ここが崩れると、変換単位の一部だけが書き込まれて文字化けする。
	// 複数選択（複数範囲）の変換。ここで固定したいのは「選択の外は触らない」ことと、
	// 「先の選択で長さが変わっても後の選択と外側の位置がずれない」ことの 2 点。
	void runMultiRangeTests()
	{
		// ＡＢＣＤ は 1 文字 3 バイト。Ａ と Ｃ だけを選んで変換する
		const std::string four = u8"ＡＢＣＤ";

		expect("ranges: converts only the selected spans",
			convertSpans(four, { { 0, 3 }, { 6, 9 } }, ConvertMode::ToHankaku),
			u8"AＢCＤ");

		expect("ranges: edits are returned in document coordinates",
			format(convertZenHankakuRanges(makeRanges(four, { { 0, 3 }, { 6, 9 } }),
				ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, SIZE_MAX)),
			"[0,3)=A|[6,9)=C");

		// 縮む方向。後ろの選択と、選択より後ろの位置が手前の増減分だけずれる
		expect("ranges: positions shift by the delta of earlier ranges",
			trackSpans(four, { { 0, 3 }, { 6, 9 } }, ConvertMode::ToHankaku, { 0, 3, 6, 9, 12 }),
			"0,1,4,5,8");

		// 伸びる方向
		expect("ranges: positions shift when ranges grow",
			trackSpans("abcd", { { 0, 1 }, { 2, 3 } }, ConvertMode::ToZenkaku, { 0, 1, 2, 3, 4 }),
			"0,3,4,7,8");

		// 選択と選択の間にある位置。手前の選択の増減だけを受ける
		{
			const std::string gapped = u8"Ａ--Ｂ"; // Ａ=0..3, -=3, -=4, Ｂ=5..8
			expect("ranges: a position between two ranges keeps its place",
				trackSpans(gapped, { { 0, 3 }, { 5, 8 } }, ConvertMode::ToHankaku, { 3, 4, 5 }),
				"1,2,3");
			expect("ranges: the text between two ranges is untouched",
				convertSpans(gapped, { { 0, 3 }, { 5, 8 } }, ConvertMode::ToHankaku),
				"A--B");
		}

		// 変換対象を含まない選択は編集を出さず、位置にも影響しない
		expect("ranges: a range without targets contributes nothing",
			trackSpans(u8"Ａ--Ｂ", { { 3, 5 }, { 5, 8 } }, ConvertMode::ToHankaku, { 0, 3, 5, 8 }),
			"0,3,5,6");

		// 空の範囲一覧なら何も起きず、位置もそのまま
		expect("ranges: no ranges means no change",
			trackSpans(four, {}, ConvertMode::ToHankaku, { 0, 6, 12 }),
			"0,6,12");

		// 結合の上限は合計件数で見る。超えたら範囲ごとに 1 件へ潰す（範囲の数までは減らない）
		{
			const std::string many = u8"Ａ-Ａ==Ａ-Ａ"; // 左 0..7 / == 7..9 / 右 9..16
			const std::vector<std::pair<size_t, size_t>> spans = { { 0, 7 }, { 9, 16 } };

			expect("ranges: stays split while under the total limit",
				format(convertZenHankakuRanges(makeRanges(many, spans),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 4)),
				"[0,3)=A|[4,7)=A|[9,12)=A|[13,16)=A");

			expect("ranges: collapses each range once over the total limit",
				format(convertZenHankakuRanges(makeRanges(many, spans),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 3)),
				"[0,7)=A-A|[9,16)=A-A");

			// 潰しても文書の中身は変わらない
			expect("ranges: collapsing does not change the resulting text",
				convertSpans(many, spans, ConvertMode::ToHankaku, 3),
				u8"A-A==A-A");

			// 1 つの範囲だけで上限を超えている場合。その範囲はその場で潰され、
			// 合計は潰す前の件数で判定するので、残りの範囲も潰れる
			expect("ranges: one range over the limit collapses every range",
				format(convertZenHankakuRanges(makeRanges(many, spans),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 1)),
				"[0,7)=A-A|[9,16)=A-A");
		}

		// 複数範囲でも、合計が上限を超えたらまず行単位までまとめる
		{
			const std::string lines = u8"Ａ漢Ａ\nＡ漢Ａ"; // 各行 2 件
			const std::vector<std::pair<size_t, size_t>> perLine = { { 0, 9 }, { 10, 19 } };

			expect("ranges: over the total limit merges per line",
				format(convertZenHankakuRanges(makeRanges(lines, perLine),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 3)),
				u8"[0,9)=A漢A|[10,19)=A漢A");

			// 各範囲が 1 行なので、行単位でも収まらない上限でも改行は巻き込まれない
			expect("ranges: single line ranges never span the line break",
				format(convertZenHankakuRanges(makeRanges(lines, perLine),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 1)),
				u8"[0,9)=A漢A|[10,19)=A漢A");

			// 1 つの範囲が複数行にまたがる場合。行単位でも収まらなければ 1 件へ落ちる
			const std::vector<std::pair<size_t, size_t>> whole = { { 0, 19 } };
			expect("ranges: a multi line range falls back to one under a tight limit",
				format(convertZenHankakuRanges(makeRanges(lines, whole),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 1)),
				u8"[0,19)=A漢A\nA漢A");

			expect("ranges: a multi line range merges per line when that fits",
				format(convertZenHankakuRanges(makeRanges(lines, whole),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 2)),
				u8"[0,9)=A漢A|[10,19)=A漢A");

			// まとめ方が変わっても文書の中身は変わらない
			expect("ranges: per line merging does not change the resulting text",
				convertSpans(lines, whole, ConvertMode::ToHankaku, 2),
				u8"A漢A\nA漢A");
		}

		// 範囲ごとの件数が違う場合。上限判定は合計で行う
		{
			const std::string uneven = u8"Ａ-Ａ==Ａ"; // 左 0..7（2 件）/ == 7..9 / 右 9..12（1 件）
			const std::vector<std::pair<size_t, size_t>> spans = { { 0, 7 }, { 9, 12 } };

			expect("ranges: total of three edits stays split at the limit of three",
				format(convertZenHankakuRanges(makeRanges(uneven, spans),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 3)),
				"[0,3)=A|[4,7)=A|[9,12)=A");

			expect("ranges: a range already at one edit is left as it is",
				format(convertZenHankakuRanges(makeRanges(uneven, spans),
					ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 1)),
				"[0,7)=A-A|[9,12)=A");
		}

		// 単一範囲は従来と同じ結果になること（全文変換の経路がこれに乗る）
		expect("ranges: a single whole-document range matches the plain path",
			convertSpans(four, { { 0, four.size() } }, ConvertMode::ToHankaku),
			convertZenHankaku(four, ConvertMode::ToHankaku));

		// **選択が大量にある場合の位置追跡。** 所属判定を二分探索へ替えたので、
		// 境界の選び方が線形版と変わっていないことを、答えが式で出る形で固定する。
		//
		// 文書は「Ａ.」の繰り返し（Ａ が 3 バイト、. が 1 バイトで 1 組 4 バイト）。
		// 選択は Ａ だけを覆う [4i, 4i+3) で、zen→han で 3 バイト → 1 バイトになる。
		// したがって i 番目より後ろの位置は 2i バイトだけ手前へ寄る。
		//
		//   4i     (Ａ_i の先頭、範囲の内側) → 2i
		//   4i + 3 (Ａ_i の末尾 = . の位置、範囲の境界) → 2i + 1
		{
			const size_t count = 2000;
			std::string document;
			document.reserve(count * 4);
			std::vector<std::pair<size_t, size_t>> spans;
			spans.reserve(count);
			std::vector<size_t> positions;
			positions.reserve(count * 2);
			std::string expected;

			for (size_t i = 0; i < count; ++i)
			{
				document += u8"Ａ.";
				spans.push_back({ i * 4, i * 4 + 3 });
				positions.push_back(i * 4);
				positions.push_back(i * 4 + 3);

				if (i != 0)
					expected += ",";
				expected += std::to_string(i * 2) + "," + std::to_string(i * 2 + 1);
			}

			expect("ranges: two thousand selections track every position",
				trackSpans(document, spans, ConvertMode::ToHankaku, positions),
				expected);

			// 末尾より後ろの位置も、全選択ぶんの縮みが乗る
			expect("ranges: a position past the last selection shifts by every range",
				trackSpans(document, spans, ConvertMode::ToHankaku, { document.size() }),
				std::to_string(document.size() - count * 2));
		}
	}

	// **編集件数の上限（途中結合）のストレステスト。**
	//
	// 変換対象と非対象が交互に並ぶ文書では、まとめる前の編集が単位ごとに 1 件になる。
	// 上限を渡すと変換の途中でまとめるので、件数がその上限で止まる。
	// 同時に「上限を渡さずに最後だけまとめた結果」と一致することも確かめる
	// （途中でまとめても、最後に選ばれる形は変わらない）。
	void runEditLimitTests()
	{
		struct Shape
		{
			const char* name;
			const char* unit;   // 1 単位ぶんのバイト列（先頭が変換対象）
			size_t maxGap;
			size_t lineEvery;   // 何単位ごとに改行を入れるか。0 なら改行なし
		};

		// 3 つの形で、途中結合の 3 つの落ち方をそれぞれ通す。
		//   1 行に 1 件: 行単位でも減らないので全体 1 件へ落ちる（collapsed）
		//   改行なし:    段階 1 の間隔結合だけで 1 件へ落ちる
		//   1 行に多数:  行の中はまとまり、行数で収まる（行のマーカーは残る）
		const Shape shapes[] = {
			{ "one target per line", u8"Ａ\n", 0, 0 },
			{ "no line breaks at all", u8"Ａ.", 1, 0 },
			{ "many targets per line", u8"Ａ.", 1, 10 },
		};

		// 編集の中身まで比べる。置換テキストが大きくなるので整形はしない
		auto sameEdits = [](const std::vector<ConvertEdit>& a, const std::vector<ConvertEdit>& b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				if (a[i].begin != b[i].begin || a[i].end != b[i].end || a[i].text != b[i].text)
					return false;
			}
			return true;
		};

		auto makeDocument = [](const Shape& shape, size_t units)
		{
			std::string bytes;
			for (size_t i = 0; i < units; ++i)
			{
				bytes += shape.unit;
				if (shape.lineEvery != 0 && (i + 1) % shape.lineEvery == 0)
					bytes += "\n";
			}
			return bytes;
		};

		// 途中結合を通した結果が、上限なしで最後だけまとめた結果と一致すること。
		// あわせて、まとめる前の件数が「上限 + compactEvery」で止まることを見る。
		auto check = [&](const Shape& shape, size_t units, const EditLimits& limits, const char* label)
		{
			const std::string bytes = makeDocument(shape, units);
			const std::string name = std::string("limits: ") + shape.name + " " + label;

			// 位置追跡も一緒に通す。途中結合は編集の形を変えるので、位置が
			// ずれていないことを上限なしの結果と突き合わせる
			std::vector<size_t> cappedPos = { 0, bytes.size() / 3, bytes.size() / 2, bytes.size() };
			std::vector<size_t> plainPos = cappedPos;

			const std::vector<ConvertEdit> capped = convertZenHankakuEdits(
				bytes, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), &cappedPos, limits);

			// 上限なし。まとめる前の編集を単位ごとに全部抱えるので、こちらは
			// 「上限を渡さなければ膨らむ」ことの実演にもなっている
			const std::vector<ConvertEdit> plain = convertZenHankakuEdits(
				bytes, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), &plainPos);

			expect((name + ": grows one edit per unit without limits").c_str(),
				plain.size() == units ? "one per unit" : ("unexpected: " + std::to_string(plain.size())),
				"one per unit");

			// 上限を渡さない呼び出しは途中結合そのものをしないので、単位ごとに 1 件。
			// まとめる件数を約束するのは上限を渡した呼び出しだけ
			const size_t bound = limits.maxCount == SIZE_MAX
				? SIZE_MAX
				: limits.maxCount + limits.compactEvery;

			expect((name + ": stays under the cap plus the compact step").c_str(),
				capped.size() <= bound
					? "bounded"
					: ("got " + std::to_string(capped.size()) + " over " + std::to_string(bound)),
				"bounded");

			// **途中結合が実際に走ったことを確かめる。** 上の上限判定だけでは、
			// 途中結合が起きていなくても「もともと上限に収まる入力」で通ってしまう。
			// 最初にまとめ直すのは compactEvery 件たまったところ。ただし上限以下の
			// 件数で結合を呼んでも 1 件も減らないので、そこは必ず上限より大きくとる。
			// 届かない入力と上限なしの入力では 1 件も減らないこと（余計な結合を
			// していないこと）も同じ式で押さえられる。
			const size_t firstCompact = limits.maxCount == SIZE_MAX
				? SIZE_MAX
				: std::max(limits.compactEvery, limits.maxCount + 1);
			const bool shouldCompact = plain.size() >= firstCompact;

			expect((name + ": compacts while converting only when the step is reached").c_str(),
				capped.size() < plain.size() ? "compacted" : "untouched",
				shouldCompact ? "compacted" : "untouched");

			// 最後の仕上げまで通した形で比べる。製品はこの形しか使わない
			expect((name + ": matches the uncapped result").c_str(),
				sameEdits(coalesceEdits(capped, bytes, limits.maxGap, limits.maxCount),
					coalesceEdits(plain, bytes, limits.maxGap, limits.maxCount))
					? "same"
					: "different",
				"same");

			expect((name + ": rebuilds the same document").c_str(),
				rebuild(bytes, capped) == convertZenHankaku(bytes, ConvertMode::ToHankaku) ? "same" : "different",
				"same");

			expect((name + ": tracks positions the same way").c_str(),
				cappedPos == plainPos ? "same" : "different",
				"same");
		};

		for (const Shape& shape : shapes)
		{
			// **閾値を小さくして何度も途中結合を通す。** 製品の 100 万件のままだと
			// 1 回しか通らないので、まとめた後に続けて書く経路が薄くしか通らない。
			check(shape, 1000, EditLimits{ shape.maxGap, 8, 16 }, "with a tiny compact step");

			// **上限を渡さなければ、閾値を小さくしても途中結合は起きない。**
			// 守るべき件数が無い結合は減らないので、置換テキストを写す損だけが残る。
			// 上の bound が SIZE_MAX になる経路と、単位ごとに 1 件のままになることを見る
			check(shape, 1000, EditLimits{ shape.maxGap, SIZE_MAX, 16 }, "with no cap to reach");

			// **上限が刻みより大きい場合。** 溜める先を刻みのままにすると、結合を
			// 上限以下の件数（16 件）で呼ぶことになり 1 件も減らない。そこで諦めると
			// 以降は途中結合が止まり、「上限 + 刻み で抑える」という約束が破れる。
			// 溜める先を上限より大きくとってあることを、この組み合わせで固定する
			check(shape, 1000, EditLimits{ shape.maxGap, 32, 16 }, "with a cap above the compact step");
		}

		// **製品の閾値そのままで、実際に途中結合が走る文書を通す。**
		// `Ａ\n` の繰り返しは 1 行 1 件で行単位でも減らないので、最悪の形になる。
		// 64MB なら約 1670 万件で、1 件 48 バイト前後だと 1GB 級になる量。
		check(shapes[0], kEditCompactThreshold + 1000, EditLimits{ 0, 4096 }, "at the real threshold");

		// **改行が 1 つも無い文書を、結合が何度も走る量で通す。**
		// 結合の改行判定を [begin, end) で区切らないと、見つからないまま文書の
		// 末尾まで走るので、結合そのものが O(編集数 × 文書サイズ) になって
		// 返ってこなくなる。区切ってあれば 1 単位ぶんしか見ないので一瞬で終わる。
		check(shapes[1], 300000, EditLimits{ 1, 4096, 1024 }, "with no newline to find");

		// **合計が刻みを超えたら、範囲をまたいで先にまとめる。**
		//
		// 途中結合は範囲ごとの記録に付くので、それだけでは「各範囲は刻み未満だが
		// 合計は巨大」という形で抑えが効かない。64MB の `Ａ\n` を 2MB ずつ 32 個の
		// 選択に分けると、各範囲は約 52 万件で刻みに届かず、行単位でも 1 行 1 件なので
		// 減らない。1670 万件を全範囲ぶん抱えることになり、単一範囲で消したはずの
		// 1GB 級のピークが戻る。
		//
		// 大きい選択を刻みの 1/5 ずつ 6 個並べる。5 個目までは合計が刻みに届かず、
		// 6 個目で超えて全範囲が 1 件へ潰れる。**そのあとに置く小さい選択が 2 件の
		// まま残ることが、先に潰れたことの証拠になる**。先に潰していなければ合計は
		// 125 万件のままで、最後の判定で上限を超えてこの選択まで 1 件へ潰れる。
		{
			const size_t unitsPerRange = kEditCompactThreshold / 5;
			const std::string big = makeDocument(shapes[0], unitsPerRange); // Ａ\n の繰り返し
			const std::string small = u8"Ａ.Ａ";                            // 間隔 1 で 2 件

			std::string document;
			document.reserve(big.size() * 6 + small.size());

			std::vector<std::pair<size_t, size_t>> spans;
			for (size_t i = 0; i < 6; ++i)
			{
				spans.push_back({ document.size(), document.size() + big.size() });
				document += big;
			}

			const size_t smallBegin = document.size();
			spans.push_back({ smallBegin, smallBegin + small.size() });
			document += small;

			std::vector<size_t> positions = { 0, smallBegin, document.size() };
			const std::vector<ConvertEdit> edits = convertZenHankakuRanges(makeRanges(document, spans),
				ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, 4096, &positions);

			// 大きい選択 6 個が 1 件ずつと、小さい選択の 2 件
			expect("limits: ranges past the compact step collapse before the last one is read",
				std::to_string(edits.size()), "8");

			size_t inSmall = 0;
			for (const ConvertEdit& edit : edits)
			{
				if (edit.begin >= smallBegin)
					++inSmall;
			}

			expect("limits: a range read after the squeeze keeps its own edits",
				std::to_string(inSmall), "2");

			expect("limits: squeezing across ranges does not change the text",
				rebuild(document, edits) == convertZenHankaku(document, ConvertMode::ToHankaku)
					? "same"
					: "different",
				"same");

			// 全角 1 文字が半角 1 文字になるので、大きい選択は 1 単位 4 バイトが 2 バイトへ
			const size_t shrunk = 6 * unitsPerRange * 2;
			expect("limits: positions still track across every squeezed range",
				std::to_string(positions[0]) + "," + std::to_string(positions[1])
					+ "," + std::to_string(positions[2]),
				"0," + std::to_string(shrunk) + "," + std::to_string(shrunk + 3));

			// **上限を渡さなければ、合計が刻みを超えても何もしない。** 上限を外した
			// 計測用ビルドの「全件そのまま見せる」という意図を崩さないため。
			// 単位ごとに 1 件のままなので、件数が式で出る
			const std::vector<ConvertEdit> uncapped = convertZenHankakuRanges(makeRanges(document, spans),
				ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), 0, SIZE_MAX, nullptr);

			expect("limits: no cap means no squeezing across ranges either",
				std::to_string(uncapped.size()), std::to_string(6 * unitsPerRange + 2));
		}
	}

	void runCodecContractTests()
	{
		{
			std::string out = "keep";
			const bool ok = utf8Codec().encode(U"AB", out);
			expect("codec: utf8 encode appends on success",
				std::string(ok ? "true" : "false") + "/" + out,
				"true/keepAB");
		}

		{
			// 復号できなかったバイトの番兵は書き戻せない
			std::string out = "keep";
			const std::u32string sentinel(1, kInvalidByteBase + 0x41);
			const bool ok = utf8Codec().encode(sentinel, out);
			expect("codec: utf8 encode rejects the invalid-byte sentinel",
				std::string(ok ? "true" : "false") + "/" + out,
				"false/keep");
		}

		{
			const FakeDbcsCodec fake;
			const RejectingCodec rejecting(fake, U"ﾞ");
			std::string out = "keep";
			const bool ok = rejecting.encode(U"ｶﾞ", out);
			expect("codec: encode leaves out untouched on failure",
				std::string(ok ? "true" : "false") + "/" + out,
				"false/keep");
		}

		{
			// 書けない文字が末尾にある場合も、先頭の書ける文字を出してはいけない
			const FakeDbcsCodec fake;
			std::string out = "keep";
			const bool ok = fake.encode(U"A\u00A5", out);
			expect("codec: partial unit is never written",
				std::string(ok ? "true" : "false") + "/" + out,
				"false/keep");
		}

		// **1 パス化したロールバックそのものの確認。**
		// 書ける文字の後ろに番兵が来る単位は、先に A を書いた後で弾かれる。
		// **その A が残らないこと**が 8.2 の 1 パス化の要点（検査を先に回していた
		// 頃は書く前に弾いていたので、この経路自体が無かった）。
		{
			std::string out = "keep";
			std::u32string text = U"A";
			text.push_back(kInvalidByteBase + 0x42);

			const bool ok = utf8Codec().encode(text, out);
			expect("codec: a rejected unit rolls back the bytes already written",
				std::string(ok ? "true" : "false") + "/" + out,
				"false/keep");
		}

		// 2 文字目まで書けて 3 文字目で弾かれる場合も、2 文字ぶん戻る
		{
			std::string out;
			std::u32string text = U"AB";
			text.push_back(kInvalidByteBase + 0x43);

			const bool ok = utf8Codec().encode(text, out);
			expect("codec: the rollback covers every byte of the unit",
				std::string(ok ? "true" : "false") + "/[" + out + "]",
				"false/[]");
		}

		// **単独のサロゲートは 3 バイトへ組み立てられてしまうが、UTF-8 としては
		// 不正な列になる。** 変換表の出力には現れないが、通ると文書へ不正な
		// バイトを書くことになるので弾く。decode 側は既に拒否している。
		{
			std::string out;
			const bool low = utf8Codec().encode(std::u32string(1, 0xD800), out);
			const bool high = utf8Codec().encode(std::u32string(1, 0xDFFF), out);
			const bool before = utf8Codec().encode(std::u32string(1, 0xD7FF), out);
			const bool after = utf8Codec().encode(std::u32string(1, 0xE000), out);

			expect("codec: utf8 encode rejects lone surrogates",
				std::string(low ? "T" : "F") + (high ? "T" : "F")
					+ "/" + (before ? "T" : "F") + (after ? "T" : "F"),
				"FF/TT");

			expect("codec: rejecting a surrogate leaves only the codepoints around it",
				hex(out),
				"ED 9F BF EE 80 80");
		}

		{
			// 単位の途中にサロゲートが来たら、その前の文字も残らない
			std::string out = "keep";
			std::u32string text = U"A";
			text.push_back(0xDC00);

			const bool ok = utf8Codec().encode(text, out);
			expect("codec: a surrogate rolls back the unit like the sentinel does",
				std::string(ok ? "true" : "false") + "/" + out,
				"false/keep");
		}

		// **末尾へ追記してから失敗するコーデックでも、編集の内容が汚れないこと。**
		// 記録側は隣接する単位を編集の置換テキストへ直接書くので（8.2）、返り値を
		// 信じるだけでは文書へごみが混ざる。記録側で書いた分を戻していることを固定する。
		//
		// **守れるのはこの形だけ。** 呼び出し前からあった部分を消したり書き換えたり
		// するコーデックは復元できない。
		{
			// 弾く対象は「encode へ渡る文字」= 変換後の半角 C
			const AppendThenFailCodec leaky(utf8Codec(), U"C");

			// ＡＢ は書けて、Ｃ だけが弾かれる。3 文字は隣接しているので、
			// Ａ の変換で作った編集へ Ｂ と Ｃ が続けて書き込まれる形になる。
			const std::vector<ConvertEdit> edits = convertZenHankakuEdits(
				enc(utf8Codec(), U"ＡＢＣ"), ConvertMode::ToHankaku, ConvertCategory::All, leaky);

			// 弾かれた Ｃ のぶんは編集に入らず、範囲も Ｂ の終わり（6 バイト）で止まる。
			// "!LEAK!" のバイト（21 4C 45 41 4B 21）が 1 つも残らないことが要点。
			expect("codec: an append-then-fail encode cannot dirty the edit text",
				formatHex(edits),
				"[0,6)=41 42");
		}
	}

	// Windows API に依存しない偽 DBCS コーデックで、ANSI 経路の性質を検証する
	void runFakeDbcsTests()
	{
		const FakeDbcsCodec fake;

		// 前提の確認: 別バイト列が同じコードポイントへ復号される
		{
			char32_t viaNec = 0;
			char32_t viaIbm = 0;
			fake.decode(db(0x9C40), 0, viaNec);
			fake.decode(db(0xE040), 0, viaIbm);
			expect("dbcs: duplicate byte sequences decode to the same char",
				(viaNec != 0 && viaNec == viaIbm) ? "same" : "different",
				"same");
		}

		// 重複マッピングの保持。変換対象でない文字は再符号化されず、元バイトのまま残る。
		// 範囲全体を置き換えていた頃は、ここで 0xE040 が 0x9C40 へ正規化されていた。
		expect("dbcs: duplicate mapping is preserved when unchanged",
			hex(convertZenHankaku(db(0xE040) + db(0x8260), ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			hex(db(0xE040) + "A"));

		expect("dbcs: duplicate mapping is preserved around conversions",
			hex(convertZenHankaku(db(0xE040) + db(0x8260) + db(0xE040) + db(0x8261) + db(0xE040),
				ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			hex(db(0xE040) + "A" + db(0xE040) + "B" + db(0xE040)));

		// 編集のオフセットは DBCS バイト単位。Ａ は 2 バイト、A は 1 バイト
		expect("dbcs: edits use document byte offsets",
			formatHex(convertZenHankakuEdits(db(0x8260) + db(0x8940) + db(0x8261),
				ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			"[0,2)=41|[4,6)=42");

		// 1 バイト -> 2 バイトへ伸びる方向
		expect("dbcs: edits for one byte growing to two",
			formatHex(convertZenHankakuEdits("A", ConvertMode::ToZenkaku, ConvertCategory::All, fake)),
			"[0,1)=82 60");

		// 半角カナ 2 バイト (ｶ + ﾞ) が全角 1 文字 2 バイトになる
		expect("dbcs: voiced pair keeps its byte range",
			formatHex(convertZenHankakuEdits(sb(0xB6) + sb(0xDE), ConvertMode::ToZenkaku, ConvertCategory::All, fake)),
			"[0,2)=83 42");

		// 位置追跡もバイト空間で行われる
		expect("dbcs: track across a two-to-one byte shrink",
			track(db(0x8260), ConvertMode::ToHankaku, { 0, 1, 2 }, fake),
			"0,1,1");

		expect("dbcs: track across a one-to-two byte growth",
			track("A", ConvertMode::ToZenkaku, { 0, 1 }, fake),
			"0,2");

		// ｶ と ﾞ の間の位置は、変換後の単位の末尾へ寄せる
		expect("dbcs: track inside a voiced pair snaps to its end",
			track(sb(0xB6) + sb(0xDE), ConvertMode::ToZenkaku, { 0, 1, 2 }, fake),
			"0,2,2");

		// 縮む方向と伸びる方向が混ざる例: ｶﾞ漢b (5 バイト) -> ガ漢ｂ (6 バイト)
		expect("dbcs: track with mixed shrink and growth",
			track(sb(0xB6) + sb(0xDE) + db(0x8940) + "b", ConvertMode::ToZenkaku, { 0, 2, 4, 5 }, fake),
			"0,2,4,6");

		// 書けない候補を含む単位は、まるごと元バイトのまま残る（Ａ￥Ｂ -> A￥B）
		expect("dbcs: unencodable candidate keeps the whole unit",
			hex(convertZenHankaku(db(0x8260) + db(0x8141) + db(0x8261),
				ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			hex("A" + db(0x8141) + "B"));

		// ガ -> ｶﾞ で ﾞ が書けないなら ｶ も出さない
		expect("dbcs: unencodable second codepoint keeps the whole unit",
			hex(convertZenHankaku(db(0x8342) + db(0x8260),
				ConvertMode::ToHankaku, ConvertCategory::All, RejectingCodec(fake, U"ﾞ"))),
			hex(db(0x8342) + "A"));

		// 末尾のリードバイトは、続きが無いので番兵になり、そのまま残る
		expect("dbcs: trailing lead byte is preserved",
			hex(convertZenHankaku(db(0x8260) + sb(0x81), ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			hex("A" + sb(0x81)));

		// 表に無いリード + トレイルの組み合わせ。リードだけを番兵にして、次のバイトから読み直す
		expect("dbcs: invalid trail byte re-syncs on the next byte",
			hex(convertZenHankaku(sb(0x81) + " ", ConvertMode::ToZenkaku, ConvertCategory::All, fake)),
			hex(sb(0x81) + db(0x8140)));

		// リードでもトレイルでも半角カナでもないバイト
		expect("dbcs: orphan byte is preserved",
			hex(convertZenHankaku(sb(0xF5) + db(0x8260), ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			hex(sb(0xF5) + "A"));

		// 不正バイトがあっても、その前後の変換は止まらない
		expect("dbcs: invalid byte does not stop conversion",
			hex(convertZenHankaku(db(0x8260) + sb(0xF5) + db(0x8261),
				ConvertMode::ToHankaku, ConvertCategory::All, fake)),
			hex("A" + sb(0xF5) + "B"));

		// 不正バイトの直後の CR/LF も改行として認識され、結合されない
		{
			const std::string source = db(0x8260) + sb(0x81) + "\r\n" + db(0x8261);
			expect("dbcs: line break after an invalid byte still blocks merging",
				formatHex(coalesceEdits(
					convertZenHankakuEdits(source, ConvertMode::ToHankaku, ConvertCategory::All, fake),
					source, 64, 1024)),
				"[0,2)=41|[5,7)=42");
		}
	}

#ifdef _WIN32
	// 実際の CP932 コーデックでの検証。リードバイト表の取得、MB_ERR_INVALID_CHARS、
	// WC_NO_BEST_FIT_CHARS といった Windows API 側の前提はここでしか確かめられない。
	void runCp932Tests()
	{
		const std::unique_ptr<DbcsCodec> codec = DbcsCodec::create(932);
		if (!codec)
		{
			std::cout << "[SKIP] cp932 tests (code page 932 is unavailable)\n";
			return;
		}

		const DbcsCodec& cp932 = *codec;

		expect("cp932: zen->han ascii",
			convertZenHankaku(enc(cp932, U"ＡＢＣ１２３！"), ConvertMode::ToHankaku, ConvertCategory::All, cp932),
			"ABC123!");

		expect("cp932: han->zen ascii",
			hex(convertZenHankaku("ABC123!", ConvertMode::ToZenkaku, ConvertCategory::All, cp932)),
			hex(enc(cp932, U"ＡＢＣ１２３！")));

		// 半角カナは 1 バイト、全角カナは 2 バイト
		expect("cp932: han->zen kana",
			hex(convertZenHankaku(enc(cp932, U"ｱｲｳ"), ConvertMode::ToZenkaku, ConvertCategory::All, cp932)),
			hex(enc(cp932, U"アイウ")));

		expect("cp932: zen->han voiced",
			hex(convertZenHankaku(enc(cp932, U"ガ"), ConvertMode::ToHankaku, ConvertCategory::All, cp932)),
			hex(enc(cp932, U"ｶﾞ")));

		expect("cp932: han->zen voiced",
			hex(convertZenHankaku(enc(cp932, U"ｶﾞ"), ConvertMode::ToZenkaku, ConvertCategory::All, cp932)),
			hex(enc(cp932, U"ガ")));

		// 前提の確認: CP932 に半角の ¥ (U+00A5) は無い。best fit で 0x5C にされないこと
		{
			std::string out = "keep";
			const bool ok = cp932.encode(U"¥", out);
			expect("cp932: encode rejects a best-fit-only char and keeps out intact",
				std::string(ok ? "true" : "false") + "/" + out,
				"false/keep");
		}

		expect("cp932: unencodable yen keeps the whole unit",
			hex(convertZenHankaku(enc(cp932, U"Ａ￥Ｂ"), ConvertMode::ToHankaku, ConvertCategory::All, cp932)),
			hex("A" + enc(cp932, U"￥") + "B"));

		// NEC 選定 IBM 拡張 (0xED40) と IBM 拡張 (0xFA5C) は同じ文字へ復号される。
		// 符号化はどちらか一方しか選べないので、範囲全体を置き換えると
		// 変換していない文字のバイト列まで書き換わってしまう。
		{
			const std::string nec("\xED\x40", 2);
			char32_t viaNec = 0;
			const size_t consumed = cp932.decode(nec, 0, viaNec);
			const std::string canonical = enc(cp932, std::u32string_view(&viaNec, 1));

			expect("cp932: the duplicate lead sequence decodes as one 2-byte char",
				std::to_string(consumed) + "/" + (viaNec < kInvalidByteBase ? "decoded" : "invalid"),
				"2/decoded");

			expect("cp932: re-encoding the duplicate picks the other byte sequence",
				hex(nec) + " -> " + hex(canonical),
				hex(nec) + " -> " + hex(std::string("\xFA\x5C", 2)));

			expect("cp932: duplicate mapping survives a conversion",
				hex(convertZenHankaku(nec + enc(cp932, U"Ａ") + nec,
					ConvertMode::ToHankaku, ConvertCategory::All, cp932)),
				hex(nec + "A" + nec));
		}

		// 末尾のリードバイト
		expect("cp932: trailing lead byte is preserved",
			hex(convertZenHankaku(enc(cp932, U"Ａ") + std::string("\x82", 1),
				ConvertMode::ToHankaku, ConvertCategory::All, cp932)),
			hex("A" + std::string("\x82", 1)));

		// 0x20 は CP932 のトレイルバイトではないので、0x82 だけが番兵になる
		expect("cp932: invalid trail byte re-syncs on the next byte",
			hex(convertZenHankaku(std::string("\x82\x20", 2),
				ConvertMode::ToZenkaku, ConvertCategory::All, cp932)),
			hex(std::string("\x82", 1) + enc(cp932, U"　")));

		// 不正バイトがあっても変換を中止しない（旧実装は文書全体をエラーにしていた）。
		// 0xFF は CP932 のリードバイトでも単バイト文字でもない
		expect("cp932: undefined byte does not stop conversion",
			hex(convertZenHankaku(std::string("\xFF", 1) + enc(cp932, U"Ａ"),
				ConvertMode::ToHankaku, ConvertCategory::All, cp932)),
			hex(std::string("\xFF", 1) + "A"));

		// リードバイト + 不正なトレイルの直後も、文字境界を取り直して変換が続く
		expect("cp932: conversion resumes after an invalid lead sequence",
			hex(convertZenHankaku(std::string("\x82\x20", 2) + enc(cp932, U"Ａ"),
				ConvertMode::ToHankaku, ConvertCategory::All, cp932)),
			hex(std::string("\x82\x20", 2) + "A"));

		{
			const std::string source = enc(cp932, U"Ａ") + std::string("\x82", 1) + "\r\n" + enc(cp932, U"Ｂ");
			expect("cp932: line break after an invalid byte still blocks merging",
				formatHex(coalesceEdits(
					convertZenHankakuEdits(source, ConvertMode::ToHankaku, ConvertCategory::All, cp932),
					source, 64, 1024)),
				"[0,2)=41|[5,7)=42");
		}

		// 単バイトコードページも同じ経路で扱える
		if (const std::unique_ptr<DbcsCodec> latin1 = DbcsCodec::create(1252))
		{
			// 1252 に全角文字は無いので、半角→全角では 1 件も編集が起きない
			expect("cp1252: han->zen has nothing representable",
				formatHex(convertZenHankakuEdits("ABC", ConvertMode::ToZenkaku, ConvertCategory::All, *latin1)),
				"");

			// 変換対象外の単バイト非 ASCII 文字はそのまま残る
			const std::string accented = enc(*latin1, U"é");
			expect("cp1252: non-ascii single byte is preserved",
				hex(convertZenHankaku(accented + " ", ConvertMode::ToZenkaku, ConvertCategory::All, *latin1)),
				hex(accented + " "));
		}

		// 1 文字が 3 バイト以上になるコードページは受け付けない
		expect("codec: utf-8 is rejected as a dbcs code page",
			DbcsCodec::create(65001) ? "created" : "rejected",
			"rejected");

		expect("codec: gb18030 is rejected as a dbcs code page",
			DbcsCodec::create(54936) ? "created" : "rejected",
			"rejected");
	}
#endif

	std::string makeLargeSource(const std::string& unit, size_t targetBytes)
	{
		std::string out;
		out.reserve(targetBytes + unit.size());
		while (out.size() < targetBytes)
			out += unit;
		return out;
	}

	double elapsedMs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count() / 1000.0;
	}

	// 中央値。**偶数個のときは中央 2 つの平均。** 上側だけを採ると、
	// `--bench 2` や `--bench 4` のような偶数回で値が片側へ寄る。
	double median(std::vector<double> values)
	{
		if (values.empty())
			return 0.0;

		std::sort(values.begin(), values.end());
		const size_t n = values.size();
		return n % 2 != 0
			? values[n / 2]
			: (values[n / 2 - 1] + values[n / 2]) / 2.0;
	}

	// 最小値と中央値を「最小/中央」の形で出す。**ばらつきを隠さないために両方出す。**
	// 2 つの数字が離れていたら、その回の測定は他の負荷に邪魔されている。
	std::string summarize(std::vector<double> samples)
	{
		std::sort(samples.begin(), samples.end());
		std::ostringstream out;
		out << std::fixed << std::setprecision(1)
			<< samples.front() << "/" << median(samples);
		return out.str();
	}

	// Scintilla への置換は含めず converter だけを測る。
	// コーデック抽象化の前後、および UTF-8 と ANSI で同じ数字を比べるためのもの。
	//
	// **同じプロセスの中で repeat 回まわして最小値と中央値を出す。** 1 回だけの測定は
	// 開発環境では同じバイナリでも 17% ばらつく。最初の 1 回は確保したページに
	// 初めて触るぶんが乗るので必ず遅く、最小値でそれを外す。
	//
	// **このケースは巨大な確保を含むので、1 割の差を見るには向かない。**
	// 抽象化の代償そのものを見たいときは下の benchScan を使う。
	void bench(const char* label, const std::string& source, ConvertMode mode, const TextCodec& codec, int repeat)
	{
		std::vector<double> convertMs;
		std::vector<double> coalesceMs;
		size_t units = 0;
		size_t mergedCount = 0;
		size_t mergedBytes = 0;

		for (int i = 0; i < repeat; ++i)
		{
			const auto started = std::chrono::steady_clock::now();
			const std::vector<ConvertEdit> got = convertZenHankakuEdits(source, mode, ConvertCategory::All, codec);
			const auto convertedAt = std::chrono::steady_clock::now();
			const std::vector<ConvertEdit> merged = coalesceEdits(got, source, 0, 4096);
			const auto mergedAt = std::chrono::steady_clock::now();

			convertMs.push_back(elapsedMs(started, convertedAt));
			coalesceMs.push_back(elapsedMs(convertedAt, mergedAt));

			// 実機の置換コストを左右するのは件数と、Scintilla へ書き込むバイト数。
			// 上限を超えると行単位までまとまるので、まとめた後の両方を出す。
			units = got.size();
			mergedCount = merged.size();
			mergedBytes = 0;
			for (const ConvertEdit& edit : merged)
				mergedBytes += edit.text.size();
		}

		std::cout << label
			<< "  input=" << source.size() / (1024 * 1024) << "MB"
			<< "  units=" << units
			<< "  merged=" << mergedCount
			<< "  mergedBytes=" << mergedBytes / (1024 * 1024) << "MB"
			<< "  convert=" << summarize(convertMs) << "ms"
			<< "  coalesce=" << summarize(coalesceMs) << "ms\n";
	}

	// 変換対象がまったく無い入力を走らせる。編集が 1 件も出ないので、巨大な vector や
	// 文字列の確保がまるごと消え、**復号と分類のループだけ**が残る。
	//
	// 4.5 の「コーデック抽象化の代償」はこのループに出るものなので、代償を測るなら
	// こちらを見る。確保が無いぶん、同じ機械でのばらつきが桁違いに小さい。
	void benchScan(const char* label, const std::string& source, const TextCodec& codec, int repeat)
	{
		std::vector<double> scanMs;
		size_t units = 0;

		for (int i = 0; i < repeat; ++i)
		{
			const auto started = std::chrono::steady_clock::now();
			const std::vector<ConvertEdit> got = convertZenHankakuEdits(source, ConvertMode::ToHankaku, ConvertCategory::All, codec);
			scanMs.push_back(elapsedMs(started, std::chrono::steady_clock::now()));
			units = got.size();
		}

		// 0 件でなければ入力の作りが崩れている（確保が混ざるので数字の意味が変わる）
		std::cout << label
			<< "  input=" << source.size() / (1024 * 1024) << "MB"
			<< "  units=" << units
			<< "  scan=" << summarize(scanMs) << "ms\n";
	}

	// 同じプロセスの中で複数の経路を**交互に**走らせ、それぞれの最小値を並べる。
	//
	// **1 割未満の差を見たいときはこれを使う。** 2 つの exe を別プロセスで比べる
	// やり方では、開発環境でのばらつき（±17%）に埋もれて判定できなかった。
	// 交互なら、周波数の変動やキャッシュの状態が両者へ同じようにかかる。
	//
	// 先頭の経路を基準にした比も出す。**同じ経路を 2 つ並べたとき（null control）の
	// 比が、この測定で判定できる限界。** それより小さい差を根拠にしてはいけない。
	struct BenchPath
	{
		const char* label;
		// 返り値は編集件数。入力の作りが崩れていないかの確認に使う。
		std::function<size_t()> run;
	};

	void benchInterleaved(const char* title, const std::string& source, int repeat, const std::vector<BenchPath>& paths)
	{
		const size_t n = paths.size();
		std::vector<std::vector<double>> samples(n);
		std::vector<std::vector<double>> ratios(n);
		std::vector<size_t> units(n, 0);

		for (int i = 0; i < repeat; ++i)
		{
			// **回すたびに開始位置をずらす。** 順序を固定すると、1 回が 1 秒近くある
			// この測定では機械側の周期的な負荷と噛み合い、特定の経路だけが決まって
			// 遅く出ることがある。8.2 の最初の測定はそれで読み違えた。
			std::vector<double> round(n, 0.0);
			for (size_t k = 0; k < n; ++k)
			{
				const size_t p = (k + static_cast<size_t>(i)) % n;
				const auto started = std::chrono::steady_clock::now();
				const size_t got = paths[p].run();
				round[p] = elapsedMs(started, std::chrono::steady_clock::now());
				units[p] = got;
			}

			// **同じ回の中での比を採る（paired）。** 別々に採った最小値を割るより、
			// 周波数や他プロセスの影響が両者へ同じようにかかったぶんだけ相殺される。
			for (size_t p = 0; p < n; ++p)
			{
				samples[p].push_back(round[p]);
				ratios[p].push_back(round[p] / round[0]);
			}
		}

		std::cout << title << "  input=" << source.size() / (1024 * 1024) << "MB\n";

		const double baseline = *std::min_element(samples[0].begin(), samples[0].end());
		for (size_t p = 0; p < n; ++p)
		{
			const double best = *std::min_element(samples[p].begin(), samples[p].end());

			std::cout << "  " << paths[p].label
				<< "  units=" << units[p]
				<< "  time=" << summarize(samples[p]) << "ms"
				<< std::showpos << std::setprecision(2)
				<< "  min vs first=" << (best / baseline - 1.0) * 100.0 << "%"
				<< "  paired median=" << (median(ratios[p]) - 1.0) * 100.0 << "%"
				<< std::noshowpos << std::setprecision(1) << "\n";
		}
	}

	// encode だけを取り出して、呼び出し 1 回あたりの値段を出す。
	//
	// **変換全体のうち encode が占める分の上限がこれで決まる。** 変換が encode を
	// 呼ぶ回数は「変換した単位の数」なので、上のケースの units と掛ければよい。
	// 1 文字の単位（全角英数など）と 2 文字の単位（濁点の分解）で別に出す。
	//
	// **8.2 の前の形も同じバイナリの中で交互に測る。** 別バイナリで測った値と
	// 引き算してはいけない（第 10 章）。
	//
	// どちらも仮想の TextCodec 経由なので、1 回ごとに間接呼び出しが 1 つ余分に乗る。
	// 製品の UTF-8 経路は非仮想なので、**絶対値は上限として安全側**。新旧の比は
	// 両方に同じ 1 段が乗るので、そのぶんだけ差が薄まって出る。
	void benchEncodeOnly(size_t calls, int repeat)
	{
		struct Case
		{
			const char* label;
			std::u32string text;
		};

		const Case cases[] = {
			{"1 char  (zen alnum -> han) ", U"A"},
			{"2 chars (zen voiced -> han)", U"ｶﾞ"},
		};

		struct Path
		{
			const char* label;
			const TextCodec* codec;
		};

		const Path paths[] = {
			{"pre-8.2 (2-pass)", &utf8CodecLegacyEncode()},
			{"product (1-pass)", &utf8Codec()},
		};

		const size_t pathCount = sizeof(paths) / sizeof(paths[0]);

		for (const Case& c : cases)
		{
			std::vector<std::vector<double>> ms(pathCount);
			std::vector<std::vector<double>> ratios(pathCount);
			size_t sink = 0;

			for (int i = 0; i < repeat; ++i)
			{
				// 交互に走らせ、回すたびに順序をずらす（benchInterleaved と同じ理由）
				std::vector<double> round(pathCount, 0.0);
				for (size_t k = 0; k < pathCount; ++k)
				{
					const size_t p = (k + static_cast<size_t>(i)) % pathCount;
					std::string out;
					const auto started = std::chrono::steady_clock::now();
					for (size_t n = 0; n < calls; ++n)
					{
						// 記録側と同じ形。clear() は容量を残すので確保は初回だけ。
						out.clear();
						paths[p].codec->encode(c.text, out);
						sink += out.size();
					}
					round[p] = elapsedMs(started, std::chrono::steady_clock::now());
				}

				for (size_t p = 0; p < pathCount; ++p)
				{
					ms[p].push_back(round[p]);
					ratios[p].push_back(round[p] / round[0]);
				}
			}

			std::cout << "  " << c.label << "  calls=" << calls << "  (sink=" << sink << ")\n";
			for (size_t p = 0; p < pathCount; ++p)
			{
				const double best = *std::min_element(ms[p].begin(), ms[p].end());

				std::cout << "    " << paths[p].label
					<< "  total=" << summarize(ms[p]) << "ms"
					<< std::setprecision(2)
					<< "  per call=" << (best * 1e6 / calls) << "ns"
					<< std::showpos << "  paired median=" << (median(ratios[p]) - 1.0) * 100.0 << "%"
					<< std::noshowpos << std::setprecision(1) << "\n";
			}
		}
	}

	int runBench(int repeat)
	{
		const size_t target = 64u * 1024 * 1024;
		const std::u32string unitText = U"全角ＡＢＣ１２３のテストです。カタカナガギグ　半角ｱｲｳ ABC123!\r\n";

		// **これは他のケースより先に測ること。** 候補ビット表（4.6）は初回の変換で
		// 作るので、そのコストはプロセスで最初の 1 回にだけ乗る。他のケースは
		// 最小値を採るぶんこの 1 回を必ず外すため、別に出さないと記録から消える。
		// 実機では「起動して最初にコマンドを叩いたとき」だけ乗る。方向ごとに
		// 別の表を作るので 2 回とも測る。
		{
			const std::string tiny = enc(utf8Codec(), U"Ａ");
			const auto started = std::chrono::steady_clock::now();
			convertZenHankakuEdits(tiny, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec());
			const auto hankakuAt = std::chrono::steady_clock::now();
			convertZenHankakuEdits(tiny, ConvertMode::ToZenkaku, ConvertCategory::All, utf8Codec());
			const auto zenkakuAt = std::chrono::steady_clock::now();

			std::cout << std::fixed << std::setprecision(1)
				<< "--- bench: first call in the process (builds the candidate table) ---\n"
				<< "zen->han  " << elapsedMs(started, hankakuAt) << "ms\n"
				<< "han->zen  " << elapsedMs(hankakuAt, zenkakuAt) << "ms\n";
		}

		{
			const std::string source = makeLargeSource(enc(utf8Codec(), unitText), target);
			std::cout << "--- bench: utf-8 ---\n";
			bench("zen->han", source, ConvertMode::ToHankaku, utf8Codec(), repeat);
			bench("han->zen", source, ConvertMode::ToZenkaku, utf8Codec(), repeat);
		}

#ifdef _WIN32
		if (const std::unique_ptr<DbcsCodec> cp932 = DbcsCodec::create(932))
		{
			const std::string unit = enc(*cp932, unitText);
			if (unit.empty())
			{
				std::cout << "[SKIP] bench: cp932 (the sample text is not representable)\n";
			}
			else
			{
				const std::string source = makeLargeSource(unit, target);
				std::cout << "--- bench: cp932 ---\n";
				bench("zen->han", source, ConvertMode::ToHankaku, *cp932, repeat);
				bench("han->zen", source, ConvertMode::ToZenkaku, *cp932, repeat);
			}
		}
#endif

		// 変換対象を含まない 64MB。ASCII は全角側の分類に一つも当たらないので、
		// 編集が 0 件になり、ループの素の速さだけが出る。
		// **ASCII は CP932 でも同じバイト列**なので、まったく同じ入力で UTF-8 と
		// ANSI を比べられる。
		{
			const std::string source = makeLargeSource("abcdefghijklmnopqrstuvwxyz0123456789\r\n", target);

			std::cout << "--- bench: scan only (no targets) ---\n";

			// 括弧の数字は 1 文字あたりの仮想呼び出しの回数。
			//
			// UTF-8 は「0 回（製品）」と「1 回」の両方を同じ実装で測れるので、その差が
			// 抽象化の代償そのもの。CP932 は非仮想経路が無いので 1 回が製品の姿。
			//
			// **UTF-8 の 0→1 を CP932 の見込みに読み替えないこと。** そう見立てて
			// 実際に非仮想経路を 2 通り作ったが、どちらも速くならなかった
			// 。1 文字あたりの処理が違うので同じ差にはならない。
			//
			// ラッパーを積んだケースは、代償が呼び出し回数に比例しないことの記録。
			// 傾きから 0 回へ外挿する見積もりは成り立たない。
			const DelegatingCodec utf8Via2(utf8Codec());

			benchScan("utf-8 non-virtual  (0 virtual/char)", source, utf8Codec(), repeat);
			benchScan("utf-8 via codec    (1 virtual/char)", source, utf8CodecForcedVirtual(), repeat);
			benchScan("utf-8 via wrapper  (2 virtual/char)", source, utf8Via2, repeat);

#ifdef _WIN32
			if (const std::unique_ptr<DbcsCodec> cp932 = DbcsCodec::create(932))
			{
				const DelegatingCodec dbcsVia2(*cp932);

				benchScan("cp932 via codec    (1 virtual/char)", source, *cp932, repeat);
				benchScan("cp932 via wrapper  (2 virtual/char)", source, dbcsVia2, repeat);
			}
#endif
		}

#ifdef _WIN32
		// ANSI 経路のコストは 1 文字あたりに乗るので、**同じバイト数でも 1 文字が
		// 何バイトかで大きく変わる。** ASCII だけで見ると 1 バイト = 1 文字で最悪の
		// 条件になり、実文書より悪く出る。ANSI 経路の非仮想化を検討したとき
		// にそれで判断を誤りかけたので、3 種類並べてある。
		//
		// どれも「zen→han で変換対象を 1 つも含まない」ように作る。漢字・ひらがなは
		// 全角側の分類に当たらず、半角カナも zen→han では対象外。編集が 0 件のまま
		// なので、確保が混ざらずループだけが出る。
		if (const std::unique_ptr<DbcsCodec> cp932 = DbcsCodec::create(932))
		{
			struct Case
			{
				const char* label;
				std::u32string unit;
			};

			const Case cases[] = {
				// 1 バイト = 1 文字
				{"ascii   ", U"abcdefghijklmnopqrstuvwxyz0123456789\r\n"},
				// ASCII・改行・半角カナ・2 バイト文字の混在
				{"mixed   ", U"漢字とかなの混ざった行 abc123 ｱｲｳｴｵ\r\n"},
				// ほぼ 2 バイト（同じバイト数なら文字数が半分になる）
				{"two-byte", U"漢字漢字漢字漢字漢字漢字漢字漢字漢字漢字\r\n"},
			};

			std::cout << "--- bench: scan only, cp932 by bytes per char ---\n";
			for (const Case& c : cases)
			{
				const std::string unit = enc(*cp932, c.unit);
				if (unit.empty())
				{
					std::cout << "[SKIP] " << c.label << " (not representable in cp932)\n";
					continue;
				}

				const std::string source = makeLargeSource(unit, target);
				std::ostringstream label;
				label << c.label << " (" << std::fixed << std::setprecision(2)
					<< static_cast<double>(unit.size()) / c.unit.size() << " bytes/char)";
				benchScan(label.str().c_str(), source, *cp932, repeat);
			}
		}
#endif

		// ここから下は encode と編集の記録（8.2）を見るためのもの。
		//
		// 上の scan only は変換対象が 0 件なので、**encode を一度も呼んでいない。**
		// 記録側を測るには、逆に「ほとんどの文字が変換される」入力が必要。
		{
			// 1 行 20 文字すべてが変換対象。改行だけが対象外なので、編集は 1 行 1 件。
			const std::u32string denseLine = U"ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴ\r\n";
			const std::string denseLineBytes = enc(utf8Codec(), denseLine);
			const std::string dense = makeLargeSource(denseLineBytes, target);

			// 上と 1 文字あたりのバイト数が同じで、候補ビット表に一つも当たらない入力。
			// ひらがなは全角側の分類に当たらないので、変換は 0 件になる。
			const std::string sparse = makeLargeSource(
				enc(utf8Codec(), U"あいうえおかきくけこさしすせそたちつてと\r\n"), target);

			// 実文書に近い密度の入力。null control と 8.2 の比較を dense と両方で出す。
			const std::string mixed = makeLargeSource(
				enc(utf8Codec(), U"全角ＡＢＣ１２３のテストです。カタカナガギグ　半角ｱｲｳ ABC123!\r\n"), target);

			const std::pair<const char*, const std::string*> inputs[] = {
				{"dense", &dense},
				{"mixed", &mixed},
			};

			// **同じ経路を 2 つ並べる。** ここに出る差が、その実行・その入力で
			// 観測された「差が無いときの差」。**これは全ケース共通の判定限界では
			// ないので、入力ごとに出す。** 以降の比較でこれより小さい差は主張できない。
			std::cout << "--- bench: the null difference of this run (same path twice) ---\n";
			for (const auto& input : inputs)
			{
				const std::string& bytes = *input.second;
				std::cout << "  input: " << input.first << "\n";
				benchInterleaved("  null control", bytes, repeat, {
					{"product A", [&bytes] { return convertZenHankakuEdits(bytes, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec()).size(); }},
					{"product B", [&bytes] { return convertZenHankakuEdits(bytes, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec()).size(); }},
				});
			}

			std::cout << "--- bench: how much of the time the recording path can account for ---\n";

			// **8.2 で取れる分の上限。** 同じバイト数・同じ 1 文字のバイト数で、
			// 変換が起きるかどうかだけが違う。差には候補判定・分類・ハッシュ検索・
			// encode・編集の記録・位置追跡がすべて入っているので、encode 単体は
			// この差のさらに一部にすぎない。
			benchInterleaved("every char converts vs nothing converts", dense, repeat, {
				{"nothing converts (hiragana) ", [&sparse] { return convertZenHankakuEdits(sparse, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec()).size(); }},
				{"every char converts (zen A) ", [&dense] { return convertZenHankakuEdits(dense, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec()).size(); }},
			});

			// **8.2 で採用した形が、前の形より速いことの確認。**
			// バイナリをまたいだ絶対時間の比較では確かめられないので、比較対象を
			// この実行ファイルへ残して同じプロセスの中で交互に走らせる。
			//
			// **これは「変換コアの計測用バイナリ」での数字。** 製品 DLL には
			// Legacy 経路が無く、リンク時に落ちるぶんコード配置が変わるので、
			// ここの比を製品 DLL の数字として語ってはいけない。
			//
			// 測る前に、**この入力そのもので 2 つの経路の結果が一致することを見る。**
			// benchInterleaved が突き合わせるのは編集の件数だけなので、中身が
			// 違っていても速さの比だけが出てしまう。単体テストは小さい入力での
			// 一致を固定しているが、測るのはこの 64MB のほうなので別に確かめる。
			for (const auto& input : inputs)
			{
				const std::string& bytes = *input.second;
				std::vector<size_t> productPos = { 0, bytes.size() / 4, bytes.size() / 2, bytes.size() };
				std::vector<size_t> legacyPos = productPos;

				const std::vector<ConvertEdit> product = convertZenHankakuEdits(
					bytes, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec(), &productPos);
				const std::vector<ConvertEdit> legacy = convertZenHankakuEditsEncode(
					bytes, ConvertMode::ToHankaku, ConvertCategory::All, EncodeVariant::Legacy, &legacyPos);

				bool same = product.size() == legacy.size() && productPos == legacyPos;
				for (size_t i = 0; same && i < product.size(); ++i)
				{
					same = product[i].begin == legacy[i].begin
						&& product[i].end == legacy[i].end
						&& product[i].text == legacy[i].text;
				}

				if (!same)
				{
					std::cout << "[NG] bench: the two 8.2 paths disagree on the " << input.first
						<< " input (" << product.size() << " vs " << legacy.size() << " edits)\n";
					return 1;
				}

				std::cout << "  " << input.first << ": the two 8.2 paths agree on "
					<< product.size() << " edits\n";
			}

			for (const auto& input : inputs)
			{
				const std::string& bytes = *input.second;
				std::cout << "--- bench: encode and recording, " << input.first << " (8.2) ---\n";

				benchInterleaved("pre-8.2 shape vs the product", bytes, repeat, {
					{"pre-8.2 (2-pass, staged copy) ", [&bytes] { return convertZenHankakuEditsEncode(bytes, ConvertMode::ToHankaku, ConvertCategory::All, EncodeVariant::Legacy).size(); }},
					{"product (1-pass, direct)      ", [&bytes] { return convertZenHankakuEdits(bytes, ConvertMode::ToHankaku, ConvertCategory::All, utf8Codec()).size(); }},
				});
			}

			std::cout << "--- bench: encode alone (upper bound for 8.2) ---\n";

			// **上の dense と同じ回数だけ呼ぶ。** 1 行は全角 20 文字（60 バイト）と
			// CRLF の 62 バイトで、encode を呼ぶのはその 20 文字ぶんだけ。
			// 「入力バイト数 / 3」にすると改行のぶんまで数えて 3.3% 多くなる。
			const size_t unitsPerLine = denseLine.size() - 2; // 末尾の CRLF は対象外
			const size_t denseUnits = dense.size() / denseLineBytes.size() * unitsPerLine;
			benchEncodeOnly(denseUnits, repeat);
		}

		return 0;
	}
}

int main(int argc, char** argv)
{
	// --bench [繰り返し回数]。既定 5 回で、各項目の最小値を出す
	if (argc > 1 && std::strcmp(argv[1], "--bench") == 0)
	{
		int repeat = 5;
		if (argc > 2)
		{
			const int given = std::atoi(argv[2]);
			if (given > 0)
				repeat = given;
		}
		return runBench(repeat);
	}

	runUtf8Tests();
	runCandidateSweepTests();
	expect("utf8: the fast path and the virtual path agree", utf8PathDifferences(), "");
	expect("encode: the pre-8.2 shape agrees with the product path", encodeVariantDifferences(), "");
	runMultiRangeTests();
	runEditLimitTests();
	runCodecContractTests();
	runFakeDbcsTests();
#ifdef _WIN32
	runCp932Tests();
#endif

	if (g_failures == 0)
	{
		std::cout << "\nAll tests passed.\n";
		return 0;
	}

	std::cout << "\n" << g_failures << " test(s) failed.\n";
	return 1;
}
