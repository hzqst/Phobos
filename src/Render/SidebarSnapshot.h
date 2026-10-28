#pragma once

#include "CursorSnapshot.h"
#include "ShapeImage.h"
#include "ButtonImage.h"
#include <ShapeButtonClass.h>
#include <SidebarClass.h>
#include <ScenarioClass.h>
#include <GameOptionsClass.h>
#include <Surface.h>
#include <Utilities/Debug.h>

// Only CaptureAfterDraw reads engine-owned resources. Present/timer uses owned pixels.
namespace SidebarSnapshot
{
	constexpr size_t BackgroundCount = 6;
	constexpr size_t ButtonCount = 10;
	constexpr size_t AssetCount = BackgroundCount + ButtonCount;
	constexpr int MaxCanvasDimension = 4096;
	constexpr int MaxRepeats = 128;
	constexpr size_t MaxEncodedBytes = 16 * 1024 * 1024;
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
		std::vector<std::uint32_t> Pixels;
	};
	struct Placement { size_t AssetIndex; int X, Y; };
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

	inline bool CopyAsset(const SHPStruct* source, const std::uint16_t* palette, Asset& asset, int frame = 0)
	{
		using CursorSnapshot::Readable;
		asset.Pixels.clear();
		asset.Left = asset.Top = asset.Width = asset.Height = asset.ShapeHeight = 0;
		asset.Flags = 0;
		asset.DataIdentity = 0;
		if (!Readable(source, sizeof(*source)))
			return false;
		// Use the same header height the native SIDE2/SIDE3 placement reads.
		asset.ShapeHeight = source->Height;
		if (source->IsReference())
		{
			const auto* reference = source->AsReference();
			if (!Readable(reference, sizeof(*reference)) || !reference->Loaded)
				return false; // Never trigger loading or borrow the engine's scratch buffer.
			source = reference->Data;
		}
		if (!Readable(source, sizeof(*source)) || source->IsReference() || frame < 0 || frame >= source->Frames)
			return false;
		const size_t headers = sizeof(SHPStruct) + size_t(source->Frames) * sizeof(SHPFrame);
		if (!Readable(source, headers))
			return false;
		const auto header = source->AsFile()->GetFrameHeader(frame);
		if (header.Width <= 0 || header.Height <= 0 || header.Width > ShapeImage::MaxDimension
			|| header.Height > ShapeImage::MaxDimension || header.Offset < 0 || size_t(header.Offset) < headers)
			return false;
		const auto base = reinterpret_cast<std::uintptr_t>(source);
		if (size_t(header.Offset) > UINTPTR_MAX - base)
			return false;
		const auto* encoded = reinterpret_cast<const std::uint8_t*>(base + header.Offset);
		size_t bound = std::min(MaxEncodedBytes, size_t(UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(encoded)));
		for (int i = 0; i < source->Frames; ++i)
		{
			const int next = source->AsFile()->GetFrameHeader(i).Offset;
			if (next > header.Offset)
				bound = std::min(bound, size_t(next - header.Offset));
		}
		size_t bytes = size_t(header.Width) * header.Height;
		if (header.Flags & 2)
		{
			bytes = 0;
			for (int y = 0; y < header.Height; ++y)
			{
				if (bytes > bound || bound - bytes < 2 || !Readable(encoded + bytes, 2))
					return false;
				const size_t length = encoded[bytes] | (size_t(encoded[bytes + 1]) << 8);
				if (length < 2 || length > bound - bytes || !Readable(encoded + bytes, length))
					return false;
				bytes += length;
			}
		}
		if (bytes > bound || !Readable(encoded, bytes))
			return false;
		std::vector<std::uint8_t> indices;
		if (!ShapeImage::Decode({encoded, bytes}, header.Width, header.Height, (header.Flags & 2) != 0, indices))
			return false;
		asset.Left = header.Left; asset.Top = header.Top;
		asset.Width = header.Width; asset.Height = header.Height; asset.Flags = header.Flags;
		asset.DataIdentity = base;
		asset.Pixels.resize(indices.size());
		for (size_t i = 0; i < indices.size(); ++i)
			asset.Pixels[i] = CursorImage::PackPixel(indices[i], palette[indices[i]]);
		return true;
	}

	inline void CaptureButtons(size_t begin, size_t end)
	{
		for (size_t i = begin; i < end; ++i)
		{
			auto& asset = Current.Assets[BackgroundCount + i];
			asset.Pixels.clear();
			if (!(i < 8 ? LowerButtonsReady : RadarButtonsReady)) continue;
			const auto* button = reinterpret_cast<const ShapeButtonClass*>(ButtonObjects[i]);
			const auto* drawer = button->Drawer;
			const int frame = ButtonImage::SelectFrame(button->Disabled, button->UseFlash,
				button->IsToFlash, button->IsOn, button->IsPressed);
			// Preserve x86 addition without signed overflow. Do not recreate native layout.
			const int x = std::int32_t(std::uint32_t(button->X) + std::uint32_t(button->DrawPosition.X));
			const int y = std::int32_t(std::uint32_t(button->Y) + std::uint32_t(button->DrawPosition.Y));
			std::array<std::uint16_t, 256> palette {};
			const bool supported = button->UseSidebarSurface && !button->IsAlpha;
			bool ready = supported && CursorSnapshot::Readable(drawer, sizeof(*drawer))
				&& drawer->BytesPerPixel == 2 && CursorSnapshot::Readable(drawer->PaletteData, sizeof(palette));
			if (ready)
			{
				std::memcpy(palette.data(), drawer->PaletteData, sizeof(palette));
				ready = CopyAsset(button->ShapeData, palette.data(), asset, frame);
			}
			if (ready) Current.Placements.push_back({BackgroundCount + i, x, y});
			if (Trace && ButtonTraceCount < 64)
			{
				const int state[] {int(Generation), button->ID, button->Disabled, button->UseFlash,
					button->IsToFlash, button->IsOn, button->IsPressed, frame, x, y, ready, supported,
					Current.OriginX, Current.Width, Current.Height, asset.Left, asset.Top, asset.Width, asset.Height};
				auto hash = Hash(state, sizeof(state));
				hash = Hash(&button->ShapeData, sizeof(button->ShapeData), hash);
				hash = Hash(&drawer, sizeof(drawer), hash);
				hash = Hash(palette.data(), sizeof(palette), hash);
				hash = Hash(asset.Pixels.data(), asset.Pixels.size() * sizeof(std::uint32_t), hash);
				if (hash != ButtonTraceHashes[i])
				{
					ButtonTraceHashes[i] = hash;
					++ButtonTraceCount;
					Debug::Log("[SidebarButtonGPU] sample=%u generation=%u thread=%lu id=%d ready=%d supported=%d state=%d,%d,%d,%d,%d frame=%d shape=%p data=%p drawer=%p palette=%016llX pos=%d,%d offset=%d,%d size=%dx%d flags=%u origin=%d clip=%dx%d\n",
						ButtonTraceCount, Generation, GetCurrentThreadId(), button->ID, ready, supported,
						button->Disabled, button->UseFlash, button->IsToFlash, button->IsOn, button->IsPressed, frame,
						button->ShapeData, reinterpret_cast<void*>(asset.DataIdentity), drawer, Hash(palette.data(), sizeof(palette)),
						x, y, asset.Left, asset.Top, asset.Width, asset.Height, asset.Flags, Current.OriginX, Current.Width, Current.Height);
				}
			}
		}
	}

	inline void CaptureAfterDraw()
	{
		Current.Visible = false;
		Current.Placements.clear();
		if (!Eligible() || !LowerButtonsReady)
			return;
		const auto* surface = DSurface::Sidebar;
		const auto* convert = FileSystem::SIDEBAR_PAL;
		if (!CursorSnapshot::Readable(surface, sizeof(*surface))
			|| !CursorSnapshot::Readable(convert, sizeof(*convert)) || convert->BytesPerPixel != 2
			|| !CursorSnapshot::Readable(convert->PaletteData, 512))
			return;
		Current.Width = surface->Width;
		Current.Height = surface->Height;
		Current.OriginX = GameOptionsClass::Instance.SidebarMode ? DSurface::ViewBounds.Width : 0;
		if (Current.Width <= 0 || Current.Height <= 0 || Current.Width > MaxCanvasDimension
			|| Current.Height > MaxCanvasDimension || !ScenarioClass::Instance)
			return;
		std::array<std::uint16_t, 256> palette;
		std::memcpy(palette.data(), convert->PaletteData, sizeof(palette));
		const int side = ScenarioClass::Instance->PlayerSideIndex;
		const int startY = *reinterpret_cast<const int*>(0xB0B4F8);
		const auto bounds = DSurface::SidebarBounds;
		const int repeats = ShapeImage::RepeatCount(bounds.Height, startY, bounds.Y, side);
		if (repeats > MaxRepeats || std::abs(std::int64_t(startY)) > MaxCanvasDimension)
			return;
		bool ready = true;
		const bool tracing = Trace && TraceCount < 64;
		std::uint64_t hash = tracing ? Hash(palette.data(), sizeof(palette)) : 0;
		const int layout[] {Current.OriginX, Current.Width, Current.Height, bounds.Y, startY, repeats, side,
			int(SidebarClass::Instance.unknown_5398), int(Generation)};
		if (tracing) hash = Hash(layout, sizeof(layout), hash);
		for (size_t i = 0; i < BackgroundCount; ++i)
		{
			const auto* source = *reinterpret_cast<SHPStruct* const*>(ResourceSlots[i]);
			const bool copied = CopyAsset(source, palette.data(), Current.Assets[i]);
			ready &= copied;
			const auto& asset = Current.Assets[i];
			if (tracing)
			{
				hash = Hash(&source, sizeof(source), hash);
				const int geometry[] {asset.Left, asset.Top, asset.Width, asset.Height, asset.ShapeHeight};
				hash = Hash(geometry, sizeof(geometry), hash);
				hash = Hash(asset.Pixels.data(), asset.Pixels.size() * sizeof(std::uint32_t), hash);
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
			// Future Strip/Power layers belong here, before TOP and the Radar buttons.
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
			Current.Visible = true;
		}
		if (Trace && hash != LastTraceHash && TraceCount < 64)
		{
			LastTraceHash = hash;
			++TraceCount;
			Debug::Log("[SidebarGPU] sample=%u generation=%u ready=%d side=%d origin=%d size=%dx%d y0=%d repeats=%d top=%d,%d overlay=%lu palette=%016llX\n",
				TraceCount, Generation, Current.Visible, side, Current.OriginX, Current.Width, Current.Height,
				startY, repeats, topX, topY, SidebarClass::Instance.unknown_5398, Hash(palette.data(), sizeof(palette)));
			for (size_t i = 0; i < BackgroundCount && TraceCount < 64; ++i)
			{
				++TraceCount;
				const auto& asset = Current.Assets[i];
				Debug::Log("[SidebarGPU] asset=%s object=%p data=%p frame=0 flags=%u offset=%d,%d size=%dx%d shapeHeight=%d pixels=%zu\n",
					Names[i], *reinterpret_cast<void* const*>(ResourceSlots[i]), reinterpret_cast<void*>(asset.DataIdentity), asset.Flags,
					asset.Left, asset.Top, asset.Width, asset.Height, asset.ShapeHeight, asset.Pixels.size());
			}
		}
	}
}
