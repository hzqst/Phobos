#include <Render/BlitFrameSource.h>
#include <cstdio>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (false)

int main()
{
	using namespace BlitFrameSource;
	// A 3x2 region of a surface with a pitch of 5: the same content is one frame wherever it lies.
	const std::uint8_t surface[] {0, 1, 2, 3, 9, 0, 4, 5, 6, 9, 7, 1, 2, 3, 9, 7, 4, 5, 6, 9};
	const auto first = AcquirePixels(surface + 1, 5, 3, 2);
	CHECK(first);
	const auto* frame = Frames.Resolve(first);
	CHECK(frame && 3 == frame->Geometry.Width && 2 == frame->Geometry.Height);
	CHECK((std::vector<std::uint8_t> {1, 2, 3, 4, 5, 6}) == frame->Indices);
	CHECK(first == AcquirePixels(surface + 11, 5, 3, 2));
	CHECK(1u == Frames.GetStats().Decoded);
	// Other pixels, or the same bytes in another shape, are other frames.
	const auto other = AcquirePixels(surface, 5, 3, 2);
	CHECK(other && other != first);
	CHECK(AcquirePixels(surface + 1, 5, 2, 3) != first);
	CHECK(!AcquirePixels(surface, 5, 0, 2) && !AcquirePixels(nullptr, 5, 3, 2));
	// Two images under one hash stay apart: compare the content, not only the hash.
	ByContent.emplace(Hash({9, 9, 9, 9, 9, 9}, 3, 2), ByContent.find(Hash({1, 2, 3, 4, 5, 6}, 3, 2))->second);
	const std::uint8_t nines[] {9, 9, 9, 9, 9, 9};
	const auto collided = AcquirePixels(nines, 3, 3, 2);
	CHECK(collided && collided != first && 9 == Frames.Resolve(collided)->Indices[0]);
	for (auto [it, end] = ByContent.equal_range(Hash({9, 9, 9, 9, 9, 9}, 3, 2)); it != end; ++it)
		if (it->second.Frame == first) { ByContent.erase(it); break; }

	// An RLE frame of 4x2: rows "1 0,2 3" and "0,4".
	const std::uint8_t rle[] {6, 0, 1, 0, 2, 3, 4, 0, 0, 4};
	const auto encoded = AcquireEncoded(rle, sizeof(rle), 4, 2);
	CHECK(encoded);
	CHECK((std::vector<std::uint8_t> {1, 0, 0, 3, 0, 0, 0, 0}) == Frames.Resolve(encoded)->Indices);
	const auto decoded = Frames.GetStats().Decoded;
	CHECK(encoded == AcquireEncoded(rle, sizeof(rle), 4, 2) && decoded == Frames.GetStats().Decoded);
	// After 5F99E0 the same address is decoded again.
	Purged();
	const auto refreshed = AcquireEncoded(rle, sizeof(rle), 4, 2);
	CHECK(refreshed && refreshed != encoded && decoded + 1 == Frames.GetStats().Decoded);
	// A row longer than the bound, or not filling the width, is not a frame.
	const std::uint8_t copy[] {6, 0, 1, 0, 2, 3, 4, 0, 0, 4};
	CHECK(!AcquireEncoded(copy, sizeof(copy) - 1, 4, 2));
	CHECK(!AcquireEncoded(copy, sizeof(copy), 5, 2));

	// Entries unused for MaxIdle captures go at the sweep; those in use stay.
	for (unsigned i = 0; i < MaxIdle; ++i)
	{
		BeginCapture();
		CHECK(first == AcquirePixels(surface + 1, 5, 3, 2));
	}
	while (Serial % SweepInterval) BeginCapture();
	CHECK(Frames.Resolve(first) && !Frames.Resolve(other) && !Frames.Resolve(encoded) && !Frames.Resolve(refreshed));
	CHECK(1u == ByContent.size() && ByAddress.empty());
	Clear();
	CHECK(!Frames.Resolve(first) && ByContent.empty());
	const auto again = AcquirePixels(surface + 1, 5, 3, 2);
	CHECK(again && again != first);
	for (unsigned i = 0; i < MaxIdle * 2; ++i)
	{
		BeginCapture();
		CHECK(Touch(again)); // Identity hits must survive ordinary sweeps without re-reading pixels.
	}
	CHECK(Frames.Resolve(again));
	++Serial;
	CHECK(Touch(again));
	Sweep(1); CHECK(Frames.Resolve(again));
	++Serial;
	Sweep(1); CHECK(!Frames.Resolve(again) && !Touch(again));
	CHECK(IdentityUses.empty());
	std::puts("BlitFrameSource: content and address keys, epochs and sweeps passed.");
	return 0;
}
