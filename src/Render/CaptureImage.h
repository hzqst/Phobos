#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RenderCapture
{
	constexpr int MaxDimension = 4096;
	constexpr size_t MaxPixels = 4096u * 2160u;
	inline bool ValidSize(int width, int height)
	{
		return width > 0 && height > 0 && width <= MaxDimension && height <= MaxDimension
			&& size_t(width) * height <= MaxPixels;
	}
	// Main-thread-owned until Dispatcher publishes completion. Then immutable.
	struct Image
	{
		std::vector<uint8_t> Pixels; // RGBA8, top-down, tightly packed, opaque output.
		int Width = 0, Height = 0, LogicalWidth = 0, LogicalHeight = 0;
		std::array<float, 4> Viewport {};
		uint64_t SimulationFrame = 0, PresentID = 0, TimestampMS = 0, Scene = 0;
		std::string Source, Backend, OutputMode, Error;
		bool Ready = false, Cancelled = false;
	};
	inline bool Decode16(const std::vector<uint16_t>& source, const std::array<int, 3>& shift,
		const std::array<int, 3>& bits, std::vector<uint8_t>& rgba)
	{
		unsigned used = 0;
		for (int c = 0; c < 3; ++c)
		{
			if (bits[c] < 1 || bits[c] > 8 || shift[c] < 0 || shift[c] + bits[c] > 16) return false;
			const unsigned mask = ((1u << bits[c]) - 1) << shift[c];
			if (used & mask) return false;
			used |= mask;
		}
		rgba.resize(source.size() * 4);
		for (size_t i = 0; i < source.size(); ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				const unsigned mask = (1u << bits[c]) - 1;
				rgba[i * 4 + c] = uint8_t((((source[i] >> shift[c]) & mask) * 255u + mask / 2) / mask);
			}
			rgba[i * 4 + 3] = 255;
		}
		return true;
	}
}
