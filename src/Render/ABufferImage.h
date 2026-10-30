#pragma once

#include "TerrainImage.h"
#include <algorithm>
#include <cstdint>

// Pure rules of the shroud and fog in the tactical ABuffer (gamemd-reverse #17): 6D71E0 scans
// the cells of a rectangle, 4801F0 picks two frames per cell through 6D8700 and 7F4194, then
// 47EFE0 overwrites and 47F250 darkens the buffer's 16-bit words with raw SHP bytes.
namespace ABufferImage
{
	// A cleared word: the default light, no shroud, fog or alpha shape.
	constexpr std::uint16_t ClearValue = 0x7F;
	// 47EFE0 keeps the old word under this source byte; its fast loop reads it from 7E4EB8.
	constexpr std::uint8_t Transparent = 0xFE;
	// 47F250 keeps the old word under a source byte above this one and darkens by the rest.
	constexpr int MaxDarken = 0x7F;
	// The first pass tests AltFlags, the second CellFlags; a neighbour without the bit sets its mask bit.
	constexpr std::uint32_t MappedBit = 0x08, MappedOrClear = 0x18;
	constexpr std::uint32_t EdgeRevealedBit = 0x02, RevealedBits = 0x03;
	// 6D8700's fallbacks for a cell without the bit, and the frames 4801F0 turns them into.
	constexpr int Covered = -2, Uncovered = -1;
	constexpr int CoveredFrame = 15, UncoveredFrame = 0;
	constexpr int CellWidth = TerrainImage::TileWidth, CellHeight = TerrainImage::CellHeight;

	// Map cell offsets in 6D8700's lookup order, with the mask bit of each.
	struct Neighbour { int X, Y; std::uint8_t Bit; };
	constexpr Neighbour Neighbours[8] {{-1, -1, 0x40}, {0, -1, 0x80}, {1, -1, 0x01}, {-1, 0, 0x20},
		{1, 0, 0x02}, {-1, 1, 0x10}, {0, 1, 0x08}, {1, 1, 0x04}};

	// neighbours holds the low flag bytes in Neighbours order; only those bytes are read.
	inline int OcclusionMask(const std::uint8_t* neighbours, std::uint32_t bit)
	{
		int mask = 0;
		for (int i = 0; i < 8; ++i)
			if (!(neighbours[i] & bit)) mask |= Neighbours[i].Bit;
		return mask;
	}

	// 6D8700: the first pass (fog false) reads AltFlags, the second CellFlags. table is 7F4194.
	inline int Occlusion(std::uint32_t center, const std::uint8_t* neighbours, bool fog, const std::int8_t* table)
	{
		const auto bit = fog ? EdgeRevealedBit : MappedBit;
		const int fallback = (center & (fog ? RevealedBits : MappedOrClear)) ? Uncovered : Covered;
		if (!(center & bit)) return fallback;
		return table[OcclusionMask(neighbours, bit)];
	}

	// 4801F0 sign-extends the returned byte and turns the fallbacks into frames.
	constexpr int FrameOf(int occlusion)
	{
		const int value = std::int8_t(std::uint8_t(occlusion));
		return value == Covered ? CoveredFrame : value == Uncovered ? UncoveredFrame : value;
	}

	struct Rect { int X, Y, Width, Height; };

	constexpr Rect Intersect(const Rect& a, const Rect& b)
	{
		const auto left = std::max<std::int64_t>(a.X, b.X), top = std::max<std::int64_t>(a.Y, b.Y);
		const auto right = std::min<std::int64_t>(std::int64_t(a.X) + a.Width, std::int64_t(b.X) + b.Width);
		const auto bottom = std::min<std::int64_t>(std::int64_t(a.Y) + a.Height, std::int64_t(b.Y) + b.Height);
		if (right <= left || bottom <= top) return {};
		return {int(left), int(top), int(right - left), int(bottom - top)};
	}

	// 6D71E0 for a legal cell: the 60x30 box at the cell's ground position, without its level. The
	// box lacks ViewBounds.X, the origin 4801F0 draws from has it; they agree while ViewBounds.X is 0.
	constexpr Rect CellBox(int cellX, int cellY, int tacticalX, int tacticalY, int viewY)
	{
		const auto position = TerrainImage::TilePosition(cellX, cellY, 0, tacticalX, tacticalY, viewY);
		return {int(position.X), int(position.Y), CellWidth, CellHeight};
	}

	// A raw SHP frame: the header's bounds and Width*Height bytes, read without RLE decoding.
	struct Frame { int Left, Top, Width, Height; const std::uint8_t* Pixels; };

	enum class Pass { Overwrite, Darken };
	enum class Drawn { Nothing, Pixels, Unsupported };

	// 47EFE0 or 47F250 drawing frame at (x, y) clipped to clip, into buffer, which covers the rectangle
	// area. Unsupported where the native loops would run over an empty span or the span leaves area.
	inline Drawn Draw(std::uint16_t* buffer, const Rect& area, int x, int y, const Rect& clip, const Frame& frame, Pass pass)
	{
		const auto frameLeft = std::int64_t(x) + frame.Left, frameTop = std::int64_t(y) + frame.Top;
		const auto frameRight = frameLeft + frame.Width, frameBottom = frameTop + frame.Height;
		const auto clipRight = std::int64_t(clip.X) + clip.Width, clipBottom = std::int64_t(clip.Y) + clip.Height;
		// The native rejection tests the frame's far edges but the bare origin.
		if (frameBottom <= clip.Y || y >= clipBottom || frameRight <= clip.X || x >= clipRight) return Drawn::Nothing;
		const auto left = std::max<std::int64_t>(clip.X, frameLeft), right = std::min(clipRight, frameRight);
		const auto top = std::max<std::int64_t>(clip.Y, frameTop), bottom = std::min(clipBottom, frameBottom);
		if (right <= left || bottom <= top || left < area.X || top < area.Y
			|| right > std::int64_t(area.X) + area.Width || bottom > std::int64_t(area.Y) + area.Height) return Drawn::Unsupported;
		for (auto row = top; row < bottom; ++row)
			for (auto column = left; column < right; ++column)
			{
				const auto value = frame.Pixels[size_t(row - frameTop) * frame.Width + size_t(column - frameLeft)];
				auto& word = buffer[size_t(row - area.Y) * area.Width + size_t(column - area.X)];
				if (pass == Pass::Overwrite)
				{
					if (value != Transparent) word = value;
				}
				// 47F250 writes the byte itself over a clear word, the same as the sum would.
				else if (value <= MaxDarken) word = std::uint16_t(std::max(0, int(word) + value - MaxDarken));
			}
		return Drawn::Pixels;
	}
}
