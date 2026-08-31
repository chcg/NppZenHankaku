#pragma once

#include "converter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 1 文字が 1〜2 バイトの Windows コードページ専用のコーデック。
//
// 対象は Scintilla が ANSI 文書として扱う 932 / 936 / 949 / 950 / 1361 と、
// 1252 などの単バイトコードページ。「リードバイトなら 2 バイト、そうでなければ 1 バイト」
// という前提で文字境界を決めているため、GB18030 (54936) のように 1 文字が
// 3 バイト以上になるコードページには対応しない。その場合 create() が nullptr を返すので、
// 呼び出し側で明示的に拒否すること。UTF-8 (65001) もここでは扱わない（utf8Codec() を使う）。
//
// 復号・符号化の結果をスレッド同期なしでキャッシュしている。Notepad++ の
// プラグインコマンドは UI スレッドからしか呼ばれないため、これで足りる。
class DbcsCodec final : public TextCodec
{
public:
	// codePage は resolveCodePage() 済みの実効値であること。
	// 対応できないコードページでは nullptr を返す。
	static std::unique_ptr<DbcsCodec> create(int codePage);

	size_t decode(std::string_view bytes, size_t pos, char32_t& out) const override;
	bool encode(std::u32string_view text, std::string& out) const override;

private:
	explicit DbcsCodec(int codePage);

	int32_t decoded(std::vector<int32_t>& cache, size_t key, const char* seq, int seqLen) const;
	int32_t decodeUncached(const char* seq, int seqLen) const;
	const std::string* encoded(char32_t cp) const;
	std::string encodeUncached(char32_t cp) const;

	int codePage_;
	bool leadByte_[256] = {};
	// 1 バイト列 / 2 バイト列からコードポイントへの遅延キャッシュ。
	// DBCS は 1 文字が最大 2 バイトなので、あり得るバイト列は高々 256 + 65536 通りしかない。
	// これで MultiByteToWideChar の呼び出し回数が、文字数ではなくバイト列の種類数で頭打ちになる。
	mutable std::vector<int32_t> single_;
	mutable std::vector<int32_t> dbcs_; // 単バイトコードページでは空
	mutable std::unordered_map<char32_t, std::string> encodeCache_;
	mutable std::string scratch_;
};
