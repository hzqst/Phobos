#include <Render/UnitImage.h>
#include <climits>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "UnitImage failed: %s\n", what); ++Failures; }
	}

	// Bridge height and level height of the native globals ([A8F234], [A8F240]).
	constexpr int BridgeHeight = 416, Step = 104;
}

int main()
{
	using namespace UnitImage;
	Check(InMargins(-360, -180, 800, 600) && InMargins(1160, 780, 800, 600) && !InMargins(-361, 0, 800, 600) && !InMargins(0, 781, 800, 600),
		"the projection margins include both ends");

	{
		const auto normal = Wrapped(0, false, false, false), indistinct = Wrapped(1, false, false, false);
		Check(normal.Drawn && normal.Shadow && !normal.Bits && indistinct.Drawn && indistinct.Shadow && indistinct.Bits == 2,
			"normal and indistinct draw with their shadow");
		const auto darken = Wrapped(2, false, false, false), shadowy = Wrapped(3, false, false, false);
		Check(darken.Drawn && !darken.Shadow && darken.Bits == 4 && shadowy.Bits == 4 && !shadowy.Shadow, "darken and shadowy halve, without a shadow");
		Check(Wrapped(4, true, false, false).Bits == 4 && Wrapped(4, false, false, false).Bits == 2 && !Wrapped(4, false, false, false).Shadow,
			"ripple by the cloak's progress");
		Check(!Wrapped(5, false, false, false).Drawn && !Wrapped(6, false, false, false).Drawn && !Wrapped(-1, false, false, false).Drawn,
			"hidden and unknown visuals draw nothing");
		Check(Wrapped(0, false, true, false).Bits == 4 && Wrapped(1, false, true, false).Bits == 6 && Wrapped(0, false, true, true).Bits == 6
			&& Wrapped(0, false, true, false).Shadow, "warping ORs 4, 6 when absorbing, and keeps the shadow");
	}

	Check(FootDepth(0, false, 30, -15) == -17 && FootDepth(0, true, 30, -15) == -32 && FootDepth(10, false, 0, 5) == 13, "foot depths");
	Check(FootDepth(INT_MIN, true, 1, 0) == INT_MAX - 2, "foot depths wrap");
	Check(ShadowDepth(0) == -4 && ShadowDepth(30) == -34, "the wrapper's shadow depth");

	Check(HeightLight(true, false, 0, 0, BridgeHeight) && !HeightLight(false, false, 1000, 0, BridgeHeight), "OnBridge takes the height path");
	Check(HeightLight(false, true, 209, 0, BridgeHeight) && !HeightLight(false, true, 208, 0, BridgeHeight), "above half the bridge height, strictly");
	Check(LevelLight(416, Step, 10, 900) == 920 && LevelLight(207, Step, 10, 900) == 900 && LevelLight(-208, Step, 10, -5) == -15,
		"whole double levels of the signed height");
	Check(GroundLight(true, 900) == 400 && GroundLight(false, -20) == -20, "500 off a raised cell");

	Check(ShadowHeight(500, BridgeHeight, false, false, true, true) == 500 && ShadowHeight(400, BridgeHeight, true, false, true, true) == 400,
		"no bridge, or below it");
	Check(ShadowHeight(500, BridgeHeight, true, true, true, false) == 84 && ShadowHeight(500, BridgeHeight, true, true, false, true) == 500,
		"flag 800h asks the north neighbour");
	Check(ShadowHeight(500, BridgeHeight, true, false, true, false) == 500 && ShadowHeight(416, BridgeHeight, true, false, false, true) == 0,
		"otherwise the west one");
	Check(InfantryY(100, -3) == 103 && InfantryShadowY(100, 5, 20) == 115 && InfantryShadowDepth(30) == -35, "INFSHDW position and depth");

	{
		const auto over = BulletLevel(false, true, 500, 0, BridgeHeight), onBridge = BulletLevel(true, true, 500, 0, BridgeHeight);
		Check(over.Height == 84 && over.Floor == BridgeHeight && onBridge.Height == 500 && onBridge.Floor == 0, "a bullet over a bridge");
		const auto below = BulletLevel(false, true, 415, 10, BridgeHeight);
		Check(below.Height == 415 && below.Floor == 10, "a bullet below the bridge");
	}
	Check(BulletShadowDepth(15) == -25 && BulletDepth(40, 0) == -70 && BulletDepth(40, 25) == -45, "bullet depths, Phobos' ZAdjust");

	if (Failures) return 1;
	std::puts("UnitImage: margins, visuals, wrapper and shadow depths, infantry light and INFSHDW, and bullet levels and depths passed.");
	return 0;
}
