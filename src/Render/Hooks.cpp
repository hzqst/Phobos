#include "GPURender.h"
#include "CursorTrace.h"
#include "OverlaySnapshot.h"
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

// 6D3D10's mode 2 around RenderLayers: the call at 6D465F and the Spotlights' call after it, both
// displaced whole and kept by return 0 (gamemd-reverse #23 §9.2). EBX is the locked target.
DEFINE_HOOK(0x6D465F, TacticalClass_Render_LayersBegin, 0x5)
{
	GET(DSurface*, pSurface, EBX);
	GPURender::OnRenderLayers(pSurface, false);
	return 0;
}

DEFINE_HOOK(0x6D4664, TacticalClass_Render_LayersEnd, 0x5)
{
	GET(DSurface*, pSurface, EBX);
	GPURender::OnRenderLayers(pSurface, true);
	return 0;
}

// The end of 6D3D10's Techno loop (action lines 6D4750, psychic and capture links), reached from 6D4664 on every
// path; before the displaced mov al / test al. Ends the recording that goes on after RenderLayers.
DEFINE_HOOK(0x6D4912, TacticalClass_Render_TechnoLinesEnd, 0x6)
{
	OverlaySnapshot::EndRecording();
	return 0;
}

// 4AED70 (CC_Draw_Shape) while RenderLayers draws: each call as passed, before the displaced
// sub esp, for OverlaySnapshot's Composite list. A call without a shape, point or clip draws nothing.
DEFINE_HOOK(0x4AED70, CC_Draw_Shape_RecordLayers, 0x6)
{
	if (!OverlaySnapshot::RecordingTarget || OverlaySnapshot::RecordingAfter) return 0;
	GET(DSurface*, pSurface, ECX);
	GET(ConvertClass*, pConvert, EDX);
	GET_STACK(std::uintptr_t, caller, 0x0);
	GET_STACK(SHPStruct*, pShape, 0x4);
	GET_STACK(Point2D*, pPoint, 0xC);
	GET_STACK(RectangleStruct*, pBounds, 0x10);
	if (!pSurface || !pShape || !pPoint || !pBounds) return 0;
	OverlaySnapshot::RecordedCall call;
	call.Convert = pConvert;
	call.Image = pShape;
	call.Frame = R->Stack<int>(0x8);
	call.Point = *pPoint;
	call.Bounds = *pBounds;
	call.Flags = R->Stack<std::uint32_t>(0x14);
	call.Remap = R->Stack<int>(0x18);
	call.ZAdjust = R->Stack<int>(0x1C);
	call.Gradient = R->Stack<int>(0x20);
	call.Brightness = R->Stack<int>(0x24);
	call.Tint = R->Stack<int>(0x28);
	call.ZShape = R->Stack<SHPStruct*>(0x2C);
	call.ZShapeFrame = R->Stack<int>(0x30);
	call.ZShapeX = R->Stack<int>(0x34);
	call.ZShapeY = R->Stack<int>(0x38);
	call.Caller = caller;
	OverlaySnapshot::RecordCall(call, pSurface);
	return 0;
}

// 4373B0 and 437A10 while RenderLayers draws, from any caller but 4AED70, whose call is recorded already:
// the voxels' blits (4AF2A0, 707480) and the vehicles' scratch (73B140), before the displaced sub esp.
// Both take the destination in ECX, its clip in EDX, then the place, the source, the source's rectangle,
// the part to draw, the blitter, Z, gradient and brightness; 4373B0 then the tint, 437A10 its unused
// value, the Z adjust surface and its offset before the tint.
static void RecordBlit(REGISTERS* R, bool compressed)
{
	if (!OverlaySnapshot::RecordingTarget || OverlaySnapshot::RecordingAfter) return;
	GET_STACK(std::uintptr_t, caller, 0x0);
	if (caller > OverlaySnapshot::DrawShapeBegin && caller < OverlaySnapshot::DrawShapeEnd) return;
	OverlaySnapshot::BlitCall call;
	call.Surface = R->ECX<DSurface*>();
	call.Bounds = R->EDX<RectangleStruct*>();
	call.Dest = R->Stack<RectangleStruct*>(0x4);
	call.Source = R->Stack<Surface*>(0x8);
	call.Limit = R->Stack<RectangleStruct*>(0xC);
	call.Part = R->Stack<RectangleStruct*>(0x10);
	call.Blitter = R->Stack<void*>(0x14);
	call.ZAdjust = R->Stack<int>(0x18);
	call.Gradient = R->Stack<int>(0x1C);
	call.Brightness = R->Stack<int>(0x20);
	call.Tint = R->Stack<int>(compressed ? 0x34 : 0x24);
	call.ZSurface = compressed ? R->Stack<void*>(0x28) : nullptr;
	call.Caller = caller;
	call.Compressed = compressed;
	if (!call.Surface || !call.Bounds || !call.Dest || !call.Limit || !call.Part || !call.Blitter) return;
	OverlaySnapshot::RecordBlit(call);
}

