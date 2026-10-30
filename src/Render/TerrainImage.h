#pragma once

#include <algorithm>
#include <cstdint>

// Pure rules of the tactical terrain pass (gamemd-reverse #16): 6D7560 scans the visible
// cells, 480350 places and lights each cell, 547CF0 draws the TMP piece.
namespace TerrainImage
{
	constexpr int TileWidth = 60, DiamondRows = 29, DiamondPixels = 900;
	constexpr int CellHeight = 30, LevelHeight = 15;
	// 7EC4C8 marks the pixels outside the diamond with a space.
	constexpr std::uint8_t TemplateBlank = 32;
	// The value the ABuffer is filled with where no shroud, fog or alpha shape lies.
	constexpr int DefaultAlpha = 0x7F;
	constexpr int AlphaScale = 32258;
	constexpr int MaxLightRow = 254;

	// Rows widen by 4 pixels up to the full 60 at row 14, then narrow again.
	constexpr int DiamondHalf(int row) { return row < 15 ? 2 * (row + 1) : 2 * (DiamondRows - row); }
	constexpr int DiamondStart(int row) { return TileWidth / 2 - DiamondHalf(row); }
	constexpr int DiamondLength(int row) { return 2 * DiamondHalf(row); }

	// 547CF0 builds its clip tables from the first run of non-blank bytes in each 60-byte
	// template row and from the packed row offsets. The GPU coverage is the analytic
	// diamond, valid only when the running game's tables describe the same shape.
	inline bool TemplateMatches(const std::uint8_t* rows, const std::int32_t* offsets)
	{
		int offset = 0;
		for (int row = 0; row < DiamondRows; ++row)
		{
			const auto* line = rows + row * TileWidth;
			int start = 0;
			while (start < TileWidth && line[start] == TemplateBlank) ++start;
			int length = 0;
			while (start + length < TileWidth && line[start + length] != TemplateBlank) ++length;
			if (start != DiamondStart(row) || length != DiamondLength(row) || offsets[row] != offset) return false;
			offset += length;
		}
		return offset == DiamondPixels;
	}

	// Packed rows into a 60x29 plane. Pixels outside the diamond stay 0; the sprite's
	// coverage masks them, while index 0 inside the diamond is drawn like any other.
	inline void ExpandDiamond(const std::uint8_t* packed, std::uint8_t* plane)
	{
		std::fill_n(plane, TileWidth * DiamondRows, std::uint8_t(0));
		for (int row = 0, offset = 0; row < DiamondRows; offset += DiamondLength(row++))
			std::copy_n(packed + offset, DiamondLength(row), plane + row * TileWidth + DiamondStart(row));
	}

	// 547CF0: Intensity_Terrain selects a row of the alpha lighting remap.
	constexpr int LightRow(int intensity)
	{
		const auto product = std::int32_t(261u * std::uint32_t(intensity <= 0 ? 0 : intensity));
		return std::min(MaxLightRow, product >> 11);
	}

	// 420140: the remap entry of light row q and ABuffer value a, as a FullColorData shade row.
	constexpr int ShadeRow(int shadeCount, int lightRow, int alpha)
	{
		const auto last = std::int32_t(std::uint32_t(shadeCount) - 1u);
		auto value = std::int32_t(std::uint32_t(last) * std::uint32_t(lightRow) * std::uint32_t(alpha)) / AlphaScale;
		if (value > last) value = last;
		return int(std::uint32_t(value) & 0xFF);
	}

	// 4814F0 without its lazy table setup: a cell of a multi-cell tile is first mapped to the
	// tile's own grid position, then a 4x4 table (up to 4 variants) or the 8x8 table picks.
	// Returns -1 where the native division would fault.
	inline int PickVariant(std::int16_t x, std::int16_t y, int subtile, int gridWidth, int gridHeight, int count,
		const std::int32_t* table4, const std::int32_t* table8)
	{
		auto xBits = std::uint16_t(x), yBits = std::uint16_t(y);
		if (subtile != 0)
		{
			if (gridWidth == 0 || gridHeight == 0) return -1;
			xBits = std::uint16_t(xBits - std::uint32_t(subtile % gridWidth));
			yBits = std::uint16_t(yBits - std::uint32_t(subtile / gridWidth));
			xBits = std::uint16_t(std::int16_t(xBits) / gridWidth);
			yBits = std::uint16_t(std::int16_t(yBits) / gridHeight);
		}
		return count > 4 ? table8[8 * (yBits & 7u) + (xBits & 7u)] : table4[(xBits & 3u) | (4 * (yBits & 3u))];
	}

	// 547CF0 follows the variant chain this many links; 0 keeps the tile. -1 where the native
	// division would fault or the walk would not end.
	constexpr int VariantSteps(int variant, int count)
	{
		if (variant == 0) return 0;
		if (variant < 0 || count <= 0) return -1;
		return variant > count - 1 ? variant % count : variant;
	}

	struct Point { std::int64_t X, Y; };

	// 6D2140 and 480350 for a legal cell: the main diamond's top-left on the Tile surface.
	// No ViewBounds.X is added here, unlike the Y.
	constexpr Point TilePosition(int cellX, int cellY, int level, int tacticalX, int tacticalY, int viewY)
	{
		return {std::int64_t(CellHeight) * (cellX - cellY) - tacticalX - CellHeight,
			std::int64_t(LevelHeight) * (cellX + cellY) - tacticalY - std::int64_t(LevelHeight) * level + viewY};
	}

	// 47FF80: a piece with an extra image reaches this far above the diamond.
	constexpr int ExtraRise(int pieceY, int extraY)
	{
		const auto difference = std::int32_t(std::uint32_t(pieceY) - std::uint32_t(extraY));
		return difference < 0 ? 0 : difference;
	}

	// 6D7560's scan of a rectangle: rows of constant x+y from far to near, each walked by
	// (+1,-1) from base + (floor(r/2), floor((r+1)/2)), where base is the anchor minus 2 in x.
	constexpr int ScanRows(int height) { return height / 15 + 17; }
	constexpr int ScanColumns(int width) { return width / TileWidth + 4; }
	constexpr int FloorHalf(int value) { return value >= 0 ? value / 2 : -((1 - value) / 2); }

	struct Cell { int X, Y; };

	// The cell under a tactical pixel. The native anchor inverts the projection in floats
	// (IsoTransformMatrix and 7C5F00); this exact inverse may differ by one cell on a
	// boundary, which the caller's scan margin absorbs.
	constexpr Cell ScanAnchor(int pixelX, int pixelY)
	{
		// px = 30(x-y)/256 and py = 15(x+y)/256 give x = (128px + 256py)/30, y = (256py - 128px)/30.
		const auto x = (std::int64_t(pixelX) * 128 + std::int64_t(pixelY) * 256) / 30;
		const auto y = (std::int64_t(pixelY) * 256 - std::int64_t(pixelX) * 128) / 30;
		return {int(x / 256), int(y / 256)};
	}
}
