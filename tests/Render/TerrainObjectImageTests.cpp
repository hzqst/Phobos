#include <Render/TerrainObjectImage.h>
#include <climits>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "TerrainObjectImage failed: %s\n", what); ++Failures; }
	}
}

int main()
{
	using namespace TerrainObjectImage;
	Check(StaticFrame(false) == 0 && StaticFrame(true) == 1, "static frames");
	Check(AnimationLength(false, 0, 40, false) == 20 && AnimationLength(false, 0, 40, true) == 10 && AnimationLength(true, 7, 40, true) == 7
		&& AnimationLength(false, 0, 41, true) == 10 && AnimationLength(true, -2, 40, false) == -2, "animation lengths");
	Check(DynamicFrame(true, false, false, false, false, 3, 20) == 3 && DynamicFrame(true, false, false, true, true, 3, 10) == 13,
		"animated frames, damaged after the animation");
	Check(DynamicFrame(true, true, true, true, false, 2, 10) == 23 && DynamicFrame(true, true, true, false, true, 2, 10) == 13,
		"animated crumbling frames after the animations");
	Check(DynamicFrame(true, true, false, false, false, 2, 10) == 2, "crumbling without its frames keeps animating");
	Check(DynamicFrame(false, true, true, false, false, 4, 0) == 5 && DynamicFrame(false, true, false, false, true, 4, 0) == 1
		&& DynamicFrame(false, true, false, false, false, 4, 0) == 0, "crumbling frames of a still object");
	Check(ShadowFrame(0, 4) == 2 && ShadowFrame(1, 4) == 3 && ShadowFrame(1, 5) == 3, "shadow frames");

	{
		// 6D8DB0: (tx + W/2, ty - H), (tx + 2W, ty + H/2), (tx - W, ty + H/2).
		const auto probes = RangeProbes(1000, 500, 857, 737);
		Check(probes.Top.X == 1428 && probes.Top.Y == -237 && probes.Right.X == 2714 && probes.Right.Y == 868
			&& probes.Left.X == 143 && probes.Left.Y == 868, "view range probes");
		Check(RangeProbes(INT_MAX, 0, 2, 0).Top.X == INT_MIN, "view range probes wrap");
		const auto range = MakeViewRange(100, -50, 160, 30);
		Check(range.Left == 100 && range.Top == -50 && range.Width == 120 && range.Height == 160, "view range spans twice the differences");
		Check(InViewRange(range, 100, -50) && InViewRange(range, 219, 109) && !InViewRange(range, 220, 0) && !InViewRange(range, 99, 0)
			&& !InViewRange(range, 150, 110) && !InViewRange(range, 150, -51), "view range is half open");
		Check(!InViewRange(MakeViewRange(10, 0, 5, 10), 8, 5), "a reversed range holds nothing");
	}
	Check(ShadowFrame(0, std::int16_t(-3)) == -1, "frames divide toward zero");
	Check(Depth(0, false) == -12 && Depth(0, true) == -3 && Depth(30, false) == -42 && Depth(-15, true) == 12, "object depths");

	// 6D7C00: rows H / 15 + 20, columns W / 60 + 7 from the anchor moved by (-2, +3).
	Check(CliffRows(600) == 60 && CliffColumns(800) == 20, "cliff scan size");
	{
		const auto first = CliffCell({50, 60}, 0, 0), next = CliffCell({50, 60}, 0, 1);
		const auto row1 = CliffCell({50, 60}, 1, 0), row2 = CliffCell({50, 60}, 2, 0);
		Check(first.X == 48 && first.Y == 63 && next.X == 49 && next.Y == 62, "cliff scan start and column step");
		Check(row1.X == 48 && row1.Y == 64 && row2.X == 49 && row2.Y == 64, "cliff rows alternate their step");
	}

	{
		// Records: frame + 1, subtile, x, y.
		std::int32_t special[SpecialTypes * RecordInts] {};
		std::int32_t regular[RangeTypes * RecordInts] {};
		const std::int32_t starts[RangeCount] {100, 200, 300, 400, 500};
		special[3 * RecordInts + 0] = 5; special[3 * RecordInts + 1] = 2; special[3 * RecordInts + 2] = -7; special[3 * RecordInts + 3] = 9;
		regular[39 * RecordInts + 0] = 1; regular[39 * RecordInts + 1] = 0; regular[39 * RecordInts + 2] = 4; regular[39 * RecordInts + 3] = -6;
		regular[1 * RecordInts + 0] = 0; regular[1 * RecordInts + 1] = 0;
		const auto a = SelectCliffShadow(53, 2, 50, 70, special, starts, regular);
		Check(a.Drawn && a.Frame == 4 && a.X == -7 && a.Y == 9, "special range record");
		const auto b = SelectCliffShadow(73, 2, 50, 70, special, starts, regular);
		Check(b.Drawn && b.Frame == 4, "fallback special range shares the records");
		Check(!SelectCliffShadow(53, 1, 50, 70, special, starts, regular).Drawn, "a different subtile draws nothing");
		const auto c = SelectCliffShadow(339, 0, 50, 70, special, starts, regular);
		Check(c.Drawn && c.Frame == 0 && c.X == 4 && c.Y == -6, "the ranges share one table");
		Check(!SelectCliffShadow(201, 0, 50, 70, special, starts, regular).Drawn, "frame 0 draws nothing");
		Check(!SelectCliffShadow(140, 0, 50, 70, special, starts, regular).Drawn && !SelectCliffShadow(99, 0, 50, 70, special, starts, regular).Drawn,
			"outside every range");
		// A special range takes precedence over a regular one.
		Check(SelectCliffShadow(103, 2, 100, 70, special, starts, regular).X == -7, "special range first");
	}

	// The tile position (100, 50) is the diamond's top-left less 30; the record moves by (dx, dy + 15).
	{
		const auto center = CliffCenter({100, 50}, -7, 9, 0);
		Check(center.X == 123 && center.Y == 74, "cliff center");
		Check(CliffCenter({100, 50}, 0, 0, 8).X == 138, "cliff center adds ViewBounds.X");
	}
	Check(CliffShadowDepth(0) == 58 && CliffShadowDepth(2) == 28 && CliffShadowDepth(-1) == 73, "cliff depths");

	if (Failures) return 1;
	std::puts("TerrainObjectImage: frames, depths, cliff scan, records and centers passed.");
	return 0;
}
