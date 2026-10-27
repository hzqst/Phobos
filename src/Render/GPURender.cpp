#include "GPURender.h"
#include "SDLLoader.h"

#include <CCINIClass.h>
#include <GameOptionsClass.h>

#include <Phobos.h>
#include <Utilities/Debug.h>

#include <SDL3/SDL_error.h>

#include <Windows.h>

#include <cwchar>
#include <iterator>
#include <utility>

bool GPURender::Enabled = true;
bool GPURender::DisabledByCommandLine = false;
GPURender::DriverPreference GPURender::Driver = GPURender::DriverPreference::Auto;
GPURender::State GPURender::CurrentState = GPURender::State::Disabled;

namespace
{
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
}

void GPURender::LoadSettings(CCINIClass& ini, const char* pSection)
{
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

void GPURender::SetState(State state)
{
	if (state == CurrentState)
		return;

	Debug::Log("[GPURender] State: %s -> %s.\n", GetStateName(CurrentState), GetStateName(state));
	CurrentState = state;
}

void GPURender::Fail(const char* pWhat)
{
	Debug::Log("[GPURender] %s failed: %s\n", pWhat, SDLLoader::IsLoaded() ? SDL_GetError() : "SDL3.dll is not loaded.");
	SetState(State::Failed);
}
