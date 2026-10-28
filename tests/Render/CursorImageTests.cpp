#include <Render/CursorImage.h>

#include <cassert>
#include <climits>
#include <cstdio>

// Do not open a CRT assertion dialog in unattended test runs.
#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "Failed: %s\n", #condition); return 1; } } while (false)

int main()
{
	using namespace CursorImage;
	assert(0u == PackPixel(0, 0xFFFF));
	assert(0x10000u == PackPixel(1, 0)); // Nonzero palette index can be opaque black.
	assert(0x1F800u == PackPixel(3, 0xF800));
	const auto full = Clip(10, 20, 46, 34, 1024, 768);
	assert(10 == full.X && 20 == full.Y && 46 == full.Width && 34 == full.Height);
	assert(0 == full.SourceX && 0 == full.SourceY);
	const auto corner = Clip(-5, -7, 46, 34, 1024, 768);
	assert(0 == corner.X && 0 == corner.Y && 41 == corner.Width && 27 == corner.Height);
	assert(5 == corner.SourceX && 7 == corner.SourceY);
	const auto far = Clip(1010, 760, 46, 34, 1024, 768);
	assert(14 == far.Width && 8 == far.Height);
	assert(0 == Clip(-46, 0, 46, 34, 1024, 768).Width);
	assert(0 == Clip(1024, 0, 46, 34, 1024, 768).Width);
	assert(0 == Clip(INT_MAX, 0, 46, 34, 1024, 768).Width);
	assert(0 == Clip(INT_MIN, 0, 46, 34, 1024, 768).Width);
	assert(0 == Clip(0, 0, 46, 34, 0, 768).Width);
	assert(0 == Clip(0, 0, -1, 34, 1024, 768).Width);
	puts("CursorImage: indexed transparency, opaque black and edge/overflow clipping passed.");
}
