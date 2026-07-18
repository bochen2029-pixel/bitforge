@echo off
setlocal enabledelayedexpansion
rem bitforge - one-shot MSVC build. No external dependencies.

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo [!] vcvars64.bat not found; edit VCVARS in build.bat
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 ( echo [!] vcvars failed & exit /b 1 )

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"
set "CF=/nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%core""
set "CORE=%ROOT%core\process_source.cpp %ROOT%core\scanner.cpp %ROOT%core\disk_source.cpp"

echo === bitforge_cli ===
cl %CF% "%ROOT%cli\bitforge_cli.cpp" %CORE% /Fe:"%OUT%\bitforge_cli.exe" /Fo:"%OUT%\\" || exit /b 1

echo === target_toy ===
cl %CF% "%ROOT%target\target_toy.cpp" /Fe:"%OUT%\target_toy.exe" /Fo:"%OUT%\\" || exit /b 1

if exist "%ROOT%gui\bitforge_gui.cpp" (
  echo === bitforge_gui ===
  cl %CF% "%ROOT%gui\bitforge_gui.cpp" %CORE% /Fe:"%OUT%\bitforge_gui.exe" /Fo:"%OUT%\\" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib || exit /b 1
)

echo.
echo === build OK -> %OUT% ===
dir /b "%OUT%\*.exe"
