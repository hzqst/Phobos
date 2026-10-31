@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
if not exist Debug\RenderTests\VoxelBenchmark mkdir Debug\RenderTests\VoxelBenchmark
cl /nologo /O2 /std:c++20 /MT /W4 /DNOMINMAX /D_HAS_EXCEPTIONS=0 /I src tests\Render\BlitFrameSourceBenchmark.cpp src\Render\BlitFrameSource.cpp src\Render\FrameCache.cpp /FoDebug\RenderTests\VoxelBenchmark\ /FeDebug\RenderTests\VoxelBenchmark\BlitFrameSourceBenchmark.exe /link /MANIFEST:EMBED /MANIFESTUAC:level='asInvoker'
if errorlevel 1 exit /b 1
rem Match the observed game's affinity. Measures acquisition only, never game FPS.
start "" /b /wait /affinity 1 Debug\RenderTests\VoxelBenchmark\BlitFrameSourceBenchmark.exe > Debug\RenderTests\VoxelBenchmark\results.csv
if errorlevel 1 exit /b 1
type Debug\RenderTests\VoxelBenchmark\results.csv
