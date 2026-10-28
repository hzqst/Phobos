#pragma once

#include "CursorSnapshot.h"
#include "ShapeImage.h"
#include "ButtonImage.h"
#include "ShapeFrameSource.h"
#include "PaletteCache.h"
#include <ShapeButtonClass.h>
#include <SidebarClass.h>
#include <ScenarioClass.h>
#include <GameOptionsClass.h>
#include <Surface.h>
#include <Utilities/Debug.h>
#include <Utilities/AresHelper.h>
#include <Utilities/AresFunctions.h>
#include <Ext/Rules/Body.h>
#include <UnitTypeClass.h>
#include <InfantryTypeClass.h>
#include <AircraftTypeClass.h>
#include <BuildingTypeClass.h>
#include <SuperWeaponTypeClass.h>

// Only CaptureAfterDraw reads engine-owned resources. Present/timer uses owned pixels.
namespace SidebarSnapshot
{
	constexpr size_t BackgroundCount = 6;
	constexpr size_t ButtonCount = 10;
	constexpr size_t CameoBegin = BackgroundCount + ButtonCount;
	constexpr size_t DarkenBegin = CameoBegin + ShapeImage::MaxCameoSlots;
	constexpr size_t ClockBegin = DarkenBegin + ShapeImage::MaxCameoSlots;
	constexpr size_t AssetCount = ClockBegin + ShapeImage::MaxCameoSlots;
	constexpr int MaxCanvasDimension = 4096;
	constexpr int MaxRepeats = 128;
	constexpr std::uintptr_t ResourceSlots[BackgroundCount] =
		{0xB0FB08, 0xB0B468, 0xB0B46C, 0xB0B470, 0xB0B474, 0xB0F9E0};
	constexpr const char* Names[BackgroundCount] =
		{"CREDITS", "SIDE1", "SIDE2", "SIDE3", "ADDON", "TOP"};
	constexpr std::uintptr_t ButtonObjects[ButtonCount] =
		{0xB07DF8, 0xB0B3A0, 0xB07C48, 0xB07CA8, 0xB07D08, 0xB07D68, 0xB0B328, 0xB0B408, 0xB04978, 0xB04910};
	static_assert(sizeof(ShapeButtonClass) == 0x60);
	static_assert(offsetof(ShapeButtonClass, Disabled) == 0x1E);
	static_assert(offsetof(ShapeButtonClass, IsPressed) == 0x2C);
	static_assert(offsetof(ShapeButtonClass, IsOn) == 0x2D);
	static_assert(offsetof(ShapeButtonClass, IsToFlash) == 0x34);
	static_assert(offsetof(ShapeButtonClass, UseFlash) == 0x40);
	static_assert(offsetof(ShapeButtonClass, DrawPosition) == 0x44);
	static_assert(offsetof(ShapeButtonClass, UseSidebarSurface) == 0x4C);
	static_assert(offsetof(ShapeButtonClass, Drawer) == 0x50);
	static_assert(offsetof(ShapeButtonClass, IsAlpha) == 0x55);
	static_assert(offsetof(ShapeButtonClass, ShapeData) == 0x58);

