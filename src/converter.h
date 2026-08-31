#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

enum class ConvertMode
{
	ToHankaku, // 全角 → 半角
	ToZenkaku  // 半角 → 全角
};

enum class ConvertCategory
{
	All,      // すべて
	AlphaNum, // 英数字
	Katakana, // カタカナ（句読点・長音・濁点含む）
	Symbol,   // 記号
	Space     // スペース
};

// 復号できなかったバイトを退避するコードポイントの基点。
// Unicode の上限 0x10FFFF を超えるので、実在の文字と衝突しない。
// 変換表のどのキーにも一致しないため、そのバイトは変換対象から外れ、元のまま残る。
inline constexpr char32_t kInvalidByteBase = 0x110000;

// 文書のバイト列とコードポイントを相互変換する。
// これを差し替えることで、変換ロジックを文書の文字コードから切り離している。
// ConvertEdit のオフセットも、位置追跡に渡す位置も、すべてこのコーデックの
// バイト空間で表される。つまり UTF-8 文書なら UTF-8 バイト、ANSI 文書なら
// そのコードページのバイトが、そのまま文書上のバイト位置になる。
class TextCodec
{
public:
	virtual ~TextCodec() = default;

	// bytes[pos] から 1 文字を復号し、消費したバイト数を返す。
	// 呼び出し側は pos < bytes.size() を保証する。戻り値は必ず 1 以上にすること。
	// 復号できないバイトは kInvalidByteBase + そのバイト値 を out へ入れて 1 を返す。
	virtual size_t decode(std::string_view bytes, size_t pos, char32_t& out) const = 0;

	// text をこのコーデックで符号化して out の末尾へ追記する。
	// 1 文字でも書けない場合は out を一切変更せずに false を返すこと。
	// この「全部書けるか、何も書かないか」が変換単位ごとの原子的フォールバックそのもので、
	// ｶ は書けるが ﾞ が書けない、という状況で ｶ だけ出力する事故を防いでいる。
	virtual bool encode(std::u32string_view text, std::string& out) const = 0;
};

// UTF-8 用のコーデック。encode は番兵（kInvalidByteBase 以上）と単独サロゲート
// （U+D800〜U+DFFF）を弾き、それ以外は成功する。decode 側もサロゲートを表す
// バイト列を不正として扱うので、可否がそろっている。
const TextCodec& utf8Codec();

// テストとベンチ用。utf8Codec() と**同じ実装の別インスタンス**。
//
// 変換の入口は「アドレスが utf8Codec() と同じか」で非仮想経路へ振り分けるので、
// こちらを渡すと同じ処理が仮想経路（TextCodec 経由）へ落ちる。実装を二重に
// 書かずに、抽象化の代償だけを差として測れる。
const TextCodec& utf8CodecForcedVirtual();

// bytes を変換する。category 以外の文字はそのまま残す。
// codec が表現できない変換は、変換表 1 エントリ分をまとめて元の文字へ差し戻す
// （例: ガ → ｶﾞ で ﾞ が書けないなら ｶ も出さずに ガ のまま残す）。
std::string convertZenHankaku(const std::string& bytes,
	ConvertMode mode,
	ConvertCategory category = ConvertCategory::All,
	const TextCodec& codec = utf8Codec());

// 入力に対する置換 1 件。begin / end は codec のバイト空間での半開区間。
struct ConvertEdit
{
	size_t begin = 0;
	size_t end = 0;
	std::string text;
};

// 変換の途中で編集リストをまとめるまでに溜める件数。1 件 48 バイト前後なので、
// 100 万件で 50MB 弱。まとめる前の編集は「maxCount + この件数」までしか
// 同時に存在しない。
constexpr size_t kEditCompactThreshold = 1u << 20;

// 編集をまとめる条件。coalesceEdits() の引数と同じ意味で、変換の途中で
// 件数が膨らんだときにも同じ条件でまとめる。
struct EditLimits
{
	size_t maxGap = 0;          // この間隔以下なら 1 件へまとめる（改行はまたがない）
	size_t maxCount = SIZE_MAX; // 件数の上限。超えたら行単位、それでも超えたら 1 件
	size_t compactEvery = kEditCompactThreshold; // 途中でまとめるまでに溜める件数

