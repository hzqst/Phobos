#pragma once

#include "SidebarSnapshot.h"
#include <MouseClass.h>
#include <cstdio>
#include <string>

// The bottom command bar that TabClass::Draw (6D0A20) paints into Composite before
// it calls the sidebar draw. Captured in the same 6A70AE callback from the bar's
// persistent native state; the GPU never decides when the native bar repaints.
namespace CommandBarSnapshot
{
	using SidebarSnapshot::Asset;
	using SidebarSnapshot::Placement;
	enum Decoration : size_t { Spacer, LeftCap, Background, RightCap, DecorationCount };
	constexpr size_t CommandCount = 25;
	constexpr size_t ThumbAsset = DecorationCount;
	constexpr size_t CommandBegin = ThumbAsset + 1;
	constexpr size_t AssetCount = CommandBegin + CommandCount;
	constexpr int MaxBackgrounds = 256;
	// LSPACER, LENDCAP, BTTNBKGD and RENDCAP owned by the UI art loader (72FA10).
	constexpr std::uintptr_t DecorationSlots[DecorationCount] = {0xB0FA3C, 0xB0FA90, 0xB0FAA8, 0xB0FABC};
	constexpr const char* Names[DecorationCount] = {"LSPACER", "LENDCAP", "BTTNBKGD", "RENDCAP"};
	// Rectangles allocated by 72FC60: the bar, the expanded and collapsed left caps,
	// the first background cell and the right cap; then the background count.
	constexpr std::uintptr_t BarRect = 0xB0FC58, ExpandedCapRect = 0xB0FC60, CollapsedCapRect = 0xB0FC64,
		FirstCellRect = 0xB0FC68, RightCapRect = 0xB0FC6C, BackgroundCount = 0xB0F9E4;
	// Command k -> display slot j; object j; Button%02d.SHP for command k.
	constexpr std::uintptr_t CommandSlots = 0xB0CB78, CommandObjects = 0xB0C1C0, CommandShapes = 0xB0C148;
	// LENDCAP controls drawn over the caps: ID 240 when expanded, ID 241 when collapsed.
	constexpr std::uintptr_t ExpandedThumb = 0xB0CCB0, CollapsedThumb = 0xB0CC40;
	// Surfaces the bar was painted on, most recent first, and their count.
	constexpr std::uintptr_t DrawnSurfaces = 0xB0CC00, DrawnSurfaceCount = 0xB0CD44;
	static_assert(offsetof(TabClass, ThumbActive) == 0x5544);
	static_assert(offsetof(TabClass, MissionTimerPinged) == 0x5545);
	static_assert(offsetof(TabClass, unknown_byte_5546) == 0x5546);

	struct Frame
	{
		bool Visible = false;
		int OriginX = 0, Width = 0, Height = 0;
		std::array<Asset, AssetCount> Assets;
		std::vector<Placement> Placements;
	};
	inline Frame Current;
	inline unsigned TraceCount = 0;
	inline std::uint64_t LastTraceHash = 0;

	inline void Invalidate()
	{
		Current.Visible = false;
		Current.Placements.clear();
		LastTraceHash = 0;
	}

	inline void InitializeTrace()
	{
		TraceCount = 0;
		LastTraceHash = 0;
	}

	inline bool Eligible()
	{
		return *reinterpret_cast<const bool*>(0xA8E9A0) && *reinterpret_cast<const bool*>(0xA8ED5C)
			&& !Unsorted::ArmageddonMode && ScenarioClass::Instance;
	}

	inline const RectangleStruct* ReadRect(std::uintptr_t slot)
	{
		const auto* rect = *reinterpret_cast<const RectangleStruct* const*>(slot);
		return CursorSnapshot::Readable(rect, sizeof(*rect)) ? rect : nullptr;
	}

	// The released Button%02d.SHP data is freed directly (6D0270) and rebound later
	// (6D0FD0); only a pointer into the current set is safe to read.
	inline bool CurrentCommandShape(const SHPStruct* shape)
	{
		const auto* shapes = reinterpret_cast<SHPStruct* const*>(CommandShapes);
		return shape && std::find(shapes, shapes + CommandCount, shape) != shapes + CommandCount;
	}

	struct ButtonResult { bool Ready = false; int Frame = -1; int X = 0, Y = 0; const char* Reason = "absent"; };

	// ShapeButton::Draw (69DEB0) with force: the frame, X/Y plus DrawPosition, the
	// whole target surface as clip. Sidebar-surface and alpha buttons are not supported.
	inline ButtonResult CaptureButton(const ShapeButtonClass* button, bool bound, Asset& asset)
	{
		ButtonResult result;
		if (!button->ShapeData) { asset = {}; return result; }
		result.Frame = ButtonImage::SelectFrame(button->Disabled, button->UseFlash, button->IsToFlash, button->IsOn, button->IsPressed);
		// Preserve x86 addition without signed overflow.
		result.X = std::int32_t(std::uint32_t(button->X) + std::uint32_t(button->DrawPosition.X));
		result.Y = std::int32_t(std::uint32_t(button->Y) + std::uint32_t(button->DrawPosition.Y));
		const auto color = SidebarSnapshot::CaptureColor(button->Drawer);
		if (!bound) result.Reason = "unbound";
		else if (button->UseSidebarSurface || button->IsAlpha) result.Reason = "target";
		else if (!color) result.Reason = "palette";
		else if (!SidebarSnapshot::CopyAsset(button->ShapeData, color, asset, result.Frame)) result.Reason = "frame";
		else { result.Ready = true; result.Reason = "ready"; return result; }
		asset = {};
		return result;
	}

