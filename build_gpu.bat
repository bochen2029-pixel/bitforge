@echo off
rem bitforge - build the CUDA pieces (standalone bench + linkable object).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [!] vcvars failed & exit /b 1 )
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

echo === nvcc: standalone CPU-vs-GPU bench ===
nvcc -O3 -DGPU_STANDALONE "%ROOT%gpu\gpu_bitforge.cu" -o "%OUT%\bitforge_gpu.exe" || exit /b 1

echo === nvcc: linkable object (gpu_bitforge.obj) ===
nvcc -O3 -c "%ROOT%gpu\gpu_bitforge.cu" -o "%OUT%\gpu_bitforge.obj" || exit /b 1

echo === GPU build OK ===
