#pragma once

// SDL3.dll is linked with /DELAYLOAD, so it is only loaded by the first call into SDL, and a
// missing DLL would raise an exception right at that call. Load() must succeed before anything
// in Phobos calls an SDL function.
class SDLLoader
{
public:
	// Loads SDL3.dll and checks its version; only the first call does the work.
	static bool Load();
	static bool IsLoaded();

private:
	static bool Attempted;
	static bool Loaded;
};
