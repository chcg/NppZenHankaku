#include "DbcsCodec.h"

#include <windows.h>

namespace
{
	// キャッシュの状態。コードポイントは 0 以上なので負値と衝突しない。
	constexpr int32_t kUnfilled = -1;
	constexpr int32_t kUndecodable = -2;

	bool isHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
	bool isLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
	bool isSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDFFF; }
}

std::unique_ptr<DbcsCodec> DbcsCodec::create(int codePage)
{
	CPINFO info = {};
	if (!::GetCPInfo(static_cast<UINT>(codePage), &info))
		return nullptr;

	// 1 文字が 3 バイト以上になるコードページはリードバイト方式で境界を決められない
	if (info.MaxCharSize < 1 || info.MaxCharSize > 2)
		return nullptr;

	std::unique_ptr<DbcsCodec> codec(new DbcsCodec(codePage));

	// LeadByte は「下限, 上限」のペアが並び、0, 0 で終端する
	for (int i = 0; i + 1 < MAX_LEADBYTES && (info.LeadByte[i] != 0 || info.LeadByte[i + 1] != 0); i += 2)
	{
		for (unsigned b = info.LeadByte[i]; b <= info.LeadByte[i + 1]; ++b)
			codec->leadByte_[b] = true;
	}

	if (info.MaxCharSize == 2)
		codec->dbcs_.assign(0x10000, kUnfilled);

	return codec;
}

DbcsCodec::DbcsCodec(int codePage)
	: codePage_(codePage)
	, single_(0x100, kUnfilled)
{
}

size_t DbcsCodec::decode(std::string_view bytes, size_t pos, char32_t& out) const
{
	const unsigned char b0 = static_cast<unsigned char>(bytes[pos]);

	if (!dbcs_.empty() && leadByte_[b0] && pos + 1 < bytes.size())
	{
		const unsigned char b1 = static_cast<unsigned char>(bytes[pos + 1]);
		const int32_t cp = decoded(dbcs_, (static_cast<size_t>(b0) << 8) | b1, bytes.data() + pos, 2);
		if (cp >= 0)
		{
			out = static_cast<char32_t>(cp);
			return 2;
		}
	}

	const int32_t cp = decoded(single_, b0, bytes.data() + pos, 1);
	if (cp >= 0)
	{
		out = static_cast<char32_t>(cp);
		return 1;
	}

	// 孤立したリードバイト、末尾のリードバイト、不正なトレイルバイトはここへ来る。
	// 1 バイトだけ番兵にして先へ進めるので、そのバイトは未変更のまま残り、
	// 後続のバイトは改めて文字境界から判定される。
	out = kInvalidByteBase + b0;
	return 1;
}

int32_t DbcsCodec::decoded(std::vector<int32_t>& cache, size_t key, const char* seq, int seqLen) const
{
	int32_t& slot = cache[key];
	if (slot == kUnfilled)
		slot = decodeUncached(seq, seqLen);
	return slot;
}

int32_t DbcsCodec::decodeUncached(const char* seq, int seqLen) const
{
	wchar_t wide[2] = {};
	// MB_ERR_INVALID_CHARS を付けることで、リードバイト表だけに頼らず
	// 「実際に復号できるバイト列か」で判定する。不完全な DBCS 列はここで落ちる。
	const int len = ::MultiByteToWideChar(
		static_cast<UINT>(codePage_),
		MB_ERR_INVALID_CHARS,
		seq,
		seqLen,
		wide,
		2);

	if (len == 1 && !isSurrogate(wide[0]))
		return static_cast<int32_t>(wide[0]);

	if (len == 2 && isHighSurrogate(wide[0]) && isLowSurrogate(wide[1]))
		return static_cast<int32_t>(0x10000 + ((wide[0] - 0xD800) << 10) + (wide[1] - 0xDC00));

	// 1 文字が複数のコードポイントへ写るコードページは想定していない。
	// 変換対象から外し、元バイトのまま残す方が安全。
	return kUndecodable;
}

bool DbcsCodec::encode(std::u32string_view text, std::string& out) const
{
	// 1 文字でも書けなければ out に触れないよう、いったん手元へ組み立てる
	scratch_.clear();
	for (const char32_t cp : text)
	{
		const std::string* bytes = encoded(cp);
		if (!bytes)
			return false;
		scratch_.append(*bytes);
	}

	out.append(scratch_);
	return true;
}

const std::string* DbcsCodec::encoded(char32_t cp) const
{
	// 復号できなかったバイトの番兵。変換表の出力には現れないが、書き戻せないので弾く。
	if (cp > 0x10FFFF)
		return nullptr;

	auto found = encodeCache_.find(cp);
	if (found == encodeCache_.end())
		found = encodeCache_.emplace(cp, encodeUncached(cp)).first;

	// 空文字列は「このコードページでは書けない」を表す
	return found->second.empty() ? nullptr : &found->second;
}

std::string DbcsCodec::encodeUncached(char32_t cp) const
{
	wchar_t wide[2] = {};
	int wideLen = 0;
	if (cp <= 0xFFFF)
	{
		if (isSurrogate(static_cast<wchar_t>(cp)))
			return {};
		wide[0] = static_cast<wchar_t>(cp);
		wideLen = 1;
	}
	else
	{
		const char32_t v = cp - 0x10000;
		wide[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
		wide[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
		wideLen = 2;
	}

	// WC_NO_BEST_FIT_CHARS と usedDefaultChar の両方を見る。片方でも欠かすと、
	// 書けない文字が黙って '?' や見た目の似た別の文字へ置き換わる。
	// 1 文字分なので、どのコードページでもこのバッファに収まる。
	char bytes[8] = {};
	BOOL usedDefaultChar = FALSE;
	const int len = ::WideCharToMultiByte(
		static_cast<UINT>(codePage_),
		WC_NO_BEST_FIT_CHARS,
		wide,
		wideLen,
		bytes,
		static_cast<int>(sizeof(bytes)),
		nullptr,
		&usedDefaultChar);
	if (len <= 0 || usedDefaultChar)
		return {};

	return std::string(bytes, static_cast<size_t>(len));
}