	inline void CaptureAfterDraw()
	{
		using CursorSnapshot::Readable;
		Current.Visible = false;
		Current.Placements.clear();
		const auto& tab = TabClass::Instance;
		const auto* composite = DSurface::Composite;
		const char* reason = nullptr;
		const auto* bar = ReadRect(BarRect);
		const auto* expandedCap = ReadRect(ExpandedCapRect);
		const auto* collapsedCap = ReadRect(CollapsedCapRect);
		const auto* firstCell = ReadRect(FirstCellRect);
		const auto* rightCap = ReadRect(RightCapRect);
		const int backgrounds = *reinterpret_cast<const int*>(BackgroundCount);
		const bool expanded = tab.ThumbActive;
		const bool drawn = tab.unknown_byte_5546;
		const auto* head = *reinterpret_cast<const DSurface* const*>(DrawnSurfaces);
		const int surfaces = *reinterpret_cast<const int*>(DrawnSurfaceCount);
		const auto color = SidebarSnapshot::CaptureColor(FileSystem::SIDEBAR_PAL);
		if (!Eligible()) reason = "ineligible";
		else if (!Readable(composite, sizeof(*composite)) || composite->Width <= 0 || composite->Height <= 0
			|| composite->Width > SidebarSnapshot::MaxCanvasDimension || composite->Height > SidebarSnapshot::MaxCanvasDimension)
			reason = "surface";
		// A repaint this frame went to the surface at the head of the list; normally Composite.
		else if (drawn && (surfaces <= 0 || head != composite)) reason = "target";
		else if (!bar || !expandedCap || !collapsedCap || !firstCell || !rightCap) reason = "layout";
		else if (backgrounds > MaxBackgrounds) reason = "backgrounds";
		else if (!color) reason = "palette";
		std::array<ButtonResult, CommandCount> commands {};
		ButtonResult thumb;
		const int* slots = reinterpret_cast<const int*>(CommandSlots);
		if (!reason)
		{
			Current.Width = composite->Width;
			Current.Height = composite->Height;
			const auto& view = DSurface::ViewBounds;
			const auto& sidebar = DSurface::SidebarBounds;
			Current.OriginX = Tooltip::CanvasOrigin(Tooltip::Target::Composite, GameOptionsClass::Instance.SidebarMode,
				view.X + view.Width, sidebar.X + sidebar.Width);
			bool ready = true;
			for (size_t i = 0; i < DecorationCount; ++i)
			{
				const auto* source = *reinterpret_cast<SHPStruct* const*>(DecorationSlots[i]);
				const bool needed = i != Background || (expanded && backgrounds > 0);
				if (!needed) { Current.Assets[i] = {}; continue; }
				if (!SidebarSnapshot::CopyAsset(source, color, Current.Assets[i])) { Current.Assets[i] = {}; ready = false; }
			}
			if (!ready) reason = "decoration";
			else
			{
				// Native order. The spacer clip is full(C) with only its width replaced.
				Current.Placements.push_back({Spacer, bar->X, bar->Y, bar->Width});
				const auto* cap = expanded ? expandedCap : collapsedCap;
				Current.Placements.push_back({LeftCap, cap->X, cap->Y});
				// The right cap keeps the Y left by the previous layer and rewrites only X.
				int y = cap->Y;
				if (expanded)
				{
					y = bar->Y;
					for (int i = 0; i < backgrounds; ++i)
						Current.Placements.push_back({Background, int(std::int64_t(firstCell->X) + std::int64_t(i) * firstCell->Width), y});
					// Iterates command kinds, not objects; a repeated slot draws its object again.
					for (size_t k = 0; k < CommandCount; ++k)
					{
						const int j = slots[k];
						if (j < 0 || j >= int(CommandCount)) continue;
						const auto* button = reinterpret_cast<const ShapeButtonClass*>(CommandObjects + j * sizeof(ShapeButtonClass));
						commands[k] = CaptureButton(button, CurrentCommandShape(button->ShapeData), Current.Assets[CommandBegin + k]);
						if (commands[k].Ready)
							Current.Placements.push_back({CommandBegin + k, commands[k].X, commands[k].Y});
					}
				}
				Current.Placements.push_back({RightCap, rightCap->X, y});
				const auto* control = reinterpret_cast<const ShapeButtonClass*>(expanded ? ExpandedThumb : CollapsedThumb);
				const auto* leftCap = *reinterpret_cast<SHPStruct* const*>(DecorationSlots[LeftCap]);
				thumb = CaptureButton(control, control->ShapeData == leftCap, Current.Assets[ThumbAsset]);
				if (thumb.Ready)
					Current.Placements.push_back({ThumbAsset, thumb.X, thumb.Y});
				Current.Placements.erase(std::remove_if(Current.Placements.begin(), Current.Placements.end(),
					[](const Placement& p) { return !ShapeFrameSource::Frames.Resolve(Current.Assets[p.AssetIndex].ID); }),
					Current.Placements.end());
				constexpr size_t required[] {Spacer, LeftCap, RightCap};
				Current.Visible = std::all_of(std::begin(required), std::end(required),
					[](size_t i) { return ShapeFrameSource::Frames.Resolve(Current.Assets[i].ID) != nullptr; });
				if (!Current.Visible) reason = "alias";
			}
		}
		if (!Current.Visible) Current.Placements.clear();
		else reason = "ready";
		if (!SidebarSnapshot::Trace || TraceCount >= 64) return;
		const int state[] {Current.Visible, expanded, tab.MissionTimerPinged, backgrounds, Current.OriginX, Current.Width, Current.Height,
			bar ? bar->X : 0, bar ? bar->Y : 0, bar ? bar->Width : 0, bar ? bar->Height : 0, rightCap ? rightCap->X : 0,
			firstCell ? firstCell->X : 0, firstCell ? firstCell->Width : 0, thumb.Frame, thumb.X, thumb.Y};
		auto hash = SidebarSnapshot::Hash(state, sizeof(state));
		hash = SidebarSnapshot::Hash(&reason, sizeof(reason), hash);
		hash = SidebarSnapshot::Hash(&thumb.Reason, sizeof(thumb.Reason), hash);
		hash = SidebarSnapshot::Hash(slots, sizeof(int) * CommandCount, hash);
		for (const auto& command : commands)
		{
			const int values[] {command.Frame, command.X, command.Y};
			hash = SidebarSnapshot::Hash(values, sizeof(values), hash);
			hash = SidebarSnapshot::Hash(&command.Reason, sizeof(command.Reason), hash);
		}
		for (const auto& asset : Current.Assets)
			hash = SidebarSnapshot::Hash(&asset.ID.Value, sizeof(asset.ID.Value), hash);
		const auto palette = SidebarSnapshot::ColorHash(color);
		hash = SidebarSnapshot::Hash(&palette, sizeof(palette), hash);
		// The repaint flag and surface list change on every mouse move over the bar; logged only.
		if (hash == LastTraceHash) return;
		LastTraceHash = hash;
		std::string buttons;
		char item[96];
		for (size_t k = 0; k < CommandCount; ++k)
		{
			if (commands[k].Frame < 0) continue;
			const auto* button = reinterpret_cast<const ShapeButtonClass*>(CommandObjects + slots[k] * sizeof(ShapeButtonClass));
			const auto& asset = Current.Assets[CommandBegin + k];
			std::snprintf(item, sizeof(item), " %zu:%d:%d:%s:f%d:%d,%d:%d,%d,%dx%d:%llu", k, slots[k], button->ID, commands[k].Reason,
				commands[k].Frame, commands[k].X, commands[k].Y, asset.Left, asset.Top, asset.Width, asset.Height, asset.ID.Value);
			buttons += item;
		}
		Debug::Log("[CommandBarGPU] sample=%u ready=%d reason=%s expanded=%d pinged=%d drawn=%d surfaces=%d head=%p composite=%p origin=%d size=%dx%d bar=%d,%d,%d,%d cells=%d first=%d,%d right=%d thumb=%s:f%d:%d,%d palette=%016llX\n",
			++TraceCount, Current.Visible, reason, expanded, tab.MissionTimerPinged, drawn, surfaces, head, composite, Current.OriginX,
			Current.Width, Current.Height, bar ? bar->X : 0, bar ? bar->Y : 0, bar ? bar->Width : 0, bar ? bar->Height : 0, backgrounds,
			firstCell ? firstCell->X : 0, firstCell ? firstCell->Width : 0, rightCap ? rightCap->X : 0, thumb.Reason, thumb.Frame, thumb.X, thumb.Y, palette);
		for (size_t i = 0; i < DecorationCount && TraceCount < 64; ++i)
		{
			const auto& asset = Current.Assets[i];
			if (!asset.ID.Value) continue;
			++TraceCount;
			Debug::Log("[CommandBarGPU] asset=%s object=%p data=%p offset=%d,%d size=%dx%d frameID=%llu\n", Names[i],
				*reinterpret_cast<void* const*>(DecorationSlots[i]), reinterpret_cast<void*>(asset.DataIdentity),
				asset.Left, asset.Top, asset.Width, asset.Height, asset.ID.Value);
		}
		if (!buttons.empty() && TraceCount < 64)
		{
			++TraceCount;
			Debug::Log("[CommandBarGPU] commands=%s\n", buttons.c_str());
		}
	}
}
