#include <Render/ShapeImage.h>
#include <Render/CursorImage.h>
#include <cstdio>
#include <climits>
#include <array>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "Failed: %s\n", #condition); return 1; } } while (false)

int main()
{
	std::vector<std::uint8_t> pixels;
	const std::uint8_t plain[] { 1, 0, 2, 3, 4, 0 };
	CHECK(ShapeImage::Decode(plain, 3, 2, false, pixels));
	CHECK((std::vector<std::uint8_t> {1, 0, 2, 3, 4, 0} == pixels));
	const std::uint8_t rle[] { 7, 0, 1, 0, 2, 2, 3, 6, 0, 0, 0, 0, 5 };
	CHECK(ShapeImage::Decode(rle, 5, 2, true, pixels));
	CHECK((std::vector<std::uint8_t> {1, 0, 0, 2, 3, 0, 0, 0, 0, 0} == pixels));
	const std::uint8_t overflow[] {4, 0, 0, 6};
	CHECK(ShapeImage::Decode(overflow, 5, 1, true, pixels));
	CHECK((std::vector<std::uint8_t>(5, 0) == pixels));
	// Actual GCLOCK2 frame 14 row 0: terminal transparency skips one past width.
	std::array<std::uint8_t, 32> clockRow {};
	clockRow[0] = 32; clockRow[3] = 2;
	std::fill(clockRow.begin() + 4, clockRow.begin() + 30, 0x50);
	clockRow[31] = 33;
	CHECK(ShapeImage::Decode(clockRow, 60, 1, true, pixels));
	std::vector<std::uint8_t> expectedClock(60, 0);
	std::fill(expectedClock.begin() + 2, expectedClock.begin() + 28, 0x50);
	CHECK(expectedClock == pixels);
	const std::uint8_t truncated[] {3, 0, 0};
	CHECK(!ShapeImage::Decode(truncated, 1, 1, true, pixels));
	const std::uint8_t shortRow[] {4, 0, 1, 2};
	CHECK(!ShapeImage::Decode(shortRow, 3, 1, true, pixels));
	const std::uint8_t badLength[] {0, 0};
	CHECK(!ShapeImage::Decode(badLength, 1, 1, true, pixels));
	CHECK(!ShapeImage::Decode(plain, 4, 2, false, pixels));
	CHECK(!ShapeImage::Decode(plain, INT_MAX, 2, false, pixels));
	CHECK(!ShapeImage::Decode(plain, 0, 2, false, pixels));
	CHECK(0u == CursorImage::PackPixel(0, 0xFFFF));
	CHECK(0x10000u == CursorImage::PackPixel(1, 0));
	CHECK(10 == ShapeImage::RepeatCount(610, 227, 158, 0));
	CHECK(0 == ShapeImage::RepeatCount(0, 227, 158, 0));
	CHECK(0 == ShapeImage::RepeatCount(100, 227, 158, 0));
	CHECK(1 == ShapeImage::RepeatCount(150, 227, 158, 1));
	puts("ShapeImage: plain/RLE, transparent runs, malformed input and layout passed.");
}
