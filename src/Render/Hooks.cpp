#include "GPURender.h"
#include "CursorTrace.h"
#include "SidebarText.h"
#include "Tooltip.h"

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
// Also the end of the top-level tooltip pass (4F4570 returns at 4F4573).
DEFINE_HOOK(0x4F4583, DrawOnTop_CursorTraceBegin, 0x6)
{
	CursorTrace::Record(R);
	GPURender::OnToolTipsDrawn();
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
	GPURender::InvalidateSidebar();
	return 0;
}

// All native sidebar layers have finished using their resources. Copy only original
// SHP frames, before BlitSidebar publishes the CPU surface. No engine pointers escape.
DEFINE_HOOK(0x6A70AE, SidebarClass_Draw_GPURenderSnapshot, 0x5)
{
	GPURender::OnSidebarDraw();
	return 0;
}

// Strip Draw has completed its native/Ares availability and progress decisions.
// Native ESP = entry ESP - 0x48C; doDraw/+16, drawProgress/+12, completed/+14,
// stage/+38 and absolute list index/+3C are locals. BL is the final darken flag.
DEFINE_HOOK(0x6A99C3, StripClass_Draw_GPUState, 0x9)
{
	if (R->Stack<unsigned char>(0x16))
	{
		SidebarText::BeginItem(R);
		GPURender::OnCameoState(R->Stack<int>(0x3C), (R->EBX() & 0xFF) != 0,
			R->Stack<unsigned char>(0x12) != 0, R->Stack<unsigned char>(0x14) != 0, R->Stack<int>(0x38));
	}
	return 0;
}

DEFINE_HOOK(0x6AA01C, StripClass_ItemEnd_GPUText, 0x5)
{
	SidebarText::EndItem(R->ESP());
	return 0;
}

DEFINE_HOOK(0x4A5EB0, SimpleText_Print_GPUText, 0x7)
{
	SidebarText::ObservePrint(R);
	return 0;
}

// SimpleText has restored its stdcall arguments; saved registers/locals occupy 0x38.
DEFINE_HOOK(0x4A5FF2, SimpleText_PrintEnd_GPUText, 0x5)
{
	SidebarText::EndPrint(R->ESP() + 0x38);
	return 0;
}

DEFINE_HOOK(0x434120, BitFont_Blit_GPUText, 0x5)
{
	if (!Tooltip::ObserveGlyph(R))
		SidebarText::ObserveGlyph(R);
	return 0;
}

// CCToolTip::Draw2 entry, mov edx,[esp+4] / sub esp,2Ch. Records the entry ESP only.
DEFINE_HOOK(0x478E30, CCToolTip_Draw2_GPUTooltipBegin, 0x7)
{
	Tooltip::BeginDraw(R);
	return 0;
}

// Epilogue pop edi / pop esi / pop ebx / add esp,2Ch shared by every return path,
// including the early surface rejections. The entry ESP is ESP+38h here.
DEFINE_HOOK(0x479042, CCToolTip_Draw2_GPUTooltipEnd, 0x6)
{
	Tooltip::EndDraw(R);
	return 0;
}

// After the Ares entry clamp and native sub esp,2Ch / push ebp / push esi.
DEFINE_HOOK(0x621B85, TextBackground_GPUText, 0x6)
{
	SidebarText::ObserveBackground(R);
	return 0;
}

// ECX is the font at these instance entries, before any owned storage is freed.
DEFINE_HOOK_AGAIN(0x434700, BitFont_Invalidating_GPUText, 0x5)
DEFINE_HOOK_AGAIN(0x433940, BitFont_Invalidating_GPUText, 0x6)
DEFINE_HOOK(0x434A40, BitFont_Invalidating_GPUText, 0x6)
{
	SidebarText::FontInvalidating(reinterpret_cast<const void*>(R->ECX()));
	return 0;
}

// The loader takes a filename in ECX, not a font. Conservatively clear all aliases.
DEFINE_HOOK(0x433990, BitFont_Load_GPUText, 0x5)
{
	SidebarText::FontInvalidating(nullptr);
	return 0;
}

// Stacked with Phobos' CSF cleanup: both only discard their own copied strings.
DEFINE_HOOK(0x734D30, StringTable_Unload_GPUText, 0x5)
{
	SidebarText::ClearRuns();
	return 0;
}

DEFINE_HOOK(0x6A5840, SidebarClass_InitForHouse_GPURenderInvalidate, 0x6)
{
	GPURender::InvalidateSidebar(true);
	return 0;
}

DEFINE_HOOK(0x6A5BF0, SidebarClass_UnloadShapes_GPURenderInvalidate, 0x7)
{
	GPURender::InvalidateSidebar(true);
	return 0;
}

DEFINE_HOOK(0x6BE1C0, Scenario_Clear_GPURenderInvalidate, 0x8)
{
	GPURender::InvalidateSidebar(true);
	return 0;
}

DEFINE_HOOK(0x6A5BDB, SidebarClass_InitForHouse_GPURenderButtonsReady, 0x5)
{
	GPURender::SetSidebarButtonsReady(false, true);
	return 0;
}

DEFINE_HOOK(0x6530E7, RadarClass_InitIO_GPURenderButtonsReady, 0x5)
{
	GPURender::SetSidebarButtonsReady(true, true);
	return 0;
}

DEFINE_HOOK(0x652D90, RadarClass_Dispose_GPURenderButtonsInvalidate, 0x5)
{
	GPURender::SetSidebarButtonsReady(true, false);
	return 0;
}

// UI resource cleanup precedes rebinding; cached non-null button aliases are unsafe here.
DEFINE_HOOK(0x730100, UI_UnloadShapes_GPURenderInvalidate, 0x5)
{
	GPURender::InvalidateSidebar(true);
	return 0;
}

DEFINE_HOOK(0x730400, UI_UnloadPalette_GPURenderInvalidate, 0x5)
{
	GPURender::InvalidateSidebarPalette();
	return 0;
}

// At each function entry ECX is this. Observe before any original release; all
// displaced instructions run unchanged in the SyringeEx trampoline.
DEFINE_HOOK(0x69DE00, ShapeButton_SetShape_GPUFrameCache, 0x6)
{
	GPURender::OnButtonShapeChanging(reinterpret_cast<const void*>(R->ECX()));
	return 0;
}

DEFINE_HOOK(0x69DE70, ShapeButton_ClearShape_GPUFrameCache, 0x6)
{
	GPURender::OnButtonShapeChanging(reinterpret_cast<const void*>(R->ECX()));
	return 0;
}

DEFINE_HOOK(0x69E100, SHP_Unload_GPUFrameCache, 0x7)
{
	GPURender::OnShapeInvalidating(reinterpret_cast<const void*>(R->ECX()));
	return 0;
}

DEFINE_HOOK(0x69E500, SHP_Destructor_GPUFrameCache, 0x7)
{
	GPURender::OnShapeInvalidating(reinterpret_cast<const void*>(R->ECX()));
	return 0;
}

DEFINE_HOOK(0x69E930, SHP_DeletingDestructor_GPUFrameCache, 0x7)
{
	GPURender::OnShapeInvalidating(reinterpret_cast<const void*>(R->ECX()));
	return 0;
}

// EAX is the owned buffer at each direct free. Avoid Ares' destructor-entry hook.
DEFINE_HOOK_AGAIN(0x711B3D, TechnoType_FreeCameo_GPUFrameCache, 0x6)
DEFINE_HOOK(0x711B16, TechnoType_FreeCameo_GPUFrameCache, 0x6)
{
	GPURender::OnShapeInvalidating(reinterpret_cast<const void*>(R->EAX()));
	return 0;
}
