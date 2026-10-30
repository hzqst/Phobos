#include <Render/TerrainBatch.h>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "TerrainBatch failed: %s\n", what); ++Failures; }
	}
}

int main()
{
	using namespace TerrainBatch;
	const Rect view {0, 0, 856, 736};
	Instance tile;
	Check(Place(100, 200, 60, 29, 0, view, 300, 40, 53, 53, 127, true, tile), "inside the view");
	Check(tile.X == 100 && tile.Y == 200 && tile.Size == Pack(60, 29) && tile.Atlas == Pack(300, 40)
		&& tile.Frame == 0 && tile.Table == Pack(53, 53) && tile.Light == Pack(127, 1) && tile.Depth == 0, "unclipped fields");
	Check(Place(100, 200, 60, 29, 0, view, 300, 40, 53, 53, 127, true, tile, Pack(32000, 29)) && tile.Depth == Pack(32000, 29),
		"depth base and Z plane offset");
	// Clipped at the top-left corner: the atlas texel and the frame offset move together.
	Check(Place(-10, -5, 60, 29, 168, view, 300, 40, 0, 27, 3, false, tile), "clipped at the corner");
	Check(tile.X == 168 && tile.Y == 0 && tile.Size == Pack(50, 24) && tile.Atlas == Pack(310, 45)
		&& tile.Frame == Pack(10, 5) && tile.Light == Pack(3, 0), "clipped fields");
	Check(Place(840, 730, 60, 29, 0, view, 0, 0, 0, 1, 0, true, tile) && tile.Size == Pack(16, 6), "clipped at the far edge");
	Check(!Place(856, 0, 60, 29, 0, view, 0, 0, 0, 1, 0, true, tile), "right of the view");
	Check(!Place(0, -29, 60, 29, 0, view, 0, 0, 0, 1, 0, true, tile), "above the view");
	Check(!Place(0, 0, 60, 29, 0, view, 0, 0, 0, 1, -1, true, tile), "negative light row");

	const int a = 0, b = 0;
	const void* pages[] {&a, &a, &b, &b, &b, &a};
	const auto runs = Split(pages);
	Check(runs.size() == 3 && runs[0].First == 0 && runs[0].Count == 2 && runs[1].Page == &b && runs[1].First == 2
		&& runs[1].Count == 3 && runs[2].First == 5 && runs[2].Count == 1, "runs keep the order");
	Check(Split({}).empty(), "no instances, no runs");
	const std::uint8_t depths[] {1, 0, 0, 0, 1, 1};
	const auto modes = Split(pages, depths);
	Check(modes.size() == 5 && modes[0].Depth && modes[0].Count == 1 && !modes[1].Depth && modes[1].First == 1
		&& modes[2].Page == &b && modes[2].Count == 2 && modes[3].First == 4 && modes[3].Depth && modes[4].Page == &a && modes[4].Depth,
		"runs also split where the depth mode changes");

	ColorTables tables(100);
	std::vector<std::uint16_t> first(53 * 256), second(27 * 256), third(40 * 256);
	for (size_t i = 0; i < first.size(); ++i) first[i] = std::uint16_t(i);
	for (size_t i = 0; i < second.size(); ++i) second[i] = std::uint16_t(i * 3);
	for (size_t i = 0; i < third.size(); ++i) third[i] = std::uint16_t(i * 7);
	auto one = tables.Acquire(first.data(), 53);
	const auto generation = tables.Generation();
	Check(one && one->Row == 0 && one->Shades == 53, "first table at row 0");
	Check(tables.Acquire(first.data(), 53) == one && tables.Generation() == generation, "same content, same table");
	auto two = tables.Acquire(second.data(), 27);
	Check(two && two->Row == 53 && tables.UsedRows() == 80 && tables.Generation() != generation, "second table packed after");
	Check(!tables.Acquire(third.data(), 40), "no room while both tables are held");
	one.reset();
	auto three = tables.Acquire(third.data(), 40);
	Check(three && three->Row == 0 && tables.Count() == 2, "unheld table evicted, its rows reused");
	first[5] ^= 1;
	Check(!tables.Acquire(first.data(), 53), "changed content needs its own rows");
	tables.Clear();
	Check(tables.Count() == 0 && tables.UsedRows() == 0, "cleared");
	Check(!tables.Acquire(nullptr, 1) && !tables.Acquire(first.data(), 101), "invalid tables");
	{
		Instance shape {};
		Check(PlaceShape(300, 50, 39, 22, -10, -20, 100, 200, 53, 53, 127, 7, shape) && shape.X == 290 && shape.Y == 30
			&& shape.Size == Pack(39, 22) && shape.Atlas == Pack(100, 200) && shape.Frame == 7 && shape.Light == Pack(127, 0) && shape.Depth == 0,
			"shape instance");
		Check(!PlaceShape(0, 0, 0, 22, 0, 0, 0, 0, 53, 53, 127, 0, shape) && !PlaceShape(0, 0, 10, 10, 0, 0, -1, 0, 53, 53, 127, 0, shape),
			"shape instances out of range");
		// RGB565 and RGB555.
		Check(HalfMask({11, 5, 0}, {5, 6, 5}) == 0x7BEF && HalfMask({10, 5, 0}, {5, 5, 5}) == 0x3DEF, "half translucency masks");
	}
	if (Failures) return 1;
	std::puts("TerrainBatch: placement, clipping, page runs, shape instances, masks and color table packing passed.");
}