DEFINE_HOOK(0x4373B0, Copy_To_Surface_RecordLayers, 0x7)
{
	RecordBlit(R, false);
	return 0;
}

DEFINE_HOOK(0x437A10, Copy_To_Surface_RLE_RecordLayers, 0x7)
{
	RecordBlit(R, true);
	return 0;
}

// Retail UnitClass::DrawAsVXL: before sub esp,1B4h, and after the final scratch clear.
// Four saved registers plus locals put the original entry ESP at epilogue ESP+1C4h.
// Shadow observation only; both hooks preserve the displaced instructions and registers.
DEFINE_HOOK(0x73B470, UnitClass_DrawAsVXL_IdentityBegin, 0x6)
{
	if (OverlaySnapshot::RecordingTarget && !OverlaySnapshot::RecordingAfter)
		OverlaySnapshot::BeginVoxelIdentity(R->ESP());
	return 0;
}

DEFINE_HOOK(0x73C5DC, UnitClass_DrawAsVXL_IdentityEnd, 0xA)
{
	if (OverlaySnapshot::RecordingTarget && !OverlaySnapshot::RecordingAfter)
		OverlaySnapshot::EndVoxelIdentity(R->ESP() + 0x1C4);
	return 0;
}

// 5F99E0 frees every type's voxel cache and resets the arena the entries live in.
DEFINE_HOOK(0x5F99E0, DestroyVoxelCaches_Purge, 0x5)
{
	BlitFrameSource::Purged();
	return 0;
}

// The lines and fills RenderLayers draws on its target, before each displaced sub esp: the building selection
// boxes (6F5190 -> 6F5EF0 -> 6DBB60) and the like through 4BFD30, which takes the clip, both ends relative to
// it, the color, the Z at each end and whether to write Z; plain lines through 7BA610 (DrawLine, DrawRect),
// which takes the clip, both ends and the color; fills through 4BB620, which takes the clip, the rectangle
// and the color. All take the surface in ECX.
DEFINE_HOOK(0x4BFD30, DSurface_DrawLineColor_AZ_RecordLayers, 0x5)
{
	if (!OverlaySnapshot::RecordingTarget) return 0;
	GET(DSurface*, pSurface, ECX);
	GET_STACK(RectangleStruct*, pClip, 0x4);
	GET_STACK(Point2D*, pFrom, 0x8);
	GET_STACK(Point2D*, pTo, 0xC);
	if (!pSurface || !pClip || !pFrom || !pTo) return 0;
	OverlaySnapshot::RecordedLine line;
	line.Clip = *pClip;
	line.From = *pFrom;
	line.To = *pTo;
	line.Color = R->Stack<std::uint16_t>(0x10);
	line.ZFrom = R->Stack<int>(0x14);
	line.ZTo = R->Stack<int>(0x18);
	line.WritesDepth = R->Stack<std::uint8_t>(0x1C) != 0;
	line.Caller = R->Stack<std::uintptr_t>(0x0);
	OverlaySnapshot::RecordLine(line, pSurface);
	return 0;
}

DEFINE_HOOK(0x7BA610, XSurface_DrawLineEx_RecordLayers, 0x6)
{
	if (!OverlaySnapshot::RecordingTarget) return 0;
	GET(DSurface*, pSurface, ECX);
	GET_STACK(RectangleStruct*, pClip, 0x4);
	GET_STACK(Point2D*, pFrom, 0x8);
	GET_STACK(Point2D*, pTo, 0xC);
	if (!pSurface || !pClip || !pFrom || !pTo) return 0;
	OverlaySnapshot::RecordedLine line;
	line.Kind = OverlaySnapshot::Primitive::PlainLine;
	line.Clip = *pClip;
	line.From = *pFrom;
	line.To = *pTo;
	line.Color = R->Stack<std::uint16_t>(0x10);
	line.Caller = R->Stack<std::uintptr_t>(0x0);
	OverlaySnapshot::RecordLine(line, pSurface);
	return 0;
}

