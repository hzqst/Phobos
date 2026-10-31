#pragma once

#include <cstdint>

// Depth rules of the Z-testing shape blitters (gamemd-reverse #18 / #19). 4AED70 draws a frame through
// the plain path 4373B0 or, for an RLE frame, 437A10. Both give each row a base depth from the
// ZBuffer's MaxValue and Bounds.Y, the call's ZAdjust and a gradient record of 817710, and the
// blitters test every nonzero source pixel with int32 depth < uint16 stored, then store the low 16
// bits. The colored blitter (flags 4E00) writes the alpha-lit color, the shadow one (4601) halves the
// old color. Unlike 547CF0 an equal depth is rejected.
namespace ShapeDepth
{
	// One 24-byte record of 817710.
	struct Gradient
	{
		std::int32_t Field0, Step, RowsPerBand, BandLimit, BandDelta;
		std::uint8_t Flag, Padding[3];
	};
	static_assert(sizeof(Gradient) == 24);

	struct Rect { int X, Y, Width, Height; };

	// The native arithmetic wraps at 32 bits.
	constexpr std::int32_t Add(std::int32_t a, std::int32_t b) { return std::int32_t(std::uint32_t(a) + std::uint32_t(b)); }
	constexpr std::int32_t Sub(std::int32_t a, std::int32_t b) { return std::int32_t(std::uint32_t(a) - std::uint32_t(b)); }
	constexpr std::int32_t Mul(std::int32_t a, std::int32_t b) { return std::int32_t(std::uint32_t(a) * std::uint32_t(b)); }

	// A gradient whose divisions cannot fault.
	constexpr bool Usable(const Gradient& g)
	{
		return g.Flag ? g.Step != 0 : g.RowsPerBand != 0 && g.BandLimit / g.RowsPerBand != 0;
	}

	// 421B60: the part of b inside a, empty (all 0) when they do not overlap.
	constexpr Rect Intersect(Rect a, Rect b)
	{
		if (a.Width <= 0 || a.Height <= 0 || b.Width <= 0 || b.Height <= 0) return {0, 0, 0, 0};
		auto x = b.X, y = b.Y, width = b.Width, height = b.Height;
		if (x < a.X) { width = Add(Sub(x, a.X), width); x = a.X; }
		if (width < 1) return {0, 0, 0, 0};
		if (y < a.Y) { height = Add(Sub(y, a.Y), height); y = a.Y; }
		if (height < 1) return {0, 0, 0, 0};
		if (Add(width, x) > Add(a.X, a.Width)) width = Add(Sub(a.X, x), a.Width);
		if (width < 1) return {0, 0, 0, 0};
		if (Add(height, y) > Add(a.Y, a.Height)) height = Sub(Add(a.Y, a.Height), y);
		if (height < 1) return {0, 0, 0, 0};
		return {x, y, width, height};
	}

	// 7BBE20 as 4373B0 (through 7BC040) and 437A10 call it: dest, the frame's place relative to the
	// clip, and source, the part of the frame to draw, have the same size and shrink together. A cut
	// at the clip's edges moves the source; then the source is held inside limit's size from its own
	// X and Y. 437A10 passes the source itself as limit and reads the frame from the source's offset
	// to it, so a source starting inside the frame loses that many columns on the right instead.
	// 4373B0 passes the whole frame and reads from the source's position.
	struct Clipped
	{
		Rect Dest {};
		// Where the first drawn pixel lies in the frame, and how many columns and rows are drawn.
		int SourceX = 0, SourceY = 0, Width = 0, Rows = 0;
		bool Drawn = false;
	};

