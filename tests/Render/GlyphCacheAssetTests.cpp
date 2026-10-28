#include <Render/GlyphCache.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

static std::vector<std::uint8_t> Read(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	assert(input);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static unsigned Word(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
	assert(offset + 2 <= bytes.size());
	return bytes[offset] | unsigned(bytes[offset + 1]) << 8;
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	const std::string root = argv[1];
	const auto table = Read(root + "/symbols.bin");
	const auto records = Read(root + "/records.bin");
	const auto fallback = Read(root + "/fallback.bin");
	// This fixture was sampled from the installed font; check its format explicitly.
	const GlyphCache::Format format {3, 16, 49, 33910};
	assert(65536 * 2 == table.size());
	assert(std::size_t(format.RecordBytes) * format.Symbols == records.size());
	assert(1 + format.Stride * format.Height == fallback.size());
	assert(0 == Word(table, 0x0378 * 2));
	const auto x = Word(table, 0x58 * 2);
	const auto ready = Word(table, 0x5C31 * 2);
	assert(x && x <= format.Symbols && ready && ready <= format.Symbols);
	const auto bitmap = [&](unsigned symbol) {
		return std::span(records).subspan(std::size_t(symbol - 1) * format.RecordBytes, format.RecordBytes);
	};
	assert(bitmap(x)[0] == fallback[0]);
	for (std::size_t i = 1; i < fallback.size(); ++i)
		assert(std::uint8_t(~bitmap(x)[i]) == fallback[i]);

	GlyphCache::Cache cache;
	const auto font = cache.Register(1, format);
	assert(font);
	const auto a = cache.Acquire(font, ready - 1, bitmap(ready));
	const auto b = cache.Acquire(font, format.Symbols, fallback);
	assert(FrameCache::Status::Ready == a.State && FrameCache::Status::Ready == b.State);
	const auto oldNormal = *a.Value, oldFallback = *b.Value;
	const auto expectedMask = cache.Storage().Resolve(oldFallback.Storage)->Indices;
	for (unsigned i = 0; i < 300; ++i)
	{
		assert(font == cache.Register(1, format));
		assert(oldNormal.ID == cache.Acquire(font, ready - 1, {}).Value->ID);
		assert(oldFallback.ID == cache.Acquire(font, format.Symbols, {}).Value->ID);
	}
	assert(2 == cache.Storage().GetStats().Decoded);
	// Native release/reload can reuse addresses. Old IDs must remain invalid forever.
	assert(cache.Invalidate(font));
	assert(!cache.Resolve(oldNormal.ID) && !cache.Resolve(oldFallback.ID));
	assert(!cache.Storage().Resolve(oldNormal.Storage) && !cache.Storage().Resolve(oldFallback.Storage));
	const auto replacement = cache.Register(1, format);
	assert(!(font == replacement));
	const auto next = cache.Acquire(replacement, format.Symbols, fallback);
	assert(FrameCache::Status::Ready == next.State && !(oldFallback.ID == next.Value->ID));
	assert(expectedMask == cache.Storage().Resolve(next.Value->Storage)->Indices);
	assert(!cache.Resolve(oldFallback.ID));
	std::cout << "GlyphCacheAssetTests PASS: native Chinese/fallback, 300 reuse cycles, same-address reload\n";
}
