#pragma once

#include <algorithm>
#include <cstdint>

namespace CursorImage
{
	constexpr int MaxDimension = 256;
	constexpr std::uint32_t Opaque = 0x10000;

	struct Region
	{
		int X = 0, Y = 0, Width = 0, Height = 0;
		int SourceX = 0, SourceY = 0;
	};

	constexpr std::uint32_t PackPixel(std::uint8_t index, std::uint16_t color)
	{
		return index ? Opaque | color : 0;
	}

	inline Region Clip(int x, int y, int width, int height, int logicalWidth, int logicalHeight)
	{
		if (width <= 0 || height <= 0 || logicalWidth <= 0 || logicalHeight <= 0)
			return {};
		const auto left = std::max<std::int64_t>(x, 0);
		const auto top = std::max<std::int64_t>(y, 0);
		const auto right = std::min<std::int64_t>(std::int64_t(x) + width, logicalWidth);
		const auto bottom = std::min<std::int64_t>(std::int64_t(y) + height, logicalHeight);
		if (right <= left || bottom <= top)
			return {};
		return { int(left), int(top), int(right - left), int(bottom - top), int(left - x), int(top - y) };
	}
}
