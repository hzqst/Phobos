#include <Render/ShapeDepth.h>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "ShapeDepth failed: %s\n", what); ++Failures; }
	}

	// 817710's records 0 and 2 (Deg90).
	constexpr ShapeDepth::Gradient Flat {1, 1, 1, 1, -1, 1, {}};
	constexpr ShapeDepth::Gradient Deg90 {1, 3, 1, 3, 1, 0, {}};
	constexpr int MaxValue = 10000, BoundsY = 0, ClipY = 100, ZAdjust = -32;
}

int main()
{
	using namespace ShapeDepth;
	Check(Usable(Flat) && Usable(Deg90), "the native gradients");
	Check(!Usable(Gradient {}) && !Usable(Gradient {1, 0, 1, 1, -1, 1, {}}) && !Usable(Gradient {1, 3, 4, 3, 1, 0, {}}),
		"gradients that would divide by zero");

	{
		const auto inside = Intersect({0, 0, 39, 22}, {24, -19, 59, 43});
		Check(inside.X == 24 && inside.Y == 0 && inside.Width == 15 && inside.Height == 22, "a Z shape cuts the frame");
		const auto apart = Intersect({0, 0, 39, 22}, {39, 0, 10, 10});
		Check(apart.Width == 0 && apart.Height == 0 && apart.X == 0, "rectangles apart");
	}
	{
		const Rect whole {0, 0, 30, 20};
		const auto cut = Clip({-5, 15, 30, 20}, whole, whole, 640, 30, true);
		Check(cut.Drawn && cut.Dest.X == 0 && cut.Dest.Y == 15 && cut.Dest.Width == 25 && cut.Dest.Height == 15, "frame cut at the left and the bottom");
		Check(cut.SourceX == 5 && cut.SourceY == 0 && cut.Width == 25 && cut.Rows == 15, "the cut moves the source");
		Check(!Clip({640, 0, 30, 20}, whole, whole, 640, 30, true).Drawn && !Clip({0, -20, 30, 20}, whole, whole, 640, 30, false).Drawn,
			"frames outside the clip");
		// A Z shape that starts 24 columns into a 39x22 frame leaves a 15-column source at x 24.
		const Rect shaped {24, 0, 15, 22};
		Check(!Clip({100, 10, 15, 22}, shaped, shaped, 640, 480, true).Drawn, "an RLE source inside the frame loses its offset on the right");
		const auto plain = Clip({100, 10, 15, 22}, shaped, {0, 0, 39, 22}, 640, 480, false);
		Check(plain.Drawn && plain.SourceX == 24 && plain.Width == 15 && plain.Dest.X == 100, "a plain source is read from its position");
		const auto narrow = Clip({100, 10, 30, 22}, {4, 0, 30, 22}, {4, 0, 30, 22}, 640, 480, true);
		Check(narrow.Drawn && narrow.SourceX == 0 && narrow.Width == 26 && narrow.Dest.Width == 26, "an RLE source is read from the frame's start");
	}

	{
		// u16(10000 - 20 - 100) - 32, then one less per row.
		auto rows = Plain(Flat, MaxValue, BoundsY, ClipY, {0, 20, 30, 10}, ZAdjust);
		Check(rows.Base == 9848 && rows.Accumulator == 0, "flat plain base");
		Next(rows, Flat);
		Check(rows.Base == 9847 && rows.Accumulator == 0, "flat plain second row");
		Check(Plain(Flat, 10, BoundsY, ClipY, {0, 0, 30, 10}, 0).Base == 65446, "the row depth wraps at 16 bits");
	}

	{
		// u16(10000 - 10 - 20 - 100 + 1) - 32 = 9839, in bands of three: 9837, less 10 / 3.
		auto rows = Plain(Deg90, MaxValue, BoundsY, ClipY, {0, 20, 30, 10}, ZAdjust);
		Check(rows.Base == 9834 && rows.Accumulator == 2, "Deg90 plain base");
		const int expected[] {9835, 9835, 9835, 9836, 9836, 9836, 9837, 9837, 9837};
		bool stepped = true;
		for (const auto base : expected)
		{
			Next(rows, Deg90);
			stepped = stepped && rows.Base == base;
		}
		Check(stepped, "Deg90 plain rows step once per band");
		// A height of whole bands starts a band at once.
		const auto whole = Plain(Deg90, MaxValue, BoundsY, ClipY, {0, 20, 30, 9}, ZAdjust);
		Check(whole.Base == 9838 && whole.Accumulator == 0, "Deg90 plain with whole bands");
	}

	{
		// An uncut frame: the RLE path agrees with the plain one.
		const Rect frame {0, 15, 30, 20};
		const auto rle = RLE(Deg90, MaxValue, BoundsY, ClipY, frame, frame, ZAdjust, false);
		const auto plain = Plain(Deg90, MaxValue, BoundsY, ClipY, frame, ZAdjust);
		Check(rle.Base == 9828 && rle.Accumulator == 1 && rle.Steps, "Deg90 RLE base");
		Check(rle.Base == plain.Base && rle.Accumulator == plain.Accumulator, "uncut RLE and plain agree");

		// Cut at the top, both continue the uncut frame's bands: its sixth row.
		const Rect high {0, -5, 30, 20};
		const auto top = Clip(high, {0, 0, 30, 20}, {0, 0, 30, 20}, 640, 30, true).Dest;
		const auto cutRLE = RLE(Deg90, MaxValue, BoundsY, ClipY, high, top, ZAdjust, false);
		const auto cutPlain = Plain(Deg90, MaxValue, BoundsY, ClipY, top, ZAdjust);
		auto uncut = Plain(Deg90, MaxValue, BoundsY, ClipY, high, ZAdjust);
		for (int row = 0; row < 5; ++row) Next(uncut, Deg90);
		Check(cutRLE.Base == 9848 && cutRLE.Accumulator == 0, "Deg90 RLE cut at the top");
		Check(cutPlain.Base == uncut.Base && cutPlain.Accumulator == uncut.Accumulator && cutRLE.Base == uncut.Base,
			"a top cut continues the bands");

		// Cut at the bottom, the RLE path keeps the uncut bottom and the plain one does not.
		const Rect low {0, 15, 30, 20};
		const auto bottom = Clip(low, {0, 0, 30, 20}, {0, 0, 30, 20}, 640, 30, true).Dest;
		const auto lowRLE = RLE(Deg90, MaxValue, BoundsY, ClipY, low, bottom, ZAdjust, false);
		const auto lowPlain = Plain(Deg90, MaxValue, BoundsY, ClipY, bottom, ZAdjust);
		Check(lowRLE.Base == 9828 && lowRLE.Accumulator == 1, "Deg90 RLE cut at the bottom");
		Check(lowPlain.Base == 9833 && lowPlain.Accumulator == 0, "Deg90 plain cut at the bottom");
	}

	{
		const Rect frame {0, 15, 30, 20};
		Check(RLE(Flat, MaxValue, BoundsY, ClipY, frame, frame, ZAdjust, false).Base
			== Plain(Flat, MaxValue, BoundsY, ClipY, frame, ZAdjust).Base, "flat RLE and plain agree");
		// With a Z shape the RLE rows keep the unbanded base.
		auto shaped = RLE(Deg90, MaxValue, BoundsY, ClipY, frame, frame, ZAdjust, true);
		Check(shaped.Base == 9834 && !shaped.Steps, "Deg90 RLE base with a Z shape");
		Next(shaped, Deg90);
		Next(shaped, Deg90);
		Next(shaped, Deg90);
		Check(shaped.Base == 9834, "rows with a Z shape keep the base");
	}

	{
		// The closed form follows Next row by row.
		bool same = ClosedForm(Flat) && ClosedForm(Deg90);
		const Gradient* gradients[] {&Flat, &Deg90};
		for (const auto* g : gradients)
			for (int height = 1; height <= 7; ++height)
			{
				auto rows = Plain(*g, MaxValue, BoundsY, ClipY, {0, 20, 30, height}, ZAdjust);
				const auto first = rows;
				for (int row = 0; row < 12; ++row, Next(rows, *g)) same = same && RowBase(first, *g, row) == rows.Base;
			}
		auto shaped = RLE(Deg90, MaxValue, BoundsY, ClipY, {0, 15, 30, 20}, {0, 15, 30, 15}, ZAdjust, true);
		Check(same && RowBase(shaped, Deg90, 9) == shaped.Base, "closed-form row bases");
		Check(!ClosedForm(Gradient {1, 1, 4, 3, 1, 0, {}}), "a gradient stepping more than a band per row");
	}

	Check(Depth(100, 0xFE) == 102 && Depth(100, 5) == 95 && Depth(100, 0) == 100, "signed Z shape bytes");
	Check(Passes(4, 5) && !Passes(5, 5) && !Passes(6, 5), "strictly smaller depths pass");
	Check(Passes(-1, 0) && !Passes(70000, 65535), "the test is signed against the stored word");
	Check(Darken(0xFFFF, 0xFFFF7BEF) == 0x7BEF && Darken(0x1084, 0x7BEF) == 0x0842 && Darken(0x0842, 0x7BEF) == 0x0021,
		"darkening halves every channel and drops the bits shifted across");
	Check(Blend(0xFFFF, 0x0000, 2, 0x7BEF, 0x39E7) == 0x39E7 && Blend(0xFFFF, 0x0000, 4, 0x7BEF, 0x39E7) == 0x7BEF
		&& Blend(0xFFFF, 0x0000, 6, 0x7BEF, 0x39E7) == 0xADB5, "translucency keeps 1/4, 1/2 and 3/4 of the old color in integer parts");
	Check(Blend(0x0000, 0xFFFF, 2, 0x7BEF, 0x39E7) == 0xADB5 && Blend(0x0000, 0xFFFF, 6, 0x7BEF, 0x39E7) == 0x39E7,
		"translucency keeps 3/4 and 1/4 of the incoming color");
	Check(Blend(0xF800, 0x001F, 4, 0x7BEF, 0x39E7) == 0x780F && Blend(0xF800, 0x001F, 0, 0x7BEF, 0x39E7) == 0x001F,
		"50% adds the channel halves; no translucency keeps the incoming color");
	// Native 50% pixels of S_CLSN22 through Phobos' patched RLE blitter (GPUDMG, old and incoming color).
	Check(RoundedBlend(0x84E7, 0x632C, 4) == 0x740A && RoundedBlend(0x9529, 0x52AA, 4) == 0x73EA && RoundedBlend(0xAD28, 0x632C, 4) == 0x8C2A,
		"the patched 50% rounds every channel's mean up");
	Check(RoundedBlend(0xFFFF, 0x0000, 4) == 0x8410 && RoundedBlend(0xFFFF, 0x0000, 6) == 0xC618 && RoundedBlend(0xFFFF, 0x0000, 2) == 0x4208
		&& RoundedBlend(0xFFFF, 0x0000, 0) == 0x0000, "the patched 25% and 75% average the mean again with the incoming and the old color");

	if (Failures) return 1;
	std::puts("ShapeDepth: clipping, plain and RLE row depths, bands, Z shapes, the strict test, darkening and the native and patched translucency passed.");
	return 0;
}
