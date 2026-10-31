#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Pure rules of the AnimClass draws of mode 2 (gamemd-reverse #27 §4): RenderLayers (6D8DB0) calls
// 422C70, which goes through 5F4B10 to DrawIt (422CA0) while LoopDelay is 0; DrawIt draws each part
// straight through 4AED70, with remap 0 and no Z shape.
namespace AnimImage
{
	constexpr int Add32(int a, int b) { return int(std::uint32_t(a) + std::uint32_t(b)); }
	constexpr int Sub32(int a, int b) { return int(std::uint32_t(a) - std::uint32_t(b)); }

	// 4AED70's flags: the shadow blitter, the translucency of a colored one (25, 50, 75%), the alpha
	// lighting DrawIt adds to a colored draw, the gradient that selects a Z-reading blitter and the Z
	// write. Both selector masks are 3000h (#27 §4.7): 4000h writes Z, else 1000h or 2000h reads it.
	constexpr std::uint32_t ShadowFlag = 0x1, TranslucencyMask = 0x6, Translucent25 = 0x2, Translucent50 = 0x4, Translucent75 = 0x6,
		AlphaFlag = 0x800, ZReadMask = 0x3000, GradientFlag = 0x2000, ZWriteFlag = 0x4000;
	// The shadow of HasExtras, and the bits DrawIt sets on a Type.Shadow shadow.
	constexpr std::uint32_t ExtrasShadowFlags = 0x2601, ShadowFlags = 0x601;
	constexpr int FullBrightness = 1000;
	// DrawIt's ZAdjust below Y + ZAdjust - h: a flat body, a plain one and a tiled one.
	constexpr int FlatDepth = 3, BodyDepth = 2, TiledDepth = 50, ShadowDepth = 2;
	// Type.Translucency's level, and the staged thresholds of Translucent=yes (of Value / End).
	constexpr int OpaqueLevel = 15;

	// 422CA0: Start + Animation.Value, wrapping.
	constexpr int Frame(int start, int value) { return Add32(start, value); }

	// UnderTemporal: 75% when the type is DoubleThick, else 50%.
	constexpr std::uint32_t TemporalBits(bool doubleThick) { return doubleThick ? Translucent75 : Translucent50; }

	struct Translucency
	{
		bool Skip = false;
		std::uint32_t Bits = 0;
	};

	// Phobos' 423183, which takes over Translucent=no: the level of the curve (a blitter flag, 0 for
	// none) of Translucency.Cloaked or Translucency, else the building's TranslucencyLevel (read
	// unsigned) maps to 50% above 5 and 25% below; above 15 (with a curve, from 15) nothing draws.
	constexpr Translucency Curve(unsigned level, std::uint32_t curve)
	{
		if (!curve)
		{
			if (!level) return {};
			if (level > unsigned(OpaqueLevel)) return {true};
			return {false, level > 5 ? Translucent50 : Translucent25};
		}
		if (level >= unsigned(OpaqueLevel)) return {true};
		return {false, curve};
	}

	// 422CA0's Translucent=yes: from TranslucencyLevel 15 (read signed) nothing draws; Value beyond
	// 0.6, 0.4 and 0.2 of End draws 75, 50 and 25%.
	constexpr Translucency Staged(std::int8_t level, int value, int end)
	{
		if (level >= OpaqueLevel) return {true};
		const double current = value, last = end;
		if (current > last * 0.6) return {false, Translucent75};
		if (current > last * 0.4) return {false, Translucent50};
		if (current > last * 0.2) return {false, Translucent25};
		return {};
	}

	// F: AnimFlags with the translucency ORed in, and the alpha lighting where it is not a shadow.
	constexpr std::uint32_t ColorFlags(std::uint32_t flags) { return flags & ShadowFlag ? flags : flags | AlphaFlag; }
	// A body adds the gradient flag; the Type.Shadow shadow keeps F's other bits, Z ones included.
	constexpr std::uint32_t BodyFlags(std::uint32_t flags) { return flags | GradientFlag; }
	constexpr std::uint32_t ShadowOf(std::uint32_t flags) { return (flags & ~TranslucencyMask) | ShadowFlags; }

	// The Z blitter flags select: test (and write), or neither.
	struct ZMode
	{
		bool Test = false, Write = false;
	};
	constexpr ZMode DepthMode(std::uint32_t flags)
	{
		const bool write = (flags & ZWriteFlag) != 0;
		return {write || (flags & ZReadMask) != 0, write};
	}

	// ZAdjust given Y (Type.YDrawOffset), the anim's ZAdjust and h (6D20E0 of 425630's Z): a plain
	// body Y + ZAdjust - h - 2, a flat one - 3, a tiled one's first - 50; the Type.Shadow shadow -2 - h;
	// HasExtras' shadow Y - h.
	constexpr int Depth(int y, int zAdjust, int height, int below) { return Sub32(Sub32(Add32(y, zAdjust), height), below); }
	constexpr int ShadowZAdjust(int height) { return Sub32(-ShadowDepth, height); }
	constexpr int ExtrasZAdjust(int y, int height) { return Sub32(y, height); }

	// The Type.Shadow frame: the frame plus half the (signed) frames of the image.
	constexpr int ShadowFrame(int frame, std::int16_t frames) { return Add32(frames / 2, frame); }

	// 422CA0's tiled loop, from P.Y less the half height (Phobos' 423660 may make it 0) with the step
	// (Phobos' 423654 may replace the frame 0 height): each round draws at the row, ends after the one
	// whose row is negative, and moves the row up by the step and the ZAdjust by half and step.
	struct TiledStep
	{
		int Y = 0, ZAdjust = 0;
	};
	// False when the loop would run past limit rounds (the native one never ends for a step of 0 or
	// less from a row that is not negative).
	inline bool TiledSteps(int y, int step, int half, int zAdjust, std::vector<TiledStep>& steps, size_t limit)
	{
		steps.clear();
		int row = Sub32(y, half);
		for (;;)
		{
			if (steps.size() >= limit) return false;
			steps.push_back({row, zAdjust});
			const bool finished = row < 0;
			row = Sub32(row, step);
			zAdjust = Sub32(zAdjust, Add32(half, step));
			if (finished) return true;
		}
	}
}
