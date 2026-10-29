#include <Render/Tooltip.h>
#include <cassert>
#include <climits>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
	// Counts how often each cell of a width x height grid at (x, y) is covered.
	std::vector<int> Coverage(int x, int y, int width, int height)
	{
		std::vector<int> cells(std::size_t(width) * height);
		for (const auto& edge : Tooltip::Border(x, y, width, height))
			for (int row = edge.Y; row < edge.Y + edge.Height; ++row)
				for (int column = edge.X; column < edge.X + edge.Width; ++column)
				{
					assert(column >= x && column < x + width && row >= y && row < y + height);
					++cells[std::size_t(row - y) * width + (column - x)];
				}
		return cells;
	}

	void CheckOutline(int width, int height)
	{
		const auto cells = Coverage(7, -3, width, height);
		for (int row = 0; row < height; ++row)
			for (int column = 0; column < width; ++column)
			{
				const bool edge = row == 0 || row == height - 1 || column == 0 || column == width - 1;
				assert((edge ? 1 : 0) == cells[std::size_t(row) * width + column]);
			}
	}
}

int main()
{
	using Tooltip::Opacity256;
	assert(256u == Opacity256(100));
	assert(0u == Opacity256(0));
	assert(127u == Opacity256(50)); // floor(255 * 50 / 100)
	assert(252u == Opacity256(99));
	// The native clamp is unsigned: negative and oversized percentages are opaque.
	assert(256u == Opacity256(101));
	assert(256u == Opacity256(-1));
	assert(256u == Opacity256(INT_MIN));

	const auto border = Tooltip::Border(10, 20, 5, 4);
	assert(10 == border[0].X && 20 == border[0].Y && 5 == border[0].Width && 1 == border[0].Height);
	assert(10 == border[1].X && 23 == border[1].Y && 5 == border[1].Width && 1 == border[1].Height);
	assert(10 == border[2].X && 21 == border[2].Y && 1 == border[2].Width && 2 == border[2].Height);
	assert(14 == border[3].X && 21 == border[3].Y && 1 == border[3].Width && 2 == border[3].Height);
	for (const auto [width, height] : {std::pair {1, 1}, {1, 2}, {2, 1}, {2, 2}, {1, 5}, {5, 1}, {3, 3}, {64, 19}})
		CheckOutline(width, height);
	for (const auto [width, height] : {std::pair {0, 5}, {5, 0}, {-1, 3}, {3, -1}})
		for (const auto& edge : Tooltip::Border(0, 0, width, height))
			assert(0 == edge.Width * edge.Height);

	using Tooltip::CanvasOrigin;
	using Tooltip::Target;
	assert(856 == CanvasOrigin(Target::Sidebar, 1, 856, 1024));
	assert(0 == CanvasOrigin(Target::Composite, 1, 856, 1024));
	assert(0 == CanvasOrigin(Target::Sidebar, 0, 1024, 168));
	assert(168 == CanvasOrigin(Target::Composite, 0, 1024, 168));
	assert(INT_MIN == CanvasOrigin(Target::None, 1, 856, 1024));
	std::puts("Tooltip: opacity clamp, outline coverage and surface origins passed.");
}
