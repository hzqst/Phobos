#include "GPURender.h"

#include <Utilities/Macro.h>

// WinMain has set the initial display mode and allocated the global surfaces.
// The first display setup does not go through Set_Video_Mode.
DEFINE_HOOK(0x6BDE19, WinMain_AllocateSurfaces_GPURender, 0x5)
{
	GPURender::OnVideoModeSet();
	return 0;
}

// Set_Video_Mode has created the surfaces for the new resolution and is about to return true.
// Reached on every later successful mode change, windowed and fullscreen alike.
DEFINE_HOOK(0x561163, Set_Video_Mode_GPURender, 0x5)
{
	GPURender::OnVideoModeSet();
	return 0;
}

// GScreenClass::DoBlit has copied the surface to the primary surface.
// Every exit with a completed blit passes here, the early ones failed to get the window rect.
DEFINE_HOOK(0x4F4B7E, GScreenClass_DoBlit_GPURender, 0x5)
{
	GPURender::OnBlit();
	return 0;
}

// WinMain is exiting and about to release the global surfaces and DirectDraw.
DEFINE_HOOK(0x6BB8E0, DirectDrawWrap_DisposeOfStuff_GPURender, 0x5)
{
	GPURender::Shutdown();
	return 0;
}
