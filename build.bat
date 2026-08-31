@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

call :build
set "BUILD_RESULT=%ERRORLEVEL%"

echo.
if "%BUILD_RESULT%"=="0" (
  echo 処理が正常に完了しました。
) else (
  echo ビルドに失敗しました（終了コード: %BUILD_RESULT%）。
)
echo 何かキーを押すと閉じます。
pause >nul
exit /b %BUILD_RESULT%

:build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio が見つかりません。Visual Studio ^(C++ デスクトップ開発^) をインストールしてください。
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo C++ ツールチェーン付きの Visual Studio が見つかりません。
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

echo --- 公開ファイル（PUBLIC_FILES）---
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_public_files.ps1 || exit /b 1

rem VS2026=v145, VS2022=v143。インストール済みの最新ツールセットを使う
set "PLATFORM_TOOLSET="
if exist "%VSINSTALL%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v145.default.txt" set "PLATFORM_TOOLSET=v145"
if not defined PLATFORM_TOOLSET if exist "%VSINSTALL%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt" set "PLATFORM_TOOLSET=v143"
if not defined PLATFORM_TOOLSET set "PLATFORM_TOOLSET=v145"
echo Using PlatformToolset=%PLATFORM_TOOLSET%

set "TESTDIR=obj\x64\Release\Tests"
if not exist "%TESTDIR%" mkdir "%TESTDIR%" || exit /b 1
rem /O2 を付ける。付けないと cl の既定は /Od なので、--bench の数字が
rem DLL（msbuild の Release）と桁違いに遅くなり、性能の記録として使えない
cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /O2 ^
  /Fo:"%TESTDIR%\converter.obj" src\converter.cpp || exit /b 1
rem DbcsCodec は Windows API を使うが Notepad++ には依存しないので、テストから直接叩ける
cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /O2 /I src ^
  /Fo:"%TESTDIR%\DbcsCodec.obj" src\DbcsCodec.cpp || exit /b 1
cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /O2 /I src ^
  /Fo:"%TESTDIR%\converter_test.obj" src\converter_test.cpp || exit /b 1
cl /nologo /Fe:"%TESTDIR%\NppZenHankakuTest.exe" ^
  "%TESTDIR%\converter.obj" "%TESTDIR%\DbcsCodec.obj" "%TESTDIR%\converter_test.obj" || exit /b 1
"%TESTDIR%\NppZenHankakuTest.exe" || exit /b 1

msbuild NppZenHankaku.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=%PLATFORM_TOOLSET% /m || exit /b 1

set "OUT=bin\x64\Release\NppZenHankaku.dll"
if not exist "%OUT%" (
  echo 出力 DLL が見つかりません: %OUT%
  exit /b 1
)

rem 公開用に PDB が残っていれば削除（Release では生成しない設定）
if exist "bin\x64\Release\NppZenHankaku.pdb" del /q "bin\x64\Release\NppZenHankaku.pdb"

echo.
echo ビルド成功: %OUT%
echo.
echo --- 公開前チェック（パス文字列）---
findstr /I /L /M /C:"\Users" /C:"/Users/" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo OK: Users パス文字列は見つかりませんでした。
) else (
  echo WARNING: Users パスらしき文字列が DLL に含まれている可能性があります。
)
echo.
echo --- 公開前チェック（版情報の著作権表示）---
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_version_info.ps1 -Rc NppZenHankaku.rc -Dll "%OUT%" -License LICENSE
if errorlevel 1 exit /b 1

echo.
echo --- 公開前チェック（構成とエクスポート）---
set "DUMPOUT=%TEMP%\NppZenHankaku_dumpbin.txt"
dumpbin /nologo /headers /exports "%OUT%" > "%DUMPOUT%" 2>&1
if errorlevel 1 (
  echo NG: dumpbin を実行できませんでした。
  exit /b 1
)

set "CHECK_NG=0"
findstr /L /C:"machine (x64)" "%DUMPOUT%" >nul || (echo NG: x64 ではありません。& set "CHECK_NG=1")
rem Notepad++ がプラグインへ要求する 6 つのエクスポート
for %%F in (setInfo getName getFuncsArray beNotified messageProc isUnicode) do (
  findstr /L /C:" %%F" "%DUMPOUT%" >nul || (echo NG: エクスポートがありません: %%F& set "CHECK_NG=1")
)
del /q "%DUMPOUT%" >nul 2>&1
if "%CHECK_NG%"=="1" exit /b 1
echo OK: x64 で、必須の 6 エクスポートがそろっています。

echo.
echo --- リリース記録用 SHA-256 ---
certutil -hashfile "%OUT%" SHA256 | findstr /V /C:"CertUtil" /C:"SHA256"

echo.
echo Notepad++ の plugins\NppZenHankaku\ に NppZenHankaku.dll を配置してください。
exit /b 0
