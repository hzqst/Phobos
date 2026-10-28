#pragma once

#include "FrameCache.h"
#include <span>

// Native 1bpp font records, independent of SHP and palettes. Main-thread only.
// The provider must invalidate before font/fallback replacement or release.
namespace GlyphCache
{
	using GlyphID = FrameCache::ID<struct GlyphTag>;
	using FontID = FrameCache::ID<struct FontTag>;
	struct Format
	{
		unsigned Stride = 0, Height = 0, RecordBytes = 0, Symbols = 0;
		bool operator==(const Format&) const = default;
	};
	struct Glyph
	{
		GlyphID ID;
		FrameCache::FrameID Storage;
		FontID Font;
		unsigned Width = 0, Height = 0;
	};
	struct Result { FrameCache::Status State; const Glyph* Value = nullptr; };
	class Cache
	{
	public:
		explicit Cache(FrameCache::Limits limits = {8 * 1024 * 1024, 65536, 8}) : Pixels(limits) { }
		// Identity is an opaque token. Equal addresses require explicit pre-free invalidation.
		FontID Register(std::uintptr_t identity, Format format);
		// symbol is the zero-based native record index. Symbols is the separate fallback slot.
		// record contains width followed by Height rows of Stride bytes, MSB first.
		// A fallback may only be supplied after the provider proves it was initialized.
		Result Acquire(FontID font, unsigned symbol, std::span<const std::uint8_t> record);
		const Glyph* Resolve(GlyphID id) const;
		bool Invalidate(FontID font);
		void Clear();
		const FrameCache::Cache& Storage() const { return Pixels; }
	private:
		struct Font
		{
			std::uintptr_t Identity;
			Format Layout;
			FrameCache::ResourceID Resource;
			std::unordered_map<unsigned, GlyphID> Symbols;
		};
		FrameCache::Cache Pixels;
		std::unordered_map<std::uintptr_t, FontID> Identities;
		std::unordered_map<std::uint64_t, Font> Fonts;
		std::unordered_map<std::uint64_t, Glyph> Glyphs;
	};
}