	// maxCount が SIZE_MAX のときは compactEvery を無視して途中結合をしない。
	// 守るべき件数が無いので、まとめても減らないため。
	//
	// maxCount と compactEvery の大小は問わない。結合は上限以下の件数で呼ぶと
	// 1 件も減らないので、まとめ直す件数は必ず上限より大きくとる。抱える件数が
	// 「maxCount + compactEvery」までに収まるのはどちらの大小でも変わらない
};

// convertZenHankaku と同じ変換を行い、変わった範囲だけを返す。
// 変換が 1 件も起きなければ空になる。隣接する変換単位はまとめられる。
//
// positions を渡すと、そこに入れた入力側のバイト位置を変換後のバイト位置へ書き換える。
// まとめる前の変換単位で対応付けるので、連続した変換にまたがっても位置がずれない。
// 変換単位の内側にある位置は、その単位の末尾へ寄せる。
// 位置ごとに全単位を見るため、渡すのはキャレットなど少数の位置に限ること。
//
// **limits.maxCount を渡すと、変換の途中でも件数がその上限を超えないようにする。**
// 変換対象と非対象が 1 文字ずつ交互に並ぶ文書では、まとめる前の編集が単位ごとに
// 1 件になり、64MB の入力で数千万件（1〜3GB）に達する。既定の SIZE_MAX では
// 途中結合をしないので、上限を効かせたい呼び出しでは必ず渡すこと。
std::vector<ConvertEdit> convertZenHankakuEdits(const std::string& bytes,
	ConvertMode mode,
	ConvertCategory category = ConvertCategory::All,
	const TextCodec& codec = utf8Codec(),
	std::vector<size_t>* positions = nullptr,
	const EditLimits& limits = {});

// 変換する範囲 1 つ。複数選択の 1 つの選択に対応する。
struct ConvertRange
{
	size_t begin = 0;   // 文書内のバイト位置
	std::string bytes;  // その範囲のバイト列（長さがそのまま範囲の長さ）
};

// 複数の範囲をまとめて変換する。範囲は begin の昇順で、互いに重なっていないこと。
//
// 返す ConvertEdit の begin / end も、positions の入出力も、すべて「変換前の文書の
// バイト位置」で表す。編集は昇順に並ぶので、後ろから適用すれば前の位置がずれない。
//
// 位置の対応付けは範囲の内外を区別して行う。範囲の中にある位置はその範囲の変換に
// 沿って動かし、範囲の外にある位置は手前の範囲で増減したバイト数だけずらす。
// これが無いと、先の選択で長さが変わった分だけ後の選択の位置がずれる。
//
// 結合は範囲ごとに行い、範囲をまたいでは結合しない。結合後の合計が maxTotalEditCount を
// 超える場合は、まず全範囲を行単位まで、それでも超える場合は各範囲をそれぞれ 1 件へ
// 潰す（範囲の数までは減らない）。
//
// **合計の判定に使うのは「まとめた後の件数」。** メモリを抑えるために、抱えている
// 合計が kEditCompactThreshold 件を超えた時点で、最後と同じまとめ方をその場で適用する。
// 先にまとまった範囲はそこで数件へ落ちるので、その後の maxTotalEditCount の判定では
// 落ちた後の件数で数える。つまり**先に潰れた選択が、他の選択を巻き込むことはない**。
// 数百万件を抱える選択と 2 件の選択が並んでいる場合、前者は 1 件へ潰れ、後者は 2 件の
// まま残る（後者の行のマーカーは失われない）。
//
// 位置の所属判定は、範囲が昇順で重なっていない性質を使った二分探索で O(位置数 log 範囲数)。
// 「すべて検索して選択」で数千個の選択が来ても止まらない。
std::vector<ConvertEdit> convertZenHankakuRanges(const std::vector<ConvertRange>& ranges,
	ConvertMode mode,
	ConvertCategory category,
	const TextCodec& codec,
	size_t maxGap,
	size_t maxTotalEditCount,
	std::vector<size_t>* positions = nullptr);

