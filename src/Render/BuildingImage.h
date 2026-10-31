#pragma once

#include <cstdint>

// Pure rules of the live buildings of the tactical Tile pass (gamemd-reverse #22): step 8
// (6D3870 -> 6D9920) walks the ground layer in order and 43CEA0 -> 43D290 draws each building's
// static parts through 705E00, which adds each part's own shadow, all through 4AED70 with a Z test
// and write (6E00, 6E01).
namespace BuildingImage
{
	// 43D290's BUILDNGZ.SHA offset before the moves; a building that wide or wider draws no Z shape.
	constexpr int ZShapeX = 198, ZShapeY = 446, NoZShapeWidth = 8;
	// 43D290: the NormalZAdjust of the roof and of the other unloading images.
	constexpr int RoofZAdjust = -40, DeployingZAdjust = -20;
	// 43D290's ZAdjust of a bib (-1 - h) and of a door below (-h); 705E00 subtracts this from each,
	// and puts a shadow at -2 - h before it.
	constexpr int BibZAdjust = -1, WrapperDepth = 2, ShadowZAdjust = -2;
	constexpr int FullBrightness = 1000;

	constexpr int Add32(int a, int b) { return int(std::uint32_t(a) + std::uint32_t(b)); }
	constexpr int Sub32(int a, int b) { return int(std::uint32_t(a) - std::uint32_t(b)); }

	// 43D290: the body draws GetCurrentFrame (43EF90), at most half the shape's frames.
	constexpr int BodyFrame(int current, std::int16_t frames) { return current >= frames / 2 ? frames / 2 : current; }

	// 705E00's ZAdjust for a part given h (6D20E0 of GetZ): the body N - h, a bib -1 - h, a door below
	// -h, each less 2; the shadow -2 - h less 2.
	constexpr int BodyDepth(int normalZAdjust, int height) { return Sub32(Sub32(normalZAdjust, height), WrapperDepth); }
	constexpr int BibDepth(int height) { return Sub32(Sub32(BibZAdjust, height), WrapperDepth); }
	constexpr int DoorDepth(int height) { return Sub32(Sub32(0, height), WrapperDepth); }
	constexpr int ShadowDepth(int height) { return Sub32(Sub32(ShadowZAdjust, height), WrapperDepth); }

	// 43D290's Z shape offset: the type's ZShapePointMove unless it is constructing or selling (Phobos'
	// 43D6E5 lets OnBuildup keep it), less the projection (6D1FE0) of the Foundation's far corner.
	struct Offset { int X = 0, Y = 0; };
	constexpr Offset ZShapeOffset(bool pointMove, int moveX, int moveY, int cornerX, int cornerY)
	{
		return {Sub32(pointMove ? Add32(ZShapeX, moveX) : ZShapeX, cornerX), Sub32(pointMove ? Add32(ZShapeY, moveY) : ZShapeY, cornerY)};
	}
	// 43D290 projects the Foundation's far corner (W - 1, H - 1) in leptons.
	constexpr int CornerLeptons(int cells) { return Sub32(int(std::uint32_t(cells) << 8), 256); }

	// 43D290's gate frame while it opens or closes: truncated progress times GateStages, reversed while
	// closing (UnloadTimer states 1, 0), 0 when idle (0, 0) and the last when open (0, 1), clamped to
	// the stages, then after the GateStages + 1 healthy frames when damaged.
	constexpr int GateFrame(int progress, int stages, bool closing, bool idle, bool open, bool damaged)
	{
		int frame = closing ? Sub32(stages, progress) : progress;
		if (idle) frame = 0;
		if (open) frame = Sub32(stages, 1);
		if (frame >= stages) frame = Sub32(stages, 1);
		if (frame < 0) frame = 0;
		return Add32(damaged ? Add32(stages, 1) : 0, frame);
	}

	// 43D290's door below: frame 1 unless healthier than ConditionYellow.
	constexpr int DoorFrame(bool healthy) { return healthy ? 0 : 1; }

	// 4D1890's building records (kind 6, step 4, gamemd-reverse #22 §6.2) call 4AED70 directly, with
	// h of the saved Location: the body N - h - 2 with gradient 2 and the Z shape at its own offset
	// (no width guard, no mission condition), or -h - 2 with gradient 0 and none for a laser fence or
	// firestorm part; its shadow -h - 2; the bib -h - 3 (BibDepth); a saved anim its saved ZAdjust - h - 2,
	// testing Z only.
	constexpr int FoggedZShapeX = 144, FoggedZShapeY = 172;
	constexpr int FoggedBodyDepth(int normalZAdjust, int height, bool flat) { return flat ? DoorDepth(height) : BodyDepth(normalZAdjust, height); }
	constexpr int FoggedShadowDepth(int height) { return DoorDepth(height); }
	constexpr int FoggedAnimDepth(int savedZAdjust, int height) { return BodyDepth(savedZAdjust, height); }
	constexpr Offset FoggedZShapeOffset(int moveX, int moveY, int cornerX, int cornerY)
	{
		return {Sub32(Add32(moveX, FoggedZShapeX), cornerX), Sub32(Add32(moveY, FoggedZShapeY), cornerY)};
	}
	// 4D1890 cuts the clip of a record's body and shadow at the image's bottom: half its (signed) height
	// below the center, given relative to the clip's top. Nothing of them draws where this is not positive.
	constexpr int FoggedClipHeight(int clipHeight, std::int16_t imageHeight, int centerY)
	{
		const int bottom = Add32(imageHeight / 2, centerY);
		return clipHeight > bottom ? bottom : clipHeight;
	}
}
