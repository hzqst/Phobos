@echo off
setlocal
cd /d "%~dp0.."
if not exist "Debug\AutomationTests" mkdir "Debug\AutomationTests"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
set "_CL_=/D_CRT_SECURE_NO_WARNINGS"
set "_LINK_=/MANIFEST:EMBED /MANIFESTUAC:level='asInvoker'"
cl /nologo /std:c++20 /MT /Gz /W4 /D_HAS_EXCEPTIONS=0 /DNOMINMAX /I src tests\Automation\ProtocolTests.cpp src\Automation\MCPProtocol.cpp src\Automation\MCPTools.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\ProtocolTests.exe
if not "%errorlevel%"=="0" exit /b 1
Debug\AutomationTests\ProtocolTests.exe
if not "%errorlevel%"=="0" exit /b 1
cl /nologo /std:c++20 /MT /Gz /W4 /D_HAS_EXCEPTIONS=0 /DNOMINMAX /I src tests\Automation\DispatcherTests.cpp src\Automation\AutomationDispatcher.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\DispatcherTests.exe
if not "%errorlevel%"=="0" exit /b 1
Debug\AutomationTests\DispatcherTests.exe
if not "%errorlevel%"=="0" exit /b 1
cl /nologo /std:c++20 /MT /Gz /W4 /D_HAS_EXCEPTIONS=0 /DNOMINMAX /I src tests\Automation\InputTests.cpp src\Automation\AutomationInput.cpp src\Automation\AutomationDispatcher.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\InputTests.exe
if not "%errorlevel%"=="0" exit /b 1
Debug\AutomationTests\InputTests.exe
if not "%errorlevel%"=="0" exit /b 1
cl /nologo /std:c++20 /MT /Gz /W4 /D_HAS_EXCEPTIONS=0 /DNOMINMAX /I src tests\Automation\FrameControlTests.cpp src\Automation\AutomationFrameControl.cpp src\Automation\AutomationDispatcher.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\FrameControlTests.exe
if not "%errorlevel%"=="0" exit /b 1
Debug\AutomationTests\FrameControlTests.exe
if not "%errorlevel%"=="0" exit /b 1
cl /nologo /std:c++20 /MT /Gz /W4 /D_HAS_EXCEPTIONS=0 /DNOMINMAX /I src tests\Automation\CaptureTests.cpp src\Automation\CaptureEncoding.cpp src\Automation\MCPProtocol.cpp src\Automation\MCPTools.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\CaptureTests.exe
if not "%errorlevel%"=="0" exit /b 1
Debug\AutomationTests\CaptureTests.exe
if not "%errorlevel%"=="0" exit /b 1
cl /nologo /std:c++20 /MT /Gz /W4 /D_HAS_EXCEPTIONS=0 /DNOMINMAX /I src tests\Automation\HTTPHarness.cpp src\Automation\CaptureEncoding.cpp src\Automation\MCPProtocol.cpp src\Automation\MCPTools.cpp src\Automation\AutomationServer.cpp /FoDebug\AutomationTests\ /FeDebug\AutomationTests\HTTPHarness.exe
exit /b %errorlevel%