	struct Asset
	{
		int Left = 0, Top = 0, Width = 0, Height = 0, ShapeHeight = 0;
		unsigned Flags = 0;
		std::uintptr_t DataIdentity = 0;
		std::uintptr_t SourceIdentity = 0;
		FrameCache::FrameID ID;
		PaletteCache::Handle Color;
	};
	struct Placement
	{
		size_t AssetIndex; int X, Y;
		int ClipWidth = INT_MAX, ClipHeight = INT_MAX;
		ShapeImage::Effect Effect = ShapeImage::Effect::Normal;
		unsigned SolidColor = 0;
		int SolidWidth = 0, SolidHeight = 0;
	};
	struct Frame
	{
		bool Visible = false;
		int OriginX = 0, Width = 0, Height = 0;
		std::array<Asset, AssetCount> Assets;
		std::vector<Placement> Placements;
	};
	inline Frame Current;
	inline unsigned Generation = 0;
	inline bool Trace = false;
	inline unsigned TraceCount = 0;
	inline std::uint64_t LastTraceHash = 0;
	inline bool LowerButtonsReady = false, RadarButtonsReady = false;
	inline unsigned ButtonTraceCount = 0;
	inline std::array<std::uint64_t, ButtonCount> ButtonTraceHashes {};
	inline std::uint64_t CaptureCount = 0, LastDecoded = 0;
	inline unsigned CacheTraceCount = 0;
	inline PaletteCache::Cache Colors;
	inline std::vector<std::pair<const ConvertClass*, PaletteCache::Handle>> CapturedColors;
	inline unsigned CameoTraceCount = 0;
	inline std::uint64_t CameoTraceHash = 0;
	inline unsigned CameoItemTraceCount = 0;
	inline std::array<std::uint64_t, ShapeImage::MaxCameoSlots> CameoItemHashes {};
	struct NativeCameoState { bool Darken; int ClockFrame; };
	inline std::array<std::unordered_map<std::uint64_t, NativeCameoState>, 4> NativeCameoStates;
	inline unsigned EffectTraceCount = 0;
	inline std::array<std::uint64_t, ShapeImage::MaxCameoSlots> EffectHashes {};
	static_assert(offsetof(SidebarClass, Tabs) == 0x1544);
	static_assert(offsetof(StripClass, Cameos) == 0x58);
	static_assert(offsetof(TechnoTypeClass, Cameo) == 0x6F0);
	static_assert(offsetof(TechnoTypeClass, AltCameo) == 0x710);

	inline bool Eligible()
	{
		return *reinterpret_cast<const bool*>(0xA8E9A0)
			&& *reinterpret_cast<const bool*>(0xA8ED5C)
			&& !Unsorted::ArmageddonMode && SidebarClass::Instance.IsSidebarActive;
	}

	inline void Invalidate()
	{
		Current.Visible = false;
		Current.Placements.clear();
		++Generation;
		LastTraceHash = 0;
		for (auto& states : NativeCameoStates) states.clear();
	}

