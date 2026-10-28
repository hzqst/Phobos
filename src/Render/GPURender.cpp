#include "GPURender.h"
#include "PresentPass.h"
#include "SDLLoader.h"
#include "SecondaryWindow.h"
#include "SurfaceMirror.h"

#include <CCINIClass.h>
#include <GameOptionsClass.h>
#include <GameStrings.h>
#include <Surface.h>

#include <Phobos.h>
#include <Utilities/Debug.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <utility>

bool GPURender::Enabled = true;
bool GPURender::DisabledByCommandLine = false;
GPURender::DriverPreference GPURender::Driver = GPURender::DriverPreference::Auto;
GPURender::State GPURender::CurrentState = GPURender::State::Disabled;
UINT_PTR GPURender::TimerID = 0;

namespace
{
	// Drives the event handling and the presentation from the game's own message loops,
	// so everything stays on the main thread whatever loop the game is in.
	constexpr UINT TimerIntervalMS = 16;

	// The game blits several surfaces per frame and the timer adds frames for the cursor,
	// presenting more often than this would only repeat the same image.
	constexpr Uint64 MinPresentIntervalNS = SDL_NS_PER_SECOND / 120;

	SDL_GPUDevice* Device = nullptr;
	bool WindowClaimed = false;
	Uint64 LastPresentTime = 0;

	// The names of the specific drivers are the ones SDL_CreateGPUDevice takes.
	constexpr std::pair<GPURender::DriverPreference, const char*> DriverNames[] =
	{
		{ GPURender::DriverPreference::Auto, "auto" },
		{ GPURender::DriverPreference::Direct3D12, "direct3d12" },
		{ GPURender::DriverPreference::Vulkan, "vulkan" },
	};

	const char* GetDriverName(GPURender::DriverPreference driver)
	{
		for (const auto& [value, pName] : DriverNames)
		{
			if (value == driver)
				return pName;
		}

		return "unknown";
	}

	const char* GetStateName(GPURender::State state)
	{
		switch (state)
		{
		case GPURender::State::Disabled:
			return "Disabled";
		case GPURender::State::Initializing:
			return "Initializing";
		case GPURender::State::Running:
			return "Running";
		case GPURender::State::Hidden:
			return "Hidden";
		case GPURender::State::Failed:
			return "Failed";
		case GPURender::State::Shutdown:
			return "Shutdown";
		}

		return "Unknown";
	}

	// Whether pPath names a file directly inside pDirectory, both as returned by the system.
	bool IsFileInDirectory(const wchar_t* pPath, const wchar_t* pDirectory)
	{
		const size_t length = wcslen(pDirectory);

		return length
			&& _wcsnicmp(pPath, pDirectory, length) == 0
			&& pPath[length] == L'\\'
			&& !wcschr(pPath + length + 1, L'\\');
	}

	SDL_GPUDevice* CreateDevice(const char* pDriverName)
	{
		const SDL_PropertiesID properties = SDL_CreateProperties();
		SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, pDriverName);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
		// Defaults to true, which enables the validation layers
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
		// Lets Intel Haswell / Broadwell GPUs run D3D12, the shaders use only a few resources
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN, true);

		SDL_GPUDevice* pDevice = SDL_CreateGPUDeviceWithProperties(properties);
		SDL_DestroyProperties(properties);

		if (pDevice)
			Debug::Log("[GPURender] Created the %s device.\n", SDL_GetGPUDeviceDriver(pDevice));
		else
			Debug::Log("[GPURender] Creating the %s device failed: %s\n", pDriverName, SDL_GetError());

		return pDevice;
	}

	SDL_GPUDevice* CreateDevice(GPURender::DriverPreference driver)
	{
		if (driver != GPURender::DriverPreference::Auto)
			return CreateDevice(GetDriverName(driver));

		if (SDL_GPUDevice* pDevice = CreateDevice(GetDriverName(GPURender::DriverPreference::Direct3D12)))
			return pDevice;

		return CreateDevice(GetDriverName(GPURender::DriverPreference::Vulkan));
	}

	// Presenting must never wait for the vertical blank of the secondary window's monitor,
	// as that would hold up the game loop.
	SDL_GPUPresentMode ChoosePresentMode(SDL_Window* pWindow)
	{
		if (SDL_WindowSupportsGPUPresentMode(Device, pWindow, SDL_GPU_PRESENTMODE_IMMEDIATE))
			return SDL_GPU_PRESENTMODE_IMMEDIATE;

		if (SDL_WindowSupportsGPUPresentMode(Device, pWindow, SDL_GPU_PRESENTMODE_MAILBOX))
			return SDL_GPU_PRESENTMODE_MAILBOX;

		Debug::Log("[GPURender] Neither immediate nor mailbox presentation is supported, falling back to vsync.\n");
		return SDL_GPU_PRESENTMODE_VSYNC;
	}

	const char* GetPresentModeName(SDL_GPUPresentMode mode)
	{
		switch (mode)
		{
		case SDL_GPU_PRESENTMODE_IMMEDIATE:
			return "immediate";
		case SDL_GPU_PRESENTMODE_MAILBOX:
			return "mailbox";
		default:
			return "vsync";
		}
	}
}

