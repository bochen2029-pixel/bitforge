@echo off
setlocal
rem bf_search - build the GPU search/bench tool (RTX 4070 Ti SUPER = sm_89).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [!] vcvars failed & exit /b 1 )
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "CUDADIR=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1"
if not exist "%OUT%" mkdir "%OUT%"

echo === nvcc: bf_search.exe (sm_89) ===
nvcc -O3 -gencode arch=compute_89,code=sm_89 --ptxas-options=-v ^
     "%ROOT%search\bf_search.cu" -o "%OUT%\bf_search.exe" || exit /b 1
copy /y "%CUDADIR%\bin\cudart64_*.dll" "%OUT%\" >nul 2>nul
echo === OK -^> %OUT%\bf_search.exe ===
