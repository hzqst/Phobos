#include <Render/ShapeFrameSource.h>
#include <cstdio>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (false)

int main()
{
	using namespace ShapeFrameSource;
	using FrameCache::Status;
	static_assert(sizeof(void*) == 4);
	// Raw file/reference bytes avoid invoking engine constructors/destructors.
	alignas(8) std::array<std::uint8_t, 80> file {};
	const std::int16_t header[] {0, 2, 1, 2};
	std::memcpy(file.data(), header, sizeof(header));
	SHPFrame first {}, second {};
	first.Width = second.Width = 2;
	first.Height = second.Height = 1;
	first.Offset = 56; second.Offset = 58; second.Flags = 2;
	std::memcpy(file.data() + 8, &first, sizeof(first));
	std::memcpy(file.data() + 32, &second, sizeof(second));
	const std::uint8_t pixels[] {1, 0, 5, 0, 1, 0, 1};
	std::memcpy(file.data() + 56, pixels, sizeof(pixels));
	const auto* shp = reinterpret_cast<const SHPStruct*>(file.data());
	alignas(8) std::array<std::uint8_t, 36> reference {};
	std::memcpy(reference.data(), header, sizeof(header));
	reference[0] = reference[1] = 255;
	std::memcpy(reference.data() + 12, &shp, sizeof(shp));
	const auto* ref = reinterpret_cast<const SHPStruct*>(reference.data());
	std::uintptr_t data = 0;
	CHECK(Status::NotReady == Acquire(ref, 0, data).State);
	reference[16] = 1;
	const auto a = Acquire(ref, 0, data);
	CHECK(Status::Ready == a.State && reinterpret_cast<std::uintptr_t>(shp) == data);
	const auto firstID = a.Frame->ID;
	CHECK(firstID == Acquire(shp, 0, data).Frame->ID); // direct and reference alias
	const auto b = Acquire(ref, 1, data);
	CHECK(Status::Ready == b.State && a.Frame->Indices == b.Frame->Indices);
	for (int i = 0; i < 300; ++i) CHECK(firstID == Acquire(ref, 0, data).Frame->ID);
	CHECK(2u == Frames.GetStats().Decoded);
	CHECK(Invalidate(reinterpret_cast<std::uintptr_t>(ref)));
	CHECK(!Frames.Resolve(firstID) && Aliases.empty());
	file[56] = 7; // same allocation, new lifetime
	const auto replacement = Acquire(ref, 0, data);
	CHECK(Status::Ready == replacement.State && replacement.Frame->ID != firstID);
	CHECK(7 == replacement.Frame->Indices[0]);
	const auto freedID = replacement.Frame->ID;
	// Owned cameo free sites pass Data directly, not the reference alias.
	CHECK(Invalidate(reinterpret_cast<std::uintptr_t>(shp)));
	CHECK(!Frames.Resolve(freedID) && Aliases.empty());
	file[56] = 9;
	const auto afterFree = Acquire(ref, 0, data);
	CHECK(Status::Ready == afterFree.State && afterFree.Frame->ID != freedID);
	CHECK(9 == afterFree.Frame->Indices[0]);
	const auto replacementID = afterFree.Frame->ID;
	first.Left = 3; // observed metadata mutation creates another resource version
	std::memcpy(file.data() + 8, &first, sizeof(first));
	CHECK(3 == Acquire(ref, 0, data).Frame->Geometry.Left);
	CHECK(!Frames.Resolve(replacementID));
	CHECK(Status::InvalidData == Acquire(ref, 2, data).State);
	reference[16] = 0;
	CHECK(Status::NotReady == Acquire(ref, 0, data).State);
	CHECK(0u == Frames.GetStats().Frames && 0u == Frames.GetStats().IndexBytes);
	Clear();
	std::puts("ShapeFrameSource: loaded/reference aliases, plain/RLE, 300 hits, unload, same-address reload and metadata changes passed.");
}