// 1 行の中の編集をすべて 1 件へまとめる。改行はまたがないので、行に付いた
// ブックマークやマーカーは残る。改行の無い 1 行だけの文書では 1 件になる。
std::vector<ConvertEdit> collapseEditsPerLine(const std::vector<ConvertEdit>& edits,
	const std::string& source);

// 最初から最後までを 1 件へまとめる。改行をまたぐので、その範囲の行に付いた
// ブックマークやマーカーは失われる。置換の回数を最小にしたいときの最後の手段。
std::vector<ConvertEdit> collapseEditsToOne(const std::vector<ConvertEdit>& edits,
	const std::string& source);

// 編集の件数を抑えるためにまとめる。段階は 3 つで、上から順に試して
// maxCount 以下に収まったところで採用する。
//   1. 間隔が maxGap バイト以下のものを結合する（改行はまたがない）
//   2. 行単位までまとめる（改行はまたがないので行のマーカーは残る）
//   3. 全体を 1 件にする（改行をまたぐので、その範囲のマーカーは失われる）
// 段階 3 に落ちるのは、行単位にしても maxCount を超える場合だけ。**maxCount は
// 「これを超えたら次の段階へ移る」しきい値であって、返る件数の上限ではない。**
// 段階 2 の結果が maxCount 以下であればそのまま返るので、返る件数は maxCount 以下。
// 改行をまたぐ結合はしない（maxGap によらず）。変換していない行を置換範囲へ巻き込まないため。
// 改行の判定はバイト検索だが、Windows の 2 バイトコードページ（932/936/949/950/1361）は
// いずれもトレイルバイトに 0x0A / 0x0D を含まないので、ANSI バイト列でも誤検出しない。
// 結合で挟まれた未変更部分は source からバイト単位でそのまま持ち越すので、内容は変化しない。
std::vector<ConvertEdit> coalesceEdits(const std::vector<ConvertEdit>& edits,
	const std::string& source,
	size_t maxGap,
	size_t maxCount);

// **8.2 の前の形をテストから呼ぶための入口。製品コードからは呼んでいない。**
//
// 8.2 では encode の検査を 1 パスにし、隣接する単位を編集の置換テキストへ直接
// 書くようにした。この改善は**同じバイナリの中で交互に走らせないと確かめられない**
// （バイナリをまたぐと、テンプレートの実体化が増えるだけで絶対時間が動く）。
// 採用した形が本当に速いことを後から確かめられるように、比較対象を残してある。
//
// convertZenHankakuEdits() と結果は完全に一致する（単体テストで固定）。
enum class EncodeVariant
{
	Product, // 1 パス + 置換テキストへ直接書く（製品と同じ）
	Legacy,  // 2 パス検査 + 中間バッファからコピー（8.2 より前）
};

std::vector<ConvertEdit> convertZenHankakuEditsEncode(const std::string& bytes,
	ConvertMode mode,
	ConvertCategory category,
	EncodeVariant variant,
	std::vector<size_t>* positions = nullptr);

// 8.2 の前の 2 パスの `encode` を持つコーデック。`decode` は製品と同じ。
// **`encode` 単体の新旧を同じバイナリの中で比べるためにある。**
// 別バイナリで測った値と引き算してはいけない（第 10 章）。
const TextCodec& utf8CodecLegacyEncode();

// テスト用。BMP (U+0000〜U+FFFF) の外に「変換単位の先頭になりうる文字」がいくつ
// あるかを返す。
//
// **0 でなければ、その規則は変換されない。** ホットループは候補ビット表で当たらない
// 文字を切り捨てており（converter.cpp 4.6 節）、その表は BMP 分しか持っていない。
// 非 BMP の規則を足すと表に載らず、静かに無効化される。足すなら表の持ち方から
// 変えること。
//
// **数えるのは先頭の文字だけ。** 2 文字を 1 単位として扱う規則（濁点付きカナ）の
// 2 文字目は、先頭が候補と分かった後に直接比べるので表を通らない。そちらは非 BMP
// でも動くため、違反にしてはいけない。
//
// 変換表のキーを列挙するのではなく、**U+10000〜U+10FFFF を分類関数へ通して**
// 数える。表に足さず分類関数や toHankakuExtra() へ直接書いた規則も拾うため。
size_t nonBmpCandidateLeads();
