#include <Render/ABufferImage.h>
#include <array>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "ABufferImage failed: %s\n", what); ++Failures; }
	}
}

int main()
{
	using namespace ABufferImage;
	// Entries of 7F4194: all revealed, the upper outer edge of a map, and every neighbour unrevealed.
	std::array<std::int8_t, 256> table {};
	table[0x00] = -1;
	table[0xE0] = 9;
	table[0xFF] = -2;

	const std::uint8_t mapped[8] {8, 8, 8, 8, 8, 8, 8, 8};
	Check(Occlusion(MappedBit, mapped, false, table.data()) == -1 && FrameOf(-1) == UncoveredFrame, "all mapped");
	Check(Occlusion(0, mapped, false, table.data()) == Covered && FrameOf(Covered) == CoveredFrame, "unmapped cell");
	Check(Occlusion(0x10, mapped, false, table.data()) == Uncovered, "unmapped cell with the clear bit");
	// (-1,-1), (0,-1) and (-1,0) lie outside the map: the dark edge frame 9.
	const std::uint8_t edge[8] {0, 0, 8, 0, 8, 8, 8, 8};
	Check(OcclusionMask(edge, MappedBit) == 0xE0 && Occlusion(MappedBit, edge, false, table.data()) == 9, "upper outer edge");
	bool bits = true;
	for (int i = 0; i < 8; ++i)
	{
		std::uint8_t one[8] {8, 8, 8, 8, 8, 8, 8, 8};
		one[i] = 0;
		bits &= OcclusionMask(one, MappedBit) == Neighbours[i].Bit;
	}
	Check(bits, "one mask bit per neighbour");
	// The second pass tests bit 1; bit 0 alone counts as unrevealed and only the center falls back on it.
	const std::uint8_t centerOnly[8] {1, 1, 1, 1, 1, 1, 1, 1};
	Check(OcclusionMask(centerOnly, EdgeRevealedBit) == 0xFF, "fog neighbours ignore bit 0");
	Check(Occlusion(1, mapped, true, table.data()) == Uncovered && Occlusion(0, mapped, true, table.data()) == Covered,
		"fog fallbacks");
	const std::uint8_t revealed[8] {2, 2, 2, 2, 2, 2, 2, 2};
	Check(Occlusion(EdgeRevealedBit, revealed, true, table.data()) == -1, "fog with all revealed");
	Check(Occlusion(MappedBit | 0x100, revealed, false, table.data()) == -2, "first pass ignores CellFlags bits");
	Check(FrameOf(0xFE) == CoveredFrame && FrameOf(0xFF) == UncoveredFrame && FrameOf(9) == 9, "frames of returned bytes");

	// 6D71E0's box lies at the ground position: the terrain's main diamond at level 0.
	constexpr auto box = CellBox(10, 4, 100, 50, 16);
	static_assert(box.X == 30 * 6 - 100 - 30 && box.Y == 15 * 14 - 50 + 16 && box.Width == 60 && box.Height == 30);
	constexpr auto overlap = Intersect({0, 0, 10, 10}, {5, -5, 10, 10});
	static_assert(overlap.X == 5 && overlap.Y == 0 && overlap.Width == 5 && overlap.Height == 5);
	static_assert(Intersect({0, 0, 10, 10}, {10, 0, 5, 5}).Width == 0, "touching rectangles do not intersect");

	// Overwrite: FE keeps the old word, 0 and FF are written.
	const std::uint8_t pixels[4] {Transparent, 0, 0xFF, 60};
	const Frame frame {0, 0, 2, 2, pixels};
	std::array<std::uint16_t, 16> buffer;
	buffer.fill(ClearValue);
	const Rect area {0, 10, 4, 4};
	Check(Draw(buffer.data(), area, 1, 11, area, frame, Pass::Overwrite) == Drawn::Pixels, "overwrite draws");
	Check(buffer[5] == ClearValue && buffer[6] == 0 && buffer[9] == 0xFF && buffer[10] == 60 && buffer[0] == ClearValue, "overwrite rule");

	// Darken: the byte over a clear word, the clamped sum elsewhere, nothing above 127, no upper clamp.
	const std::uint8_t fog[4] {60, 60, 100, 128};
	buffer.fill(ClearValue);
	buffer[1] = 60;
	buffer[4] = 300;
	buffer[5] = 300;
	Check(Draw(buffer.data(), area, 0, 10, area, {0, 0, 2, 2, fog}, Pass::Darken) == Drawn::Pixels, "darken draws");
	Check(buffer[0] == 60 && buffer[1] == 0 && buffer[4] == 273 && buffer[5] == 300, "darken rule");

	// Clipping: only the part inside clip, nothing outside it.
	buffer.fill(ClearValue);
	Check(Draw(buffer.data(), area, 1, 11, {2, 10, 2, 4}, frame, Pass::Overwrite) == Drawn::Pixels
		&& buffer[5] == ClearValue && buffer[9] == ClearValue && buffer[6] == 0 && buffer[10] == 60, "clipped overwrite");
	Check(Draw(buffer.data(), area, 1, 11, {3, 10, 1, 4}, frame, Pass::Overwrite) == Drawn::Nothing, "frame left of the clip");
	Check(Draw(buffer.data(), area, 1, 11, {0, 13, 4, 1}, frame, Pass::Overwrite) == Drawn::Nothing, "frame above the clip");
	// The native test uses the bare origin, so a frame offset past the clip still enters its loops.
	Check(Draw(buffer.data(), area, 0, 10, {0, 10, 2, 2}, {3, 0, 1, 2, pixels}, Pass::Overwrite) == Drawn::Unsupported,
		"empty span after the origin test");
	Check(Draw(buffer.data(), area, 3, 13, {0, 0, 8, 20}, frame, Pass::Overwrite) == Drawn::Unsupported, "span leaves the buffer");
	if (Failures) return 1;
	std::puts("ABufferImage: occlusion, frame, box, overwrite, darken and clip rules passed.");
}