// The display is set up before OptionsClass::LoadSettings reads the Phobos settings,
// so RA2MD.INI is read separately here.
void GPURender::LoadSettings()
{
	constexpr auto pSection = "Phobos";

	CCINIClass ini {};
	ini.LoadFromFile(GameStrings::RA2MD_INI);

	Enabled = ini.ReadBool(pSection, "GPURender.Enable", true);
	Driver = DriverPreference::Auto;

	if (ini.ReadString(pSection, "GPURender.Driver", "", Phobos::readBuffer) > 0)
	{
		bool parsed = false;

		for (const auto& [value, pName] : DriverNames)
		{
			if (_stricmp(Phobos::readBuffer, pName) == 0)
			{
				Driver = value;
				parsed = true;
				break;
			}
		}

		if (!parsed)
			Debug::INIParseFailed(pSection, "GPURender.Driver", Phobos::readBuffer, "Expected auto, direct3d12 or vulkan");
	}

	Debug::Log("[GPURender] Settings: Enable=%s, Driver=%s.\n", Enabled ? "yes" : "no", GetDriverName(Driver));
}

void GPURender::DisableByCommandLine()
{
	DisabledByCommandLine = true;
}

void GPURender::OnVideoModeSet()
{
	const int width = DSurface::WindowBounds.Width;
	const int height = DSurface::WindowBounds.Height;

	switch (CurrentState)
	{
	case State::Disabled:
		LoadSettings();

		if (ShouldStart())
			Initialize(width, height);

		break;

	case State::Running:
	case State::Hidden:
		SecondaryWindow::OnGameResolutionChanged(width, height);
		break;

	default:
		break;
	}
}

void GPURender::Shutdown()
{
	if (CurrentState == State::Shutdown)
		return;

	ReleaseResources();
	SetState(State::Shutdown);
}

GPURender::State GPURender::GetState()
{
	return CurrentState;
}

bool GPURender::ShouldStart()
{
	if (DisabledByCommandLine)
	{
		Debug::Log("[GPURender] Disabled by -NoGPURender.\n");
		return false;
	}

	if (!Enabled)
	{
		Debug::Log("[GPURender] Disabled by GPURender.Enable=no.\n");
		return false;
	}

	if (!IsDisplayEnvironmentSupported())
	{
		Debug::Log("[GPURender] Not started: native DirectDraw in fullscreen hides every other window, "
			"use windowed mode or a DirectDraw wrapper.\n");
		return false;
	}

	return true;
}

// Native DirectDraw takes the display exclusively in fullscreen, while in windowed mode, or
// with a wrapper like cnc-ddraw or DDrawCompat, the game is presented through a normal window
// next to which the secondary window can be shown.
bool GPURender::IsDisplayEnvironmentSupported()
{
	return GameOptionsClass::WindowedMode || IsDirectDrawWrapped();
}

bool GPURender::IsDirectDrawWrapped()
{
	const HMODULE hDirectDraw = GetModuleHandleW(L"ddraw.dll");

	if (!hDirectDraw)
		return false;

	wchar_t path[MAX_PATH];
	const DWORD length = GetModuleFileNameW(hDirectDraw, path, std::size(path));

	if (!length || length == std::size(path))
		return false;

	wchar_t directory[MAX_PATH];

	// A 32-bit process on 64-bit Windows maps the system DLLs from SysWOW64.
	if (GetSystemWow64DirectoryW(directory, std::size(directory)) && IsFileInDirectory(path, directory))
		return false;

	if (GetSystemDirectoryW(directory, std::size(directory)) && IsFileInDirectory(path, directory))
		return false;

	Debug::Log("[GPURender] DirectDraw is provided by %ls.\n", path);
	return true;
}

void GPURender::Initialize(int width, int height)
{
	SetState(State::Initializing);

	if (!SDLLoader::Load())
		return Fail("Loading SDL3.dll", "see above");

	// The game's own message loops dispatch the messages of the secondary window. SDL must
	// not pump the thread's message queue itself, it would take messages (WM_QUIT included)
	// meant for the game.
	SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
	// Read by SDL's Windows video backend, which otherwise makes the whole process per
	// monitor DPI aware. "unaware" only fails if the awareness has been set before.
	SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "unaware");
	// Leave the system timer resolution, the screen saver and the focus to the game
	SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
	SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
	SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
		return Fail("SDL_InitSubSystem");

	Device = CreateDevice(Driver);

	if (!Device)
		return Fail("Creating the GPU device", "no driver could be used");

	char title[64];
	sprintf_s(title, "Phobos GPU Render (%s)", SDL_GetGPUDeviceDriver(Device));

	if (!SecondaryWindow::Create(title, width, height))
		return Fail("Creating the window");

	if (!SDL_ClaimWindowForGPUDevice(Device, SecondaryWindow::Get()))
		return Fail("SDL_ClaimWindowForGPUDevice");

	WindowClaimed = true;

	const SDL_GPUPresentMode presentMode = ChoosePresentMode(SecondaryWindow::Get());

	if (!SDL_SetGPUSwapchainParameters(Device, SecondaryWindow::Get(), SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode))
		return Fail("SDL_SetGPUSwapchainParameters");

	Debug::Log("[GPURender] Presenting with %s mode.\n", GetPresentModeName(presentMode));

	if (!PresentPass::Create(Device, SecondaryWindow::Get()))
		return Fail("Creating the present pipeline");

	TimerID = SetTimer(nullptr, 0, TimerIntervalMS, OnTimer);

	if (!TimerID)
	{
		char reason[32];
		sprintf_s(reason, "error %lu", GetLastError());
		return Fail("SetTimer", reason);
	}

	SetState(State::Running);
	Present();
}

