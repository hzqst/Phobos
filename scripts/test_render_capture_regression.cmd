@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
if not exist Debug\RenderTests mkdir Debug\RenderTests
set "_CL_=/D_CRT_SECURE_NO_WARNINGS"
set "_LINK_=/MANIFEST:EMBED /MANIFESTUAC:level='asInvoker'"
rem Sources that define the state a test's header declares.
set "EXTRA_Atlas=src\Render\PaletteCache.cpp"
set "EXTRA_ShapeFrameSource=src\Render\ShapeFrameSource.cpp"
set "EXTRA_CursorTrace=src\Render\CursorTrace.cpp"
for %%T in (Atlas ButtonImage CameoLayout CursorImage FrameCache GlyphCache ShapeImage ShapeFrameSource CursorTrace Tooltip TerrainImage TerrainBatch ABufferImage ShapeDepth OverlayImage TerrainObjectImage BuildingImage) do (
    call :unit %%T
    if errorlevel 1 exit /b 1
)
cl /nologo /std:c++20 /MT /W4 /DNOMINMAX /I src /I lib\SDL3\include tests\Render\FrameAtlasLifetimeTests.cpp src\Render\FrameAtlas.cpp src\Render\FrameCache.cpp src\Render\PaletteCache.cpp /FoDebug\RenderTests\ /FeDebug\RenderTests\FrameAtlasLifetimeTests.exe
if errorlevel 1 exit /b 1
Debug\RenderTests\FrameAtlasLifetimeTests.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /MT /W4 /DNOMINMAX /I src /I lib\SDL3\include /I Debug\IntDir\Generated tests\Render\FrameAtlasGPUTests.cpp src\Render\FrameAtlas.cpp src\Render\FrameCache.cpp src\Render\PaletteCache.cpp /FoDebug\RenderTests\ /FeDebug\RenderTests\FrameAtlasGPUTests.exe /link /LIBPATH:lib\SDL3\lib\x86 SDL3.lib
if errorlevel 1 exit /b 1
set "PATH=%CD%\Debug\AutomationGame;%CD%\Debug;%PATH%"
Debug\RenderTests\FrameAtlasGPUTests.exe
exit /b %errorlevel%
:unit
set "EXTRA="
call set "EXTRA=%%EXTRA_%1%%"
cl /nologo /std:c++20 /MT /W4 /DNOMINMAX /DSYR_VER=2 /D_HAS_EXCEPTIONS=0 /I src /I YRpp /I lib\SDL3\include tests\Render\%1Tests.cpp src\Render\FrameCache.cpp src\Render\GlyphCache.cpp %EXTRA% /FoDebug\RenderTests\ /FeDebug\RenderTests\%1Tests.exe
if errorlevel 1 exit /b 1
Debug\RenderTests\%1Tests.exe
exit /b %errorlevel%