	inline void InitializeTrace()
	{
		char value[2] {};
		Trace = GetEnvironmentVariableA("PHOBOS_SIDEBAR_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
		TraceCount = 0;
		ButtonTraceCount = 0;
		ButtonTraceHashes.fill(0);
	}

	inline std::uint64_t Hash(const void* data, size_t size, std::uint64_t hash = 14695981039346656037ull)
	{
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		return hash;
	}

	inline PaletteCache::Handle CaptureColor(const ConvertClass* convert)
	{
		for (const auto& pair : CapturedColors) if (pair.first == convert) return pair.second;
		PaletteCache::Handle color;
		if (CursorSnapshot::Readable(convert, sizeof(*convert)) && convert->BytesPerPixel == 2
			&& CursorSnapshot::Readable(convert->PaletteData, 512))
		{
			PaletteCache::Data data;
			std::memcpy(data.Colors.data(), convert->PaletteData, sizeof(data.Colors));
			const auto& pack = Unsorted::ColorPackData;
			data.Shift = {unsigned(pack._R_SHL), unsigned(pack._G_SHL), unsigned(pack._B_SHL)};
			data.Bits = {unsigned(8 - pack._R_SHR), unsigned(8 - pack._G_SHR), unsigned(8 - pack._B_SHR)};
			color = Colors.Acquire(data);
		}
		CapturedColors.emplace_back(convert, color);
		return color;
	}

	inline std::uint64_t ColorHash(const PaletteCache::Handle& color)
	{
		return color ? Hash(color->Values.Colors.data(), sizeof(color->Values.Colors)) : 0;
	}

	inline bool CopyAsset(const SHPStruct* source, const PaletteCache::Handle& color, Asset& asset, int frame = 0)
	{
		asset.Color = color;
		if (!color) { asset.ID = {}; return false; }
		std::uintptr_t data = 0;
		const auto result = ShapeFrameSource::Acquire(source, frame, data);
		if (result.State != FrameCache::Status::Ready)
		{
			asset.ID = {};
			return false;
		}
		const auto& cached = *result.Frame;
		const auto& geometry = cached.Geometry;
		asset.ShapeHeight = source->Height; // Native layout reads the original SHP/reference header.
		asset.Left = geometry.Left; asset.Top = geometry.Top;
		asset.Width = geometry.Width; asset.Height = geometry.Height; asset.Flags = geometry.Flags;
		asset.DataIdentity = data;
		asset.SourceIdentity = reinterpret_cast<std::uintptr_t>(source);
		asset.ID = cached.ID;
		return true;
	}

	inline void CaptureButtons(size_t begin, size_t end)
	{
		for (size_t i = begin; i < end; ++i)
		{
			auto& asset = Current.Assets[BackgroundCount + i];
			if (!(i < 8 ? LowerButtonsReady : RadarButtonsReady)) { asset.ID = {}; asset.Color.reset(); continue; }
			const auto* button = reinterpret_cast<const ShapeButtonClass*>(ButtonObjects[i]);
			const auto* drawer = button->Drawer;
			const int frame = ButtonImage::SelectFrame(button->Disabled, button->UseFlash,
				button->IsToFlash, button->IsOn, button->IsPressed);
			// Preserve x86 addition without signed overflow. Do not recreate native layout.
			const int x = std::int32_t(std::uint32_t(button->X) + std::uint32_t(button->DrawPosition.X));
			const int y = std::int32_t(std::uint32_t(button->Y) + std::uint32_t(button->DrawPosition.Y));
			const bool supported = button->UseSidebarSurface && !button->IsAlpha;
			const auto color = supported ? CaptureColor(drawer) : PaletteCache::Handle {};
			bool ready = bool(color);
			if (ready)
			{
				ready = CopyAsset(button->ShapeData, color, asset, frame);
			}
			else { asset.ID = {}; asset.Color.reset(); }
			if (ready) Current.Placements.push_back({BackgroundCount + i, x, y});
			if (Trace && ButtonTraceCount < 64)
			{
				const int state[] {int(Generation), button->ID, button->Disabled, button->UseFlash,
					button->IsToFlash, button->IsOn, button->IsPressed, frame, x, y, ready, supported,
					Current.OriginX, Current.Width, Current.Height, asset.Left, asset.Top, asset.Width, asset.Height};
				auto hash = Hash(state, sizeof(state));
				hash = Hash(&button->ShapeData, sizeof(button->ShapeData), hash);
				hash = Hash(&drawer, sizeof(drawer), hash);
				const auto colorHash = ColorHash(color);
				hash = Hash(&colorHash, sizeof(colorHash), hash);
				hash = Hash(&asset.ID.Value, sizeof(asset.ID.Value), hash);
				if (hash != ButtonTraceHashes[i])
				{
					ButtonTraceHashes[i] = hash;
					++ButtonTraceCount;
					Debug::Log("[SidebarButtonGPU] sample=%u generation=%u thread=%lu id=%d ready=%d supported=%d state=%d,%d,%d,%d,%d frame=%d shape=%p data=%p drawer=%p palette=%016llX pos=%d,%d offset=%d,%d size=%dx%d flags=%u origin=%d clip=%dx%d frameID=%llu\n",
						ButtonTraceCount, Generation, GetCurrentThreadId(), button->ID, ready, supported,
						button->Disabled, button->UseFlash, button->IsToFlash, button->IsOn, button->IsPressed, frame,
						button->ShapeData, reinterpret_cast<void*>(asset.DataIdentity), drawer, colorHash,
						x, y, asset.Left, asset.Top, asset.Width, asset.Height, asset.Flags, Current.OriginX, Current.Width, Current.Height, asset.ID.Value);
				}
			}
		}
	}

	// Only the installed, inspected Ares 3.0p1 layout is accepted. Other extensions
	// must supply a verified provider rather than falling back to stale vanilla slots.
	inline bool KnownAresCameos()
	{
		return AresHelper::CanUseAres && AresHelper::AresVersion == AresHelper::Version::Ares30p
			&& reinterpret_cast<std::uintptr_t>(GetProcAddress(reinterpret_cast<HMODULE>(AresHelper::AresBaseAddress), "StripClass_Draw_GetCameo1"))
				== AresHelper::AresBaseAddress + 0x2CF90
			&& reinterpret_cast<std::uintptr_t>(GetProcAddress(reinterpret_cast<HMODULE>(AresHelper::AresBaseAddress), "StripClass_Draw_Main"))
				== AresHelper::AresBaseAddress + 0x5F180;
	}

	template<typename T> inline T* CameoTypeAt(int index)
	{
		const auto& types = T::Array;
		return index >= 0 && index < types.Count && types.Items && CursorSnapshot::Readable(types.Items + index, sizeof(T*))
			? types.Items[index] : nullptr;
	}

	inline TechnoTypeClass* ResolveCameoType(const BuildType& item)
	{
		switch (item.ItemType)
		{
		case AbstractType::Unit: case AbstractType::UnitType: return CameoTypeAt<UnitTypeClass>(item.ItemIndex);
		case AbstractType::Aircraft: case AbstractType::AircraftType: return CameoTypeAt<AircraftTypeClass>(item.ItemIndex);
		case AbstractType::Building: case AbstractType::BuildingType: return CameoTypeAt<BuildingTypeClass>(item.ItemIndex);
		case AbstractType::Infantry: case AbstractType::InfantryType: return CameoTypeAt<InfantryTypeClass>(item.ItemIndex);
		default: return nullptr;
		}
	}

	// 0 = native SHP, 1 = extension/PCX/custom palette, 2 = invalid or not ready.
	inline const SHPStruct* CameoSource(const BuildType& item, bool ares, int& sourceKind)
	{
		using CursorSnapshot::Readable;
		sourceKind = 2;
		const SHPStruct* source = nullptr;
		if (item.ItemType == AbstractType::Special)
		{
			const auto* type = CameoTypeAt<SuperWeaponTypeClass>(item.ItemIndex);
			if (!Readable(type, sizeof(*type))) return nullptr;
			if (ares)
			{
				// Ares' own SW extension lookup, also used elsewhere in Phobos. No load or AI.
				const auto* ext = reinterpret_cast<const std::uint8_t*>(AresFunctions::SWTypeExtMap_Find(const_cast<SuperWeaponTypeClass*>(type)));
				if (!Readable(ext, 0x4D4)) return nullptr;
				if (*reinterpret_cast<void* const*>(ext + 0x494) || *reinterpret_cast<void* const*>(ext + 0x4D0))
					{ sourceKind = 1; return nullptr; }
			}
			source = type->SidebarImage;
		}
		else
		{
			const auto* type = ResolveCameoType(item);
			if (!Readable(type, sizeof(*type))) return nullptr;
			// Unknown virtual selectors are not assumed to be pure field readers.
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(type);
			if (!Readable(vtable, 0x9C) || vtable[0x98 / 4] != 0x712040) { sourceKind = 1; return nullptr; }
			if (ares)
			{
				const auto* ext = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(type) + 0x2FC);
				if (!Readable(ext, 0x250)) return nullptr;
				// Conservatively exclude either PCX binding; never show its SHP fallback.
				if (*reinterpret_cast<void* const*>(ext + 0x120) || *reinterpret_cast<void* const*>(ext + 0x228)
					|| *reinterpret_cast<void* const*>(ext + 0x24C)) { sourceKind = 1; return nullptr; }
			}
			// The verified vanilla/Ares selector preserves current infiltration/veterancy rules.
			source = type->GetCameo();
		}
		if (!Readable(source, sizeof(SHPStruct))) return nullptr;
		if (source->Type == 0xFFFF)
		{
			const auto* ref = reinterpret_cast<const SHPReference*>(source);
			if (!Readable(ref, sizeof(*ref))) return nullptr;
			const auto* rules = RulesExt::Global();
			const auto* missing = rules ? rules->MissingCameo.data() : "";
			const auto length = std::strlen(missing);
			if (Readable(ref->Filename, 11) && !_strnicmp(ref->Filename, "XXICON.SHP", 11)
				&& length >= 4 && !_stricmp(missing + length - 4, ".pcx"))
				{ sourceKind = 1; return nullptr; }
		}
		sourceKind = 0;
		return source;
	}

