# Compare git ls-files with PUBLIC_FILES. Extra tracked files fail the build.
param(
	[string]$Root = ""
)

$ErrorActionPreference = "Stop"
if (-not $Root) {
	$Root = Split-Path -Parent $PSScriptRoot
}

$allowPath = Join-Path $Root "PUBLIC_FILES"
if (-not (Test-Path $allowPath)) {
	Write-Host "NG: PUBLIC_FILES is missing: $allowPath"
	exit 1
}

$allow = Get-Content -LiteralPath $allowPath -Encoding UTF8 |
	ForEach-Object { $_.Trim() } |
	Where-Object { $_ -and -not $_.StartsWith("#") }

$dup = @($allow | Group-Object | Where-Object { $_.Count -gt 1 })
if ($dup.Count -gt 0) {
	Write-Host "NG: PUBLIC_FILES has duplicate paths:"
	$dup | ForEach-Object { Write-Host ("  " + $_.Name) }
	exit 1
}

Push-Location $Root
$tracked = @(git -c core.quotepath=false ls-files)
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) {
	Write-Host "NG: git ls-files failed. Run this from the repository root."
	exit 1
}

$trackedSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$tracked)
$allowSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$allow)

$extra = @($tracked | Where-Object { -not $allowSet.Contains($_) } | Sort-Object)
$missing = @($allow | Where-Object { -not $trackedSet.Contains($_) } | Sort-Object)

$ng = $false
if ($extra.Count -gt 0) {
	Write-Host "NG: tracked files not listed in PUBLIC_FILES:"
	$extra | ForEach-Object { Write-Host ("  " + $_) }
	$ng = $true
}
if ($missing.Count -gt 0) {
	Write-Host "NG: PUBLIC_FILES entries that are not tracked:"
	$missing | ForEach-Object { Write-Host ("  " + $_) }
	$ng = $true
}

if ($ng) { exit 1 }

Write-Host ("OK: tracked files match PUBLIC_FILES ({0} files)." -f $allow.Count)
exit 0