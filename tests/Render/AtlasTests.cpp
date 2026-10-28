#include <Render/AtlasAllocator.h>
#include <Render/PaletteCache.h>
#include <cstdio>
#include <climits>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (false)
int main()
{
	AtlasAllocator page(8);
	const auto a = page.Allocate(3, 3), b = page.Allocate(5, 2), c = page.Allocate(8, 5);
	CHECK(a && 0 == a->X && 0 == a->Y);
	CHECK(b && 3 == b->X && 0 == b->Y);
	CHECK(c && 0 == c->X && 3 == c->Y);
	CHECK(!page.Allocate(1, 1) && !page.Allocate(0, 2) && !page.Allocate(INT_MAX, 1));
	CHECK(59u == page.UsedPixels());
	AtlasAllocator exact(4);
	CHECK(exact.Allocate(4, 4) && !exact.Allocate(1, 1));
	CHECK(256u == AtlasAllocator::RowPitch(17, 1));
	CHECK(512u == AtlasAllocator::RowPitch(65, 4));
	CHECK(1024u == AtlasAllocator::AlignedOffset(513));
	PaletteCache::Cache colors(2);
	PaletteCache::Data red {}, blue {};
	red.Colors[1] = 0xF800; blue.Colors[1] = 0x001F;
	red.Shift = blue.Shift = {11, 5, 0}; red.Bits = blue.Bits = {5, 6, 5};
	auto first = colors.Acquire(red), second = colors.Acquire(blue);
	CHECK(first && second && first->ID != second->ID);
	CHECK(first->ID == colors.Acquire(red)->ID);
	auto differentFormat = red; differentFormat.Shift[0] = 10;
	CHECK(!colors.Acquire(differentFormat)); // both versions pinned
	const auto old = second->ID; second.reset();
	auto third = colors.Acquire(differentFormat);
	CHECK(third && old != third->ID && first->ID == colors.Acquire(red)->ID);
	auto invalidFormat = red; invalidFormat.Bits[1] = 0;
	CHECK(!colors.Acquire(invalidFormat));
	std::puts("Atlas: shelf bounds/occupancy, upload alignment, palette reuse/pinning/format versions passed.");
}
