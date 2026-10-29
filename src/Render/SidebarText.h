#pragma once

#include "GlyphCache.h"
#include <array>
#include <string>

class REGISTERS;
struct BuildType;

namespace SidebarText
{
	struct Command
	{
		GlyphCache::GlyphID Glyph; // Empty means a background rectangle.
		int X = 0, Y = 0, Width = 0, Height = 0;
		int ClipLeft = 0, ClipTop = 0, ClipRight = 0, ClipBottom = 0;
		unsigned Color = 0, Opacity256 = 256;
		std::array<unsigned, 3> Shift {}, Bits {};
		bool AfterClock = false;
	};
	struct Run
	{
		std::vector<Command> Commands;
		std::vector<std::wstring> Texts;
		std::uint64_t Fingerprint = 0;
		bool Known = true;
	};
	enum class GlyphResult { Captured, Skipped, Failed };
	GlyphCache::Cache& Glyphs();
	bool BindFont();
	// Packed engine color layout of the current ColorPacker.
	void ColorFormat(Command& command);
	// Resolves the glyph of a BitFont::Blit (434120) entry and fills its mask,
	// pen, inclusive font bounds as a half-open clip and native color.
	GlyphResult CaptureGlyph(REGISTERS* registers, Command& command);
	void BeginItem(REGISTERS* registers);
	void EndItem(std::uintptr_t stack);
	void ObservePrint(REGISTERS* registers);
	void EndPrint(std::uintptr_t entryStack);
	void ObserveGlyph(REGISTERS* registers);
	void ObserveBackground(REGISTERS* registers);
	void FontInvalidating(const void* font);
	void ClearRuns();
	void Refresh();
	const Run* Find(int tab, const BuildType& item);
}