// 4C0750 (DSurface::DrawDashedLine_), before the displaced sub esp / mov eax: the action lines' dashed part (7049C0).
// It takes both ends on the surface (its caller cut them to ViewBounds), the color, a pattern of 16 bools, the
// offset into it, and whether it draws where A is 0 instead of where it is not. Not the clip.
DEFINE_HOOK(0x4C0750, DSurface_DrawDashedLine_RecordLayers, 0x7)
{
	if (!OverlaySnapshot::RecordingTarget) return 0;
	GET(DSurface*, pSurface, ECX);
	GET_STACK(Point2D*, pFrom, 0x4);
	GET_STACK(Point2D*, pTo, 0x8);
	GET_STACK(const bool*, pPattern, 0x10);
	if (!pSurface || !pFrom || !pTo || !pPattern) return 0;
	OverlaySnapshot::RecordedLine line;
	line.Kind = OverlaySnapshot::Primitive::Dashed;
	line.From = *pFrom;
	line.To = *pTo;
	line.Color = R->Stack<std::uint16_t>(0xC);
	for (int i = 0; i < 16; ++i) line.Pattern |= std::uint16_t(pPattern[i] ? 1u << i : 0u);
	line.Offset = R->Stack<int>(0x14);
	line.Shrouded = R->Stack<std::uint8_t>(0x18) != 0;
	line.Caller = R->Stack<std::uintptr_t>(0x0);
	OverlaySnapshot::RecordLine(line, pSurface);
	return 0;
}

// 4BB620 draws nothing for an empty rectangle.
DEFINE_HOOK(0x4BB620, DSurface_FillRectEx_RecordLayers, 0x6)
{
	if (!OverlaySnapshot::RecordingTarget) return 0;
	GET(DSurface*, pSurface, ECX);
	GET_STACK(RectangleStruct*, pClip, 0x4);
	GET_STACK(RectangleStruct*, pFill, 0x8);
	if (!pSurface || !pClip || !pFill || pFill->Width <= 0 || pFill->Height <= 0) return 0;
	OverlaySnapshot::RecordedLine line;
	line.Kind = OverlaySnapshot::Primitive::Fill;
	line.Clip = *pClip;
	line.Fill = *pFill;
	line.Color = R->Stack<std::uint16_t>(0xC);
	line.Caller = R->Stack<std::uintptr_t>(0x0);
	OverlaySnapshot::RecordLine(line, pSurface);
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
	GPURender::InvalidateTerrain();
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

// BitFont::Lock (4348F0), before the displaced sub esp / push esi / push edi: the surface the font's following
// 434120 glyphs write to, its second argument (ECX is the font). Kept only while RenderLayers draws.
static const void* GlyphSurface = nullptr;

DEFINE_HOOK(0x4348F0, BitFont_Lock_RecordLayers, 0x5)
{
	GlyphSurface = OverlaySnapshot::RecordingTarget ? R->Stack<const void*>(0x4) : nullptr;
	return 0;
}

// While RenderLayers draws, its text's glyphs (70AA60's primary and power labels through 4A5EB0 -> 434B90 ->
// 434500) go to its list; SidebarText takes the bitmap, place, font bounds and color as for the sidebar.
static void RecordGlyph(REGISTERS* R)
{
	OverlaySnapshot::RecordedLine line;
	line.Kind = OverlaySnapshot::Primitive::Glyph;
	line.Caller = R->Stack<std::uintptr_t>(0x0);
	SidebarText::Command command;
	const auto result = SidebarText::CaptureGlyph(R, command);
	// A tab moves the pen and draws nothing.
	if (result == SidebarText::GlyphResult::Skipped) return;
	if (result == SidebarText::GlyphResult::Captured)
	{
		line.Glyph = command.Glyph;
		line.From = {command.X, command.Y};
		line.Fill = {0, 0, command.Width, command.Height};
		line.Clip = {command.ClipLeft, command.ClipTop, command.ClipRight - command.ClipLeft, command.ClipBottom - command.ClipTop};
		line.Color = std::uint16_t(command.Color);
	}
	OverlaySnapshot::RecordLine(line, static_cast<DSurface*>(const_cast<void*>(GlyphSurface)));
}

DEFINE_HOOK(0x434120, BitFont_Blit_GPUText, 0x5)
{
	if (OverlaySnapshot::RecordingTarget && GlyphSurface)
	{
		RecordGlyph(R);
		return 0;
	}
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
	GPURender::InvalidateTerrain(true);
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
