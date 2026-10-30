#pragma once

#include "TerrainImage.h"

// Pure rules of the static Terrain objects and cliff shadows of the tactical Tile pass
// (gamemd-reverse #19): after the overlays, 6D97D0 draws every static Terrain object of the ground
// layer, each body (71C1B0) followed by its own shadow, and 6D7C00 then draws the cliff shadows of
// C_SHADOW.SHP (4802A0, 547230), all through 4AED70 with a Z test. The animated or crumbling objects
// draw later on Composite (6D8DB0, gamemd-reverse #23) through the same 71C1B0.
namespace TerrainObjectImage
{
	using TerrainImage::Point;

	// 71C1B0: ZAdjust of the body and of its shadow below the object's screen height.
	constexpr int BodyDepth = -12, ShadowDepth = -3;
	// 71C1B0: a SpawnsTiberium object draws this much higher.
	constexpr int TiberiumLift = 16;
	// 4802A0: ZAdjust of a cliff shadow on level 0.
	constexpr int CliffDepth = 58;
	// 547230: the ranges its records cover. Each record holds the frame + 1, the subtile and the offset.
	constexpr int SpecialTypes = 10, RangeCount = 5, RangeTypes = 40, RecordInts = 4;

	// 71C1B0 with Phobos' 71C1FE for an object that is neither animated nor crumbling, the only
	// ones Tile draws: frame 1 when damaged, otherwise 0.
	constexpr int StaticFrame(bool damaged) { return damaged ? 1 : 0; }

	// Phobos' 71C1FE: the animation length, AnimationLength where set, else the shape's frames over
	// 2 x (HasDamagedFrames + 1).
	constexpr int AnimationLength(bool set, int length, std::int16_t frames, bool damagedFrames)
	{
		return set ? length : frames / (2 * (damagedFrames + 1));
	}

	// 71C1B0 with Phobos' 71C1FE for an animated or crumbling object, the ones 6D8DB0 draws on
	// Composite (gamemd-reverse #23): value is Animation.Value, length AnimationLength's result.
	constexpr int DynamicFrame(bool animated, bool crumbling, bool crumblingFrames, bool damagedFrames, bool damaged, int value, int length)
	{
		if (animated)
			return crumblingFrames && crumbling ? length * (damagedFrames + 1) + 1 + value : value + (damaged ? length : 0);
		if (crumblingFrames && crumbling) return 1 + value;
		return StaticFrame(damaged);
	}

	// 71C1B0: the shadow is the second half of the shape's frames.
	constexpr int ShadowFrame(int frame, std::int16_t frames) { return frame + frames / 2; }

	// 6D8DB0's 32-bit arithmetic.
	constexpr int Add32(int a, int b) { return int(std::uint32_t(a) + std::uint32_t(b)); }
	constexpr int Sub32(int a, int b) { return int(std::uint32_t(a) - std::uint32_t(b)); }
	constexpr int Mul32(int a, int b) { return int(std::uint32_t(a) * std::uint32_t(b)); }

	// 6D8DB0's view range of Locations. It applies IsoTransformMatrix (5AFB80) to the three points
	// RangeProbes gives from TacticalPos and ViewBounds' size (the top, right and left probe, with Z 0),
	// truncates the top probe's X and Y, the right probe's X and the left probe's Y, and MakeViewRange
	// spans twice their differences. Not the drawn rectangle: a coarse filter with no Z.
	struct ViewProbe { int X = 0, Y = 0; };
	struct ViewProbes { ViewProbe Top, Right, Left; };
	constexpr ViewProbes RangeProbes(int tacticalX, int tacticalY, int width, int height)
	{
		const int lower = Add32(tacticalY, height / 2);
		return {{Add32(tacticalX, width / 2), Sub32(tacticalY, height)}, {Add32(tacticalX, Mul32(width, 2)), lower}, {Sub32(tacticalX, width), lower}};
	}
	struct ViewRange { int Left = 0, Top = 0, Width = 0, Height = 0; };
	constexpr ViewRange MakeViewRange(int left, int top, int right, int bottom)
	{
		return {left, top, Mul32(Sub32(right, left), 2), Mul32(Sub32(bottom, top), 2)};
	}
	// The half-open test of 6D8DB0, in its order: x against left and left + width, then y.
	constexpr bool InViewRange(const ViewRange& range, int x, int y)
	{
		return x >= range.Left && x < Add32(range.Left, range.Width) && y >= range.Top && y < Add32(range.Top, range.Height);
	}

	// 71C1B0's ZAdjust from the object's screen height (6D20E0 of its Z).
	constexpr int Depth(int screenHeight, bool shadow)
	{
		return int(0u - std::uint32_t(screenHeight) + std::uint32_t(shadow ? ShadowDepth : BodyDepth));
	}

	// 6D7C00's scan of a rectangle: rows of constant x+y from far to near, each walked by (+1,-1)
	// from the anchor moved by (-2,+3) plus (floor(r/2), floor((r+1)/2)).
	constexpr int CliffRows(int height) { return height / 15 + 20; }
	constexpr int CliffColumns(int width) { return width / TerrainImage::TileWidth + 7; }
	constexpr TerrainImage::Cell CliffCell(TerrainImage::Cell anchor, int row, int column)
	{
		return TerrainImage::ScanCell({anchor.X, anchor.Y + 3}, row, column);
	}

	// 547230's record for a tile type and subtile; not Drawn where it has none, its subtile differs
	// or its frame is 0. special is the record table of the two special ranges (from specialBase,
	// else fallbackBase), regular the one shared by the RangeCount ranges starting at rangeStarts.
	struct CliffShadow { int Frame = 0, X = 0, Y = 0; bool Drawn = false; };
	inline CliffShadow SelectCliffShadow(int tileIndex, int subtile, int specialBase, int fallbackBase,
		const std::int32_t* special, const std::int32_t* rangeStarts, const std::int32_t* regular)
	{
		const auto inside = [tileIndex](int base, int count)
		{
			return tileIndex >= base && tileIndex < int(std::uint32_t(base) + std::uint32_t(count));
		};
		const std::int32_t* record = nullptr;
		if (inside(specialBase, SpecialTypes)) record = special + size_t(tileIndex - specialBase) * RecordInts;
		else if (inside(fallbackBase, SpecialTypes)) record = special + size_t(tileIndex - fallbackBase) * RecordInts;
		else
			for (int range = 0; range < RangeCount && !record; ++range)
			{
				const int index = int(std::uint32_t(tileIndex) - std::uint32_t(rangeStarts[range]));
				if (index >= 0 && index < RangeTypes) record = regular + size_t(index) * RecordInts;
			}
		if (!record || record[1] != subtile || record[0] == 0) return {};
		return {int(std::uint32_t(record[0]) - 1u), record[2], record[3], true};
	}

	// 6D7C00, 4802A0 and 547230: the point 4AED70 centers a cliff shadow on, from the cell's tile
	// position (TerrainImage::TilePosition, which already holds the level and ViewBounds.Y).
	constexpr Point CliffCenter(Point tile, int x, int y, int viewX)
	{
		return {tile.X + TerrainImage::TileWidth / 2 + x + viewX, tile.Y + TerrainImage::LevelHeight + y};
	}

	// 4802A0: ZAdjust of a cliff shadow; the level is the cell's signed byte.
	constexpr int CliffShadowDepth(int level) { return CliffDepth - TerrainImage::LevelHeight * level; }
}
