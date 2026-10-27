#pragma once

class CCINIClass;

// Mirrors the game screen into a secondary window rendered with SDL_GPU, as the first step
// towards drawing the game on the GPU. SDL is only included inside src\Render, so this header
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

	// [Phobos] GPURender.Enable / GPURender.Driver in RA2MD.INI
	static void LoadSettings(CCINIClass& ini, const char* pSection);
	// -NoGPURender, takes precedence over the settings
	static void DisableByCommandLine();

	static State GetState();

private:
	static bool ShouldStart();
	static bool IsDisplayEnvironmentSupported();
	static bool IsDirectDrawWrapped();

	static void SetState(State state);
	static void Fail(const char* pWhat);

	static bool Enabled;
	static bool DisabledByCommandLine;
	static DriverPreference Driver;
	static State CurrentState;
};
