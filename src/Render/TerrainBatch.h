#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

// CPU side of the instanced terrain draw: one GPU instance per clipped tile image, and the
// LightConvert color tables the pixel shader indexes by shade row.
namespace TerrainBatch
{
	// Matches struct Tile in TerrainTile.vs.hlsl / TerrainTile.ps.hlsl; pairs pack X in the low 16 bits.
	struct Instance
	{
		std::int32_t X, Y;   // Canvas position of the clipped rectangle.
		std::uint32_t Size;  // Width, height.
		std::uint32_t Atlas; // Atlas texel of the clipped rectangle's top-left.
		std::uint32_t Frame; // The same texel within the frame, for the diamond coverage.
		std::uint32_t Table; // First row of the color table, its shade count.
		std::uint32_t Light; // Light row q, 1 for a main diamond.
		std::uint32_t Depth; // Base depth, rows from the index plane down to the Z plane (0 without Z).
	};
	static_assert(sizeof(Instance) == 32);

	constexpr std::uint32_t Pack(std::uint32_t low, std::uint32_t high) { return (low & 0xFFFF) | (high << 16); }

	struct Rect { int X, Y, Width, Height; };

	// A tile image at (x, y) on Tile, clipped to clip (Tile coordinates) and moved to the target
	// by originX. depth is the packed Depth field. False when nothing remains or a value leaves its packed range.
	inline bool Place(int x, int y, int width, int height, int originX, const Rect& clip, int atlasX, int atlasY,
		unsigned tableRow, unsigned shades, int lightRow, bool diamond, Instance& out, std::uint32_t depth = 0)
	{
		const auto left = std::max<std::int64_t>(x, clip.X);
		const auto top = std::max<std::int64_t>(y, clip.Y);
		const auto right = std::min<std::int64_t>(std::int64_t(x) + width, std::int64_t(clip.X) + clip.Width);
		const auto bottom = std::min<std::int64_t>(std::int64_t(y) + height, std::int64_t(clip.Y) + clip.Height);
		const auto canvasX = left + originX;
		const auto frameX = left - x, frameY = top - y;
		const auto texelX = atlasX + frameX, texelY = atlasY + frameY;
		if (right <= left || bottom <= top || canvasX < INT_MIN || canvasX > INT_MAX
			|| right - left > 0xFFFF || bottom - top > 0xFFFF || frameX > 0xFFFF || frameY > 0xFFFF
			|| texelX < 0 || texelX > 0xFFFF || texelY < 0 || texelY > 0xFFFF
			|| tableRow > 0xFFFF || shades > 0xFFFF || lightRow < 0 || lightRow > 0xFFFF) return false;
		out = {int(canvasX), int(top), Pack(std::uint32_t(right - left), std::uint32_t(bottom - top)),
			Pack(std::uint32_t(texelX), std::uint32_t(texelY)), Pack(std::uint32_t(frameX), std::uint32_t(frameY)),
			Pack(tableRow, shades), Pack(std::uint32_t(lightRow), diamond ? 1u : 0u), depth};
		return true;
	}

	// Matches struct Shape in TerrainShape.ps.hlsl: the depth of a 4AED70 draw (ShapeDepth). Drawn row j's
	// base is Base + BandDelta * ((Accumulator + j * RowsPerBand) / BandLimit); a pixel's depth is the
	// base less the signed byte at ZOffset + j * width + i of the Z bytes buffer, when ZOffset is not NoZ.
	struct Shape
	{
		std::int32_t Base;
		std::uint32_t Accumulator, RowsPerBand, BandLimit;
		std::int32_t BandDelta;
		std::uint32_t ZOffset;
		// ShadowShape: halve the target's color instead of drawing; TranslucentShape: mix the color with
		// the target's by the blitter's translucency bits (2, 4, 6), rounded with RoundedShape
		// (ShapeDepth::RoundedBlend); UntestedShape: no Z test, so no depth is out of range. Tint: the
		// 16 bits an RLE body of 4AED70 ORs into each palette color (Blit_Copy_Tinted), 0 for the others.
		std::uint32_t Flags, Tint;
	};
	static_assert(sizeof(Shape) == 32);
	constexpr std::uint32_t NoZ = 0xFFFFFFFF, ShadowShape = 1, TranslucentShape = 6, UntestedShape = 8, RoundedShape = 16;

	// A clipped shape draw at (x, y) on Tile, wholly inside the view: the instance that moves it to
	// the target by (originX, originY) and reads the atlas from (atlasX, atlasY). Frame holds the
	// index of its Shape. False when a value leaves its packed range.
	inline bool PlaceShape(int x, int y, int width, int height, int originX, int originY, int atlasX, int atlasY,
		unsigned tableRow, unsigned shades, int lightRow, std::uint32_t shape, Instance& out)
	{
		if (width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF || atlasX < 0 || atlasX > 0xFFFF || atlasY < 0 || atlasY > 0xFFFF
			|| tableRow > 0xFFFF || shades > 0xFFFF || lightRow < 0 || lightRow > 0xFFFF) return false;
		const auto canvasX = std::int64_t(x) + originX, canvasY = std::int64_t(y) + originY;
		if (canvasX < INT_MIN || canvasX > INT_MAX || canvasY < INT_MIN || canvasY > INT_MAX) return false;
		out = {int(canvasX), int(canvasY), Pack(std::uint32_t(width), std::uint32_t(height)), Pack(std::uint32_t(atlasX), std::uint32_t(atlasY)),
			shape, Pack(tableRow, shades), Pack(std::uint32_t(lightRow), 0), 0};
		return true;
	}

