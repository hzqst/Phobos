#include "SidebarText.h"
#include "SidebarSnapshot.h"
#include "GPURender.h"
#include <BitFont.h>
#include <FactoryClass.h>
#include <SuperClass.h>
#include <Syringe.h>

namespace SidebarText
{
	namespace
	{
		using CursorSnapshot::Readable;
		constexpr unsigned MaxText = 512, MaxCommands = 1024, MaxNesting = 8;
		constexpr int MaxCoordinate = 16384;
		GlyphCache::Cache Cache;
		std::array<std::unordered_map<std::uint64_t, Run>, 4> Runs;
		std::array<std::vector<std::uint64_t>, 4> Lists;
		struct ItemScope
		{
			std::uintptr_t Stack;
			int Tab, X, Y;
			BuildType Item;
			Run Pending;
		};
		struct PrintScope { std::uintptr_t Stack; bool Accepted, AfterClock; };
		std::vector<ItemScope> Items;
		std::vector<PrintScope> Prints;
		const BitFont* BoundFont = nullptr;
		GlyphCache::FontID BoundID;
		std::array<std::uintptr_t, 10> Binding {};
		std::array<int, 4> Metrics {};
		unsigned TraceCount = 0;
		std::unordered_set<std::uint64_t> TraceHashes;
		unsigned LifetimeTraceCount = 0, MissingTraceCount = 0, SuperTraceCount = 0;
		GlyphCache::GlyphID LastGlyph;
		std::unordered_set<std::uint64_t> SuperTraceHashes;
		void TraceLifetime(const char* event, GlyphCache::FontID font, GlyphCache::GlyphID glyph, bool resolvedBefore)
		{
			// The game closes debug.log before its final resource teardown.
			// Keep this bounded trace independent of that logger's lifetime.
			char filename[96];
			std::snprintf(filename, sizeof(filename), "debug/SidebarText-lifetime-%lu.log", GetCurrentProcessId());
			std::FILE* file = nullptr;
			if (fopen_s(&file, filename, "a") || !file) return;
			const auto& stats = Cache.Storage().GetStats();
			std::fprintf(file, "event=%s pid=%lu fontID=%llu glyphID=%llu resolvedBefore=%d resolvedNow=%d frames=%zu resources=%zu invalidations=%llu\n",
				event, GetCurrentProcessId(), font.Value, glyph.Value, resolvedBefore, Cache.Resolve(glyph) != nullptr,
				stats.Frames, stats.Resources, stats.Invalidations);
			std::fclose(file);
		}
		bool Coordinate(int value) { return value >= -MaxCoordinate && value <= MaxCoordinate; }

