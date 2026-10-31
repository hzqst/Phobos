#include <Render/AnimImage.h>
#include <climits>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "AnimImage failed: %s\n", what); ++Failures; }
	}
}

int main()
{
	using namespace AnimImage;
	Check(Frame(3, 4) == 7 && Frame(INT_MAX, 1) == INT_MIN, "frames add Start and Value, wrapping");
	Check(TemporalBits(true) == Translucent75 && TemporalBits(false) == Translucent50, "temporal translucency");

	{
		const auto none = Curve(0, 0), low = Curve(5, 0), mid = Curve(6, 0), top = Curve(15, 0), hidden = Curve(16, 0);
		Check(!none.Skip && none.Bits == 0 && low.Bits == Translucent25 && mid.Bits == Translucent50 && top.Bits == Translucent50 && hidden.Skip,
			"without a curve the building's level maps to 25 or 50%, above 15 hides");
		const auto curve = Curve(14, Translucent75), opaque = Curve(15, Translucent25);
		Check(!curve.Skip && curve.Bits == Translucent75 && opaque.Skip, "a curve's level applies below 15");
		Check(Curve(200, 0).Skip, "the level is read unsigned");
	}
	{
		Check(Staged(0, 7, 10).Bits == Translucent75 && Staged(0, 6, 10).Bits == Translucent50 && Staged(0, 5, 10).Bits == Translucent50
			&& Staged(0, 4, 10).Bits == Translucent25 && Staged(0, 3, 10).Bits == Translucent25 && Staged(0, 2, 10).Bits == 0,
			"Translucent=yes stages by Value beyond 0.6, 0.4 and 0.2 of End");
		Check(Staged(0, 3, 5).Bits == Translucent50 && Staged(0, 1, 5).Bits == 0, "stage thresholds are strict in double precision");
		Check(Staged(15, 0, 10).Skip && !Staged(-1, 0, 10).Skip, "the level is read signed");
	}

	Check(ColorFlags(0x600) == 0xE00 && ColorFlags(0x601) == 0x601 && ColorFlags(0x1606) == 0x1E06, "colored draws add the alpha lighting");
	Check(BodyFlags(0xE00) == 0x2E00 && BodyFlags(0x1E00) == 0x3E00 && BodyFlags(0x4E04) == 0x6E04, "bodies add the gradient flag");
	Check(ShadowOf(0xE00) == 0xE01 && ShadowOf(0x1E06) == 0x1E01 && ShadowOf(0x4E02) == 0x4E01, "shadows keep F's Z bits and drop its translucency");

	{
		const auto standalone = DepthMode(0x2E00), slot = DepthMode(0x3E00), shadow = DepthMode(0xE01), extras = DepthMode(ExtrasShadowFlags),
			slotShadow = DepthMode(0x1E01), write = DepthMode(0x6E00);
		Check(standalone.Test && !standalone.Write && slot.Test && !slot.Write && slotShadow.Test && !slotShadow.Write, "Z-reading draws");
		Check(!shadow.Test && !shadow.Write, "a standalone anim's shadow neither tests nor writes Z");
		Check(extras.Test && !extras.Write && write.Test && write.Write, "the extras shadow reads, 4000h writes");
	}

	Check(Depth(10, 5, 30, BodyDepth) == -17 && Depth(10, 5, 30, FlatDepth) == -18 && Depth(10, 5, 30, TiledDepth) == -65, "body depths");
	Check(ShadowZAdjust(30) == -32 && ExtrasZAdjust(10, 30) == -20 && Depth(INT_MIN, 0, 1, 0) == INT_MAX, "shadow depths, wrapping");
	Check(ShadowFrame(3, 20) == 13 && ShadowFrame(3, -21) == -7, "shadow frames take half the signed frames");

	{
		std::vector<TiledStep> steps;
		Check(TiledSteps(100, 40, 20, -50, steps, 16) && steps.size() == 4 && steps[0].Y == 80 && steps[0].ZAdjust == -50
			&& steps[1].Y == 40 && steps[1].ZAdjust == -110 && steps[2].Y == 0 && steps[3].Y == -40 && steps[3].ZAdjust == -230,
			"tiles go up a step at a time and end after the first negative row");
		Check(TiledSteps(10, 40, 20, 0, steps, 16) && steps.size() == 1 && steps[0].Y == -10, "a first negative row draws once");
		Check(TiledSteps(10, 40, 0, 0, steps, 16) && steps.size() == 2 && steps[1].Y == -30 && steps[1].ZAdjust == -40,
			"Phobos' alignment drops the half height from the start and the steps");
		Check(!TiledSteps(10, 0, 0, 0, steps, 16) && steps.size() == 16, "a step of 0 never ends");
	}

	if (Failures) return 1;
	std::puts("AnimImage: frames, translucency, flags, Z modes, depths and tiles passed.");
	return 0;
}