	// The HalfTranslucencyMask a color pack implies: each channel's top bit cleared after a shift
	// right by one, so (c >> 1) & mask halves every channel. shift is a channel's _SHL, bits its width.
	constexpr std::uint32_t HalfMask(const int (&shift)[3], const int (&bits)[3])
	{
		std::uint32_t mask = 0;
		for (int i = 0; i < 3; ++i)
		{
			const auto field = ((1u << bits[i]) - 1u) << shift[i];
			mask |= (field >> 1) & field;
		}
		return mask;
	}

	// The QuatTranslucencyMask a color pack implies: each channel's two top bits cleared after a shift
	// right by two.
	constexpr std::uint32_t QuarterMask(const int (&shift)[3], const int (&bits)[3])
	{
		std::uint32_t mask = 0;
		for (int i = 0; i < 3; ++i)
		{
			const auto field = ((1u << bits[i]) - 1u) << shift[i];
			mask |= (field >> 2) & field;
		}
		return mask;
	}

	// Mode: the shape pipeline of a run of shapes, and where it changes; Order: the place in its draw list of the
	// run's first draw, which the lines drawn before it precede.
	struct Run { const void* Page; std::uint32_t First, Count; bool Depth = false; std::uint32_t Mode = 0; std::uint32_t Order = 0; };

	// Runs of consecutive instances (the first one first) with the same page and mode; orders[i] is instance i's
	// draw in its list, and a run also ends before each draw a line precedes (breaks, sorted), so lines can
	// be drawn between runs in the native order.
	inline std::vector<Run> SplitShapes(std::span<const void* const> pages, std::span<const std::uint32_t> modes,
		std::span<const std::uint32_t> orders, std::span<const std::uint32_t> breaks, std::uint32_t first)
	{
		std::vector<Run> runs;
		auto next = breaks.begin();
		for (std::uint32_t i = 0; i < pages.size(); ++i)
		{
			// Whether a line precedes a draw after the previous instance's and up to this one's.
			bool broken = false;
			while (next != breaks.end() && *next <= orders[i])
			{
				broken = broken || (i > 0 && *next > orders[i - 1]);
				++next;
			}
			if (runs.empty() || broken || runs.back().Page != pages[i] || runs.back().Mode != modes[i])
				runs.push_back({pages[i], first + i, 1, false, modes[i], orders[i]});
			else ++runs.back().Count;
		}
		return runs;
	}

	// Matches struct Line in TerrainLine.vs.hlsl: a line, fill or glyph RenderLayers drew outside 4AED70 and its
	// blits, at target pixels. A line goes from From to To, its depth from Depth.x to Depth.y; a fill (FilledLine)
	// covers From up to To, which is past its last pixel. Only the pixels from ClipMin up to ClipMax draw (packed X
	// in the low 16 bits). AlphaLine: the pixel needs an ABuffer word other than 0 and its color is scaled by it.
	// GlyphLine (with FilledLine): a glyph whose mask starts at the atlas texel Depth0 (packed), drawn where it is set.
	struct Line
	{
		std::int32_t FromX, FromY, ToX, ToY;
		std::int32_t Depth0, Depth1;
		std::uint32_t ClipMin, ClipMax;
		std::uint32_t Color, Flags;
	};
	static_assert(sizeof(Line) == 40);
	constexpr std::uint32_t FilledLine = 1, AlphaLine = 2, GlyphLine = 4;

	// A line or fill at Tile coordinates moved to the target by (originX, originY), its clip cut to the target's
	// width x height. False when nothing of the clip remains.
	inline bool PlaceLine(int fromX, int fromY, int toX, int toY, const Rect& clip, int originX, int originY, int width, int height,
		int depth0, int depth1, std::uint16_t color, std::uint32_t flags, Line& out)
	{
		const auto left = std::max<std::int64_t>(std::int64_t(clip.X) + originX, 0);
		const auto top = std::max<std::int64_t>(std::int64_t(clip.Y) + originY, 0);
		const auto right = std::min<std::int64_t>(std::int64_t(clip.X) + clip.Width + originX, std::min(width, 0xFFFF));
		const auto bottom = std::min<std::int64_t>(std::int64_t(clip.Y) + clip.Height + originY, std::min(height, 0xFFFF));
		const auto x0 = std::int64_t(fromX) + originX, y0 = std::int64_t(fromY) + originY;
		const auto x1 = std::int64_t(toX) + originX, y1 = std::int64_t(toY) + originY;
		if (right <= left || bottom <= top || std::min({x0, y0, x1, y1}) < INT_MIN || std::max({x0, y0, x1, y1}) > INT_MAX) return false;
		out = {int(x0), int(y0), int(x1), int(y1), depth0, depth1, Pack(std::uint32_t(left), std::uint32_t(top)),
			Pack(std::uint32_t(right), std::uint32_t(bottom)), color, flags};
		return true;
	}

