#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ShapeImage
{
	constexpr int MaxDimension = 2048;
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
						if (offset == end || source[offset] > width - x)
							return false;
						x += source[offset++];
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
}
