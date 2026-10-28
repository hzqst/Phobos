#include "GPURender.h"
#include "CursorTrace.h"

#include <Utilities/Macro.h>

// WinMain has set the initial display mode and allocated the global surfaces.
// The first display setup does not go through Set_Video_Mode.
DEFINE_HOOK(0x6BDE19, WinMain_AllocateSurfaces_GPURender, 0x5)
{
	CursorTrace::Initialize();
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
	CursorTrace::Record(R);
	GPURender::OnBlit();
	return 0;
}

// WinMain is exiting and about to release the global surfaces and DirectDraw.
DEFINE_HOOK(0x6BB8E0, DirectDrawWrap_DisposeOfStuff_GPURender, 0x5)
{
	CursorTrace::Flush();
	GPURender::Shutdown();
	return 0;
}

// Observational probes: return 0 preserves every displaced instruction via SyringeEx.
// No outer MouseMutex transaction is introduced by this diagnostic stage.
// Intentionally stacked with GScreenClass_DrawText: both return 0 and the observation
// does not depend on which callback runs first, only on preceding the native staging.
DEFINE_HOOK(0x4F4583, DrawOnTop_CursorTraceBegin, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x4F459D, DrawOnTop_CursorTraceEnd, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x4F4780, DoBlit_CursorTraceBegin, 0x7)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x4F4B75, DoBlit_CursorTraceCopy, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x4F4BA4, DoBlit_CursorTraceEnd, 0x7)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B90C0, WWMouse_CursorTraceStage, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B9182, WWMouse_CursorTraceWait, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B9193, WWMouse_CursorTraceWaitResult, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B91FF, WWMouse_CursorTraceStageSaved, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B928A, WWMouse_CursorTracePrimaryUpdated, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B9299, WWMouse_CursorTraceRelease, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B9022, WWMouse_CursorTracePrimarySave, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7B93F0, WWMouse_CursorTraceDraw, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7BA20B, WWMouse_CursorTraceRestored, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7BA273, WWMouse_CursorTraceSaved, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x7BA282, WWMouse_CursorTraceDrawn, 0x6)
{
	CursorTrace::Record(R);
	return 0;
}

DEFINE_HOOK(0x560BF0, SetVideoMode_CursorTraceInvalidate, 0x5)
{
	CursorTrace::Record(R);
	return 0;
}