	inline bool GetCameoList(int tab, const BuildType*& items, int& count)
	{
		using CursorSnapshot::Readable;
		if (tab < 0 || tab >= 4) return false;
		if (AresHelper::AresBaseAddress)
		{
			static const bool known = KnownAresCameos(); // Module selection is fixed before gameplay.
			if (!known) return false;
			const auto* list = reinterpret_cast<const DynamicVectorClass<BuildType>*>(AresHelper::AresBaseAddress + 0xC1150) + tab;
			static_assert(sizeof(DynamicVectorClass<BuildType>) == 0x18);
			if (!Readable(list, sizeof(*list)) || list->Count < 0 || list->Count > list->Capacity || list->Count > 4096) return false;
			items = list->Items; count = list->Count;
		}
		else
		{
			const auto& strip = SidebarClass::Instance.Tabs[tab];
			items = strip.Cameos; count = strip.CameoCount;
			if (count < 0 || count > 75) return false;
		}
		return !count || (items && Readable(items, sizeof(BuildType) * count));
	}

	inline std::uint64_t CameoKey(const BuildType& item)
	{
		return (std::uint64_t(unsigned(item.ItemType)) << 32) | unsigned(item.ItemIndex);
	}

	inline void CaptureNativeCameoState(int index, bool darken, bool progress, bool completed, int stage)
	{
		if (!Eligible() || !LowerButtonsReady || HouseClass::IsCurrentPlayerObserver()) return;
		const int tab = SidebarClass::Instance.ActiveTabIndex;
		const BuildType* items = nullptr; int count = 0;
		if (!GetCameoList(tab, items, count) || index < 0 || index >= count) return;
		auto& states = NativeCameoStates[tab];
		const auto key = CameoKey(items[index]);
		if (states.size() >= 4096 && !states.contains(key)) return;
		// Native stage+1, not a wall-clock estimate. Invalid frames are omitted, never clamped.
		states[key] = {darken, progress && !completed && stage >= 0 && stage < INT_MAX ? stage + 1 : -1};
	}

