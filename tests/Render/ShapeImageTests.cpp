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
	using namespace ShapeImage;
	std::vector<int> frames;
	// N = (L+3)/3 truncates rather than rounding up: 101 cells for 300, 167 for 500.
	CHECK(PowerFrames(300, 0, 0, 0, 0, frames));
	CHECK(101u == frames.size());
	CHECK(PowerFrames(500, 0, 0, 0, 0, frames));
	CHECK(167u == frames.size());
	CHECK(PowerFrames(12, 1, 2, 1, 0, frames)); // N=5
	CHECK((std::vector<int> {PowerEmpty, PowerExcess, PowerMargin, PowerMargin, PowerDrain} == frames));
	CHECK(PowerFrames(12, 2, 1, 1, 10, frames));
	CHECK((std::vector<int> {PowerEmpty, PowerFlash, PowerExcess, PowerMargin, PowerDrain} == frames));
	// An empty segment passes the flash cell on to the next non-empty one.
	CHECK(PowerFrames(12, 0, 2, 1, 4, frames));
	CHECK((std::vector<int> {PowerEmpty, PowerEmpty, PowerFlash, PowerMargin, PowerDrain} == frames));
	CHECK(PowerFrames(12, 0, 0, 1, 2, frames));
	CHECK((std::vector<int> {PowerEmpty, PowerEmpty, PowerEmpty, PowerEmpty, PowerFlash} == frames));
	// Odd phases draw no flash; all-empty with an even phase draws one extra cell.
	CHECK(PowerFrames(12, 0, 0, 1, 9, frames));
	CHECK((std::vector<int> {PowerEmpty, PowerEmpty, PowerEmpty, PowerEmpty, PowerDrain} == frames));
	CHECK(PowerFrames(0, 0, 0, 0, 2, frames)); // N=1
	CHECK((std::vector<int> {PowerEmpty, PowerFlash} == frames));
	// A transient sum above N draws no empty cells, as the native signed count does.
	CHECK(PowerFrames(6, 3, 0, 0, 0, frames)); // N=3
	CHECK((std::vector<int> {PowerExcess, PowerExcess, PowerExcess} == frames));
	CHECK(PowerFrames(6, 2, 0, 2, 0, frames));
	CHECK((std::vector<int> {PowerExcess, PowerExcess, PowerDrain, PowerDrain} == frames));
	CHECK(!PowerFrames(-1, 0, 0, 0, 0, frames) && frames.empty());
	CHECK(!PowerFrames(MaxPowerLayoutHeight + 1, 0, 0, 0, 0, frames));
	CHECK(!PowerFrames(12, -1, 0, 0, 0, frames));
	CHECK(!PowerFrames(12, 0, 6, 0, 0, frames));
	CHECK(PowerFrames(MaxPowerLayoutHeight, 0, 0, 0, 0, frames));
	puts("ShapeImage: plain/RLE, transparent runs, malformed input, layout and power cells passed.");
}
