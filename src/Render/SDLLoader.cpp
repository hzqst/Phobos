#include "SDLLoader.h"

#include <Utilities/Debug.h>

#include <SDL3/SDL_version.h>

#include <Windows.h>

bool SDLLoader::Attempted = false;
bool SDLLoader::Loaded = false;

namespace
{
	constexpr auto SDLLibraryName = "SDL3.dll";

	// SDL only adds functions in minor releases, so every patch release of the minor version
	// the headers come from exports everything Phobos may call.
	constexpr int MinimumVersion = SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, 0);
	constexpr int NextMajorVersion = SDL_VERSIONNUM(SDL_MAJOR_VERSION + 1, 0, 0);
}

bool SDLLoader::Load()
{
	if (Attempted)
		return Loaded;

	Attempted = true;

	// The delay load helper searches for the DLL the same way and then reuses this module.
	// It is never freed, as the delay load thunks keep pointing into it.
	if (!LoadLibraryA(SDLLibraryName))
	{
		Debug::Log("[GPURender] %s could not be loaded (error %lu).\n", SDLLibraryName, GetLastError());
		return false;
	}

	const int version = SDL_GetVersion();

	Debug::Log("[GPURender] Loaded %s %d.%d.%d (%s).\n", SDLLibraryName,
		SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version),
		SDL_GetRevision());

	if (version < MinimumVersion || version >= NextMajorVersion)
	{
		Debug::Log("[GPURender] %s %d.%d.x is required.\n", SDLLibraryName,
			SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
		return false;
	}

	Loaded = true;
	return true;
}

bool SDLLoader::IsLoaded()
{
	return Loaded;
}
