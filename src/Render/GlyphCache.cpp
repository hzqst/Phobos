#include "GlyphCache.h"

namespace GlyphCache
{
	namespace
	{
		FrameCache::IDSequence FontIDs;
		FrameCache::IDSequence GlyphIDs;
		constexpr unsigned MaxStride = 32; // Native width is a byte; up to 256 mask pixels.
		constexpr unsigned MaxHeight = 256;
		constexpr unsigned MaxSymbols = 65535;
		constexpr std::uint64_t MaxNativeBytes = 64 * 1024 * 1024;
	}

	FontID Cache::Register(std::uintptr_t identity, Format format)
	{
		if (!identity || !format.Stride || format.Stride > MaxStride
			|| !format.Height || format.Height > MaxHeight || !format.Symbols || format.Symbols > MaxSymbols
			|| format.RecordBytes < 1 + format.Stride * format.Height
			|| std::uint64_t(format.RecordBytes) * format.Symbols > MaxNativeBytes)
			return {};
		if (const auto found = Identities.find(identity); found != Identities.end())
		{
			if (Fonts.at(found->second.Value).Layout == format) return found->second;
			Invalidate(found->second);
		}
		FrameCache::ResourceInfo info;
		info.Width = int(format.Stride * 8);
		info.Height = int(format.Height);
		info.Frames.resize(std::size_t(format.Symbols) + 1);
		for (auto& descriptor : info.Frames)
		{
			descriptor.Width = info.Width;
			descriptor.Height = info.Height;
		}
		const auto resource = Pixels.Register(identity, info);
		if (!resource) return {};
		const FontID id {FontIDs.Next()};
		if (!id) { Pixels.Invalidate(resource); return {}; }
		Fonts.emplace(id.Value, Font {identity, format, resource, {}});
		Identities.emplace(identity, id);
		return id;
	}

	Result Cache::Acquire(FontID fontID, unsigned symbol, std::span<const std::uint8_t> record)
	{
		const auto found = Fonts.find(fontID.Value);
		if (found == Fonts.end()) return {FrameCache::Status::NotReady};
		auto& font = found->second;
		if (symbol > font.Layout.Symbols) return {FrameCache::Status::InvalidData};
		if (const auto cached = font.Symbols.find(symbol); cached != font.Symbols.end())
			return {FrameCache::Status::Ready, Resolve(cached->second)};
		const auto format = font.Layout;
		// Do not poison the cache when a provider has not yet obtained readable bytes.
		const auto required = std::size_t(1) + format.Stride * format.Height;
		if (record.size() < required || record[0] > format.Stride * 8)
			return {FrameCache::Status::InvalidData};
		const auto width = record[0];
		const auto decoded = Pixels.GetOrDecode(font.Resource, symbol,
			[&](const FrameCache::Descriptor& d, std::vector<std::uint8_t>& output)
			{
				output.assign(std::size_t(d.Width) * d.Height, 0);
				for (unsigned y = 0; y < format.Height; ++y)
					for (unsigned x = 0; x < width; ++x)
						output[std::size_t(y) * d.Width + x] =
							(record[1 + std::size_t(y) * format.Stride + x / 8] >> (7 - x % 8)) & 1;
				return FrameCache::Status::Ready;
			});
		if (decoded.State != FrameCache::Status::Ready) return {decoded.State};
		const GlyphID id {GlyphIDs.Next()};
		if (!id) return {FrameCache::Status::IDExhausted};
		const auto [glyph, inserted] = Glyphs.emplace(id.Value,
			Glyph {id, decoded.Frame->ID, fontID, width, format.Height});
		font.Symbols.emplace(symbol, id);
		return {FrameCache::Status::Ready, &glyph->second};
	}

	const Glyph* Cache::Resolve(GlyphID id) const
	{
		const auto found = Glyphs.find(id.Value);
		return found == Glyphs.end() ? nullptr : &found->second;
	}

	bool Cache::Invalidate(FontID id)
	{
		const auto found = Fonts.find(id.Value);
		if (found == Fonts.end()) return false;
		for (const auto& [symbol, glyph] : found->second.Symbols) Glyphs.erase(glyph.Value);
		Pixels.Invalidate(found->second.Resource);
		Identities.erase(found->second.Identity);
		Fonts.erase(found);
		return true;
	}

	void Cache::Clear()
	{
		while (!Fonts.empty()) Invalidate(FontID {Fonts.begin()->first});
	}
}
