#include <Render/OverlayImage.h>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "OverlayImage failed: %s\n", what); ++Failures; }
	}

	// 81CC30.
	constexpr std::int32_t Pattern[16] {0, 1, 2, 3, 3, 2, 1, 0, 2, 3, 0, 1, 1, 0, 3, 2};
}

int main()
{
	using namespace OverlayImage;
	// 6D6D10: rows m + 1 .. 0 with m = H / 15 + 20, columns W / 60 + 4.
	Check(ScanRows(600) == 62 && ScanColumns(800) == 17, "scan size");
	{
		const auto first = ScanCell({50, 60}, 0, 0), next = ScanCell({50, 60}, 0, 1);
		const auto row1 = ScanCell({50, 60}, 1, 0), row2 = ScanCell({50, 60}, 2, 0), before = ScanCell({50, 60}, -1, 0);
		Check(first.X == 46 && first.Y == 58 && next.X == 47 && next.Y == 57, "scan start and column step");
		Check(row1.X == 46 && row1.Y == 59 && row2.X == 47 && row2.Y == 59, "rows alternate their step");
		Check(before.X == 45 && before.Y == 58 && before.X + before.Y == first.X + first.Y - 1, "a margin row continues the lattice");
	}

	Check(BaseOffset(false, false, false, 10, 0, 10) == 0, "plain overlay offset");
	Check(BaseOffset(true, false, false, 10, 0, 10) == -12 && BaseOffset(false, true, false, 10, 0, 10) == -12
		&& BaseOffset(false, false, true, 10, 0, 10) == -12, "Tiberium, wall and crate offsets");
	Check(BaseOffset(false, false, false, 10, RailroadLand, 10) == -1, "railroad offset");
	Check(BaseOffset(false, false, false, Veins, 0, Veins) == -13, "veins offset");

	Check(Lift(false, 0, 10) == 0 && Lift(false, 0, RaisedIndex) == 15, "overlay lift");
	Check(Lift(true, 0, 24) == 16 && Lift(true, 9, 24) == 31 && Lift(true, 17, 24) == 31 && Lift(true, 18, 24) == 16, "bridge lift");
	{
		const auto center = Center({100, 50}, -12, 0);
		Check(center.X == 130 && center.Y == 53, "center of a low overlay");
		const auto bridge = Center({100, 50}, 0, Lift(true, 9, 24));
		Check(bridge.X == 130 && bridge.Y == 34, "center of a raised bridge piece");
		const auto shadow = ShadowCenter(bridge, true, 9), flat = ShadowCenter(bridge, true, 3);
		Check(shadow.X == 115 && shadow.Y == 41 && flat.X == 130 && flat.Y == 34, "bridge shadow shift");
		Check(ShadowCenter(bridge, false, 9).X == 130, "only bridges shift their shadow");
	}
	{
		const auto origin = FrameOrigin({130, 53}, 60, 30, 4, 2);
		Check(origin.X == 104 && origin.Y == 40, "frame origin");
		// Odd canvases truncate their half.
		const auto odd = FrameOrigin({130, 53}, 61, 31, 0, 0);
		Check(odd.X == 100 && odd.Y == 38, "odd canvas");
	}

	Check(HeightAdjust(2, false, 0) == 30 && HeightAdjust(2, true, 0) == 90 && HeightAdjust(0, false, 5) == -5, "height adjust");
	Check(BridgeFrame(0, 0, 0, Pattern) == 0 && BridgeFrame(0, 3, 0, Pattern) == 3 && BridgeFrame(9, 1, 1, Pattern) == 11
		&& BridgeFrame(9, 7, 3, Pattern) == 11 && BridgeFrame(4, 3, 3, Pattern) == 4, "bridge pattern");

	Check(RampAllowed(1, true, true) && RampAllowed(4, true, true), "cardinal ramps");
	Check(!RampAllowed(0, true, true) && !RampAllowed(5, true, true) && !RampAllowed(2, false, true) && !RampAllowed(2, true, false),
		"ramps without permission or art");

	// y * x = 12: 12 % 4 = 0 flat; ramp slope 3 of 8 slopes: 12 % 2 + 2 * 2 after the 4 flat images.
	Check(TiberiumImage(3, 4, 0, false, 100, 4, 8) == 100, "flat Tiberium image");
	Check(TiberiumImage(3, 5, 0, false, 100, 4, 8) == 103, "flat Tiberium image by position");
	Check(TiberiumImage(3, 4, 3, true, 100, 4, 8) == 108 && TiberiumImage(3, 5, 3, true, 100, 4, 8) == 109, "ramp Tiberium image");
	Check(TiberiumImage(-3, 5, 0, false, 100, 4, 8) == 97, "the remainder keeps the product's sign");
	Check(TiberiumImage(3, 4, 0, false, 100, 0, 8) == -1 && TiberiumImage(3, 4, 1, true, 100, 4, 3) == -1, "division by zero");

	Check(ShadowFrame(20, 3, false) == 13 && ShadowFrame(20, 3, true) == 10 && ShadowFrame(21, 0, false) == 10, "shadow frame");
	Check(ShadowFrame(std::int16_t(-3), 0, false) == -1, "frames divide toward zero");

	if (Failures) return 1;
	std::puts("OverlayImage: scan, offsets, lifts, centers, frames, ramps and Tiberium images passed.");
	return 0;
}