	inline void CaptureCameoEffects(const BuildType& item, int tab, int slot, const ShapeImage::CameoSlot& p)
	{
		using ShapeImage::Effect;
		const auto found = NativeCameoStates[tab].find(CameoKey(item));
		const bool observed = found != NativeCameoStates[tab].end();
		const bool darken = observed && found->second.Darken;
		const int clockFrame = observed ? found->second.ClockFrame : -1;
		const int currentFrame = Unsorted::CurrentFrame;
		const bool flash = item.FlashEndFrame > currentFrame && currentFrame % 16 > 8;
		const bool hover = (SelectClass::Array()[tab * ShapeImage::MaxCameoSlots + slot].unknown_34 & 0xFF)
			&& !*reinterpret_cast<const unsigned char*>(0xA8ED9C);
		if (hover)
		{
			const auto rgb = *reinterpret_cast<const unsigned*>(0xB0FA1C);
			const auto& format = Current.Assets[0].Color->Values;
			unsigned packed = 0;
			for (int channel = 0; channel < 3; ++channel)
				packed |= (((rgb >> (8 * channel)) & 255u) >> (8 - format.Bits[channel])) << format.Shift[channel];
			// Four non-overlapping one-pixel edges, before darken/flash/clock.
			for (const auto& edge : {RectangleStruct{p.X,p.Y,60,1}, {p.X,p.Y+47,60,1}, {p.X,p.Y+1,1,46}, {p.X+59,p.Y+1,1,46}})
				Current.Placements.push_back({0, edge.X, edge.Y, p.ClipWidth, p.ClipHeight, Effect::Solid, packed, edge.Width, edge.Height});
		}
		const auto color = CaptureColor(FileSystem::SIDEBAR_PAL);
		bool darkenReady = false, clockReady = false;
		if ((darken || flash) && CopyAsset(FileSystem::DARKEN_SHP, color, Current.Assets[DarkenBegin + slot]))
		{
			darkenReady = true;
			if (darken) Current.Placements.push_back({DarkenBegin + slot,p.X,p.Y,p.ClipWidth,p.ClipHeight,Effect::Darken});
			if (flash) Current.Placements.push_back({DarkenBegin + slot,p.X,p.Y,p.ClipWidth,p.ClipHeight,Effect::Translucent50});
		}
		if (clockFrame >= 0 && CopyAsset(FileSystem::GCLOCK2_SHP, color, Current.Assets[ClockBegin + slot], clockFrame))
		{
			clockReady = true;
			Current.Placements.push_back({ClockBegin + slot,p.X,p.Y,p.ClipWidth,p.ClipHeight,Effect::Translucent50});
		}
		if (Trace && EffectTraceCount < 64)
		{
			const int values[] {tab, int(item.ItemType), item.ItemIndex, observed, darken, flash, hover, clockFrame, darkenReady, clockReady, int(Generation)};
			const auto hash = Hash(values, sizeof(values));
			if (hash != EffectHashes[slot])
			{
				EffectHashes[slot] = hash;
				Debug::Log("[CameoEffectGPU] sample=%u thread=%lu tab=%d slot=%d type=%d index=%d observed=%d darken=%d flash=%d hover=%d clock=%d darkenReady=%d clockReady=%d clockID=%llu\n",
					++EffectTraceCount,GetCurrentThreadId(),tab,slot,int(item.ItemType),item.ItemIndex,observed,darken,flash,hover,clockFrame,darkenReady,clockReady,
					clockReady ? Current.Assets[ClockBegin + slot].ID.Value : 0);
			}
		}
	}

