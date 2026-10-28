#include "GPURender.h"
#include "PresentPass.h"
#include "SDLLoader.h"
#include "SecondaryWindow.h"
#include "SurfaceMirror.h"
#include "CursorTrace.h"
#include "CursorSnapshot.h"
#include "SidebarSnapshot.h"
#include "FrameAtlas.h"

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
	bool MirrorMode = false;
	CursorSnapshot::Frame CursorFrame;
	SDL_GPUTexture* CursorTexture = nullptr;
	SDL_GPUTransferBuffer* CursorTransfer = nullptr;
	std::array<std::uint32_t, CursorImage::MaxDimension * CursorImage::MaxDimension> UploadedCursor {};
	int UploadedWidth = 0, UploadedHeight = 0;
	bool CursorDirty = false;
	FrameAtlas Atlas;
	unsigned AtlasTraceCount = 0;
	std::uint64_t LastAtlasIndexBytes = 0, LastAtlasPaletteBytes = 0, LastAtlasDecoded = 0;
	std::vector<PresentPass::Sprite> Sprites;

	FrameAtlas::BeginResult PrepareSidebar()
	{
		Sprites.clear();
		const auto& frame = SidebarSnapshot::Current;
		const bool visible = frame.Visible && SidebarSnapshot::Eligible();
		std::vector<FrameAtlas::Request> requests;
		if (visible)
			for (const auto& placement : frame.Placements)
			{
				const auto& asset = frame.Assets[placement.AssetIndex];
				requests.push_back({asset.ID, asset.Color});
			}
		const auto result = Atlas.Begin(ShapeFrameSource::Frames, requests);
		if (result != FrameAtlas::BeginResult::Ready || !visible) return result;
		for (const auto& placement : frame.Placements)
		{
			const auto& asset = frame.Assets[placement.AssetIndex];
			const auto view = Atlas.Resolve({asset.ID, asset.Color});
			if (!view.Texture) continue;
			const auto x = std::int64_t(placement.X) + asset.Left;
			const auto y = std::int64_t(placement.Y) + asset.Top;
			if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) continue;
			const auto clipped = CursorImage::Clip(int(x), int(y), asset.Width, asset.Height, frame.Width, frame.Height);
			const auto canvasX = std::int64_t(clipped.X) + frame.OriginX;
			if (clipped.Width <= 0 || clipped.Height <= 0 || canvasX < INT_MIN || canvasX > INT_MAX) continue;
			Sprites.push_back({view.Texture, int(canvasX), clipped.Y, clipped.Width, clipped.Height,
				view.X + clipped.SourceX, view.Y + clipped.SourceY, true, view.Palette, asset.Color->Values.Shift, asset.Color->Values.Bits});
		}
		return result;
	}

	void TraceAtlas()
	{
		const auto stats = Atlas.GetStats();
		if (SidebarSnapshot::Trace && AtlasTraceCount < 64 && (stats.Submissions == 1 || stats.Submissions % 300 == 0))
		{
			++AtlasTraceCount;
			const auto decoded = ShapeFrameSource::Frames.GetStats().Decoded;
			Debug::Log("[FrameAtlas] submissions=%llu bpp=%u pages=%u retiring=%u pageBytes=%llu occupiedPixels=%llu palettes=%u inFlight=%u uploads=%llu paletteUploads=%llu indexBytes=%llu indexDelta=%llu paletteBytes=%llu paletteDelta=%llu transferBytes=%llu decoded=%llu decodedDelta=%llu evictions=%llu budgetMisses=%llu busy=%llu\n",
				stats.Submissions, stats.BytesPerPixel, stats.Pages, stats.RetiringPages, stats.PageBytes, stats.OccupiedPixels, stats.Palettes, stats.InFlight,
				stats.FrameUploads, stats.PaletteUploads, stats.IndexBytes, stats.IndexBytes - LastAtlasIndexBytes, stats.PaletteBytes,
				stats.PaletteBytes - LastAtlasPaletteBytes, stats.TransferBytes, decoded, decoded - LastAtlasDecoded, stats.Evictions, stats.BudgetMisses, stats.Busy);
			LastAtlasIndexBytes = stats.IndexBytes; LastAtlasPaletteBytes = stats.PaletteBytes; LastAtlasDecoded = decoded;
		}
	}

	void ReleaseCursorResources()
	{
		if (CursorTexture)
			SDL_ReleaseGPUTexture(Device, CursorTexture);
		if (CursorTransfer)
			SDL_ReleaseGPUTransferBuffer(Device, CursorTransfer);
		CursorTexture = nullptr;
		CursorTransfer = nullptr;
		UploadedWidth = UploadedHeight = 0;
		CursorDirty = false;
		CursorFrame.Visible = false;
	}

	bool PrepareCursor()
	{
		CursorSnapshot::Capture(CursorFrame);
		if (!CursorFrame.Visible)
			return true;
		if (!CursorTexture)
		{
			SDL_GPUTextureCreateInfo texture {};
			texture.type = SDL_GPU_TEXTURETYPE_2D;
			texture.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
			texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			texture.width = texture.height = CursorImage::MaxDimension;
			texture.layer_count_or_depth = texture.num_levels = 1;
			CursorTexture = SDL_CreateGPUTexture(Device, &texture);
			SDL_GPUTransferBufferCreateInfo transfer {};
			transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer.size = static_cast<Uint32>(sizeof(UploadedCursor));
			CursorTransfer = SDL_CreateGPUTransferBuffer(Device, &transfer);
			if (!CursorTexture || !CursorTransfer)
				return false;
			Debug::Log("[GPURender] Created the independent cursor texture (%dx%d first frame).\n",
				CursorFrame.Width, CursorFrame.Height);
		}

		const size_t bytes = size_t(CursorFrame.Width) * CursorFrame.Height * sizeof(std::uint32_t);
		if (CursorFrame.Width != UploadedWidth || CursorFrame.Height != UploadedHeight
			|| std::memcmp(CursorFrame.Pixels.data(), UploadedCursor.data(), bytes) != 0)
		{
			auto* destination = SDL_MapGPUTransferBuffer(Device, CursorTransfer, true);
			if (!destination)
				return false;
			std::memcpy(destination, CursorFrame.Pixels.data(), bytes);
			SDL_UnmapGPUTransferBuffer(Device, CursorTransfer);
			std::memcpy(UploadedCursor.data(), CursorFrame.Pixels.data(), bytes);
			UploadedWidth = CursorFrame.Width;
			UploadedHeight = CursorFrame.Height;
			CursorDirty = true;
		}
		return true;
	}

	void UploadCursor(SDL_GPUCopyPass* pass)
	{
		SDL_GPUTextureTransferInfo source {};
		source.transfer_buffer = CursorTransfer;
		source.pixels_per_row = UploadedWidth;
		source.rows_per_layer = UploadedHeight;
		SDL_GPUTextureRegion destination {};
		destination.texture = CursorTexture;
		destination.w = UploadedWidth;
		destination.h = UploadedHeight;
		destination.d = 1;
		SDL_UploadToGPUTexture(pass, &source, &destination, true);
	}

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
	SidebarSnapshot::Invalidate();
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
	char mirror[2] {};
	MirrorMode = GetEnvironmentVariableA("PHOBOS_GPU_MIRROR", mirror, sizeof(mirror)) == 1 && mirror[0] == '1';
	SidebarSnapshot::InitializeTrace();
	Debug::Log("[GPURender] Output: %s.\n", MirrorMode ? "diagnostic CPU mirror" : "black background + GPU sidebar + GPU cursor");

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
	if (!MirrorMode && !Atlas.Create(Device, {})) return Fail("Creating the indexed frame atlas");
	AtlasTraceCount = 0;
	LastAtlasIndexBytes = LastAtlasPaletteBytes = 0;
	LastAtlasDecoded = ShapeFrameSource::Frames.GetStats().Decoded;

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
		ReleaseCursorResources();
		Atlas.Release();
		SidebarSnapshot::Invalidate();

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

	if (MirrorMode)
	{
		if (SurfaceMirror::Capture(Device) == SurfaceMirror::CaptureResult::Failed)
			return Fail("Capturing the game screen");
	}
	else
	{
		const auto prepared = PrepareSidebar();
		if (prepared == FrameAtlas::BeginResult::Busy) return;
		if (prepared == FrameAtlas::BeginResult::Failed) return Fail("Preparing the GPU sidebar atlas");
		if (!PrepareCursor()) return Fail("Preparing the GPU cursor");
		if (CursorFrame.Visible)
			Sprites.push_back({CursorTexture, CursorFrame.X, CursorFrame.Y, CursorFrame.Width, CursorFrame.Height, 0, 0});
	}

	SDL_GPUCommandBuffer* pCommandBuffer = SDL_AcquireGPUCommandBuffer(Device);

	if (!pCommandBuffer)
		return Fail("SDL_AcquireGPUCommandBuffer");

	if (MirrorMode ? SurfaceMirror::HasPendingUpload() : (CursorDirty || Atlas.HasUploads()))
	{
		SDL_GPUCopyPass* pCopyPass = SDL_BeginGPUCopyPass(pCommandBuffer);
		if (!pCopyPass)
		{
			SDL_CancelGPUCommandBuffer(pCommandBuffer);
			return Fail("SDL_BeginGPUCopyPass");
		}
		if (MirrorMode)
			SurfaceMirror::Upload(pCopyPass);
		else
		{
			Atlas.EncodeUploads(pCopyPass);
			if (CursorDirty) UploadCursor(pCopyPass);
		}
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

	bool drawn = true;
	if (pSwapchainTexture)
	{
		drawn = MirrorMode
			? PresentPass::Draw(pCommandBuffer, pSwapchainTexture, swapchainWidth, swapchainHeight,
				SurfaceMirror::GetTexture(), SurfaceMirror::GetWidth(), SurfaceMirror::GetHeight())
			: PresentPass::DrawSprites(pCommandBuffer, pSwapchainTexture, swapchainWidth, swapchainHeight,
				DSurface::WindowBounds.Width, DSurface::WindowBounds.Height, Sprites);
	}

	// Acquiring a swapchain texture forbids Cancel, even if beginning the render pass failed.
	if (!(MirrorMode ? SDL_SubmitGPUCommandBuffer(pCommandBuffer) : Atlas.Submit(pCommandBuffer)))
		Fail("SDL_SubmitGPUCommandBuffer");
	else
	{
		CursorDirty = false;
		if (!MirrorMode) TraceAtlas();
		if (!drawn) Fail("Drawing the GPU output", "Could not begin the render pass");
	}
}

