#include "converter.h"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	enum class CharClass
	{
		Space,
		AlphaNum,
		Katakana,
		Symbol,
		Other
	};

	bool decodeUtf8(std::string_view utf8, size_t& i, char32_t& out)
	{
		if (i >= utf8.size())
			return false;

		const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
		if (c0 < 0x80)
		{
			out = c0;
			++i;
			return true;
		}

		if ((c0 & 0xE0) == 0xC0)
		{
			if (c0 < 0xC2)
				return false;
			if (i + 1 >= utf8.size())
				return false;
			const unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
			if ((c1 & 0xC0) != 0x80)
				return false;
			out = (static_cast<char32_t>(c0 & 0x1F) << 6) | (c1 & 0x3F);
			i += 2;
			return true;
		}

		if ((c0 & 0xF0) == 0xE0)
		{
			if (i + 2 >= utf8.size())
				return false;
			const unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
			const unsigned char c2 = static_cast<unsigned char>(utf8[i + 2]);
			if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
				return false;
			if ((c0 == 0xE0 && c1 < 0xA0) || (c0 == 0xED && c1 >= 0xA0))
				return false;
			out = (static_cast<char32_t>(c0 & 0x0F) << 12)
				| (static_cast<char32_t>(c1 & 0x3F) << 6)
				| (c2 & 0x3F);
			i += 3;
			return true;
		}

		if ((c0 & 0xF8) == 0xF0)
		{
			if (c0 > 0xF4)
				return false;
			if (i + 3 >= utf8.size())
				return false;
			const unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
			const unsigned char c2 = static_cast<unsigned char>(utf8[i + 2]);
			const unsigned char c3 = static_cast<unsigned char>(utf8[i + 3]);
			if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
				return false;
			if ((c0 == 0xF0 && c1 < 0x90) || (c0 == 0xF4 && c1 > 0x8F))
				return false;
			out = (static_cast<char32_t>(c0 & 0x07) << 18)
				| (static_cast<char32_t>(c1 & 0x3F) << 12)
				| (static_cast<char32_t>(c2 & 0x3F) << 6)
				| (c3 & 0x3F);
			i += 4;
			return true;
		}

		return false;
	}

	// UTF-8 として書けないコードポイント。
	//
	// Unicode の上限より上は、復号できなかったバイトの番兵（kInvalidByteBase）。
	// **単独のサロゲートは 3 バイトへ組み立てられてしまうが、それは UTF-8 として
	// 不正な列**（CESU-8）なので弾く。decodeUtf8() 側は ED A0.. を既に拒否して
	// いるので、これで復号と符号化の可否がそろう。
	// どちらも変換表の出力には現れないため、実際には到達しない防御。
	bool encodableAsUtf8(char32_t cp)
	{
		return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
	}

	void appendUtf8(std::string& out, char32_t cp)
	{
		if (cp <= 0x7F)
		{
			out.push_back(static_cast<char>(cp));
		}
		else if (cp <= 0x7FF)
		{
			out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else if (cp <= 0xFFFF)
		{
			out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	// UTF-8 の復号・符号化の実体。変換ループからは仮想呼び出しを挟まずに直接呼ぶ。
	// 1 文字ごとに間接呼び出しになると、変換対象を含まない 64MB の素通しが
	// 269ms から 450ms へ（1.7 倍）落ちるため、UTF-8 だけはテンプレートで
	// 直接束縛している（下の Utf8Access / CodecAccess）。
	//
	// **Legacy = true は 8.2 の前の形で、テストからしか作らない。** 8.2 の改善は
	// 「同じバイナリの中で交互に走らせて比べる」ことでしか確かめられなかった
	// （バイナリをまたいだ絶対時間の比較では、テンプレートの実体化が増えるだけで
	// 数字が動く）。採用した形が本当に速いことを後から確かめられるように、
	// 比較対象を製品バイナリへ残してある。EncodeVariant / 10 章を参照。
	template <bool Legacy>
	class Utf8AccessT
	{
	public:
		// EditRecorder がこれを見て、中間バッファ経由の記録へ切り替える
		static constexpr bool kStagedRecord = Legacy;

		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const
		{
			size_t i = pos;
			char32_t cp = 0;
			if (decodeUtf8(bytes, i, cp))
			{
				out = cp;
				return i - pos;
			}

			// 不正バイトは値を退避して 1 バイトだけ進める。番兵は変換表に一致しないので、
			// このバイトは未変更部分として元のまま残る。
			out = kInvalidByteBase + static_cast<unsigned char>(bytes[pos]);
			return 1;
		}

		// **契約: 書けない文字が 1 つでもあれば out は呼び出し前のまま。**
		// 復号できなかったバイトの番兵は書き戻せないので、ここで弾く
		// （変換表の出力に番兵は現れないため、実際には到達しない防御）。
		//
		// 検査と書き込みを 1 パスにまとめ、弾いたときだけ書いた分を戻す。
		// 検査を先に回す 2 パスの形から変えたのは、1 単位が 1〜2 文字しかないのに
		// ループを 2 つ回すぶんが、開発環境での計測では無視できなかったため。
		bool encode(std::u32string_view text, std::string& out) const
		{
			if constexpr (Legacy)
			{
				for (const char32_t cp : text)
				{
					if (!encodableAsUtf8(cp))
						return false;
				}

				for (const char32_t cp : text)
					appendUtf8(out, cp);
				return true;
			}
			else
			{
				const size_t saved = out.size();
				for (const char32_t cp : text)
				{
					if (!encodableAsUtf8(cp))
					{
						out.resize(saved);
						return false;
					}

					appendUtf8(out, cp);
				}
				return true;
			}
		}
	};

	using Utf8Access = Utf8AccessT<false>;

	template <bool Legacy>
	class Utf8CodecT final : public TextCodec
	{
	public:
		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const override
		{
			return access_.decode(bytes, pos, out);
		}

		bool encode(std::u32string_view text, std::string& out) const override
		{
			return access_.encode(text, out);
		}

	private:
		Utf8AccessT<Legacy> access_;
	};

	using Utf8Codec = Utf8CodecT<false>;

	// UTF-8 以外のコーデックはこちら。1 文字ごとに仮想呼び出しになる。
	// ウォーム済み ASCII の素通しでは、この経路の CP932 と、UTF-8 を同じく仮想 1 回で
	// 通したものに測れる差は出なかった。
	//
	// **ANSI 側にも非仮想経路を作る案は、2 通り実装して測ったうえで見送った**
	// 。このツールチェーンでは改善を実証できず、経路が 3 本目に
	// 増える保守コストに見合わなかった。余地が無いと確かめたわけではないので、
	// やり直すなら experiment/8.1-dbcs-nonvirtual ブランチから。
	class CodecAccess
	{
	public:
		static constexpr bool kStagedRecord = false;

		explicit CodecAccess(const TextCodec& codec)
			: codec_(&codec)
		{
		}

		size_t decode(std::string_view bytes, size_t pos, char32_t& out) const
		{
			// コーデックが契約を破っても前進が止まらず、末尾も越えないようにする。
			// この防御は外部実装を呼ぶこちら側にだけ置き、UTF-8 の経路には持ち込まない。
			const size_t consumed = codec_->decode(bytes, pos, out);
			if (consumed == 0)
				return 1;
			return (consumed > bytes.size() - pos) ? bytes.size() - pos : consumed;
		}

		bool encode(std::u32string_view text, std::string& out) const
		{
			return codec_->encode(text, out);
		}

	private:
		const TextCodec* codec_;
	};

	struct DecodedChar
	{
		char32_t cp = 0;
		bool valid = false;
		size_t begin = 0;
		size_t end = 0;
	};

	// 1 文字先読みしながらコーデックで復号する。バイト位置を保持するので、
	// コードポイント列を丸ごと配列に持たずに済む。
	template <class Access>
	class CodePointReader
	{
	public:
		CodePointReader(const std::string& bytes, Access access)
			: bytes_(bytes)
			, access_(access)
			, current_(read(0))
			, next_(current_.valid ? read(current_.end) : DecodedChar{})
		{
		}

		bool atEnd() const { return !current_.valid; }
		const DecodedChar& current() const { return current_; }
		const DecodedChar& next() const { return next_; }

		void advance()
		{
			current_ = next_;
			next_ = current_.valid ? read(current_.end) : DecodedChar{};
		}

	private:
		DecodedChar read(size_t pos) const
		{
			DecodedChar out;
			if (pos >= bytes_.size())
				return out;

			out.valid = true;
			out.begin = pos;
			// 1 以上を返すこと、末尾を越えないことは Access 側が保証する
			out.end = pos + access_.decode(bytes_, pos, out.cp);
			return out;
		}

		const std::string& bytes_;
		Access access_;
		DecodedChar current_;
		DecodedChar next_;
	};

	// 変換候補が出力先で表現できないときは、その変換単位をまるごと元の入力へ戻す
	// （＝編集を記録しない）。
	// 追跡したい位置を、変換単位ごとに変換後の位置へ解決する。
	// 隣り合う単位は 1 件の編集にまとめてしまうので、まとまった後では単位の境界が分からない。
	// 位置の対応付けは、必ずまとめる前のこの時点で済ませる。
	class PositionMapper
	{
	public:
		explicit PositionMapper(std::vector<size_t>* positions)
			: positions_(positions)
		{
			if (positions_)
				resolved_.assign(positions_->size(), false);
		}

		// 採用された変換単位 1 件を通知する。convertedSize は変換後のバイト数。
		void onUnit(size_t begin, size_t end, size_t convertedSize)
		{
			if (!positions_)
				return;

			for (size_t i = 0; i < positions_->size(); ++i)
			{
				if (resolved_[i])
					continue;

				size_t& pos = (*positions_)[i];
				if (pos <= begin)
				{
					// これより後ろの単位は pos に影響しない
					pos = shift(pos);
					resolved_[i] = true;
				}
				else if (pos < end)
				{
					// 単位の内側は分割できないので、その単位の末尾へ寄せる
					pos = shift(begin) + convertedSize;
					resolved_[i] = true;
				}
			}

			delta_ += static_cast<ptrdiff_t>(convertedSize) - static_cast<ptrdiff_t>(end - begin);
		}

		void finish()
		{
			if (!positions_)
				return;

			for (size_t i = 0; i < positions_->size(); ++i)
			{
				if (!resolved_[i])
					(*positions_)[i] = shift((*positions_)[i]);
			}
		}

	private:
		size_t shift(size_t pos) const
		{
			return static_cast<size_t>(static_cast<ptrdiff_t>(pos) + delta_);
		}

		std::vector<size_t>* positions_;
		std::vector<bool> resolved_;
		ptrdiff_t delta_ = 0;
	};

	// 改行をまたいで結合すると、変換していない行まで置換範囲に入り、
	// その行のブックマークやマーカーを巻き込む。
	// 対応している DBCS コードページはトレイルバイトに 0x0A / 0x0D を含まないので、
	// ANSI のバイト列でもバイト検索で誤検出しない。
	//
	// **探す範囲は必ず [begin, end) で区切る。** 以前は `find_first_of` で begin から
	// 探して結果を end と比べていたが、それだと見つからないときに文書の末尾まで走る。
	// 改行の少ない文書（1 行しかない巨大な JSON など）で編集ごとに呼ぶことになるので、
	// 結合そのものが O(文書サイズ × 編集数) になっていた。
	bool crossesLineBreak(const std::string& source, size_t begin, size_t end)
	{
		if (begin >= end)
			return false;

		const char* from = source.data() + begin;
		const size_t length = end - begin;
		return std::memchr(from, '\n', length) != nullptr
			|| std::memchr(from, '\r', length) != nullptr;
	}

	// 途中結合が要る理由。
	//
	// 編集は「変わった範囲」だけを持つので、通常の文書では文書サイズに対して
	// 十分小さい。ただし**変換対象と非対象が 1 文字ずつ交互に並ぶ文書**では
	// 単位ごとに 1 件になる。`Ａ\n` の繰り返しなら 64MB で約 1670 万件、
	// 半角→全角の `A\n` なら約 3350 万件で、1 件 48 バイト前後なので
	// 再確保の山を含めて 1〜3GB に達する。文書は壊れないが、OS 全体へ
	// 強いメモリ圧力がかかる。
	//
	// そこで EditLimits::compactEvery 件たまったら、**その場で最後と同じ結合を
	// 通してから続ける**（溜める先の決め方は compactAt() を見ること）。
	//
	// **上限（EditLimits::maxCount）を渡していない呼び出しでは何もしない。**
	// 守るべき件数が無い結合は減らないので、compactEvery 件ぶんの置換テキストを
	// 写すだけ損になる（同じバイナリの中の paired 中央値で +3〜8%。第 10 章）。
	template <class Access>
	class EditRecorder
	{
	public:
		EditRecorder(std::vector<ConvertEdit>& edits, Access access, PositionMapper& positions,
			const std::string& source, const EditLimits& limits)
			: edits_(edits)
			, access_(access)
			, positions_(positions)
			, source_(source)
			, limits_(limits)
			, nextCompact_(compactAt(limits, 0))
		{
		}

		void convert(size_t begin, size_t end, std::u32string_view converted)
		{
			// 直前の編集へ続く単位は、置換テキストへ直接書いて 1 件へ伸ばす。
			// collapsed_ のときは、間に挟まる未変更バイトも一緒に写して伸ばす。
			const bool extend = !edits_.empty() && (edits_.back().end == begin || collapsed_);

			// **中間バッファへ書いてからコピーするのではなく、置換テキストへ直接書く。**
			// 連続して変換される文字が多い文書で効く。
			if constexpr (!Access::kStagedRecord)
			{
				if (extend)
				{
					const size_t gap = begin - edits_.back().end;
					std::string& out = edits_.back().text;
					const size_t saved = out.size();
					if (gap != 0)
						out.append(source_, edits_.back().end, gap);

					if (!access_.encode(converted, out))
					{
						// encode は失敗時に out を変えない契約だが、**ここは文書へ書き戻す
						// 文字列なので、外部のコーデックが契約を破ったときの被害が大きい。**
						// 中間バッファ経由だった頃は、汚れるのは使い捨てのバッファだけだった。
						//
						// **戻せるのは「末尾へ追記してから失敗した」場合だけ。** 呼び出し
						// 前からあった部分を消したり書き換えたりする実装は復元できない。
						out.resize(saved);
						return;
					}

					positions_.onUnit(begin, end, out.size() - saved - gap);
					edits_.back().end = end;
					return;
				}
			}

			// 書けない候補は編集を記録しない。その単位はまるごと元のバイト列のまま残る。
			text_.clear();
			if (!access_.encode(converted, text_))
				return;

			positions_.onUnit(begin, end, text_.size());

			// 8.2 の前の形。中間バッファから置換テキストへコピーする
			if constexpr (Access::kStagedRecord)
			{
				if (extend)
				{
					ConvertEdit& last = edits_.back();
					if (const size_t gap = begin - last.end; gap != 0)
						last.text.append(source_, last.end, gap);

					last.end = end;
					last.text += text_;
					return;
				}
			}

			// text_ はムーブ後に不定になるが、次回の先頭で clear() するので問題ない
			edits_.push_back({ begin, end, std::move(text_) });
			compactIfNeeded();
		}

		void convertOne(size_t begin, size_t end, char32_t converted)
		{
			convert(begin, end, std::u32string_view(&converted, 1));
		}

	private:
		// 次にまとめ直す件数。上限が無い呼び出しでは何もしない（SIZE_MAX を返す）。
		//
		// **必ず上限を超えた件数で結合を呼ぶ。** coalesceEdits() は件数が上限以下なら
		// そのまま返すので、上限より少ない件数で呼ぶと 1 件も減らない。上限が刻みより
		// 大きい呼び出し（上限 200 万件・刻み 100 万件など）では毎回それに当たり、
		// 「上限 + 刻み で抑える」という約束が果たされなくなる。製品の 4096 件は
		// 刻みよりずっと小さいので、そちらは刻みがそのまま溜める先になる。
		static size_t compactAt(const EditLimits& limits, size_t held)
		{
			if (limits.maxCount == SIZE_MAX)
				return SIZE_MAX;

			const size_t next = held > SIZE_MAX - limits.compactEvery
				? SIZE_MAX
				: held + limits.compactEvery;

			return next > limits.maxCount ? next : limits.maxCount + 1;
		}

		// 件数が膨らんだら、最後に通すのと同じ結合をその場で通す。
		// 結合後は必ず maxCount 件以下になるので、山はこの閾値で抑えられる。
		void compactIfNeeded()
		{
			if (edits_.size() < nextCompact_)
				return;

			const size_t before = edits_.size();
			edits_ = coalesceEdits(edits_, source_, limits_.maxGap, limits_.maxCount);

			// 上限を超えた状態で呼んでいるので、結合は上限以下まで必ず落ちる
			// （間隔・行単位で足りなければ最後は 1 件）。**それでも減らなかったときの
			// 保険。** 閾値を据え置くと以降は単位ごとにまとめ直すことになり、
			// 変換全体が O(編集数 × 文書サイズ) になる。
			nextCompact_ = edits_.size() < before
				? compactAt(limits_, edits_.size())
				: SIZE_MAX;

			// **1 件へ潰れ、その 1 件が改行をまたいでいる**なら、最後の結合が選ぶ
			// 「全体を 1 件」と同じ状態。以降はそこへ書き足す。まとめ直すたびに
			// 範囲全体ぶんの置換テキストを作り直すのを避けるため。
			//
			// 改行をまたいでいない 1 件は、行の中だけがまとまった段階 1 の結果で、
			// 次の行からは別の編集になる。ここで書き足しへ移ると、最後の結合が
			// 選ばない形（行をまたぐ 1 件）になってしまうので移らない。
			collapsed_ = edits_.size() == 1
				&& crossesLineBreak(source_, edits_.front().begin, edits_.front().end);
		}

		std::vector<ConvertEdit>& edits_;
		Access access_;
		PositionMapper& positions_;
		std::string text_;
		const std::string& source_;
		EditLimits limits_;
		size_t nextCompact_ = SIZE_MAX;
		bool collapsed_ = false;
	};

	bool categoryAllows(ConvertCategory category, CharClass cls)
	{
		if (category == ConvertCategory::All)
			return cls != CharClass::Other;

		switch (category)
		{
		case ConvertCategory::AlphaNum: return cls == CharClass::AlphaNum;
		case ConvertCategory::Katakana: return cls == CharClass::Katakana;
		case ConvertCategory::Symbol:   return cls == CharClass::Symbol;
		case ConvertCategory::Space:    return cls == CharClass::Space;
		default: return false;
		}
	}

	const std::unordered_map<char32_t, std::pair<char32_t, char32_t>>& zenVoicedToHan()
	{
		static const std::unordered_map<char32_t, std::pair<char32_t, char32_t>> table = {
			{U'ガ', {U'ｶ', U'ﾞ'}}, {U'ギ', {U'ｷ', U'ﾞ'}}, {U'グ', {U'ｸ', U'ﾞ'}},
			{U'ゲ', {U'ｹ', U'ﾞ'}}, {U'ゴ', {U'ｺ', U'ﾞ'}},
			{U'ザ', {U'ｻ', U'ﾞ'}}, {U'ジ', {U'ｼ', U'ﾞ'}}, {U'ズ', {U'ｽ', U'ﾞ'}},
			{U'ゼ', {U'ｾ', U'ﾞ'}}, {U'ゾ', {U'ｿ', U'ﾞ'}},
			{U'ダ', {U'ﾀ', U'ﾞ'}}, {U'ヂ', {U'ﾁ', U'ﾞ'}}, {U'ヅ', {U'ﾂ', U'ﾞ'}},
			{U'デ', {U'ﾃ', U'ﾞ'}}, {U'ド', {U'ﾄ', U'ﾞ'}},
			{U'バ', {U'ﾊ', U'ﾞ'}}, {U'ビ', {U'ﾋ', U'ﾞ'}}, {U'ブ', {U'ﾌ', U'ﾞ'}},
			{U'ベ', {U'ﾍ', U'ﾞ'}}, {U'ボ', {U'ﾎ', U'ﾞ'}},
			{U'パ', {U'ﾊ', U'ﾟ'}}, {U'ピ', {U'ﾋ', U'ﾟ'}}, {U'プ', {U'ﾌ', U'ﾟ'}},
			{U'ペ', {U'ﾍ', U'ﾟ'}}, {U'ポ', {U'ﾎ', U'ﾟ'}},
			{U'ヴ', {U'ｳ', U'ﾞ'}},
			// ヸ(U+30F8) / ヹ(U+30F9) は基底の ヰ / ヱ に半角が無いため対象外
			{U'ヷ', {U'ﾜ', U'ﾞ'}}, {U'ヺ', {U'ｦ', U'ﾞ'}},
		};
		return table;
	}

	const std::unordered_map<uint64_t, char32_t>& hanVoicedToZen()
	{
		static const std::unordered_map<uint64_t, char32_t> table = {
			{(static_cast<uint64_t>(U'ｶ') << 32) | U'ﾞ', U'ガ'},
			{(static_cast<uint64_t>(U'ｷ') << 32) | U'ﾞ', U'ギ'},
			{(static_cast<uint64_t>(U'ｸ') << 32) | U'ﾞ', U'グ'},
			{(static_cast<uint64_t>(U'ｹ') << 32) | U'ﾞ', U'ゲ'},
			{(static_cast<uint64_t>(U'ｺ') << 32) | U'ﾞ', U'ゴ'},
			{(static_cast<uint64_t>(U'ｻ') << 32) | U'ﾞ', U'ザ'},
			{(static_cast<uint64_t>(U'ｼ') << 32) | U'ﾞ', U'ジ'},
			{(static_cast<uint64_t>(U'ｽ') << 32) | U'ﾞ', U'ズ'},
			{(static_cast<uint64_t>(U'ｾ') << 32) | U'ﾞ', U'ゼ'},
			{(static_cast<uint64_t>(U'ｿ') << 32) | U'ﾞ', U'ゾ'},
			{(static_cast<uint64_t>(U'ﾀ') << 32) | U'ﾞ', U'ダ'},
			{(static_cast<uint64_t>(U'ﾁ') << 32) | U'ﾞ', U'ヂ'},
			{(static_cast<uint64_t>(U'ﾂ') << 32) | U'ﾞ', U'ヅ'},
			{(static_cast<uint64_t>(U'ﾃ') << 32) | U'ﾞ', U'デ'},
			{(static_cast<uint64_t>(U'ﾄ') << 32) | U'ﾞ', U'ド'},
			{(static_cast<uint64_t>(U'ﾊ') << 32) | U'ﾞ', U'バ'},
			{(static_cast<uint64_t>(U'ﾋ') << 32) | U'ﾞ', U'ビ'},
			{(static_cast<uint64_t>(U'ﾌ') << 32) | U'ﾞ', U'ブ'},
			{(static_cast<uint64_t>(U'ﾍ') << 32) | U'ﾞ', U'ベ'},
			{(static_cast<uint64_t>(U'ﾎ') << 32) | U'ﾞ', U'ボ'},
			{(static_cast<uint64_t>(U'ﾊ') << 32) | U'ﾟ', U'パ'},
			{(static_cast<uint64_t>(U'ﾋ') << 32) | U'ﾟ', U'ピ'},
			{(static_cast<uint64_t>(U'ﾌ') << 32) | U'ﾟ', U'プ'},
			{(static_cast<uint64_t>(U'ﾍ') << 32) | U'ﾟ', U'ペ'},
			{(static_cast<uint64_t>(U'ﾎ') << 32) | U'ﾟ', U'ポ'},
			{(static_cast<uint64_t>(U'ｳ') << 32) | U'ﾞ', U'ヴ'},
			{(static_cast<uint64_t>(U'ﾜ') << 32) | U'ﾞ', U'ヷ'},
			{(static_cast<uint64_t>(U'ｦ') << 32) | U'ﾞ', U'ヺ'},
		};
		return table;
	}

	const std::unordered_map<uint64_t, std::pair<char32_t, char32_t>>& zenCombiningVoicedToHan()
	{
		static const std::unordered_map<uint64_t, std::pair<char32_t, char32_t>> table = {
			{(static_cast<uint64_t>(U'カ') << 32) | U'\u3099', {U'ｶ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'キ') << 32) | U'\u3099', {U'ｷ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ク') << 32) | U'\u3099', {U'ｸ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ケ') << 32) | U'\u3099', {U'ｹ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'コ') << 32) | U'\u3099', {U'ｺ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'サ') << 32) | U'\u3099', {U'ｻ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'シ') << 32) | U'\u3099', {U'ｼ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ス') << 32) | U'\u3099', {U'ｽ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'セ') << 32) | U'\u3099', {U'ｾ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ソ') << 32) | U'\u3099', {U'ｿ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'タ') << 32) | U'\u3099', {U'ﾀ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'チ') << 32) | U'\u3099', {U'ﾁ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ツ') << 32) | U'\u3099', {U'ﾂ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'テ') << 32) | U'\u3099', {U'ﾃ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ト') << 32) | U'\u3099', {U'ﾄ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ハ') << 32) | U'\u3099', {U'ﾊ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ヒ') << 32) | U'\u3099', {U'ﾋ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'フ') << 32) | U'\u3099', {U'ﾌ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ヘ') << 32) | U'\u3099', {U'ﾍ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ホ') << 32) | U'\u3099', {U'ﾎ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ハ') << 32) | U'\u309A', {U'ﾊ', U'ﾟ'}},
			{(static_cast<uint64_t>(U'ヒ') << 32) | U'\u309A', {U'ﾋ', U'ﾟ'}},
			{(static_cast<uint64_t>(U'フ') << 32) | U'\u309A', {U'ﾌ', U'ﾟ'}},
			{(static_cast<uint64_t>(U'ヘ') << 32) | U'\u309A', {U'ﾍ', U'ﾟ'}},
			{(static_cast<uint64_t>(U'ホ') << 32) | U'\u309A', {U'ﾎ', U'ﾟ'}},
			{(static_cast<uint64_t>(U'ウ') << 32) | U'\u3099', {U'ｳ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ワ') << 32) | U'\u3099', {U'ﾜ', U'ﾞ'}},
			{(static_cast<uint64_t>(U'ヲ') << 32) | U'\u3099', {U'ｦ', U'ﾞ'}},
		};
		return table;
	}

	const std::unordered_map<char32_t, char32_t>& zenKanaToHan()
	{
		static const std::unordered_map<char32_t, char32_t> table = {
			{U'。', U'｡'}, {U'「', U'｢'}, {U'」', U'｣'}, {U'、', U'､'}, {U'・', U'･'},
			{U'ヲ', U'ｦ'},
			{U'ァ', U'ｧ'}, {U'ィ', U'ｨ'}, {U'ゥ', U'ｩ'}, {U'ェ', U'ｪ'}, {U'ォ', U'ｫ'},
			{U'ャ', U'ｬ'}, {U'ュ', U'ｭ'}, {U'ョ', U'ｮ'}, {U'ッ', U'ｯ'},
			{U'ー', U'ｰ'},
			{U'ア', U'ｱ'}, {U'イ', U'ｲ'}, {U'ウ', U'ｳ'}, {U'エ', U'ｴ'}, {U'オ', U'ｵ'},
			{U'カ', U'ｶ'}, {U'キ', U'ｷ'}, {U'ク', U'ｸ'}, {U'ケ', U'ｹ'}, {U'コ', U'ｺ'},
			{U'サ', U'ｻ'}, {U'シ', U'ｼ'}, {U'ス', U'ｽ'}, {U'セ', U'ｾ'}, {U'ソ', U'ｿ'},
			{U'タ', U'ﾀ'}, {U'チ', U'ﾁ'}, {U'ツ', U'ﾂ'}, {U'テ', U'ﾃ'}, {U'ト', U'ﾄ'},
			{U'ナ', U'ﾅ'}, {U'ニ', U'ﾆ'}, {U'ヌ', U'ﾇ'}, {U'ネ', U'ﾈ'}, {U'ノ', U'ﾉ'},
			{U'ハ', U'ﾊ'}, {U'ヒ', U'ﾋ'}, {U'フ', U'ﾌ'}, {U'ヘ', U'ﾍ'}, {U'ホ', U'ﾎ'},
			{U'マ', U'ﾏ'}, {U'ミ', U'ﾐ'}, {U'ム', U'ﾑ'}, {U'メ', U'ﾒ'}, {U'モ', U'ﾓ'},
			{U'ヤ', U'ﾔ'}, {U'ユ', U'ﾕ'}, {U'ヨ', U'ﾖ'},
			{U'ラ', U'ﾗ'}, {U'リ', U'ﾘ'}, {U'ル', U'ﾙ'}, {U'レ', U'ﾚ'}, {U'ロ', U'ﾛ'},
			{U'ワ', U'ﾜ'}, {U'ン', U'ﾝ'},
			{U'゛', U'ﾞ'}, {U'゜', U'ﾟ'},
		};
		return table;
	}

	const std::unordered_map<char32_t, char32_t>& hanKanaToZen()
	{
		static const std::unordered_map<char32_t, char32_t> table = {
			{U'｡', U'。'}, {U'｢', U'「'}, {U'｣', U'」'}, {U'､', U'、'}, {U'･', U'・'},
			{U'ｦ', U'ヲ'},
			{U'ｧ', U'ァ'}, {U'ｨ', U'ィ'}, {U'ｩ', U'ゥ'}, {U'ｪ', U'ェ'}, {U'ｫ', U'ォ'},
			{U'ｬ', U'ャ'}, {U'ｭ', U'ュ'}, {U'ｮ', U'ョ'}, {U'ｯ', U'ッ'},
			{U'ｰ', U'ー'},
			{U'ｱ', U'ア'}, {U'ｲ', U'イ'}, {U'ｳ', U'ウ'}, {U'ｴ', U'エ'}, {U'ｵ', U'オ'},
			{U'ｶ', U'カ'}, {U'ｷ', U'キ'}, {U'ｸ', U'ク'}, {U'ｹ', U'ケ'}, {U'ｺ', U'コ'},
			{U'ｻ', U'サ'}, {U'ｼ', U'シ'}, {U'ｽ', U'ス'}, {U'ｾ', U'セ'}, {U'ｿ', U'ソ'},
			{U'ﾀ', U'タ'}, {U'ﾁ', U'チ'}, {U'ﾂ', U'ツ'}, {U'ﾃ', U'テ'}, {U'ﾄ', U'ト'},
			{U'ﾅ', U'ナ'}, {U'ﾆ', U'ニ'}, {U'ﾇ', U'ヌ'}, {U'ﾈ', U'ネ'}, {U'ﾉ', U'ノ'},
			{U'ﾊ', U'ハ'}, {U'ﾋ', U'ヒ'}, {U'ﾌ', U'フ'}, {U'ﾍ', U'ヘ'}, {U'ﾎ', U'ホ'},
			{U'ﾏ', U'マ'}, {U'ﾐ', U'ミ'}, {U'ﾑ', U'ム'}, {U'ﾒ', U'メ'}, {U'ﾓ', U'モ'},
			{U'ﾔ', U'ヤ'}, {U'ﾕ', U'ユ'}, {U'ﾖ', U'ヨ'},
			{U'ﾗ', U'ラ'}, {U'ﾘ', U'リ'}, {U'ﾙ', U'ル'}, {U'ﾚ', U'レ'}, {U'ﾛ', U'ロ'},
			{U'ﾜ', U'ワ'}, {U'ﾝ', U'ン'},
			{U'ﾞ', U'゛'}, {U'ﾟ', U'゜'},
		};
		return table;
	}

	char32_t toHankakuExtra(char32_t cp)
	{
		switch (cp)
		{
		case U'￠': return U'¢';
		case U'￡': return U'£';
		case U'￢': return U'¬';
		case U'￣': return U'¯';
		case U'￤': return U'¦';
		case U'￥': return U'¥';
		case U'￦': return U'₩';
		default: return 0;
		}
	}

	char32_t toZenkakuExtra(char32_t cp)
	{
		switch (cp)
		{
		case U'¢': return U'￠';
		case U'£': return U'￡';
		case U'¬': return U'￢';
		case U'¯': return U'￣';
		case U'¦': return U'￤';
		case U'¥': return U'￥';
		case U'₩': return U'￦';
		default: return 0;
		}
	}

	bool isAsciiAlphaNum(char32_t cp)
	{
		return (cp >= U'0' && cp <= U'9')
			|| (cp >= U'A' && cp <= U'Z')
			|| (cp >= U'a' && cp <= U'z');
	}

	bool isZenAlphaNum(char32_t cp)
	{
		return (cp >= U'０' && cp <= U'９')
			|| (cp >= U'Ａ' && cp <= U'Ｚ')
			|| (cp >= U'ａ' && cp <= U'ｚ');
	}

	CharClass classifyZen(char32_t cp)
	{
		if (cp == U'　')
			return CharClass::Space;
		if (isZenAlphaNum(cp))
			return CharClass::AlphaNum;
		if (zenVoicedToHan().find(cp) != zenVoicedToHan().end())
			return CharClass::Katakana;
		if (zenKanaToHan().find(cp) != zenKanaToHan().end())
			return CharClass::Katakana;
		if ((cp >= 0xFF01 && cp <= 0xFF5E) || toHankakuExtra(cp) != 0)
			return CharClass::Symbol;
		return CharClass::Other;
	}

	CharClass classifyHan(char32_t cp, char32_t next, bool& consumeNext)
	{
		consumeNext = false;

		if (cp == U' ')
			return CharClass::Space;

		if (isAsciiAlphaNum(cp))
			return CharClass::AlphaNum;

		if (next == U'ﾞ' || next == U'ﾟ')
		{
			const uint64_t key = (static_cast<uint64_t>(cp) << 32) | next;
			if (hanVoicedToZen().find(key) != hanVoicedToZen().end())
			{
				consumeNext = true;
				return CharClass::Katakana;
			}
		}

		if (hanKanaToZen().find(cp) != hanKanaToZen().end())
			return CharClass::Katakana;

		if ((cp >= 0x21 && cp <= 0x7E) || toZenkakuExtra(cp) != 0)
			return CharClass::Symbol;

		return CharClass::Other;
	}

	// 変換候補になりうるコードポイントの表 ← 性能の要
	//
	// ホットループは 1 文字ごとに「結合濁点の表」「濁点付きカナの表」「カナの表」で
	// ハッシュ検索を 3 回していた。ASCII や漢字はどれにも当たらないので全部空振りで、
	// 変換対象を含まない 64MB を素通しするだけで 1.5 秒かかっていた（第 10 章）。
	// ビット 1 つの判定に置き換える。
	//
	// **表は classify から作る。** 候補の条件を手で書き写すと、変換表を増やしたときに
	// 静かに食い違う。分類が Other 以外になるコードポイントを全部立てているので、
	// 定義上ズレようがない。結合濁点と組む基底だけは単体で対象外になりうるため別に足す。
	//
	// BMP の外は候補が無い。**これは現在の規則がすべて U+FFFF 以下だから言えることで、
	// 分類から導出しても保証されない。** 非 BMP の規則を足すとこの表に載らず、静かに
	// 無効化される。`nonBmpCandidateLeads()` が分類関数を U+10000〜U+10FFFF へ通して
	// それを検査し、単体テストで 0 に固定してある。復号できなかったバイトの番兵は
	// kInvalidByteBase 以上なので、ここで落ちる。
	constexpr size_t kCandidateLimit = 0x10000;
	using CandidateBits = std::bitset<kCandidateLimit>;

	const CandidateBits& zenCandidates()
	{
		static const CandidateBits bits = []
		{
			CandidateBits b;
			for (char32_t cp = 0; cp < kCandidateLimit; ++cp)
			{
				if (classifyZen(cp) != CharClass::Other)
					b.set(cp);
			}
			for (const auto& entry : zenCombiningVoicedToHan())
				b.set(static_cast<size_t>(entry.first >> 32));
			return b;
		}();
		return bits;
	}

	const CandidateBits& hanCandidates()
	{
		static const CandidateBits bits = []
		{
			CandidateBits b;
			for (char32_t cp = 0; cp < kCandidateLimit; ++cp)
			{
				bool consumeNext = false;
				if (classifyHan(cp, 0, consumeNext) != CharClass::Other)
					b.set(cp);
			}
			for (const auto& entry : hanVoicedToZen())
				b.set(static_cast<size_t>(entry.first >> 32));
			return b;
		}();
		return bits;
	}

	template <class Access>
	void convertToHankaku(const std::string& bytes, std::vector<ConvertEdit>& edits, ConvertCategory category, const Access& access, PositionMapper& positions, const EditLimits& limits)
	{
		const auto& voiced = zenVoicedToHan();
		const auto& combiningVoiced = zenCombiningVoicedToHan();
		const auto& kana = zenKanaToHan();
		const CandidateBits& candidates = zenCandidates();
		EditRecorder<Access> recorder(edits, access, positions, bytes, limits);

		for (CodePointReader<Access> reader(bytes, access); !reader.atEnd(); reader.advance())
		{
			const DecodedChar cur = reader.current();
			const char32_t cp = cur.cp;

			// 候補でなければ、下のどの分岐にも当たらない
			if (cp >= kCandidateLimit || !candidates[cp])
				continue;

			if (reader.next().valid && categoryAllows(category, CharClass::Katakana))
			{
				const uint64_t key = (static_cast<uint64_t>(cp) << 32) | reader.next().cp;
				if (auto it = combiningVoiced.find(key); it != combiningVoiced.end())
				{
					const char32_t dst[2] = { it->second.first, it->second.second };
					recorder.convert(cur.begin, reader.next().end, std::u32string_view(dst, 2));
					reader.advance();
					continue;
				}
			}

			const CharClass cls = classifyZen(cp);
			if (!categoryAllows(category, cls))
				continue;

			if (cp == U'　')
			{
				recorder.convertOne(cur.begin, cur.end, U' ');
				continue;
			}

			if (cp >= 0xFF01 && cp <= 0xFF5E)
			{
				recorder.convertOne(cur.begin, cur.end, cp - 0xFEE0);
				continue;
			}

			if (const char32_t extra = toHankakuExtra(cp); extra != 0)
			{
				recorder.convertOne(cur.begin, cur.end, extra);
				continue;
			}

			if (auto it = voiced.find(cp); it != voiced.end())
			{
				const char32_t dst[2] = { it->second.first, it->second.second };
				recorder.convert(cur.begin, cur.end, std::u32string_view(dst, 2));
				continue;
			}

			if (auto it = kana.find(cp); it != kana.end())
				recorder.convertOne(cur.begin, cur.end, it->second);
		}
	}

	template <class Access>
	void convertToZenkaku(const std::string& bytes, std::vector<ConvertEdit>& edits, ConvertCategory category, const Access& access, PositionMapper& positions, const EditLimits& limits)
	{
		const auto& voiced = hanVoicedToZen();
		const auto& kana = hanKanaToZen();
		const CandidateBits& candidates = hanCandidates();
		EditRecorder<Access> recorder(edits, access, positions, bytes, limits);

		for (CodePointReader<Access> reader(bytes, access); !reader.atEnd(); reader.advance())
		{
			const DecodedChar cur = reader.current();
			const char32_t cp = cur.cp;

			// 候補でなければ、下のどの分岐にも当たらない
			if (cp >= kCandidateLimit || !candidates[cp])
				continue;

			const char32_t next = reader.next().valid ? reader.next().cp : 0;
			bool consumeNext = false;
			const CharClass cls = classifyHan(cp, next, consumeNext);

			if (!categoryAllows(category, cls))
				continue;

			if (consumeNext)
			{
				const uint64_t key = (static_cast<uint64_t>(cp) << 32) | next;
				const char32_t dst = voiced.at(key);
				recorder.convert(cur.begin, reader.next().end, std::u32string_view(&dst, 1));
				reader.advance();
				continue;
			}

			if (cp == U' ')
			{
				recorder.convertOne(cur.begin, cur.end, U'　');
				continue;
			}

			if (cp >= 0x21 && cp <= 0x7E)
			{
				recorder.convertOne(cur.begin, cur.end, cp + 0xFEE0);
				continue;
			}

			if (const char32_t extra = toZenkakuExtra(cp); extra != 0)
			{
				recorder.convertOne(cur.begin, cur.end, extra);
				continue;
			}

			if (auto it = kana.find(cp); it != kana.end())
				recorder.convertOne(cur.begin, cur.end, it->second);
		}
	}
}