	inline void CaptureCameos(int rows)
	{
		using CursorSnapshot::Readable;
		const auto& sidebar = SidebarClass::Instance;
		const int tab = sidebar.ActiveTabIndex;
		if (tab < 0 || tab >= 4 || !HouseClass::CurrentPlayer || HouseClass::IsCurrentPlayerObserver()) return;
		const auto& strip = sidebar.Tabs[tab];
		const bool ares = AresHelper::AresBaseAddress != 0;
		const BuildType* items = nullptr; int count = 0;
		const bool supported = GetCameoList(tab, items, count);
		const auto color = CaptureColor(FileSystem::CAMEO_PAL);
		const int slots = std::min(ShapeImage::MaxCameoSlots, (rows + int(strip.IsScrolling)) * 2);
		const int state[] {int(Generation), tab, count, strip.TopRowIndex, strip.IsScrolling, strip.Slid, supported, ares};
		auto hash = Hash(state, sizeof(state));
		int readyCount = 0, unsupportedCount = 0, pendingCount = 0;
		for (int slot = 0; supported && slot < slots; ++slot)
		{
			ShapeImage::CameoSlot placement;
			if (!ShapeImage::PlaceCameo(slot, strip.TopRowIndex, count, strip.Location.X, strip.Location.Y,
				SelectClass::CameoPitchX(), SelectClass::CameoPitchY(), strip.IsScrolling, strip.Slid,
				DSurface::SidebarBounds.X, Current.Width, Current.Height, placement)) continue;
			if (!items || !Readable(items + placement.Item, sizeof(BuildType))) { ++pendingCount; continue; }
			const auto item = items[placement.Item];
			int kind = 2;
			const auto* source = CameoSource(item, ares, kind);
			auto& asset = Current.Assets[CameoBegin + slot];
			asset = {};
			const bool ready = source && CopyAsset(source, color, asset);
			if (ready)
			{
				++readyCount;
				Current.Placements.push_back({CameoBegin + slot, placement.X, placement.Y, placement.ClipWidth, placement.ClipHeight});
			}
			else if (kind == 1) ++unsupportedCount; else ++pendingCount;
			// Unsupported base providers must not leave floating state masks.
			if (ready) CaptureCameoEffects(item, tab, slot, placement);
			const int identity[] {item.ItemIndex, int(item.ItemType), kind, placement.X, placement.Y, placement.ClipWidth, placement.ClipHeight};
			hash = Hash(identity, sizeof(identity), hash);
			hash = Hash(&asset.ID.Value, sizeof(asset.ID.Value), hash);
			if (Trace && CameoItemTraceCount < 64)
			{
				auto itemHash = Hash(identity, sizeof(identity));
				itemHash = Hash(&asset.ID.Value, sizeof(asset.ID.Value), itemHash);
				itemHash = Hash(&Generation, sizeof(Generation), itemHash);
				if (itemHash != CameoItemHashes[slot])
				{
					CameoItemHashes[slot] = itemHash;
					Debug::Log("[SidebarCameoGPU] itemSample=%u generation=%u tab=%d slot=%d type=%d index=%d sourceKind=%d ready=%d shape=%p data=%p frameID=%llu pos=%d,%d offset=%d,%d size=%dx%d clip=%dx%d\n",
						++CameoItemTraceCount, Generation, tab, slot, int(item.ItemType), item.ItemIndex, kind, ready,
						source, reinterpret_cast<const void*>(asset.DataIdentity), asset.ID.Value, placement.X, placement.Y,
						asset.Left, asset.Top, asset.Width, asset.Height, placement.ClipWidth, placement.ClipHeight);
				}
			}
		}
		const auto palette = ColorHash(color);
		hash = Hash(&palette, sizeof(palette), hash);
		if (Trace && CameoTraceCount < 64 && hash != CameoTraceHash)
		{
			CameoTraceHash = hash;
			Debug::Log("[SidebarCameoGPU] sample=%u generation=%u thread=%lu tab=%d count=%d top=%d scroll=%d slid=%d slots=%d provider=%s supported=%d ready=%d unsupported=%d pending=%d palette=%016llX\n",
				++CameoTraceCount, Generation, GetCurrentThreadId(), tab, count, strip.TopRowIndex, strip.IsScrolling, strip.Slid,
				slots, ares ? "ares-3.0p1" : "vanilla", supported, readyCount, unsupportedCount, pendingCount, palette);
		}
	}

