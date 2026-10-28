#pragma once
#include <cstdint>
#include <optional>
#include <vector>

class AtlasAllocator
{
public:
	struct Region { int X, Y, Width, Height; };
	explicit AtlasAllocator(int side) : Side(side) { }
	std::optional<Region> Allocate(int width, int height)
	{
		if (width <= 0 || height <= 0 || width > Side || height > Side) return {};
		for (auto& shelf : Shelves)
			if (height <= shelf.Height && width <= Side - shelf.X)
			{
				const Region region {shelf.X, shelf.Y, width, height};
				shelf.X += width; Pixels += std::uint64_t(width) * height;
				return region;
			}
		if (height > Side - Bottom) return {};
		const Region region {0, Bottom, width, height};
		Shelves.push_back({width, Bottom, height});
		Bottom += height; Pixels += std::uint64_t(width) * height;
		return region;
	}
	std::uint64_t UsedPixels() const { return Pixels; }
	// Callers validate dimensions <= 2048 and bytesPerPixel in {1,4} first.
	static std::uint32_t RowPitch(std::uint32_t width, std::uint32_t bytesPerPixel) { return (width * bytesPerPixel + 255u) & ~255u; }
	static std::uint32_t AlignedOffset(std::uint32_t offset) { return (offset + 511u) & ~511u; }
private:
	struct Shelf { int X, Y, Height; };
	int Side, Bottom = 0;
	std::uint64_t Pixels = 0;
	std::vector<Shelf> Shelves;
};