namespace
{
	template <class Access>
	void convertWith(const std::string& bytes, std::vector<ConvertEdit>& edits, ConvertMode mode, ConvertCategory category, const Access& access, PositionMapper& positions, const EditLimits& limits)
	{
		if (mode == ConvertMode::ToHankaku)
			convertToHankaku(bytes, edits, category, access, positions, limits);
		else
			convertToZenkaku(bytes, edits, category, access, positions, limits);
	}
}

const TextCodec& utf8Codec()
{
	static const Utf8Codec codec;
	return codec;
}

const TextCodec& utf8CodecForcedVirtual()
{
	// utf8Codec() とは別の静的インスタンス。下の振り分けのアドレス比較に外れる
	static const Utf8Codec codec;
	return codec;
}

const TextCodec& utf8CodecLegacyEncode()
{
	static const Utf8CodecT<true> codec;
	return codec;
}

size_t nonBmpCandidateLeads()
{
	size_t count = 0;

	// 分類関数へ通す。変換表に足した規則も、分類関数や toHankakuExtra() へ
	// 直接書いた規則も、ここに出る。kInvalidByteBase (0x110000) は
	// 復号できなかったバイトの番兵なので、Unicode の上限で止める。
	for (char32_t cp = kCandidateLimit; cp <= 0x10FFFF; ++cp)
	{
		if (classifyZen(cp) != CharClass::Other)
			++count;

		bool consumeNext = false;
		if (classifyHan(cp, 0, consumeNext) != CharClass::Other)
			++count;
	}

	// 2 文字を 1 単位として扱う表は、キーに 2 つのコードポイントを詰めてある。
	// 上位が先頭の文字で、候補ビット表に載る側。下位（2 文字目）は載らないので
	// 数えない ← 非 BMP でも動くため。
	for (const auto& entry : zenCombiningVoicedToHan())
	{
		if (static_cast<char32_t>(entry.first >> 32) >= kCandidateLimit)
			++count;
	}
	for (const auto& entry : hanVoicedToZen())
	{
		if (static_cast<char32_t>(entry.first >> 32) >= kCandidateLimit)
			++count;
	}

	return count;
}

