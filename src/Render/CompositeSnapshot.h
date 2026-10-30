#pragma once

#include <GeneralStructures.h>
#include <cstdint>
#include <vector>

class DSurface;

// The native surface the tactical mode 2 draws its layers on (gamemd-reverse #23 §9): 6D3D10 calls
// RenderLayers (6D8DB0) at 6D465F with its target locked as Temp, and the Spotlights at 6D4664 right
// after it. Copied there, before and after the layers, as the reference of the animated Terrain
// (stage D2) and of Automation's native_composite. Only the ViewBounds rectangle, which mode 1 filled
// from Tile; under the layers' conditions of #23 §9.2 (no units, no selection, no effects) nothing
// else has drawn into it yet.
namespace CompositeSnapshot
{
	// 6D3D10's target: Temp at 887314 while the layers draw, and DSurface's lock count, bytes per
	// pixel and buffer (4BAD80).
	constexpr std::uintptr_t TargetSlot = 0x887314;
	constexpr int NativeBytesPerPixel = 2;

	struct Copy
	{
		// ViewBounds of the target, top-down and tightly packed.
		std::vector<std::uint16_t> Pixels;
		RectangleStruct Bounds {};
		std::uintptr_t Surface = 0;
		int SurfaceWidth = 0, SurfaceHeight = 0, TacticalX = 0, TacticalY = 0;
		// Unsorted::CurrentFrame when copied.
		int Frame = -1;
		bool Valid = false;
		// Why the last attempt copied nothing; empty when Valid.
		const char* Error = "composite_not_copied";
	};
	// 6D465F (only for a traced render) and 6D4664.
	extern Copy Input, Output;
	// Renders whose copy failed.
	extern unsigned Failed;

	// EBX at 6D465F / 6D4664, 6D3D10's target. Requires it to be Temp and Composite, locked, 16 bit,
	// with ViewBounds inside it; otherwise leaves the copy invalid with the reason.
	void Record(Copy& copy, DSurface* surface);
	void Invalidate();
}
