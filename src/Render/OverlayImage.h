#pragma once

#include "TerrainImage.h"

// Pure rules of the overlay layer of the tactical Tile pass (gamemd-reverse #18): 6D6D10 scans the
// cells and draws every overlay body (47F6A0), then every overlay shadow (47F510), through 4AED70
// centered on a point 480110 places relative to the cell's diamond.
namespace OverlayImage
{
	using TerrainImage::Point;

	// Overlay indices the native code tests by number.
	constexpr int NoBodyA = 167, NoBodyB = 178, Veins = 126, RaisedIndex = 239;
	constexpr int RailroadLand = 9;
	// Bridge data in this range is a bridge piece that rises a cell more.
	constexpr int HighBridgeFirst = 9, HighBridgeLast = 17;
	constexpr int BridgeLift = 16, CellLift = 15;
	// 5FDCC0 lowers Tiberium, walls, veins and crates by this much.
	constexpr int LowOverlayOffset = -12;
	// A raised bridge piece's shadow moves by this.
	constexpr int BridgeShadowX = -15, BridgeShadowY = 7;
	// 81CC30 has 16 entries; bridge data 0 and 9 add one by the cell's position.
	constexpr int BridgePatternSize = 16, PatternedBridgeA = 0, PatternedBridgeB = 9;
	// Phobos ramps: cardinal slopes only, with at least two images per slope.
	constexpr int MaxCardinalRamp = 4, RampImages = 8, SlopeQuarters = 4;
	// 47F6A0's height adjust: a bridge counts four levels.
	constexpr int BridgeLevels = 4;

	// 6D6D10's scan of a rectangle: rows from near to far, each walked by (+1,-1) from the anchor
	// moved by (-4,-2) plus (floor(r/2), floor((r+1)/2)).
	constexpr int ScanRows(int height) { return height / 15 + 22; }
	constexpr int ScanColumns(int width) { return width / TerrainImage::TileWidth + 4; }
	constexpr TerrainImage::Cell ScanCell(TerrainImage::Cell anchor, int row, int column)
	{
		return {anchor.X - 4 + TerrainImage::FloorHalf(row) + column, anchor.Y - 2 + TerrainImage::FloorHalf(row + 1) - column};
	}

	// 5FDCC0: the vertical offset of an overlay type. arrayIndex is the type's, index the cell's.
	constexpr int BaseOffset(bool tiberium, bool wall, bool crate, int arrayIndex, int landType, int index)
	{
		int offset = tiberium || wall || arrayIndex == Veins || crate ? LowOverlayOffset : 0;
		if (landType == RailroadLand) --offset;
		if (index == Veins) --offset;
		return offset;
	}

	// 480110: how far a bridge piece or overlay 239 rises above its base offset.
	constexpr int Lift(bool bridge, int data, int index)
	{
		if (bridge) return BridgeLift + (data >= HighBridgeFirst && data <= HighBridgeLast ? CellLift : 0);
		return index == RaisedIndex ? CellLift : 0;
	}

	// 480110 with 6D6D10's client position: the point 4AED70 centers the shape on, relative to the
	// top-left of the cell's diamond.
	constexpr Point Center(Point tile, int baseOffset, int lift)
	{
		return {tile.X + TerrainImage::TileWidth / 2, tile.Y + TerrainImage::CellHeight / 2 + baseOffset - lift};
	}

	// 47F510: a raised bridge piece's shadow moves.
	constexpr Point ShadowCenter(Point center, bool bridge, int data)
	{
		if (bridge && data >= HighBridgeFirst && data <= HighBridgeLast) return {center.X + BridgeShadowX, center.Y + BridgeShadowY};
		return center;
	}

	// 4AED70 with flag 200: the top-left of a frame centered on a point.
	constexpr Point FrameOrigin(Point center, int canvasWidth, int canvasHeight, int frameLeft, int frameTop)
	{
		return {center.X - canvasWidth / 2 + frameLeft, center.Y - canvasHeight / 2 + frameTop};
	}

	// 47F6A0 (with Phobos' ZAdjust at 47F71D): the height the body's ZAdjust starts from.
	constexpr int HeightAdjust(int level, bool bridge, int typeZAdjust)
	{
		return TerrainImage::LevelHeight * (level + (bridge ? BridgeLevels : 0)) - typeZAdjust;
	}

	// 47F6A0: bridge data 0 and 9 add a frame from 81CC30 by the cell's position.
	constexpr int BridgeFrame(int data, int x, int y, const std::int32_t* pattern)
	{
		if (data != PatternedBridgeA && data != PatternedBridgeB) return data;
		return data + pattern[(x & 3) | ((y & 3) << 2)];
	}

	// Phobos' CanResourceGerminateOnRamp at 47F87D: a slope draws ramp art only for a cardinal slope
	// of a Tiberium with AllowRamps and ramp overlays, which then counts at least RampImages slopes.
	constexpr bool RampAllowed(int slope, bool allowRamps, bool hasRampArt)
	{
		return slope >= 1 && slope <= MaxCardinalRamp && allowRamps && hasRampArt;
	}

	// 47F6A0: the overlay type whose image a Tiberium cell draws, from p = y * x of the cell. A
	// ramp picks among numSlopes / 4 images per slope after the numImages flat ones. -1 where the
	// native division would fault.
	constexpr int TiberiumImage(std::int16_t x, std::int16_t y, int slope, bool ramp, int imageBase, int numImages, int numSlopes)
	{
		const int product = int(y) * int(x);
		if (ramp)
		{
			const int perSlope = numSlopes / SlopeQuarters;
			if (perSlope == 0) return -1;
			return int(std::uint32_t(product % perSlope) + std::uint32_t(slope - 1) * std::uint32_t(perSlope)
				+ std::uint32_t(imageBase) + std::uint32_t(numImages));
		}
		if (numImages == 0) return -1;
		return int(std::uint32_t(product % numImages) + std::uint32_t(imageBase));
	}

	// 47F510: the shadow is the second half of the type's own frames.
	constexpr int ShadowFrame(std::int16_t frames, int data, bool crate)
	{
		return frames / 2 + (crate ? 0 : data);
	}
}