std::vector<ConvertEdit> convertZenHankakuEdits(const std::string& bytes, ConvertMode mode, ConvertCategory category, const TextCodec& codec, std::vector<size_t>* positions, const EditLimits& limits)
{
	std::vector<ConvertEdit> edits;
	PositionMapper mapper(positions);

	// UTF-8 は仮想呼び出しを介さない実装へ振り分ける。どちらの経路も結果は同じで、
	// 速度だけが違う。utf8Codec() は唯一の静的インスタンスなのでアドレスで判別できる。
	if (&codec == &utf8Codec())
		convertWith(bytes, edits, mode, category, Utf8Access(), mapper, limits);
	else
		convertWith(bytes, edits, mode, category, CodecAccess(codec), mapper, limits);

	mapper.finish();
	return edits;
}

std::vector<ConvertEdit> convertZenHankakuEditsEncode(const std::string& bytes, ConvertMode mode, ConvertCategory category, EncodeVariant variant, std::vector<size_t>* positions)
{
	std::vector<ConvertEdit> edits;
	PositionMapper mapper(positions);

	const EditLimits limits; // 8.2 の A/B は途中結合を挟まない条件で比べる
	if (variant == EncodeVariant::Legacy)
		convertWith(bytes, edits, mode, category, Utf8AccessT<true>(), mapper, limits);
	else
		convertWith(bytes, edits, mode, category, Utf8Access(), mapper, limits);

	mapper.finish();
	return edits;
}

