#pragma once

#include "SidebarText.h"
#include <array>
#include <climits>
#include <string>
#include <vector>

class REGISTERS;
struct RectangleStruct;
struct ColorStruct;

// Owned output of the native CCToolTip::Draw2 (478E30). Each call fills one
// rectangle, outlines it and wraps the final text into the selected surface.
namespace Tooltip
{
	enum class Target : unsigned char { None, Composite, Sidebar };

	struct Draw
	{
		Target Surface = Target::None;
		// Logical canvas X of the target surface's origin, and its size for clipping.
		int OriginX = 0, Width = 0, Height = 0;
		// Native order: background, four border edges, then glyphs.
		std::vector<SidebarText::Command> Commands;
		std::wstring Text;
		bool Known = true, Blur = false;
	};

	struct Edge { int X, Y, Width, Height; };

	// FillRectTrans (4BB830) clamps 0..100 unsigned and blends (a*src+(255-a)*dst)>>8
	// with a=255*p/100. Only p=100 leaves no destination contribution.
	constexpr unsigned Opacity256(int percent)
	{
		const unsigned clamped = unsigned(percent) > 100 ? 100 : unsigned(percent);
		return clamped == 100 ? 256 : 255 * clamped / 100;
	}

	// DrawRect outlines the inclusive cells x..x+w-1, y..y+h-1 as four
	// non-overlapping one-pixel edges. Empty edges have zero size.
	constexpr std::array<Edge, 4> Border(int x, int y, int width, int height)
	{
		if (width <= 0 || height <= 0) return {};
		const int inner = height > 2 ? height - 2 : 0;
		return {Edge {x, y, width, 1}, Edge {x, y + height - 1, height > 1 ? width : 0, 1},
			Edge {x, y + 1, 1, inner}, Edge {x + width - 1, y + 1, width > 1 ? 1 : 0, inner}};
	}

	// Inverse of the Draw2 surface selection. Mode 1 puts the sidebar right of
	// the tactical view; other modes put it left. Y is never translated.
	constexpr int CanvasOrigin(Target target, int sidebarMode, int viewRight, int sidebarRight)
	{
		if (target == Target::Sidebar) return sidebarMode == 1 ? viewRight : 0;
		if (target == Target::Composite) return sidebarMode == 1 ? 0 : sidebarRight;
		return INT_MIN;
	}

	// Entry 478E30 and the shared epilogue 479042.
	void BeginDraw(REGISTERS* registers);
	void EndDraw(REGISTERS* registers);
	// Called by the existing 478FDC hook after Phobos chose the final rectangle and fill.
	void ObserveFill(REGISTERS* registers, const RectangleStruct* rect, const ColorStruct& color, int opacity, bool blur);
	// True when the Blit belongs to an open Draw2 scope; the sidebar observer must then skip it.
	bool ObserveGlyph(REGISTERS* registers);
	// The sidebar draw (6A70AE) and the top-level tooltip pass (after 4F4573) replace
	// their own target's draws, so hidden tooltips publish nothing.
	void PublishSidebar();
	void PublishTopLevel();
	void Clear();
	const std::vector<Draw>& Published(Target target);
}
