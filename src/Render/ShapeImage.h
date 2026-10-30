#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ShapeImage
{
	constexpr int MaxDimension = 2048;
	constexpr int MaxCameoSlots = 60;
	// Matches Effect.x in IndexedSprite.ps.hlsl. Alpha blending is an intentional
	// approximation of the original RGB16 integer darken/translucent blitters.
	// TileDiamond draws index 0 too, inside the TMP diamond around the frame origin in Effect.w.
	enum class Effect : std::uint32_t { Normal, Darken, Translucent50, Solid, GlyphMask, TileDiamond };
	struct CameoSlot { int Item, X, Y, ClipWidth, ClipHeight; };
	inline bool PlaceCameo(int slot, int topRow, int count, int originX, int originY,
		int pitchX, int pitchY, bool scrolling, int slid, int sidebarX, int width, int height, CameoSlot& out)
	{
		if (slot < 0 || slot >= MaxCameoSlots || topRow < 0 || count < 0
			|| width <= 0 || height <= 0 || pitchX <= 0 || pitchY <= 0) return false;
		const auto item = std::int64_t(topRow) * 2 + slot;
		const auto x = std::int64_t(originX) + (slot % 2) * std::int64_t(pitchX) - sidebarX;
		const auto y = std::int64_t(originY) + (slot / 2) * std::int64_t(pitchY) + 1
			+ (scrolling ? std::int64_t(slid) - pitchY : 0);
		// Native bounds start at the surface origin, NOT at the cell's left/top.
		if (item >= count || x < INT32_MIN || x > INT32_MAX - 60 || y < INT32_MIN || y > INT32_MAX - 48
			|| x + 60 <= 0 || y + 48 <= 0) return false;
		out = {int(item), int(x), int(y), int(std::min<std::int64_t>(width, x + 60)),
			int(std::min<std::int64_t>(height, y + 48))};
		return true;
	}
	// SHP RLE rows include a little-endian length WORD, then literals or 0,run.
	// The engine permits zero-length runs; the encoded byte bound guarantees progress.
	inline bool Decode(std::span<const std::uint8_t> source, int width, int height,
		bool compressed, std::vector<std::uint8_t>& pixels)
	{
		pixels.clear();
		if (width <= 0 || height <= 0 || width > MaxDimension || height > MaxDimension)
			return false;
		std::vector<std::uint8_t> result(size_t(width) * height, 0);
		if (!compressed)
		{
			if (source.size() < result.size())
				return false;
			std::copy_n(source.begin(), result.size(), result.begin());
		}
		else
		{
			size_t offset = 0;
			for (int y = 0; y < height; ++y)
			{
				if (source.size() - offset < 2)
					return false;
				const size_t length = source[offset] | (size_t(source[offset + 1]) << 8);
				if (length < 2 || length > source.size() - offset)
					return false;
				const size_t end = offset + length;
				offset += 2;
				int x = 0;
				while (x < width && offset < end)
				{
					const auto value = source[offset++];
					if (value)
						result[size_t(y) * width + x++] = value;
					else
					{
							if (offset == end)
							return false;
							// Native RLE blitters stop when remaining <= 0. GCLOCK2 has
							// terminal transparent runs past the right edge; no pixels are written there.
							x += std::min<int>(source[offset++], width - x);
					}
				}
				if (x != width)
					return false;
				offset = end; // Encoded rows may contain padding.
			}
		}
		pixels.swap(result);
		return true;
	}

	inline int RepeatCount(int height, int startY, int sidebarY, int side)
	{
		// Model the original x86 wrapping arithmetic, followed by signed IDIV.
		const auto numerator = std::uint32_t(height) - std::uint32_t(startY)
			- (side == 0 ? 26u : 18u) + std::uint32_t(sidebarY) - 7u;
		return std::max(0, static_cast<std::int32_t>(numerator) / 50);
	}

	// PowerClass::Draw (63FB20) stacks POWERP.SHP frames downwards from SidebarBounds.Y+69.
	enum PowerFrame : int { PowerEmpty, PowerExcess, PowerMargin, PowerDrain, PowerFlash, PowerFrameCount };
	constexpr int PowerRowPitch = 3, PowerTopOffset = 69, PowerGDIX = 5, PowerNODX = 0;
	constexpr int MaxPowerLayoutHeight = 4096;
	// excess/margin/drain are the displayed cell counts (+152C/+1530/+1534), flash is +151C.
	// Rejects counts the native loop would turn into unbounded or negative sequences.
	inline bool PowerFrames(int layoutHeight, int excess, int margin, int drain, int flash, std::vector<int>& frames)
	{
		frames.clear();
		if (layoutHeight < 0 || layoutHeight > MaxPowerLayoutHeight) return false;
		const int total = (layoutHeight + PowerRowPitch) / PowerRowPitch;
		for (const int count : {excess, margin, drain})
			if (count < 0 || count > total) return false;
		for (int i = total - drain - margin - excess; i > 0; --i) frames.push_back(PowerEmpty);
		// The flash cell replaces the first cell of the first non-empty segment.
		int index = 0;
		if (flash > 0 && flash % 2 == 0) { frames.push_back(PowerFlash); index = 1; }
		for (const auto [count, frame] : {std::pair {excess, PowerExcess}, {margin, PowerMargin}, {drain, PowerDrain}})
		{
			if (count <= 0) continue;
			for (; index < count; ++index) frames.push_back(frame);
			index = 0;
		}
		return true;
	}
}