namespace
{
	// 間隔が gap 以下の編集をまとめる。改行をまたぐ結合はしない。
	std::vector<ConvertEdit> mergeWithin(const std::vector<ConvertEdit>& edits, const std::string& source, size_t gap)
	{
		std::vector<ConvertEdit> out;
		for (const ConvertEdit& edit : edits)
		{
			if (!out.empty()
				&& edit.begin - out.back().end <= gap
				&& !crossesLineBreak(source, out.back().end, edit.begin))
			{
				ConvertEdit& last = out.back();
				last.text.append(source, last.end, edit.begin - last.end);
				last.text.append(edit.text);
				last.end = edit.end;
				continue;
			}
			out.push_back(edit);
		}
		return out;
	}
}

std::vector<ConvertEdit> collapseEditsPerLine(const std::vector<ConvertEdit>& edits, const std::string& source)
{
	// 間隔の上限を外すだけでよい。「改行をまたぐ結合はしない」規則がそのまま
	// 働くので、1 行の中の編集だけが 1 件にまとまる。
	return mergeWithin(edits, source, SIZE_MAX);
}

std::vector<ConvertEdit> collapseEditsToOne(const std::vector<ConvertEdit>& edits, const std::string& source)
{
	if (edits.empty())
		return {};

	ConvertEdit whole;
	whole.begin = edits.front().begin;
	whole.end = edits.back().end;

	size_t pos = whole.begin;
	for (const ConvertEdit& edit : edits)
	{
		whole.text.append(source, pos, edit.begin - pos);
		whole.text.append(edit.text);
		pos = edit.end;
	}

	return { whole };
}

