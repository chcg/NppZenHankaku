# DLL の版情報が rc と LICENSE と食い違っていないかを見る。build.bat から呼ばれる。
#
# 著作権者は LICENSE・ZH_COMPANY・ZH_COPYRIGHT の 3 箇所に書いてあり、
# どれか 1 つだけ直した状態で公開できてしまう。ここで 3 つを突き合わせる。
#
# **build.bat へ直接書かずにここへ置いてある。** cmd は .bat を ACP（日本語環境なら
# CP932）として読むので、UTF-8 で書いた日本語コメントの末尾がリードバイトになると
# 行末の CR を食い、次の行が途中から実行されてしまう。二重引用符の入れ子も壊れる。
#
# このファイルは **UTF-8 (BOM 付き)** で保存すること。BOM が無いと Windows
# PowerShell 5.1 は ANSI として読むので、日本語が化ける。

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Rc,
	[Parameter(Mandatory = $true)][string]$Dll,
	[string]$License = 'LICENSE'
)

$ErrorActionPreference = 'Stop'

# rc の #define から文字列リテラルを取り出す
function Get-RcString([string]$text, [string]$name) {
	$m = [regex]::Match($text, ('#define\s+{0}\s+"([^"]*)"' -f [regex]::Escape($name)))
	if (-not $m.Success) { return $null }
	return $m.Groups[1].Value
}

$rcText = Get-Content -LiteralPath $Rc -Raw -Encoding UTF8
$company = Get-RcString $rcText 'ZH_COMPANY'
$copyright = Get-RcString $rcText 'ZH_COPYRIGHT'

$ng = @()
if (-not $company) { $ng += 'rc から ZH_COMPANY を読めません' }
if (-not $copyright) { $ng += 'rc から ZH_COPYRIGHT を読めません' }

if ($ng.Count -eq 0) {
	# 版情報は UTF-16 で埋め込まれるので findstr では読めない
	$info = (Get-Item -LiteralPath $Dll).VersionInfo
	if ($info.CompanyName -ne $company) {
		$ng += ('版情報の会社名が rc と違います: [{0}] / rc [{1}]' -f $info.CompanyName, $company)
	}
	if ($info.LegalCopyright -ne $copyright) {
		$ng += ('版情報の著作権表示が rc と違います: [{0}] / rc [{1}]' -f $info.LegalCopyright, $copyright)
	}

	# ZH_COPYRIGHT の「Copyright (c) 年 著作権者」の部分が LICENSE にもあること。
	# 年と著作権者の両方を見るので、どちらを直し忘れても止まる
	$m = [regex]::Match($copyright, '^Copyright \(c\) (\d{4}) (.+)$')
	if (-not $m.Success) {
		$ng += ('ZH_COPYRIGHT が "Copyright (c) 年 著作権者." の形ではありません: [{0}]' -f $copyright)
	}
	else {
		# 著作権者は年の後ろの最初の文。**「ピリオド + 空白」で切る。**
		# 単に最初のピリオドまでとすると、"Example Inc." のように名前が
		# ピリオドで終わる著作権者を拒否してしまう。
		# 末尾のピリオドは会社名側にも付きうるので、両方から外して比べる
		$holder = (($m.Groups[2].Value -split '\.\s+', 2)[0]).Trim().TrimEnd('.')
		$year = $m.Groups[1].Value

		$line = 'Copyright (c) {0} {1}' -f $year, $holder
		if ((Get-Content -LiteralPath $License -Raw -Encoding UTF8) -notmatch [regex]::Escape($line)) {
			$ng += ('LICENSE に [{0}] がありません' -f $line)
		}

		# **会社名と著作権者が同じ名前であること。** ここを見ないと、
		# 「rc と DLL」「著作権表示と LICENSE」がそれぞれ一致していても、
		# ZH_COMPANY だけ別人の名前になった状態で通ってしまう
		if ($holder -ne $company.Trim().TrimEnd('.')) {
			$ng += ('ZH_COMPANY と著作権者が違います: 会社名 [{0}] / 著作権者 [{1}]' -f $company, $holder)
		}
	}
}

if ($ng.Count -ne 0) {
	foreach ($line in $ng) { Write-Host ('NG: ' + $line) }
	exit 1
}

Write-Host ('OK: {0} / {1}' -f $company, $copyright)
exit 0
