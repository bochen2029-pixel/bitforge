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

rem --- optional CUDA analytics island (linked in when nvcc is available) ---
set "CUDADIR=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1"
set "GPUDEF="
set "GPUOBJ="
set "GPULINK="
where nvcc >nul 2>nul
if not errorlevel 1 (
  echo === nvcc: gpu_bitforge.obj ===
  nvcc -O3 -c "%ROOT%gpu\gpu_bitforge.cu" -o "%OUT%\gpu_bitforge.obj" || exit /b 1
  set "GPUDEF=/DBITFORGE_CUDA"
  set "GPUOBJ=%OUT%\gpu_bitforge.obj"
  set "GPULINK=cudart.lib"
  set "LIB=%LIB%;%CUDADIR%\lib\x64"
  copy /y "%CUDADIR%\bin\cudart64_*.dll" "%OUT%\" >nul
)

echo === bitforge_cli ===
cl %CF% %GPUDEF% "%ROOT%cli\bitforge_cli.cpp" %CORE% %GPUOBJ% /Fe:"%OUT%\bitforge_cli.exe" /Fo:"%OUT%\\" /link %GPULINK% || exit /b 1

echo === target_toy ===
cl %CF% "%ROOT%target\target_toy.cpp" /Fe:"%OUT%\target_toy.exe" /Fo:"%OUT%\\" || exit /b 1

if exist "%ROOT%gui\bitforge_gui.cpp" (
  echo === bitforge_gui ===
  cl %CF% %GPUDEF% "%ROOT%gui\bitforge_gui.cpp" %CORE% %GPUOBJ% /Fe:"%OUT%\bitforge_gui.exe" /Fo:"%OUT%\\" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib %GPULINK% || exit /b 1
)

echo.
echo === build OK -> %OUT% ===
dir /b "%OUT%\*.exe"