std::vector<ConvertEdit> coalesceEdits(const std::vector<ConvertEdit>& edits, const std::string& source, size_t maxGap, size_t maxCount)
{
	std::vector<ConvertEdit> merged = mergeWithin(edits, source, maxGap);
	if (merged.empty() || merged.size() <= maxCount)
		return merged;

	// 行単位までまとめると何件になるかを、置換テキストを作らずに先に数える。
	// 多すぎるときに数十 MB の中間結果を抱え込まずに済む。
	size_t perLineCount = 0;
	for (size_t i = 0; i < merged.size(); ++i)
	{
		if (i == 0 || crossesLineBreak(source, merged[i - 1].end, merged[i].begin))
			++perLineCount;
	}

	// 行単位で収まるならそちらを採る。改行が消えないので、行に付いた
	// ブックマークやマーカーは残る。
	if (perLineCount <= maxCount)
		return collapseEditsPerLine(merged, source);

	// 行単位でも多すぎる。置換の回数を抑えるほうを取り、最初から最後までを
	// 1 件にする。この置換は改行をまたぐので、その範囲のマーカーは失われる。
	return collapseEditsToOne(merged, source);
}

std::vector<ConvertEdit> convertZenHankakuRanges(const std::vector<ConvertRange>& ranges,
	ConvertMode mode, ConvertCategory category, const TextCodec& codec,
	size_t maxGap, size_t maxTotalEditCount, std::vector<size_t>* positions)
{
	// 範囲ごとの結果。delta はその範囲でバイト数がどれだけ増減したか。
	struct RangeResult
	{
		std::vector<ConvertEdit> edits; // 範囲内オフセット
		std::vector<size_t> mapped;     // 範囲内オフセットの対応付け結果
		std::vector<size_t> slots;      // mapped[i] が positions の何番目か
		ptrdiff_t delta = 0;
		bool perLine = false;           // 既に行単位までまとめてあるか
	};

	std::vector<RangeResult> results(ranges.size());

	// 位置ごとの所属を先に確定させる。変換後に判定すると、対応付けで書き換えた値を
	// もう一度別の範囲の内側と誤認しうるため、必ず変換前に決めておく。
	struct SlotInfo
	{
		size_t range = SIZE_MAX; // 属する範囲。SIZE_MAX ならどの範囲にも入らない
		size_t before = 0;       // その位置より前で終わっている範囲の数
	};

	std::vector<SlotInfo> slots;
	if (positions)
	{
		slots.resize(positions->size());
		for (size_t slot = 0; slot < positions->size(); ++slot)
		{
			const size_t pos = (*positions)[slot];

			// **範囲は begin の昇順で重なっていないので、end も昇順。** そこで
			// 「pos 以上で終わる最初の範囲」を二分探索すれば、所属と「手前で
			// 終わった範囲の数」が同時に決まる。
			//
			// 線形に見る形だと位置数 × 範囲数になり、「すべて検索して選択」で
			// 数千個の選択を作ると 2N² 回になって UI が止まる。
			const auto found = std::lower_bound(ranges.begin(), ranges.end(), pos,
				[](const ConvertRange& range, size_t value)
				{
					return range.begin + range.bytes.size() < value;
				});

			SlotInfo info;
			// 手前で終わった範囲の数。pos 未満で終わる範囲がちょうどここまで並ぶ
			info.before = static_cast<size_t>(found - ranges.begin());

			// 境界（ある範囲の end と次の範囲の begin）はどちらで数えても同じ結果に
			// なる。二分探索は end 側で当てるので、線形版と同じく手前の範囲を選ぶ。
			if (found != ranges.end() && pos >= found->begin)
				info.range = info.before;

			slots[slot] = info;

			if (info.range != SIZE_MAX)
			{
				results[info.range].mapped.push_back(pos - ranges[info.range].begin);
				results[info.range].slots.push_back(slot);
			}
		}
	}

	// 範囲が 1 つなら上限もその場で適用できる。全文変換はこちらを通り、
	// 数百万件の編集に対して結合を 2 回走らせずに済む。
	const bool singleRange = (ranges.size() == 1);

	// 手前の upto 個の範囲を段階的にまとめる。件数が limit 以下になったところで
	// 止め、まとめ終わった合計を返す。閾値を超えたその場でも、最後にも同じものを使う。
	//
	// **upto で区切るのは、変換の途中で呼ぶときに未処理の範囲まで見ないため。**
	// 未処理の範囲は編集が 0 件なので結果は変わらないが、選択が刻みを大きく
	// 超える数あると「選択数 × まとめ直した回数」の空走りになる。
	auto squeezeAll = [&](size_t upto, size_t limit)
	{
		// まず行単位まで。改行をまたがないので行のマーカーは残る。
		size_t perLineTotal = 0;
		for (size_t i = 0; i < upto; ++i)
		{
			// 済んでいる範囲は通さない。もう一度通しても結果は同じだが、
			// 範囲全体ぶんの置換テキスト（大きい選択なら数十 MB）をまるごと
			// 作り直すことになる。1 件以下も同じ理由で外す。
			if (!results[i].perLine && results[i].edits.size() > 1)
			{
				results[i].edits = collapseEditsPerLine(results[i].edits, ranges[i].bytes);
				results[i].perLine = true;
			}
			perLineTotal += results[i].edits.size();
		}

		if (perLineTotal <= limit)
			return perLineTotal;

		// 行単位でも多すぎるなら、各範囲を 1 件へ潰す。潰れる範囲は選択の中に
		// 収まるが、選択が複数行にまたがっていればその行のマーカーは失われる。
		size_t oneTotal = 0;
		for (size_t i = 0; i < upto; ++i)
		{
			if (results[i].edits.size() > 1)
				results[i].edits = collapseEditsToOne(results[i].edits, ranges[i].bytes);
			oneTotal += results[i].edits.size();
		}
		return oneTotal;
	};

	// 抱えている編集の合計。上限の判定もこの値で行う。
	size_t totalEdits = 0;

	// **合計を範囲をまたいで見る。** 範囲ごとの途中結合だけでは、
	// 「各範囲は刻み未満だが合計は巨大」という形で抑えが効かない。
	// 64MB の `Ａ\n` を 2MB ずつ 32 個の選択に分けると、各範囲は約 52 万件で
	// 100 万件の刻みに届かず、行単位でも 1 行 1 件なので減らない。結果として
	// 1670 万件を全範囲ぶん抱えることになり、単一範囲で消したはずの
	// 1GB 級のピークがそのまま戻る。
	//
	// **上限を渡していない呼び出しでは何もしない。** 範囲ごとの途中結合と同じ扱いで、
	// 上限を外した計測用ビルド（`NPPZH_MAX_EDIT_COUNT` を最大値にしたもの）では
	// 「全件そのまま見せる」という意図を崩さない。
	size_t nextSqueeze = maxTotalEditCount == SIZE_MAX ? SIZE_MAX : kEditCompactThreshold;

	for (size_t i = 0; i < ranges.size(); ++i)
	{
		RangeResult& result = results[i];

		// **上限を変換の途中でも効かせる。** 渡さないと、変換対象と非対象が
		// 1 文字ずつ交互に並ぶ文書で、まとめる前の編集が数千万件になる。
		const EditLimits limits{ maxGap, maxTotalEditCount };
		result.edits = convertZenHankakuEdits(ranges[i].bytes, mode, category, codec, &result.mapped, limits);
		result.edits = coalesceEdits(result.edits, ranges[i].bytes, maxGap,
			singleRange ? maxTotalEditCount : SIZE_MAX);
		totalEdits += result.edits.size();

		if (singleRange)
			continue;

		// この範囲だけで上限を超えているなら、合計も必ず超える。ここで行単位まで
		// 潰しておけば、潰す前の数百万件を範囲の数だけ同時に抱え込まずに済む。
		if (result.edits.size() > maxTotalEditCount)
		{
			totalEdits -= result.edits.size();
			result.edits = collapseEditsPerLine(result.edits, ranges[i].bytes);
			result.perLine = true;
			totalEdits += result.edits.size();
		}

		if (totalEdits <= nextSqueeze)
			continue;

		// 合計が刻みを超えた。**最後にやるのと同じまとめ方を先に適用する。**
		// 抑えたいのはメモリなので、ここでの目標は上限ではなく刻みのほう。
		// 行単位で刻みに収まるなら、そこで止まって行のマーカーが残る。
		totalEdits = squeezeAll(i + 1, kEditCompactThreshold);

		// **減らなくても次の閾値は上げる。** 全範囲が 1 件まで潰れていれば
		// それ以上減らせないので（選択が 100 万個ある場合など）、据え置くと
		// 範囲ごとに全範囲を走査することになり O(選択数²) になる。
		// 上げておけば、抱える量は「下限 + 刻み」で収まる。
		nextSqueeze = totalEdits > SIZE_MAX - kEditCompactThreshold
			? SIZE_MAX
			: totalEdits + kEditCompactThreshold;
	}

	// 合計が多すぎるときは、残りの範囲もまとめる。単一範囲なら coalesceEdits が
	// 既に段階を踏んでいるので、ここには入らない。
	if (!singleRange && totalEdits > maxTotalEditCount)
		squeezeAll(ranges.size(), maxTotalEditCount);

	// 潰した後の件数。reserve に潰す前の件数を使うと、数百万件を 1 件へ
	// 潰した後でも巨大な容量を確保してしまう。
	size_t finalCount = 0;
	for (RangeResult& result : results)
	{
		finalCount += result.edits.size();
		for (const ConvertEdit& edit : result.edits)
			result.delta += static_cast<ptrdiff_t>(edit.text.size()) - static_cast<ptrdiff_t>(edit.end - edit.begin);
	}

	std::vector<ConvertEdit> all;
	all.reserve(finalCount);
	for (size_t i = 0; i < ranges.size(); ++i)
	{
		for (ConvertEdit& edit : results[i].edits)
		{
			edit.begin += ranges[i].begin;
			edit.end += ranges[i].begin;
			all.push_back(std::move(edit));
		}
	}

	if (!positions)
		return all;

	// 手前の範囲がもたらした増減の累計。範囲の外にある位置はこれだけずらす。
	std::vector<ptrdiff_t> shiftBefore(ranges.size() + 1, 0);
	for (size_t i = 0; i < ranges.size(); ++i)
		shiftBefore[i + 1] = shiftBefore[i] + results[i].delta;

	// 範囲の外にある位置を先に処理する。内側の位置を書き換える前に済ませておけば、
	// どちらのループも元の値だけを見ることになる。
	for (size_t slot = 0; slot < positions->size(); ++slot)
	{
		if (slots[slot].range != SIZE_MAX)
			continue;
		(*positions)[slot] =
			static_cast<size_t>(static_cast<ptrdiff_t>((*positions)[slot]) + shiftBefore[slots[slot].before]);
	}

	for (size_t i = 0; i < ranges.size(); ++i)
	{
		const RangeResult& result = results[i];
		for (size_t k = 0; k < result.slots.size(); ++k)
		{
			(*positions)[result.slots[k]] =
				ranges[i].begin + static_cast<size_t>(shiftBefore[i] + static_cast<ptrdiff_t>(result.mapped[k]));
		}
	}

	return all;
}

std::string convertZenHankaku(const std::string& bytes, ConvertMode mode, ConvertCategory category, const TextCodec& codec)
{
	const std::vector<ConvertEdit> edits = convertZenHankakuEdits(bytes, mode, category, codec);

	std::string out;
	out.reserve(bytes.size() + bytes.size() / 2);

	// 未変更部分は元バイト列からそのままコピーする。復号できなかったバイトも、
	// 出力コードページに複数の表現がある文字も、これで元のバイトのまま残る。
	size_t pos = 0;
	for (const ConvertEdit& edit : edits)
	{
		out.append(bytes, pos, edit.begin - pos);
		out.append(edit.text);
		pos = edit.end;
	}
	out.append(bytes, pos, bytes.size() - pos);

	return out;
}
