#include <Render/ShapeImage.h>
#include <Render/CursorImage.h>
#include <cassert>
#include <climits>
#include <cstdio>

int main()
{
	using ShapeImage::CameoSlot;
	using ShapeImage::PlaceCameo;
	CameoSlot p;
	assert(PlaceCameo(0, 0, 3, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	assert(0 == p.Item && 22 == p.X && 70 == p.Y && 82 == p.ClipWidth && 118 == p.ClipHeight);
	assert(PlaceCameo(1, 0, 3, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	assert(1 == p.Item && 85 == p.X && 145 == p.ClipWidth);
	assert(PlaceCameo(0, 2, 10, 878, 69, 64, 50, true, 25, 856, 168, 768, p));
	assert(4 == p.Item && 22 == p.X && 45 == p.Y);
	assert(PlaceCameo(2, 2, 10, 878, 69, 64, 50, true, 25, 856, 168, 768, p));
	assert(6 == p.Item && 95 == p.Y);
	assert(!PlaceCameo(3, 0, 3, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	assert(!PlaceCameo(60, 0, 75, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	assert(!PlaceCameo(0, -1, 75, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	assert(!PlaceCameo(0, INT_MAX, 75, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	assert(!PlaceCameo(0, 0, 75, INT_MAX, 69, 63, 50, false, 0, -1, 168, 768, p));
	assert(PlaceCameo(0, 0, 1, -20, -10, 63, 50, false, 0, 0, 168, 768, p));
	assert(40 == p.ClipWidth && 39 == p.ClipHeight); // Native bounds clip right/bottom, not cell left/top.
	assert(PlaceCameo(0, 0, 1, 150, 750, 63, 50, false, 0, 0, 168, 768, p));
	assert(168 == p.ClipWidth && 768 == p.ClipHeight);
	assert(PlaceCameo(0, 0, 1, 22, 69, 63, 50, false, 0, 0, 168, 768, p));
	const auto negativeOffset = CursorImage::Clip(p.X - 30, p.Y - 80, 100, 100, p.ClipWidth, p.ClipHeight);
	assert(0 == negativeOffset.X && 0 == negativeOffset.Y && 8 == negativeOffset.SourceX && 10 == negativeOffset.SourceY);
	assert(82 == negativeOffset.Width && 90 == negativeOffset.Height);
	const auto rightCrop = CursorImage::Clip(p.X + 5, p.Y + 3, 60, 48, p.ClipWidth, p.ClipHeight);
	assert(55 == rightCrop.Width && 45 == rightCrop.Height && 0 == rightCrop.SourceX && 0 == rightCrop.SourceY);
	std::puts("Cameo layout: slots, scrolling, origin, native clip and invalid bounds passed.");
}
