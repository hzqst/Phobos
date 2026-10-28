#include <Render/GlyphCache.h>
#include <cassert>
#include <iostream>
#include <type_traits>

int main()
{
	using namespace GlyphCache;
	using FrameCache::Status;
	static_assert(!std::is_convertible_v<GlyphID, FrameCache::FrameID>);
	Cache cache;
	const Format format {2, 2, 5, 3};
	const auto font = cache.Register(1, format);
	assert(font);
	assert(font == cache.Register(1, format));
	assert(cache.Storage().GetStats().Decoded == 0);
	const std::uint8_t record[] {9, 0x81, 0xFF, 0x40, 0x80};
	const auto first = cache.Acquire(font, 0, record);
	assert(Status::Ready == first.State);
	assert(9 == first.Value->Width && 2 == first.Value->Height);
	const auto id = first.Value->ID;
	const auto storage = first.Value->Storage;
	const auto* pixels = cache.Storage().Resolve(storage);
	const std::vector<std::uint8_t> expected {
		1,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
		0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0};
	assert(expected == pixels->Indices);
	// Reuse by real record, independent of text character aliases and source lifetime.
	assert(id == cache.Acquire(font, 0, {}).Value->ID);
	assert(1 == cache.Storage().GetStats().Decoded);
	assert(Status::InvalidData == cache.Acquire(font, 1, {record, 4}).State);
	assert(Status::Ready == cache.Acquire(font, 1, record).State);
	const auto fallback = cache.Acquire(font, format.Symbols, record);
	assert(Status::Ready == fallback.State && !(id == fallback.Value->ID));
	assert(Status::InvalidData == cache.Acquire(font, format.Symbols + 1, record).State);
	const std::uint8_t invalid[] {17, 0, 0, 0, 0};
	assert(Status::InvalidData == cache.Acquire(font, 2, invalid).State);
	const std::uint8_t space[] {0, 0xFF, 0xFF, 0xFF, 0xFF};
	const auto blank = cache.Acquire(font, 2, space);
	assert(Status::Ready == blank.State);
	assert(std::vector<std::uint8_t>(32, 0) == cache.Storage().Resolve(blank.Value->Storage)->Indices);
	assert(cache.Invalidate(font));
	assert(!cache.Resolve(id) && !cache.Storage().Resolve(storage));
	assert(Status::NotReady == cache.Acquire(font, 0, record).State);
	const auto replacement = cache.Register(1, format);
	assert(!(font == replacement));
	assert(!(id == cache.Acquire(replacement, 0, record).Value->ID));
	const auto changed = cache.Register(1, {1, 2, 3, 3});
	assert(changed && !(replacement == changed));
	assert(Status::NotReady == cache.Acquire(replacement, 0, record).State);
	assert(!cache.Register(2, {0, 2, 5, 3}));
	assert(!cache.Register(2, {33, 2, 100, 3}));
	assert(!cache.Register(2, {2, 257, 1000, 3}));
	assert(!cache.Register(2, {2, 2, 4, 3}));
	assert(!cache.Register(2, {2, 2, 5, 65536}));
	assert(!cache.Register(2, {2, 2, 0xFFFFFFFFu, 3}));
	Cache limited({31, 4, 1});
	const auto limitedFont = limited.Register(1, format);
	assert(limitedFont);
	assert(Status::BudgetExceeded == limited.Acquire(limitedFont, 0, record).State);
	assert(!limited.Register(2, format));
	cache.Clear();
	assert(0 == cache.Storage().GetStats().Resources);
	assert(0 == cache.Storage().GetStats().IndexBytes);
	std::cout << "GlyphCacheTests PASS\n";
}