	// Runs of consecutive lines (first one first) that precede the same draw (orders[i], ascending) with the same
	// mode and atlas page (null for lines and fills).
	inline std::vector<Run> SplitLines(std::span<const std::uint32_t> orders, std::span<const std::uint32_t> modes,
		std::span<const void* const> pages, std::uint32_t first)
	{
		std::vector<Run> runs;
		for (std::uint32_t i = 0; i < orders.size(); ++i)
		{
			if (runs.empty() || runs.back().Order != orders[i] || runs.back().Mode != modes[i] || runs.back().Page != pages[i])
				runs.push_back({pages[i], first + i, 1, false, modes[i], orders[i]});
			else ++runs.back().Count;
		}
		return runs;
	}

	// Consecutive instances on the same atlas page and with the same depth mode (none when depths is
	// empty), so each draw keeps the native order.
	inline std::vector<Run> Split(std::span<const void* const> pages, std::span<const std::uint8_t> depths = {})
	{
		std::vector<Run> runs;
		for (std::uint32_t i = 0; i < pages.size(); ++i)
		{
			const bool depth = i < depths.size() && depths[i] != 0;
			if (runs.empty() || runs.back().Page != pages[i] || runs.back().Depth != depth) runs.push_back({pages[i], i, 1, depth});
			else ++runs.back().Count;
		}
		return runs;
	}

	// Whole FullColorData tables, deduplicated by content, packed into the rows of one texture.
	// A table no snapshot holds may be evicted to make room; any change bumps the generation.
	class ColorTables
	{
	public:
		struct Table
		{
			std::uint64_t ID = 0, Hash = 0;
			std::uint32_t Row = 0, Shades = 0;
			std::vector<std::uint16_t> Colors;
		};
		using Handle = std::shared_ptr<const Table>;

		explicit ColorTables(std::uint32_t rows = 1024) : Rows(rows) { }

		Handle Acquire(const std::uint16_t* colors, std::uint32_t shades)
		{
			if (!colors || !shades || shades > Rows) return {};
			const size_t count = size_t(shades) * 256;
			std::uint64_t hash = 14695981039346656037ull;
			for (size_t i = 0; i < count; ++i) hash = (hash ^ colors[i]) * 1099511628211ull;
			for (auto& entry : Entries)
				if (entry.Value->Hash == hash && entry.Value->Shades == shades
					&& std::equal(colors, colors + count, entry.Value->Colors.begin()))
				{
					entry.LastUse = ++Clock;
					return entry.Value;
				}
			std::uint32_t row = 0;
			while (!FindRow(shades, row))
			{
				auto victim = Entries.end();
				for (auto it = Entries.begin(); it != Entries.end(); ++it)
					if (it->Value.use_count() == 1 && (victim == Entries.end() || it->LastUse < victim->LastUse)) victim = it;
				if (victim == Entries.end()) return {};
				Entries.erase(victim);
				++Version;
			}
			auto table = std::make_shared<Table>();
			table->ID = ++NextID;
			table->Hash = hash;
			table->Row = row;
			table->Shades = shades;
			table->Colors.assign(colors, colors + count);
			Entries.push_back({table, ++Clock});
			++Version;
			return table;
		}

		std::uint64_t Generation() const { return Version; }
		// Rows below this one hold every live table.
		std::uint32_t UsedRows() const
		{
			std::uint32_t used = 0;
			for (const auto& entry : Entries) used = std::max(used, entry.Value->Row + entry.Value->Shades);
			return used;
		}
		template<class Visit> void ForEach(Visit visit) const { for (const auto& entry : Entries) visit(*entry.Value); }
		size_t Count() const { return Entries.size(); }
		void Clear() { Entries.clear(); ++Version; }

	private:
		// First fit among the gaps between the live tables.
		bool FindRow(std::uint32_t shades, std::uint32_t& row) const
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> used;
			for (const auto& entry : Entries) used.push_back({entry.Value->Row, entry.Value->Row + entry.Value->Shades});
			std::sort(used.begin(), used.end());
			std::uint32_t next = 0;
			for (const auto& [begin, end] : used)
			{
				if (begin - next >= shades) { row = next; return true; }
				next = std::max(next, end);
			}
			if (Rows - next >= shades) { row = next; return true; }
			return false;
		}

		struct Entry { Handle Value; std::uint64_t LastUse; };
		std::uint32_t Rows;
		std::uint64_t Clock = 0, NextID = 0, Version = 0;
		std::vector<Entry> Entries;
	};
}