// In the order SDL requires: the GPU work first, then the device, then the window and SDL.
void GPURender::ReleaseResources()
{
	if (TimerID)
	{
		KillTimer(nullptr, TimerID);
		TimerID = 0;
	}

	if (Device)
	{
		SDL_WaitForGPUIdle(Device);
		PresentPass::Release(Device);
		SurfaceMirror::Release(Device);

		if (WindowClaimed)
		{
			SDL_ReleaseWindowFromGPUDevice(Device, SecondaryWindow::Get());
			WindowClaimed = false;
		}

		SDL_DestroyGPUDevice(Device);
		Device = nullptr;
	}

	if (SDLLoader::IsLoaded())
	{
		SecondaryWindow::Destroy();
		SDL_Quit();
	}
}

void GPURender::Present()
{
	LastPresentTime = SDL_GetTicksNS();

	if (SurfaceMirror::Capture(Device) == SurfaceMirror::CaptureResult::Failed)
		return Fail("Capturing the game screen");

	SDL_GPUCommandBuffer* pCommandBuffer = SDL_AcquireGPUCommandBuffer(Device);

	if (!pCommandBuffer)
		return Fail("SDL_AcquireGPUCommandBuffer");

	if (SurfaceMirror::HasPendingUpload())
	{
		SDL_GPUCopyPass* pCopyPass = SDL_BeginGPUCopyPass(pCommandBuffer);
		SurfaceMirror::Upload(pCopyPass);
		SDL_EndGPUCopyPass(pCopyPass);
	}

	// Does not wait; there is no texture while the window is minimized or too many frames are queued
	SDL_GPUTexture* pSwapchainTexture = nullptr;
	Uint32 swapchainWidth = 0;
	Uint32 swapchainHeight = 0;

	if (!SDL_AcquireGPUSwapchainTexture(pCommandBuffer, SecondaryWindow::Get(), &pSwapchainTexture, &swapchainWidth, &swapchainHeight))
	{
		SDL_CancelGPUCommandBuffer(pCommandBuffer);
		return Fail("SDL_AcquireGPUSwapchainTexture");
	}

	if (pSwapchainTexture)
	{
		PresentPass::Draw(pCommandBuffer, pSwapchainTexture, swapchainWidth, swapchainHeight,
			SurfaceMirror::GetTexture(), SurfaceMirror::GetWidth(), SurfaceMirror::GetHeight());
	}

	if (!SDL_SubmitGPUCommandBuffer(pCommandBuffer))
		Fail("SDL_SubmitGPUCommandBuffer");
}

void GPURender::RequestPresent()
{
	if (SDL_GetTicksNS() - LastPresentTime >= MinPresentIntervalNS)
		Present();
}

void GPURender::OnBlit()
{
	if (CurrentState == State::Running)
		RequestPresent();
}

void CALLBACK GPURender::OnTimer(HWND, UINT, UINT_PTR, DWORD)
{
	if (CurrentState != State::Running && CurrentState != State::Hidden)
		return;

	// Only processes what the game's message loop has dispatched to the window
	SDL_PumpEvents();

	bool needsPresent = false;
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
		switch (SecondaryWindow::HandleEvent(event))
		{
		case SecondaryWindow::EventResult::CloseRequested:
			if (CurrentState == State::Running)
			{
				SecondaryWindow::Hide();
				SetState(State::Hidden);
			}

			break;

		case SecondaryWindow::EventResult::NeedsPresent:
			needsPresent = true;
			break;

		default:
			break;
		}
	}

	if (CurrentState != State::Running)
		return;

	// The mouse thread moves the cursor on the primary surface between the game's blits
	if (needsPresent)
		Present();
	else
		RequestPresent();
}

void GPURender::SetState(State state)
{
	if (state == CurrentState)
		return;

	Debug::Log("[GPURender] State: %s -> %s.\n", GetStateName(CurrentState), GetStateName(state));
	CurrentState = state;
}

void GPURender::Fail(const char* pWhat, const char* pReason)
{
	if (!pReason)
		pReason = SDLLoader::IsLoaded() ? SDL_GetError() : "SDL3.dll is not loaded";

	Debug::Log("[GPURender] %s failed: %s\n", pWhat, pReason);
	ReleaseResources();
	SetState(State::Failed);
}
