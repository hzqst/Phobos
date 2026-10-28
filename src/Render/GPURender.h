#pragma once

#include <Windows.h>

// Draws independent game elements on black in a secondary SDL_GPU window. The old screen
// mirror is an opt-in diagnostic mode. SDL is only included inside src\Render, so this header
// must stay free of SDL types.
class GPURender
{
public:
	enum class State
	{
		Disabled,     // not started, or turned off by the settings
		Initializing,
		Running,
		Hidden,       // window closed by the user, the device is kept
		Failed,       // unrecoverable error, everything is released for the rest of the process
		Shutdown,
	};

	enum class DriverPreference
	{
		Auto,         // D3D12 first, Vulkan if that fails
		Direct3D12,
		Vulkan,
	};

	// -NoGPURender, takes precedence over the settings
	static void DisableByCommandLine();

	// The game surfaces have been (re)created for a resolution, at startup or by Set_Video_Mode:
	// starts the GPU render on the first call, then follows the resolution
	static void OnVideoModeSet();
	// Releases everything before DirectDraw goes away on exit
	static void Shutdown();
	// The game has blitted a surface to the primary surface
	static void OnBlit();
	static void OnSidebarDraw();
	static void InvalidateSidebar(bool resources = false);
	static void SetSidebarButtonsReady(bool radar, bool ready);

	static State GetState();

private:
	// [Phobos] GPURender.Enable / GPURender.Driver in RA2MD.INI
	static void LoadSettings();
	static bool ShouldStart();
	static bool IsDisplayEnvironmentSupported();
	static bool IsDirectDrawWrapped();

	static void Initialize(int width, int height);
	static void ReleaseResources();
	static void Present();
	static void RequestPresent();
	static void CALLBACK OnTimer(HWND hWnd, UINT message, UINT_PTR idEvent, DWORD time);

	static void SetState(State state);
	// Logs pReason, or SDL_GetError() without one, releases everything and enters Failed
	static void Fail(const char* pWhat, const char* pReason = nullptr);

	static bool Enabled;
	static bool DisabledByCommandLine;
	static DriverPreference Driver;
	static State CurrentState;
	static UINT_PTR TimerID;
};