	inline void CaptureAfterDraw()
	{
		Current.Visible = false;
		Current.Placements.clear();
		CapturedColors.clear();
		if (!Eligible() || !LowerButtonsReady)
			return;
		const auto* surface = DSurface::Sidebar;
		const auto* convert = FileSystem::SIDEBAR_PAL;
		const auto color = CaptureColor(convert);
		if (!CursorSnapshot::Readable(surface, sizeof(*surface)) || !color)
			return;
		Current.Width = surface->Width;
		Current.Height = surface->Height;
		Current.OriginX = GameOptionsClass::Instance.SidebarMode ? DSurface::ViewBounds.Width : 0;
		if (Current.Width <= 0 || Current.Height <= 0 || Current.Width > MaxCanvasDimension
			|| Current.Height > MaxCanvasDimension || !ScenarioClass::Instance)
			return;
		const int side = ScenarioClass::Instance->PlayerSideIndex;
		const int startY = *reinterpret_cast<const int*>(0xB0B4F8);
		const auto bounds = DSurface::SidebarBounds;
		const int repeats = ShapeImage::RepeatCount(bounds.Height, startY, bounds.Y, side);
		if (repeats > MaxRepeats || std::abs(std::int64_t(startY)) > MaxCanvasDimension)
			return;
		bool ready = true;
		const bool tracing = Trace && TraceCount < 64;
		std::uint64_t hash = tracing ? ColorHash(color) : 0;
		const int layout[] {Current.OriginX, Current.Width, Current.Height, bounds.Y, startY, repeats, side,
			int(SidebarClass::Instance.unknown_5398), int(Generation)};
		if (tracing) hash = Hash(layout, sizeof(layout), hash);
		for (size_t i = 0; i < BackgroundCount; ++i)
		{
			const auto* source = *reinterpret_cast<SHPStruct* const*>(ResourceSlots[i]);
			const bool copied = CopyAsset(source, color, Current.Assets[i]);
			ready &= copied;
			const auto& asset = Current.Assets[i];
			if (tracing)
			{
				hash = Hash(&source, sizeof(source), hash);
				const int geometry[] {asset.Left, asset.Top, asset.Width, asset.Height, asset.ShapeHeight};
				hash = Hash(geometry, sizeof(geometry), hash);
				hash = Hash(&asset.ID.Value, sizeof(asset.ID.Value), hash);
			}
		}
		const auto* self = reinterpret_cast<const std::uint8_t*>(&SidebarClass::Instance);
		const int topX = *reinterpret_cast<const int*>(self + 0x11E4);
		const int topY = *reinterpret_cast<const int*>(self + 0x11E8);
		if (tracing)
		{
			hash = Hash(&topX, sizeof(topX), hash);
			hash = Hash(&topY, sizeof(topY), hash);
		}
		if (ready && Current.Assets[2].ShapeHeight > 0 && Current.Assets[2].ShapeHeight <= ShapeImage::MaxDimension
			&& Current.Assets[3].ShapeHeight > 0 && Current.Assets[3].ShapeHeight <= ShapeImage::MaxDimension)
		{
			Current.Placements.push_back({0, 0, 0});
			Current.Placements.push_back({1, 0, bounds.Y});
			int y = startY;
			for (int i = 0; i < repeats; ++i, y += Current.Assets[2].ShapeHeight)
				Current.Placements.push_back({2, 0, y});
			Current.Placements.push_back({3, 0, y});
			Current.Placements.push_back({4, 0, y + Current.Assets[3].ShapeHeight});
			CaptureButtons(0, 8);
			CaptureCameos(repeats);
			Current.Placements.push_back({5, topX, topY});
			CaptureButtons(8, ButtonCount);
			// Native Radar may repaint repair then sell after its contents.
			for (const size_t i : {size_t(1), size_t(0)})
			{
				const auto found = std::find_if(Current.Placements.begin(), Current.Placements.end(),
					[i](const Placement& p) { return p.AssetIndex == BackgroundCount + i; });
				if (found != Current.Placements.end())
				{
					const auto placement = *found;
					Current.Placements.push_back(placement);
				}
			}
			// An alias observed later in this capture may invalidate an earlier resource.
			Current.Visible = std::all_of(Current.Assets.begin(), Current.Assets.begin() + BackgroundCount,
				[](const Asset& asset) { return ShapeFrameSource::Frames.Resolve(asset.ID) != nullptr; });
			Current.Placements.erase(std::remove_if(Current.Placements.begin(), Current.Placements.end(),
				[](const Placement& p) { return !ShapeFrameSource::Frames.Resolve(Current.Assets[p.AssetIndex].ID); }), Current.Placements.end());
		}
		++CaptureCount;
		if (Trace && CacheTraceCount < 64 && (CaptureCount == 1 || CaptureCount % 300 == 0))
		{
			const auto& stats = ShapeFrameSource::Frames.GetStats();
			++CacheTraceCount;
			Debug::Log("[FrameCache] captures=%llu hits=%llu misses=%llu attempts=%llu decoded=%llu decodedDelta=%llu bytes=%zu frames=%zu resources=%zu invalidations=%llu invalid=%llu budgetRejected=%llu\n",
				CaptureCount, stats.Hits, stats.Misses, stats.DecodeAttempts, stats.Decoded, stats.Decoded - LastDecoded,
				stats.IndexBytes, stats.Frames, stats.Resources, stats.Invalidations, stats.Invalid, stats.BudgetRejected);
			LastDecoded = stats.Decoded;
		}
		if (Trace && hash != LastTraceHash && TraceCount < 64)
		{
			LastTraceHash = hash;
			++TraceCount;
			Debug::Log("[SidebarGPU] sample=%u generation=%u ready=%d side=%d origin=%d size=%dx%d y0=%d repeats=%d top=%d,%d overlay=%lu palette=%016llX\n",
				TraceCount, Generation, Current.Visible, side, Current.OriginX, Current.Width, Current.Height,
				startY, repeats, topX, topY, SidebarClass::Instance.unknown_5398, ColorHash(color));
			for (size_t i = 0; i < BackgroundCount && TraceCount < 64; ++i)
			{
				++TraceCount;
				const auto& asset = Current.Assets[i];
				Debug::Log("[SidebarGPU] asset=%s object=%p data=%p frame=0 flags=%u offset=%d,%d size=%dx%d shapeHeight=%d frameID=%llu\n",
					Names[i], *reinterpret_cast<void* const*>(ResourceSlots[i]), reinterpret_cast<void*>(asset.DataIdentity), asset.Flags,
					asset.Left, asset.Top, asset.Width, asset.Height, asset.ShapeHeight, asset.ID.Value);
			}
		}
	}
}
