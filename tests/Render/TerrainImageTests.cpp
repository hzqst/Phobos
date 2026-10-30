#include <Render/TerrainImage.h>
#include <array>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "TerrainImage failed: %s\n", what); ++Failures; }
	}
}

int main()
{
	using namespace TerrainImage;
	// The diamond of 547CF0: rows 4, 8, ..., 60, ..., 8, 4 centered in 60 columns, 900 pixels.
	Check(DiamondStart(0) == 28 && DiamondLength(0) == 4, "first row");
	Check(DiamondStart(14) == 0 && DiamondLength(14) == 60, "widest row");
	Check(DiamondStart(15) == 2 && DiamondLength(15) == 56, "row after the widest");
	Check(DiamondStart(28) == 28 && DiamondLength(28) == 4, "last row");

	std::array<std::uint8_t, TileWidth * DiamondRows> rows;
	std::array<std::int32_t, DiamondRows> offsets;
	rows.fill(TemplateBlank);
	for (int row = 0, offset = 0; row < DiamondRows; offset += DiamondLength(row++))
	{
		offsets[row] = offset;
		for (int i = 0; i < DiamondLength(row); ++i) rows[row * TileWidth + DiamondStart(row) + i] = 'X';
	}
	Check(TemplateMatches(rows.data(), offsets.data()), "matching template");
	auto shifted = rows;
	shifted[7 * TileWidth + DiamondStart(7) - 1] = 'X';
	Check(!TemplateMatches(shifted.data(), offsets.data()), "row starting one pixel early");
	auto skewed = offsets;
	++skewed[3];
	Check(!TemplateMatches(rows.data(), skewed.data()), "row offset off by one");

	std::array<std::uint8_t, DiamondPixels> packed;
	for (int i = 0; i < DiamondPixels; ++i) packed[i] = std::uint8_t(i % 251 + 1);
	std::array<std::uint8_t, TileWidth * DiamondRows> plane;
	ExpandDiamond(packed.data(), plane.data());
	int covered = 0;
	bool ordered = true;
	for (int row = 0, next = 0; row < DiamondRows; ++row)
		for (int x = 0; x < TileWidth; ++x)
		{
			const bool inside = x >= DiamondStart(row) && x < DiamondStart(row) + DiamondLength(row);
			if (inside) { ordered &= plane[row * TileWidth + x] == packed[next++]; ++covered; }
			else ordered &= plane[row * TileWidth + x] == 0;
		}
	Check(covered == DiamondPixels && ordered, "expanded diamond");

	// q = min(254, 261*I >> 11); default ABuffer 127 at full intensity picks the middle shade.
	Check(LightRow(1000) == 127, "light row at intensity 1000");
	Check(LightRow(-5) == 0 && LightRow(0) == 0, "light row clamps negative intensity");
	Check(LightRow(4000) == 254, "light row clamps to 254");
	Check(ShadeRow(53, 127, DefaultAlpha) == 26, "53 shades");
	Check(ShadeRow(27, 127, DefaultAlpha) == 13, "27 shades");
	Check(ShadeRow(53, 254, 255) == 52, "shade clamps to the last row");
	Check(ShadeRow(53, 0, DefaultAlpha) == 0, "dark row");

	std::array<std::int32_t, 16> table4;
	std::array<std::int32_t, 64> table8;
	for (int i = 0; i < 16; ++i) table4[i] = i;
	for (int i = 0; i < 64; ++i) table8[i] = 100 + i;
	Check(PickVariant(5, 6, 0, 1, 1, 4, table4.data(), table8.data()) == (1 | 4 * 2), "single cell, 4x4 table");
	Check(PickVariant(13, 10, 0, 1, 1, 5, table4.data(), table8.data()) == 100 + 8 * 2 + 5, "single cell, 8x8 table");
	// Piece 3 of a 2x2 tile: back to the origin cell (4,6), then grid position (2,3).
	Check(PickVariant(5, 7, 3, 2, 2, 4, table4.data(), table8.data()) == (2 | 4 * 3), "multi-cell tile");
	Check(PickVariant(5, 7, 3, 0, 2, 4, table4.data(), table8.data()) == -1, "zero grid width");
	Check(PickVariant(-1, -3, 0, 1, 1, 4, table4.data(), table8.data()) == (3 | 4 * 1), "16-bit wrap of negative cells");

	Check(VariantSteps(0, 3) == 0 && VariantSteps(2, 3) == 2 && VariantSteps(5, 3) == 2, "variant steps");
	Check(VariantSteps(-1, 3) == -1 && VariantSteps(1, 0) == -1, "invalid variant steps");

	constexpr auto position = TilePosition(10, 4, 2, 100, 50, 16);
	static_assert(position.X == 30 * 6 - 100 - 30 && position.Y == 15 * 14 - 50 - 30 + 16);
	Check(ExtraRise(10, 4) == 6 && ExtraRise(4, 10) == 0, "extra rise");
	Check(ScanRows(736) == 49 + 17 && ScanColumns(856) == 14 + 4, "1024x768 scan");
	Check(ScanRows(568) == 37 + 17 && ScanColumns(632) == 10 + 4, "800x600 scan");
	Check(FloorHalf(-3) == -2 && FloorHalf(-2) == -1 && FloorHalf(-1) == -1 && FloorHalf(3) == 1, "floor half");
	// Rows start 2 left of the anchor and step down alternately in y and x; columns walk (+1,-1).
	static_assert(ScanCell({10, 20}, 0, 0).X == 8 && ScanCell({10, 20}, 0, 0).Y == 20);
	static_assert(ScanCell({10, 20}, 1, 0).X == 8 && ScanCell({10, 20}, 1, 0).Y == 21);
	static_assert(ScanCell({10, 20}, 2, 3).X == 12 && ScanCell({10, 20}, 2, 3).Y == 18);
	static_assert(ScanCell({10, 20}, -1, 0).X == 7 && ScanCell({10, 20}, -1, 0).Y == 20, "margin rows floor toward minus");
	bool anchors = true;
	for (int x = 0; x < 64; ++x)
		for (int y = 0; y < 64; ++y)
		{
			const auto cell = ScanAnchor(30 * (x - y) + 1, 15 * (x + y) + 1);
			anchors &= cell.X == x && cell.Y == y;
		}
	Check(anchors, "anchor of a pixel just inside a cell corner");

	// Base depth: uint16(MaxValue + Bounds.Y - y - PixelHeight), then trunc0(PixelHeight * level / -2).
	static_assert(BaseDepth(32768, 0, 100, 30, 0) == 32768 - 130);
	static_assert(BaseDepth(32768, 0, 100, 30, 3) == 32768 - 130 - 45);
	static_assert(BaseDepth(32768, 0, 100, 29, 1) == 32768 - 129 - 14, "odd product truncates toward zero");
	static_assert(BaseDepth(10, 0, 100, 30, 0) == 65536 - 120, "16-bit wrap below zero");
	static_assert(BaseDepth(65535, 40, 0, 30, 0) == 9, "16-bit wrap above 0xFFFF");
	static_assert(PixelDepth(0xFFF0, 0x20) == 0x10 && PixelDepth(100, 5) == 105);
	// Moving a row of cells down 15 pixels and up one level keeps the depth: the level cancels.
	static_assert(BaseDepth(32768, 0, 100 - 15, 30, 1) == BaseDepth(32768, 0, 100, 30, 0));

	// The Z test: draw when the old depth is not smaller, equal depth overwrites.
	std::array<std::uint8_t, TileWidth * DiamondRows> zeros {}, depths;
	depths.fill(5);
	std::array<std::uint16_t, TileWidth * DiamondRows> buffer;
	buffer.fill(0xFFFF);
	Check(DrawDepth(buffer.data(), 0, 0, TileWidth, DiamondRows, {0, 0, TileWidth, DiamondRows, zeros.data(), depths.data(), 100, true}) == 0,
		"first diamond draws everywhere");
	Check(buffer[14 * TileWidth] == 105 && buffer[0] == 0xFFFF, "diamond covers index 0 inside, nothing outside");
	Check(DrawDepth(buffer.data(), 0, 0, TileWidth, DiamondRows, {0, 0, TileWidth, DiamondRows, zeros.data(), depths.data(), 100, true}) == 0,
		"equal depth overwrites");
	Check(DrawDepth(buffer.data(), 0, 0, TileWidth, DiamondRows, {0, 0, TileWidth, DiamondRows, zeros.data(), depths.data(), 101, true}) == DiamondPixels,
		"farther diamond is rejected");
	Check(DrawDepth(buffer.data(), 0, 0, TileWidth, DiamondRows, {0, 0, TileWidth, DiamondRows, zeros.data(), nullptr, 200, true}) == 0
		&& buffer[14 * TileWidth] == 105, "no depth data neither tests nor writes");
	std::array<std::uint8_t, 4> extraIndices {0, 7, 7, 0}, extraDepths {0, 0, 9, 9};
	Check(DrawDepth(buffer.data(), 0, 0, TileWidth, DiamondRows, {28, 13, 2, 2, extraIndices.data(), extraDepths.data(), 96, false}) == 0
		&& buffer[13 * TileWidth + 28] == 105 && buffer[13 * TileWidth + 29] == 96 && buffer[14 * TileWidth + 28] == 105
		&& buffer[14 * TileWidth + 29] == 105, "extra image skips index 0");
	Check(DrawDepth(buffer.data(), 10, 10, 4, 4, {-5, -5, TileWidth, DiamondRows, zeros.data(), depths.data(), 0, true}) == 0 && buffer[0] == 5,
		"clipped to the buffer rectangle");
	if (Failures) return 1;
	std::puts("TerrainImage: diamond, template, lighting, variant, position, scan and depth rules passed.");
}