		bool RefreshFont()
		{
			const auto* font = BitFont::Instance;
			if (!Readable(font, sizeof(*font)) || *reinterpret_cast<const unsigned*>(font) != 0x7E3A78
				|| !Readable(font->InternalPTR, sizeof(BitFont::InternalData)))
			{
				FontInvalidating(nullptr);
				return false;
			}
			const auto& data = *font->InternalPTR;
			const std::array<std::uintptr_t, 10> binding {reinterpret_cast<std::uintptr_t>(font),
				reinterpret_cast<std::uintptr_t>(font->InternalPTR), reinterpret_cast<std::uintptr_t>(font->Pointer_8),
				reinterpret_cast<std::uintptr_t>(data.SymbolTable), reinterpret_cast<std::uintptr_t>(data.Bitmaps),
				unsigned(data.Stride), unsigned(data.FontHeight), unsigned(data.SymbolDataSize), unsigned(data.Count),
				reinterpret_cast<std::uintptr_t>(font->field_18)};
			const std::array<int, 4> metrics {font->field_1C, font->Unknown_28, font->State_2C, data.Lines};
			if (binding != Binding)
			{
				FontInvalidating(nullptr);
				if (binding[9] != unsigned(data.Stride) || !Readable(data.SymbolTable, 65536 * sizeof(short))) return false;
				BoundID = Cache.Register(reinterpret_cast<std::uintptr_t>(font),
					{unsigned(data.Stride), unsigned(data.FontHeight), unsigned(data.SymbolDataSize), unsigned(data.Count)});
				if (!BoundID) return false;
				if (!Readable(data.Bitmaps, std::size_t(data.Count) * data.SymbolDataSize))
					{ FontInvalidating(nullptr); return false; }
				BoundFont = font; Binding = binding; Metrics = metrics;
				if (SidebarSnapshot::Trace && TraceCount < 96)
				{
					TraceLifetime("bind", BoundID, LastGlyph, Cache.Resolve(LastGlyph) != nullptr);
					Debug::Log("[SidebarTextGPU] font sample=%u object=%p fontID=%llu symbols=%d stride=%d height=%d recordBytes=%d\n",
						++TraceCount, font, BoundID.Value, data.Count, data.Stride, data.FontHeight, data.SymbolDataSize);
				}
			}
			else if (metrics != Metrics) { ClearRuns(); Metrics = metrics; }
			return bool(BoundID);
		}

#ifdef DEBUG
		void ProbeFontLifetime()
		{
			static bool done = false;
			if (done || !BoundFont || BoundFont->pGraphBuffer) return;
			char enabled[2] {};
			if (GetEnvironmentVariableA("PHOBOS_FONT_LIFETIME_TEST", enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
				{ done = true; return; }
			done = true;
			// Explicit debug-only regression probe. Own this allocation and every
			// constructed font; never destroy the game's original font. Reuse the
			// object allocation to exercise native teardown with identical addresses.
			auto* original = BitFont::Instance;
			auto* probe = static_cast<BitFont*>(YRMemory::Allocate(sizeof(BitFont)));
			if (!probe) { Debug::Log("[SidebarTextGPU] lifetimeProbe allocation failed\n"); return; }
			using Constructor = void (__thiscall*)(BitFont*, const char*);
			using Destructor = void (__thiscall*)(BitFont*);
			const auto construct = reinterpret_cast<Constructor>(0x433880);
			const auto destroy = reinterpret_cast<Destructor>(0x433940);
			GlyphCache::FontID previousFont;
			GlyphCache::GlyphID previousGlyph;
			bool passed = true;
			for (unsigned cycle = 1; cycle <= 2; ++cycle)
			{
				construct(probe, "GAME.FNT");
				BitFont::Instance = probe;
				const bool bound = RefreshFont();
				GlyphCache::GlyphID glyph;
				FrameCache::FrameID storage;
				const auto font = BoundID;
				if (bound)
				{
					const auto& data = *probe->InternalPTR;
					const auto symbol = static_cast<unsigned short>(data.SymbolTable[0x58]);
					if (symbol && symbol <= data.Count)
					{
						const auto* record = reinterpret_cast<const std::uint8_t*>(data.Bitmaps) + std::size_t(symbol - 1) * data.SymbolDataSize;
						const auto acquired = Cache.Acquire(font, symbol - 1, {record, std::size_t(data.SymbolDataSize)});
						if (acquired.State == FrameCache::Status::Ready)
							{ glyph = acquired.Value->ID; storage = acquired.Value->Storage; LastGlyph = glyph; }
					}
				}
				const bool newIdentity = font && glyph && !(font == previousFont) && !(glyph == previousGlyph)
					&& !Cache.Resolve(previousGlyph);
				destroy(probe); // Ordinary destructor: hooks run before native data/fallback frees.
				BitFont::Instance = original;
				const bool invalidated = !BoundID && !Cache.Resolve(glyph) && !Cache.Storage().Resolve(storage)
					&& Cache.Storage().GetStats().Frames == 0 && Cache.Storage().GetStats().Resources == 0;
				passed &= newIdentity && invalidated;
				Debug::Log("[SidebarTextGPU] lifetimeProbe cycle=%u object=%p fontID=%llu glyphID=%llu newIdentity=%d invalidated=%d\n",
					cycle, probe, font.Value, glyph.Value, newIdentity, invalidated);
				previousFont = font; previousGlyph = glyph;
			}
			YRMemory::Deallocate(probe);
			const bool restored = RefreshFont() && BoundFont == original && !Cache.Resolve(previousGlyph);
			Debug::Log("[SidebarTextGPU] lifetimeProbe result=%s original=%p restored=%d\n", passed && restored ? "PASS" : "FAIL", original, restored);
		}
#endif

		std::uint64_t Fingerprint(const BuildType& item)
		{
			auto hash = SidebarSnapshot::Hash(&item.CurrentFactory, sizeof(item.CurrentFactory));
			hash = SidebarSnapshot::Hash(&item.unknown_10, sizeof(item.unknown_10), hash);
			hash = SidebarSnapshot::Hash(&item.IsAlt, sizeof(item.IsAlt), hash);
			hash = SidebarSnapshot::Hash(reinterpret_cast<const void*>(0xB0FA1C), 3, hash);
			hash = SidebarSnapshot::Hash(&Unsorted::ColorPackData, sizeof(Unsorted::ColorPackData), hash);
			// The count's primary factory can differ from CurrentFactory. Observe owned
			// factory queues without invoking lookup/AI or mutating progress flags.
			const auto& factories = FactoryClass::Array;
			if (factories.Count < 0 || factories.Count > 4096
				|| (factories.Count && !Readable(factories.Items, factories.Count * sizeof(void*)))) return 0;
			for (const auto* factory : factories)
			{
				if (!Readable(factory, sizeof(*factory))) return 0;
				if (factory->Owner != HouseClass::CurrentPlayer) continue;
				const std::uintptr_t fields[] {reinterpret_cast<std::uintptr_t>(factory),
					reinterpret_cast<std::uintptr_t>(factory->Object), unsigned(factory->Production.Value >= 54),
					unsigned(factory->Production.Rate == 0), factory->IsSuspended, factory->IsManual};
				hash = SidebarSnapshot::Hash(fields, sizeof(fields), hash);
				const auto& queue = factory->QueuedObjects;
				if (queue.Count < 0 || queue.Count > 4096 || (queue.Count && !Readable(queue.Items, queue.Count * sizeof(void*)))) return 0;
				hash = SidebarSnapshot::Hash(&queue.Count, sizeof(queue.Count), hash);
				if (queue.Count) hash = SidebarSnapshot::Hash(queue.Items, queue.Count * sizeof(void*), hash);
			}
			if (item.ItemType == AbstractType::Special)
			{
				const auto* house = HouseClass::CurrentPlayer;
				if (!house || item.ItemIndex < 0 || item.ItemIndex >= house->Supers.Count) return 0;
				const auto* super = house->Supers[item.ItemIndex];
				if (!Readable(super, sizeof(*super))) return 0;
				const std::uintptr_t state[] {reinterpret_cast<std::uintptr_t>(super), reinterpret_cast<std::uintptr_t>(super->Type),
					super->IsPresent, super->IsReady, super->IsSuspended, unsigned(super->ChargeDrainState)};
				hash = SidebarSnapshot::Hash(state, sizeof(state), hash);
			}
			return hash;
		}

		void SyncList(int tab)
		{
			const BuildType* items = nullptr; int count = 0;
			std::vector<std::uint64_t> keys;
			if (SidebarSnapshot::GetCameoList(tab, items, count))
				for (int i = 0; i < count; ++i) keys.push_back(SidebarSnapshot::CameoKey(items[i]));
			if (keys != Lists[tab]) { Runs[tab].clear(); Lists[tab] = std::move(keys); }
		}

		void ColorFormat(Command& command)
		{
			const auto& pack = Unsorted::ColorPackData;
			command.Shift = {unsigned(pack._R_SHL), unsigned(pack._G_SHL), unsigned(pack._B_SHL)};
			command.Bits = {unsigned(8 - pack._R_SHR), unsigned(8 - pack._G_SHR), unsigned(8 - pack._B_SHR)};
		}

		void Append(Command command)
		{
			auto& scope = Items.back();
			if (!Coordinate(command.X) || !Coordinate(command.Y) || !Coordinate(scope.X) || !Coordinate(scope.Y)
				|| scope.Pending.Commands.size() >= MaxCommands) { scope.Pending.Known = false; return; }
			command.X -= scope.X; command.Y -= scope.Y;
			// Left/top are fixed surface bounds; right/bottom follow the cameo cell.
			command.ClipRight -= scope.X; command.ClipBottom -= scope.Y;
			ColorFormat(command);
			scope.Pending.Commands.push_back(command);
		}
	}

	GlyphCache::Cache& Glyphs() { return Cache; }

	void ClearRuns()
	{
		for (auto& runs : Runs) runs.clear();
		Items.clear(); Prints.clear();
		SidebarSnapshot::Current.Visible = false;
	}

	void FontInvalidating(const void* font)
	{
		if (font && font != BoundFont) return;
		const auto oldFont = BoundID;
		const auto oldGlyph = LastGlyph;
		const auto before = Cache.Storage().GetStats();
		const bool oldResolved = Cache.Resolve(oldGlyph) != nullptr;
		ClearRuns(); Cache.Clear(); BoundFont = nullptr; BoundID = {}; Binding = {}; Metrics = {};
		if (SidebarSnapshot::Trace && oldFont && LifetimeTraceCount < 32)
		{
			TraceLifetime("invalidate", oldFont, oldGlyph, oldResolved);
			const auto& after = Cache.Storage().GetStats();
			Debug::Log("[SidebarTextGPU] invalidate sample=%u pid=%lu requestedFont=%p oldFontID=%llu oldGlyphID=%llu resolvedBefore=%d resolvedAfter=%d framesBefore=%zu framesAfter=%zu resourcesAfter=%zu invalidations=%llu\n",
				++LifetimeTraceCount, GetCurrentProcessId(), font, oldFont.Value, oldGlyph.Value,
				oldResolved, Cache.Resolve(oldGlyph) != nullptr, before.Frames, after.Frames, after.Resources, after.Invalidations);
		}
	}

	void Refresh()
	{
		RefreshFont();
#ifdef DEBUG
		ProbeFontLifetime();
#endif
		for (int tab = 0; tab < 4; ++tab) SyncList(tab);
		// A draw must finish each item before publishing. Reject interrupted scopes.
		Items.clear(); Prints.clear();
	}

	void BeginItem(REGISTERS* r)
	{
		const auto state = GPURender::GetState();
		if ((state != GPURender::State::Running && state != GPURender::State::Hidden)
			|| !SidebarSnapshot::Eligible() || HouseClass::IsCurrentPlayerObserver() || !RefreshFont()) return;
		const auto* strip = r->Stack<StripClass*>(0x24);
		const int tab = SidebarClass::Instance.ActiveTabIndex;
		if (tab < 0 || tab >= 4 || strip != &SidebarClass::Instance.Tabs[tab]) return;
		SyncList(tab);
		const BuildType* items = nullptr; int count = 0;
		const int index = r->Stack<int>(0x3C);
		if (!SidebarSnapshot::GetCameoList(tab, items, count) || index < 0 || index >= count) return;
		if (Items.size() >= MaxNesting) { ClearRuns(); return; }
		Items.push_back({r->ESP(), tab, int(r->ESI()), int(r->EBP()), items[index], {}});
	}

	void EndItem(std::uintptr_t stack)
	{
		if (Items.empty() || Items.back().Stack != stack) return;
		auto scope = std::move(Items.back()); Items.pop_back();
		while (!Prints.empty() && Prints.back().Stack < stack) Prints.pop_back();
		scope.Pending.Fingerprint = Fingerprint(scope.Item);
		if (!scope.Pending.Fingerprint) scope.Pending.Known = false;
		if (SidebarSnapshot::Trace && SuperTraceCount < 64 && scope.Item.ItemType == AbstractType::Special)
		{
			const auto* house = HouseClass::CurrentPlayer;
			const auto* super = house && scope.Item.ItemIndex >= 0 && scope.Item.ItemIndex < house->Supers.Count
				? house->Supers[scope.Item.ItemIndex] : nullptr;
			if (Readable(super, sizeof(*super)) && Readable(super->Type, sizeof(*super->Type)))
			{
				const unsigned state[] {unsigned(scope.Tab), unsigned(scope.Item.ItemIndex), super->Type->UseChargeDrain,
					super->IsReady, super->IsSuspended, unsigned(super->ChargeDrainState), unsigned(scope.Pending.Texts.size()), scope.Pending.Known};
				if (SuperTraceHashes.insert(SidebarSnapshot::Hash(state, sizeof(state))).second)
					Debug::Log("[SidebarTextGPU] super sample=%u tab=%d index=%d chargeDrain=%d ready=%d onHold=%d drainState=%d texts=%zu commands=%zu known=%d\n",
						++SuperTraceCount, scope.Tab, scope.Item.ItemIndex, super->Type->UseChargeDrain, super->IsReady,
						super->IsSuspended, int(super->ChargeDrainState), scope.Pending.Texts.size(), scope.Pending.Commands.size(), scope.Pending.Known);
			}
		}
		if (SidebarSnapshot::Trace && TraceCount < 96 && !scope.Pending.Texts.empty())
		{
			auto hash = scope.Pending.Fingerprint;
			for (const auto& text : scope.Pending.Texts) hash = SidebarSnapshot::Hash(text.data(), text.size() * sizeof(wchar_t), hash);
			if (TraceHashes.insert(hash).second)
			{
				Debug::Log("[SidebarTextGPU] run sample=%u tab=%d type=%d index=%d known=%d texts=%zu commands=%zu glyphDecoded=%llu\n",
					++TraceCount, scope.Tab, int(scope.Item.ItemType), scope.Item.ItemIndex, scope.Pending.Known,
					scope.Pending.Texts.size(), scope.Pending.Commands.size(), Cache.Storage().GetStats().Decoded);
				for (const auto& text : scope.Pending.Texts)
				{
					std::string units;
					for (const auto unit : text)
					{
						char hex[6]; std::snprintf(hex, sizeof(hex), "%04X ", unsigned(unit)); units += hex;
					}
					Debug::Log("[SidebarTextGPU] utf16=%s\n", units.c_str());
				}
			}
		}
		Runs[scope.Tab][SidebarSnapshot::CameoKey(scope.Item)] = std::move(scope.Pending);
	}

	void ObservePrint(REGISTERS* r)
	{
		if (Items.empty()) return;
		if (Prints.size() >= MaxNesting) { Items.back().Pending.Known = false; return; }
		Prints.push_back({r->ESP(), false, false});
		if (r->Stack<void*>(4) != DSurface::Sidebar) return;
		const auto direct = r->Stack<unsigned>(0);
		if ((direct != 0x4A6143 && direct != 0x4A619A) || !Readable(r->lea_Stack<void*>(0x428), 4)) return;
		const auto caller = r->Stack<unsigned>(0x428);
		if (caller != 0x6A9D80 && caller != 0x6A9E3B && caller != 0x6AA019) return;
		const unsigned flags = r->Stack<unsigned>(0x18);
		if (flags != 0x42 && flags != 0x142 && flags != 0x242) { Items.back().Pending.Known = false; return; }
		const auto* input = reinterpret_cast<const wchar_t*>(r->EDX());
		std::wstring text;
		for (unsigned i = 0; i < MaxText; ++i)
		{
			if (!Readable(input + i, sizeof(wchar_t))) break;
			if (!input[i])
			{
				Items.back().Pending.Texts.push_back(std::move(text));
				Prints.back().Accepted = true; Prints.back().AfterClock = caller == 0x6AA019;
				return;
			}
			text.push_back(input[i]);
		}
		Items.back().Pending.Known = false;
	}

	void EndPrint(std::uintptr_t stack)
	{
		if (!Prints.empty() && Prints.back().Stack == stack) Prints.pop_back();
	}

	void ObserveGlyph(REGISTERS* r)
	{
		if (Items.empty() || Prints.empty() || !Prints.back().Accepted || r->Stack<unsigned>(0) != 0x43464D) return;
		const auto* font = reinterpret_cast<const BitFont*>(r->ECX());
		const auto unit = r->Stack<unsigned short>(4);
		if (unit == 9) return; // Native tab moves the pen without a bitmap.
		if (font != BoundFont || !Readable(font, sizeof(*font)) || !font->pGraphBuffer)
			{ Items.back().Pending.Known = false; return; }
		const auto& data = *font->InternalPTR;
		const auto mapped = static_cast<unsigned short>(data.SymbolTable[unit]);
		const auto bytes = std::size_t(1) + data.Stride * data.FontHeight;
		const std::uint8_t* record = nullptr;
		if (mapped && mapped <= data.Count)
			record = reinterpret_cast<const std::uint8_t*>(data.Bitmaps) + std::size_t(mapped - 1) * data.SymbolDataSize;
		else if (!mapped)
		{
			// Native fallback is inverse X, not a replacement '?' glyph. Validate that
			// it was initialized, including when the bound font predates our first sample.
			const auto x = static_cast<unsigned short>(data.SymbolTable[0x58]);
			if (x && x <= data.Count)
			{
				const auto* original = reinterpret_cast<const std::uint8_t*>(data.Bitmaps) + std::size_t(x - 1) * data.SymbolDataSize;
				const auto* fallback = static_cast<const std::uint8_t*>(font->Pointer_8);
				if (Readable(original, bytes) && Readable(fallback, bytes) && original[0] == fallback[0])
				{
					bool valid = true;
					for (std::size_t i = 1; i < bytes; ++i) valid &= fallback[i] == std::uint8_t(~original[i]);
					if (valid) record = fallback;
				}
			}
		}
		if (!Readable(record, bytes))
		{
			if (!mapped && SidebarSnapshot::Trace && MissingTraceCount < 32)
				Debug::Log("[SidebarTextGPU] missing sample=%u unit=%04X fontID=%llu fallback=unsupported\n",
					++MissingTraceCount, unsigned(unit), BoundID.Value);
			Items.back().Pending.Known = false; return;
		}
		const auto acquired = Cache.Acquire(BoundID, mapped ? mapped - 1 : unsigned(data.Count), {record, bytes});
		if (acquired.State != FrameCache::Status::Ready) { Items.back().Pending.Known = false; return; }
		LastGlyph = acquired.Value->ID;
		if (!mapped && SidebarSnapshot::Trace && MissingTraceCount < 32)
			Debug::Log("[SidebarTextGPU] missing sample=%u unit=%04X fontID=%llu fallback=verified glyphID=%llu\n",
				++MissingTraceCount, unsigned(unit), BoundID.Value, LastGlyph.Value);
		Command command;
		command.Glyph = acquired.Value->ID;
		command.X = r->Stack<int>(8); command.Y = r->Stack<int>(12);
		command.Width = acquired.Value->Width; command.Height = acquired.Value->Height;
		const auto* clip = reinterpret_cast<const int*>(&font->Bounds);
		if (!font->field_41 || !Coordinate(clip[0]) || !Coordinate(clip[1]) || !Coordinate(clip[2]) || !Coordinate(clip[3]))
			{ Items.back().Pending.Known = false; return; }
		command.ClipLeft = clip[0]; command.ClipTop = clip[1];
		command.ClipRight = clip[2] + 1; command.ClipBottom = clip[3] + 1;
		const auto color = r->Stack<int>(16);
		command.Color = (color == -1 ? font->Color : static_cast<unsigned short>(color)) ^ (mapped ? 0 : 0x5555);
		command.AfterClock = Prints.back().AfterClock;
		Append(command);
	}

	void ObserveBackground(REGISTERS* r)
	{
		if (Items.empty() || reinterpret_cast<void*>(r->EDX()) != DSurface::Sidebar) return;
		constexpr unsigned EntryStack = 0x34;
		const auto caller = r->Stack<unsigned>(EntryStack);
		if (caller != 0x6A9D51 && caller != 0x6A9E11 && caller != 0x6A9F5E && caller != 0x6A9FF3) return;
		const auto* rect = reinterpret_cast<const RectangleStruct*>(r->ECX());
		if (!Readable(rect, sizeof(*rect)) || rect->Width < 0 || rect->Height < 0
			|| rect->Width > MaxCoordinate || rect->Height > MaxCoordinate
			|| r->Stack<unsigned short>(EntryStack + 4) != 0 || r->Stack<unsigned char>(EntryStack + 8) != 175)
			{ Items.back().Pending.Known = false; return; }
		Command command;
		command.X = rect->X; command.Y = rect->Y; command.Width = rect->Width; command.Height = rect->Height;
		command.Opacity256 = 176; // Native black background leaves dst * 80 / 256.
		command.AfterClock = caller == 0x6A9F5E || caller == 0x6A9FF3;
		Append(command);
	}

	const Run* Find(int tab, const BuildType& item)
	{
		if (tab < 0 || tab >= 4) return nullptr;
		const auto found = Runs[tab].find(SidebarSnapshot::CameoKey(item));
		if (found == Runs[tab].end() || !found->second.Known) return nullptr;
		if (found->second.Fingerprint != Fingerprint(item)) { Runs[tab].erase(found); return nullptr; }
		return &found->second;
	}
}
