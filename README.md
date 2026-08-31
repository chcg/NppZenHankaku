# NppZenHankaku

[日本語](#日本語) | [English](#english)

`Notepad++` plugin for converting between full-width (zenkaku) and half-width (hankaku) characters.  
`Notepad++` 用の全角／半角変換プラグインです。

---

## 日本語

### 概要

選択範囲（未選択時は文書全体）の文字を、全角↔半角に変換します。  
英数字・カタカナ・記号・スペースを個別に変換することもできます。

### 機能

- 全角→半角／半角→全角
- 対象カテゴリ
  - すべて
  - 英数字
  - カタカナ（句読点・長音・濁点などを含む）  
    `ガ`↔`ｶﾞ` のほか、`ヷ`↔`ﾜﾞ`、`ヺ`↔`ｦﾞ`、合成用濁点（`カ` + U+3099）にも対応します。  
    `ヸ` `ヹ` は基底の `ヰ` `ヱ` に半角がないため変換しません。
  - 記号  
    対象は `！`〜`～`（U+FF01〜U+FF5E）と `￠￡￢￣￤￥￦`（U+FFE0〜U+FFE6）で、それぞれ ASCII の `!`〜`~` と `¢£¬¯¦¥₩` に対応します。  
    Unicode の「Halfwidth and Fullwidth Forms」のうち、`｟` `｠`（U+FF5F/U+FF60）と `￨`〜`￮`（U+FFE8〜U+FFEE）は対象外です。相手側が `⦅` `⦆` や `│` `←` `■` `○` といった ASCII 外の記号になり、半角文字として扱えないためです。
  - スペース
- ツールバーボタン（全角→半角／半角→全角）
- メニュー: プラグイン → 全角半角変換
- 複数選択と矩形選択に対応します。選択した箇所だけを変換し、各選択の向きと並びを保ちます。
- UTF-8 / ANSI とも、変換した箇所だけを置き換え、キャレット・選択範囲・スクロール位置を保ちます。
- 変換しなかった部分はバイト単位でそのまま残します。
- 1 回の変換上限: 64 MB

### 動作環境

- `Notepad++` 64-bit（Windows）
- エンコーディング: UTF-8 と、1 文字が 1〜2 バイトのコードページ（CP932 などの ANSI）に対応します。  
  エンコードが ANSI の文書では、変換後の文字がそのコードページで表現できない場合、その文字だけ変換せず元のまま残します。  
  例: 日本語環境（CP932）の ANSI 文書で `ＡＢＣ￥` は `ABC￥` になります（半角の `¥` が CP932 に無いため `￥` だけ据え置き）。  
  変換しなかった文字は復号も再符号化もされないため、CP932 の NEC/IBM 重複マッピングのように同じ文字を表すバイト列が複数ある場合でも、元のバイト列がそのまま保たれます。  
  文書の文字コードとして解釈できないバイトも変換せずそのまま残し、その前後の変換は通常どおり行います。  
  UTF-8 の文書と、エンコード → 文字セットで指定した文書（Shift-JIS 等）は、`Notepad++` が内部的に UTF-8 で編集する仕様のため据え置きは行われず、`￥` は `¥` に変換されます。文字セット指定の文書では保存時に `Notepad++` 側が元の文字コードへ変換するため、変換結果がその文字セットに存在しない場合は代替文字へ置き換わることがあります。バイト単位での保持が必要な場合は、UTF-8 または ANSI の文書で事前に確認してください。

### インストール

1. [Releases](https://github.com/RivieraSystems/NppZenHankaku/releases) から `NppZenHankaku.dll`（または zip）を入手します。  
   （`build.bat` でビルドした場合は `bin\x64\Release\NppZenHankaku.dll`）
2. 次のフォルダを作成し、DLL を配置します。  
   `<Notepad++のインストールフォルダ>\plugins\NppZenHankaku\NppZenHankaku.dll`  
   例: `C:\Program Files\Notepad++\plugins\NppZenHankaku\NppZenHankaku.dll`
3. `Notepad++` を再起動します。

> フォルダ名と DLL 名は一致させてください。`Notepad++` は `plugins\<DLL名>\<DLL名>.dll` の形でのみプラグインを読み込みます。

### 使い方

1. 変換したい文字列を選択します（Ctrl を押しながら選ぶと複数選択。Alt を押しながらドラッグすると矩形選択。選択しない場合は文書全体）。
2. メニューから プラグイン → 全角半角変換 を選びます。  
   またはツールバーのボタンを使います。

> 複数選択・矩形選択で実行すると、選択した箇所だけがまとめて変換されます。Undo は一回で全部戻ります。幅 0 の矩形選択（まっすぐ下へドラッグした状態）では何も起きません。

### 既知の制限

- 矩形選択は変換で文字幅が変わると、元と同じ列には戻りません。矩形の上下 2 つの端点はそれぞれの文字に付いて動きますが、その間の行の範囲は新しい 2 点から矩形として引き直されます。そのため、間の行では選ばれる文字が元と変わることがあります（変換されなかった行でも起こります）。
- 選択範囲が重なっている場合には対応していません（メッセージを表示して何もしません）。
- 空のキャレットが複数ある状態で実行すると、何も起きません（文書全体の変換にはなりません）。
- GB18030 のように 1 文字が 3 バイト以上になるコードページの ANSI 文書には対応していません（メッセージを表示して何もしません）。
- 変換箇所が合計 4096 件を超える場合は、まず 1 行ぶんをまとめて 1 回の置換にします。改行をまたがないので、ブックマークやマーカーはそのまま残ります。ただし置換がその行の最初の変換箇所から最後の変換箇所までになるので、変更履歴（Change History）の帯や下線（インジケータ）は、その間の変わっていない部分にも広がります。それでも 4096 件を超える場合（変換対象のある行が 4096 行を超える場合）だけ、選択ごとに最初の変換箇所から最後の変換箇所までを 1 回の置換にまとめ、その範囲のブックマークやマーカーは失われます。件数は文書の大きさではなく変換対象の散らばり方で決まります（連続した変換箇所は 1 件として数えます）。
- 変換箇所が数百万件になる文書では、メモリを抑えるために、変換の途中で上と同じまとめ方を先に適用します。先にまとまった選択はそこで数件まで減るので、そのあとの 4096 件の判定には減ったあとの件数で入ります。数百万件を抱えた選択が、他の選択のブックマークやマーカーまで巻き込むことはありません。
- 折り返し表示では、キャレットと文書行は維持しますが、同じ行の何番目の折り返し行を表示していたかまでは復元しません。

### ビルド方法2つ

ビルドには Visual Studio の「`C++` によるデスクトップ開発」が必要です。  
Visual Studio インストール時に「`C++` によるデスクトップ開発」を選択してください。

- batファイル
1. `build.bat` を実行します。

- Visual Studio
1. `NppZenHankaku.sln` を開きます。
2. ツールバーで Release と x64 を選びます。
3. メニューから ビルド → ソリューションのビルド を実行します。

いずれも出力は `bin\x64\Release\NppZenHankaku.dll` です。

### 公開ファイル

GitHub に載せるファイルは [PUBLIC_FILES](PUBLIC_FILES) が正本です。  
`build.bat` は追跡中のファイルをこの一覧と突き合わせ、余分があれば失敗します。

### ライセンス

[MIT License](LICENSE) です。Copyright (c) 2026 RivieraSystems  
著作権者は `LICENSE` と `NppZenHankaku.rc` の `ZH_COMPANY` / `ZH_COPYRIGHT` の 3 箇所に書いています。  
DLL のバージョンは `NppZenHankaku.rc` 冒頭の `ZH_VER_*` で定義しています。リリースのたびに更新してください。

---

## English

### Overview

Convert selected text (or the whole document if nothing is selected) between full-width and half-width Japanese/ASCII-related characters.  
You can limit conversion to alphanumerics, katakana, symbols, or spaces.

### Features

- Full-width→half-width／half-width→full-width
- Categories:
  - All
  - Alphanumeric
  - Katakana (including related punctuation, prolonged sound mark, dakuten, etc.)  
    Besides `ガ`↔`ｶﾞ`, this covers `ヷ`↔`ﾜﾞ`, `ヺ`↔`ｦﾞ` and combining dakuten (`カ` + U+3099).  
    `ヸ` and `ヹ` are left unchanged, since their base letters `ヰ` and `ヱ` have no half-width form.
  - Symbols  
    This covers `！` to `～` (U+FF01–U+FF5E) and `￠￡￢￣￤￥￦` (U+FFE0–U+FFE6), which map to ASCII `!` to `~` and to `¢£¬¯¦¥₩`.  
    Within Unicode's Halfwidth and Fullwidth Forms block, `｟` `｠` (U+FF5F/U+FF60) and `￨` to `￮` (U+FFE8–U+FFEE) are not converted: their counterparts are non-ASCII symbols such as `⦅` `⦆`, `│`, `←`, `■` and `○`, which are not half-width characters in any useful sense.
  - Spaces
- Toolbar buttons (to half-width / to full-width)
- Menu: Plugins → 全角半角変換
- Supports multiple and rectangular selections. Only the selected spans are converted, and the direction and order of every selection is preserved
- In both UTF-8 and ANSI documents only the converted spans are replaced, keeping the caret, the selection and the scroll position
- Everything that is not converted is preserved byte for byte
- Maximum conversion size per operation: 64 MB

### Requirements

- `Notepad++` 64-bit (Windows)
- Encodings: UTF-8, and code pages where one character is one or two bytes (ANSI such as CP932)  
  In documents encoded as ANSI, a character whose converted form cannot be represented by the code page is left unchanged, individually.  
  Example: in an ANSI document on a Japanese system (CP932), `ＡＢＣ￥` becomes `ABC￥`, because half-width `¥` does not exist in CP932.  
  Characters that are not converted are never decoded or re-encoded, so even where a code page has several byte sequences for the same character (such as the NEC/IBM duplicate mappings in CP932), the original bytes are kept.  
  Bytes that cannot be interpreted in the document's code page are also left as they are, and conversion continues normally around them.  
  UTF-8 documents and documents using Encoding → Character sets (Shift-JIS etc.) are edited internally as UTF-8 by `Notepad++`, so no character is held back there and `￥` becomes `¥`. For character-set documents `Notepad++` converts back to the original encoding on save, so a converted character that does not exist in that character set may be replaced with a substitute. If exact bytes matter, verify in a UTF-8 or ANSI document first.

### Installation

1. Get `NppZenHankaku.dll` from [Releases](https://github.com/RivieraSystems/NppZenHankaku/releases) (or run `build.bat`: `bin\x64\Release\NppZenHankaku.dll`)
2. Place it here:  
   `<Notepad++ install dir>\plugins\NppZenHankaku\NppZenHankaku.dll`  
   Example: `C:\Program Files\Notepad++\plugins\NppZenHankaku\NppZenHankaku.dll`
3. Restart `Notepad++`

> The folder name must match the DLL name. `Notepad++` only loads plugins from `plugins\<DllName>\<DllName>.dll`.

### Usage

1. Select the text to convert (hold Ctrl for multiple selections, hold Alt and drag for a rectangular selection, or select nothing to convert the whole document)
2. Choose a command from Plugins → 全角半角変換, or use the toolbar buttons

> With multiple or rectangular selections, every selected span is converted in one operation and a single Undo reverts all of it. A zero-width rectangular selection (dragged straight down) does nothing.

### Known limitations

- After a rectangular conversion the rectangle does not stay on the same columns if their widths changed. The two corner points each follow their own character, but the range on every line in between is re-derived from those two points, so on those lines the characters covered can differ from the original ones (this happens even on lines that were not converted)
- Overlapping selections are not supported (the plugin shows a message and does nothing)
- Running the command with several empty carets does nothing (it does not fall back to converting the whole document)
- ANSI documents in code pages where one character can take three or more bytes (such as GB18030) are not supported (the plugin shows a message and does nothing)
- If there are more than 4096 converted spans in total, the spans within each line are first merged into a single replacement per line. Line breaks are never crossed, so bookmarks and markers are kept. That replacement does run from the line's first span to its last one, however, so Change History bands and indicator underlines spread over the unchanged text in between. Only when that still exceeds 4096 (that is, when more than 4096 lines contain conversions) does the plugin fall back to replacing everything from the first to the last span within each selection, and bookmarks and markers in that range are lost. The count depends on how the conversion targets are scattered rather than on document size, since consecutive targets count as one span
- In documents with millions of converted spans, the same merging is applied early, while converting, to keep memory bounded. A selection merged early drops to a handful of spans, and it is that reduced count which enters the 4096 check afterwards, so a selection holding millions of spans never drags the bookmarks and markers of the other selections down with it
- With word wrap on, the caret and the document line are preserved, but not which wrapped line within that document line was at the top of the view

### Two ways to build

Building requires the Desktop development with `C++` workload in Visual Studio.  
When you install Visual Studio, please select Desktop development with `C++`.

- bat file
1. Run `build.bat`.

- Visual Studio
1. Open `NppZenHankaku.sln`.
2. On the toolbar, select Release and x64.
3. From the menu, run Build → Build Solution.

Either way, the output is `bin\x64\Release\NppZenHankaku.dll`.

### Published files

The files that belong on GitHub are listed in [PUBLIC_FILES](PUBLIC_FILES).  
`build.bat` compares the Git-tracked files with that list and fails if anything extra is present.

### License

[MIT License](LICENSE). Copyright (c) 2026 RivieraSystems  
The copyright holder appears in three places: `LICENSE`, and `ZH_COMPANY` / `ZH_COPYRIGHT` in `NppZenHankaku.rc`.  
The DLL version is defined by `ZH_VER_*` at the top of `NppZenHankaku.rc`; bump it for each release.
