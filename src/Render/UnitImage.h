#pragma once

#include <cstdint>

// Pure rules of the units RenderLayers (6D8DB0) draws on Composite (gamemd-reverse #26): the SHP
// wrapper every Techno draws through (705E00), the infantry of 518F90 with its INFSHDW, and the SHP
// bullets of 468090, all through 4AED70 testing Z without writing it.
namespace UnitImage
{
	constexpr int Add32(int a, int b) { return int(std::uint32_t(a) + std::uint32_t(b)); }
	constexpr int Sub32(int a, int b) { return int(std::uint32_t(a) - std::uint32_t(b)); }

	constexpr int FullBrightness = 1000;

	// 6D8DB0 draws a Foot or a plain object (a bullet) whose GetCoords project (6D1F10, less
	// TacticalPos) within these margins of the tactical view (B0CE28), both ends included.
	constexpr int MarginX = 360, MarginY = 180;
	constexpr bool InMargins(int x, int y, int width, int height)
	{
		return x >= -MarginX && x <= Add32(width, MarginX) && y >= -MarginY && y <= Add32(height, MarginY);
	}

	// 705E00's choice for VisualCharacter (0 normal, 1 indistinct, 2 darken, 3 shadowy, 4 ripple,
	// 5 hidden): the translucency bits of the body, whether it draws at all and whether its shadow
	// follows; a warping object ORs 4 into them (6 for an InfantryAbsorb building).
	struct Visual
	{
		std::uint8_t Bits = 0;
		bool Drawn = false, Shadow = false;
	};
	constexpr Visual Wrapped(int visual, bool cloaking, bool warping, bool absorbs)
	{
		Visual result;
		switch (visual)
		{
		case 0: result = {0, true, true}; break;
		case 1: result = {2, true, true}; break;
		case 2: case 3: result = {4, true, false}; break;
		case 4: result = {std::uint8_t(cloaking ? 4 : 2), true, false}; break;
		default: return {};
		}
		if (warping) result.Bits |= absorbs ? 6 : 4;
		return result;
	}

	// 705E00's ZAdjust of a Unit or an infantry, less 4AED70's 2: off the ground (GetHeight) the given
	// one less h (6D20E0 of GetZ), with no shadow, else the given one plus GetZAdjustment.
	constexpr int WrapperDepth = 2, ShadowZAdjust = -2;
	constexpr int FootDepth(int zAdjust, bool airborne, int height, int zAdjustment)
	{
		return Sub32(airborne ? Sub32(zAdjust, height) : Add32(zAdjustment, zAdjust), WrapperDepth);
	}
	// 705E00's shadow at half the frames: -2 - h, less 2, wherever the body sits; 14 higher for an
	// object on a carryall.
	constexpr int ShadowDepth(int height) { return Sub32(Sub32(ShadowZAdjust, height), WrapperDepth); }
	constexpr int CarryallLift = 14;

	// 518F90's light: the height path (OnBridge, or a cell with flag 10000h below a Location.Z above
	// floor + half the bridge height) adds GetHeight / (2 x [A8F240]) levels to the cell's signed
	// Intensity_Normal, the ground path takes 500 off it on such a cell; ExtraInfantryLight's DWORD
	// follows either. Phobos' 51933B replaces the first part for a jumpjet.
	constexpr std::uint32_t RaisedCellFlag = 0x10000, BridgeCellFlag = 0x100, BridgeDirectionFlag = 0x800;
	constexpr int RaisedCellLight = -500;
	constexpr bool HeightLight(bool onBridge, bool raisedCell, int z, int floor, int bridgeHeight)
	{
		return onBridge || (raisedCell && z > Add32(floor, bridgeHeight / 2));
	}
	// The height path; step is [A8F240], which must not be 0.
	constexpr int LevelLight(int height, int step, int level, std::int16_t intensity)
	{
		return Add32(int(std::uint32_t(height / Add32(step, step)) * std::uint32_t(level)), intensity);
	}
	constexpr int GroundLight(bool raisedCell, std::int16_t intensity) { return Add32(raisedCell ? RaisedCellLight : 0, intensity); }

	// 518F90's INFSHDW (frame 1 of the shape): over a bridge cell at or above the bridge height,
	// the height counts from the bridge where the neighbour the cell's direction picks (0 with flag
	// 800h, else 6) holds a bridge; the shadow draws where the height left is positive, that much
	// lower than the body (whose Y is the point's less int8 [6B4]), with ZAdjust -5 - 6D20E0 of
	// Location.Z less it, ground gradient, full brightness and no tint (2601).
	constexpr int InfantryShadowFrame = 1, InfantryShadowZAdjust = -5;
	constexpr int ShadowHeight(int height, int bridgeHeight, bool bridgeCell, bool directionFlag, bool northBridge, bool westBridge)
	{
		if (!bridgeCell || height < bridgeHeight) return height;
		return (directionFlag ? northBridge : westBridge) ? Sub32(height, bridgeHeight) : height;
	}
	constexpr int InfantryY(int y, std::int8_t offset) { return Sub32(y, offset); }
	constexpr int InfantryShadowY(int y, std::int8_t offset, int screenHeight) { return Add32(screenHeight, InfantryY(y, offset)); }
	constexpr int InfantryShadowDepth(int screenZ) { return Sub32(InfantryShadowZAdjust, screenZ); }

	// 468090 for a bullet that is not Location-fogged, Inviso, SpawnNextAnim or a voxel: over a
	// bridge cell, one not OnBridge at or above the bridge height counts its height from the bridge
	// and its floor on it. With Type.Shadow and a positive height its shadow draws first, at the point
	// moved down by 6D20E0 of the height, ZAdjust -10 - 6D20E0 of the floor (2601); then the body
	// at the point, ZAdjust -30 - 6D20E0 of GetZ, which Phobos' 4683F2 moves by the type's ZAdjust
	// (2E00). Both with the ground gradient, full brightness and no tint.
	constexpr int BulletShadowZAdjust = -10, BulletZAdjust = -30;
	struct Level
	{
		int Height = 0, Floor = 0;
	};
	constexpr Level BulletLevel(bool onBridge, bool bridgeCell, int height, int floor, int bridgeHeight)
	{
		if (!onBridge && bridgeCell && height >= bridgeHeight) return {Sub32(height, bridgeHeight), Add32(floor, bridgeHeight)};
		return {height, floor};
	}
	constexpr int BulletShadowDepth(int screenFloor) { return Sub32(BulletShadowZAdjust, screenFloor); }
	constexpr int BulletDepth(int screenZ, int typeZAdjust) { return Sub32(BulletZAdjust, Sub32(screenZ, typeZAdjust)); }
}