	constexpr Clipped Clip(Rect dest, Rect source, Rect limit, int clipWidth, int clipHeight, bool compressed)
	{
		auto r = dest, a = source;
		if (r.X < 0)
		{
			a.X = Sub(a.X, r.X);
			a.Width = Add(r.X, a.Width);
			r.Width = Add(r.Width, r.X);
			r.X = 0;
		}
		if (r.Y < 0)
		{
			a.Y = Sub(a.Y, r.Y);
			a.Height = Add(a.Height, r.Y);
			r.Height = Add(r.Height, r.Y);
			r.Y = 0;
		}
		if (const auto over = Add(Sub(r.X, clipWidth), r.Width); over > 0) { a.Width = Sub(a.Width, over); r.Width = Sub(r.Width, over); }
		if (const auto over = Add(Sub(r.Height, clipHeight), r.Y); over > 0) { a.Height = Sub(a.Height, over); r.Height = Sub(r.Height, over); }
		if (a.X < 0)
		{
			r.X = Sub(r.X, a.X);
			a.Width = Add(a.Width, a.X);
			r.Width = Add(r.Width, a.X);
			a.X = 0;
		}
		if (a.Y < 0)
		{
			r.Y = Sub(r.Y, a.Y);
			a.Height = Add(a.Height, a.Y);
			r.Height = Add(r.Height, a.Y);
			a.Y = 0;
		}
		if (const auto over = Add(Sub(a.Width, limit.Width), a.X); over > 0) { a.Width = Sub(a.Width, over); r.Width = Sub(r.Width, over); }
		if (const auto over = Add(Sub(a.Y, limit.Height), a.Height); over > 0) { a.Height = Sub(a.Height, over); r.Height = Sub(r.Height, over); }
		Clipped clipped;
		clipped.Dest = r;
		clipped.SourceX = compressed ? Sub(a.X, source.X) : a.X;
		clipped.SourceY = compressed ? Sub(a.Y, source.Y) : a.Y;
		clipped.Width = a.Width;
		clipped.Rows = a.Height < r.Height ? a.Height : r.Height;
		clipped.Drawn = r.Width > 0 && r.Height > 0;
		return clipped;
	}

	// The base depth of the first visible row and the band accumulator, which Next steps per row.
	struct Rows
	{
		std::int32_t Base = 0, Accumulator = 0;
		// False with a Z shape on the RLE path: every row keeps the base.
		bool Steps = true;
	};

	// 4373B0, a plain frame: everything follows Clip's Dest, relative to the clip at clipY.
	constexpr Rows Plain(const Gradient& g, int maxValue, int boundsY, int clipY, Rect visible, int zAdjust)
	{
		Rows rows;
		if (g.Flag)
		{
			const auto z = std::uint16_t(Sub(Sub(Add(maxValue, boundsY), visible.Y), clipY));
			rows.Base = Mul(Add(z, zAdjust) / g.Step, g.Step);
			return rows;
		}
		const auto bands = g.BandLimit / g.RowsPerBand;
		const auto z = std::uint16_t(Add(Sub(Sub(Sub(Add(maxValue, boundsY), visible.Height), visible.Y), clipY), 1));
		rows.Base = Sub(Mul(Add(z, zAdjust) / bands, bands), visible.Height / bands);
		rows.Accumulator = Sub(g.BandLimit, visible.Height % bands);
		if (rows.Accumulator == g.BandLimit)
		{
			rows.Accumulator = 0;
			rows.Base = Add(rows.Base, g.BandDelta);
		}
		return rows;
	}

	// 437A10, an RLE frame: frame is the dest given to Clip and visible its Dest, so a frame cut at the
	// clip's bottom starts its bands from the uncut bottom. With a Z shape the rows keep the base and each
	// pixel subtracts its signed Z shape byte.
	constexpr Rows RLE(const Gradient& g, int maxValue, int boundsY, int clipY, Rect frame, Rect visible, int zAdjust, bool zShape)
	{
		Rows rows;
		rows.Steps = !zShape;
		if (g.Flag)
		{
			const auto row = Sub(Add(visible.Y, clipY), boundsY);
			const auto depth = std::uint16_t(std::uint16_t(maxValue) - std::uint16_t(row));
			rows.Base = Mul(g.Step, Add(zAdjust, depth) / g.Step);
			return rows;
		}
		const auto scale = g.BandLimit / g.RowsPerBand;
		const auto depth = std::uint16_t(std::uint16_t(boundsY) + std::uint16_t(maxValue) - std::uint16_t(clipY)
			- std::uint16_t(frame.Height) - std::uint16_t(frame.Y) + 1);
		rows.Base = Add(zAdjust, depth);
		if (zShape) return rows;
		const auto skipped = Sub(Add(frame.Y, frame.Height), visible.Y);
		rows.Base = Sub(Mul(scale, rows.Base / scale), skipped / scale);
		rows.Accumulator = Sub(g.BandLimit, skipped % scale);
		if (rows.Accumulator == g.BandLimit)
		{
			rows.Accumulator = 0;
			rows.Base = Add(rows.Base, g.BandDelta);
		}
		return rows;
	}