void GPURender::OnSidebarDraw()
{
	if (!MirrorMode && (CurrentState == State::Running || CurrentState == State::Hidden))
		SidebarSnapshot::CaptureAfterDraw();
}

void GPURender::InvalidateSidebar(bool resources)
{
	SidebarSnapshot::Invalidate();
	if (resources)
	{
		ShapeFrameSource::Clear();
		SidebarSnapshot::LowerButtonsReady = false;
		SidebarSnapshot::RadarButtonsReady = false;
	}
}

void GPURender::SetSidebarButtonsReady(bool radar, bool ready)
{
	SidebarSnapshot::Invalidate();
	if (!ready)
	{
		const size_t begin = radar ? 8 : 0;
		const size_t end = radar ? SidebarSnapshot::ButtonCount : 8;
		for (size_t i = begin; i < end; ++i)
			ShapeFrameSource::Invalidate(SidebarSnapshot::Current.Assets[SidebarSnapshot::BackgroundCount + i].SourceIdentity);
	}
	(radar ? SidebarSnapshot::RadarButtonsReady : SidebarSnapshot::LowerButtonsReady) = ready;
}

void GPURender::InvalidateSidebarPalette()
{
	// Color-table lifetime changes do not change immutable SHP indices or FrameIDs.
	SidebarSnapshot::Invalidate();
	SidebarSnapshot::LowerButtonsReady = false;
	SidebarSnapshot::RadarButtonsReady = false;
}

void GPURender::OnShapeInvalidating(const void* shape)
{
	if (ShapeFrameSource::Invalidate(reinterpret_cast<std::uintptr_t>(shape)))
		SidebarSnapshot::Invalidate();
}

void GPURender::OnButtonShapeChanging(const void* object)
{
	const auto* button = static_cast<const ShapeButtonClass*>(object);
	// SetShape/ClearShape free owned data directly, bypassing SHP destructors.
	if (button->IsShapeLoaded) OnShapeInvalidating(button->ShapeData);
	if (std::find(std::begin(SidebarSnapshot::ButtonObjects), std::end(SidebarSnapshot::ButtonObjects),
		reinterpret_cast<std::uintptr_t>(object)) != std::end(SidebarSnapshot::ButtonObjects))
		SidebarSnapshot::Invalidate();
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
	CursorTrace::Poll();
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

	SecondaryWindow::UpdatePosition();

	// Refresh the independent cursor position even when the scene has not blitted.
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
