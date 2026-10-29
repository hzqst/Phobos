@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
if not exist "Debug\AutomationTests" mkdir "Debug\AutomationTests"
set "_LINK_=/MANIFEST:EMBED /MANIFESTUAC:level='asInvoker'"
cl /nologo /std:c++20 /MT /W4 /DNOMINMAX /I src /I lib\SDL3\include tests\Render\GPUReadbackLifetimeTests.cpp src\Render\GPUReadback.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\GPUReadbackLifetimeTests.exe
if errorlevel 1 exit /b 1
Debug\AutomationTests\GPUReadbackLifetimeTests.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /MT /W4 /DNOMINMAX /I src /I lib\SDL3\include tests\Render\GPUReadbackTests.cpp src\Render\GPUReadback.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\GPUReadbackTests.exe /link /LIBPATH:lib\SDL3\lib\x86 SDL3.lib
if errorlevel 1 exit /b 1
set "PATH=%CD%\Debug\AutomationGame;%CD%\Debug;%PATH%"
Debug\AutomationTests\GPUReadbackTests.exe %*
exit /b %errorlevel%