	// After each drawn row.
	constexpr void Next(Rows& rows, const Gradient& g)
	{
		if (!rows.Steps) return;
		rows.Accumulator = Add(rows.Accumulator, g.RowsPerBand);
		if (rows.Accumulator >= g.BandLimit)
		{
			rows.Base = Add(rows.Base, g.BandDelta);
			rows.Accumulator = Sub(rows.Accumulator, g.BandLimit);
		}
	}

	// Next's stepping in closed form, as the GPU computes it: row j's base. Holds while a row adds no
	// more than a band, which the native gradients do; Rows keeps its accumulator below the limit.
	constexpr bool ClosedForm(const Gradient& g) { return g.BandLimit > 0 && g.RowsPerBand >= 0 && g.RowsPerBand <= g.BandLimit; }
	constexpr std::int32_t RowBase(const Rows& rows, const Gradient& g, int row)
	{
		if (!rows.Steps) return rows.Base;
		return Add(rows.Base, Mul(g.BandDelta, Add(rows.Accumulator, Mul(row, g.RowsPerBand)) / g.BandLimit));
	}

	// A pixel's depth: the row base less its signed Z shape byte (0 without one).
	constexpr std::int32_t Depth(std::int32_t base, std::uint8_t zShape) { return Sub(base, std::int8_t(zShape)); }

	constexpr bool Passes(std::int32_t depth, std::uint16_t stored) { return depth < std::int32_t(stored); }

	// The shadow blitter's darkening of the old 16-bit color.
	constexpr std::uint16_t Darken(std::uint16_t color, std::uint32_t mask) { return std::uint16_t((color >> 1) & mask); }

	// The translucent blitters (flag bits 2, 4, 6) add integer channel parts of the incoming and the old
	// 16-bit color (gamemd-reverse #22 §7): 25% keeps three quarters of the incoming one, 50% half of
	// each, 75% a quarter of the incoming one. half and quarter are the HalfTranslucencyMask and
	// QuarterTranslucencyMask. Other bits leave the incoming color.
	constexpr std::uint16_t Blend(std::uint16_t old, std::uint16_t incoming, std::uint32_t bits, std::uint32_t half, std::uint32_t quarter)
	{
		const std::uint32_t a = (incoming >> 2) & quarter, b = (old >> 2) & quarter;
		switch (bits)
		{
		case 2: return std::uint16_t(3 * a + b);
		case 4: return std::uint16_t(((incoming >> 1) & half) + ((old >> 1) & half));
		case 6: return std::uint16_t(a + 3 * b);
		default: return incoming;
		}
	}

	// Phobos' FixTransparencyBlitters (BlittersFix.cpp) replaces the mixing of the RLE AlphaZRead blitters
	// (437A10's frames that read but do not write Z) with Average, the rounded-up mean of two 16-bit colors
	// for 565's channel low bits: 50% averages once, 25% and 75% average the mean again with the incoming
	// and the old color. The other blitters keep Blend.
	constexpr std::uint32_t RoundedMask = 0xF7DE;
	constexpr std::uint16_t Average(std::uint16_t a, std::uint16_t b)
	{
		return std::uint16_t((a | b) - (((a ^ b) & RoundedMask) >> 1));
	}

	constexpr std::uint16_t RoundedBlend(std::uint16_t old, std::uint16_t incoming, std::uint32_t bits)
	{
		const auto mean = Average(incoming, old);
		switch (bits)
		{
		case 2: return Average(mean, incoming);
		case 4: return mean;
		case 6: return Average(mean, old);
		default: return incoming;
		}
	}
}
