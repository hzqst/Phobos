#pragma once

#include "ABufferImage.h"
#include "CursorSnapshot.h"
#include "SidebarSnapshot.h"
#include <AlphaShapeClass.h>
#include <CellClass.h>
#include <Drawing.h>
#include <FileFormats/SHP.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <TacticalClass.h>
#include <Unsorted.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

// The shroud and fog the terrain pass writes into the ABuffer (6D3660 -> 6D71E0 -> 4801F0) before
// 547CF0 reads it, rebuilt from the current cell flags as a full redraw of ViewBounds. Stage C4.1:
// built for the trace only and checked in the 4F4583 callback against the native ABuffer, which
// still holds what the tiles read while no alpha shape or later Alpha writer has touched it.
namespace ABufferSnapshot
{
	using namespace ABufferImage;
	constexpr std::uintptr_t OcclusionTable = 0x7F4194;
	// 47EFE0 binds its shroud and fog shapes once, 47F250 its own fog shape.
	constexpr std::uintptr_t OverwriteShapesReady = 0x89E7C5, ShroudShape = 0x89E794, FogShape = 0x89E790;
	constexpr std::uintptr_t DarkenShapeReady = 0x89E7C6, DarkenShape = 0x89E760;
	// 47EFE0's fast loop compares the source byte with this DWORD, its ring loop with 0xFE.
	constexpr std::uintptr_t TransparentValue = 0x7E4EB8;
	constexpr std::uint32_t FogOfWarFlag = 0x1000;
	constexpr int Passes = 2;
	static_assert(offsetof(CellClass, AltFlags) == 0x12C);
	static_assert(offsetof(CellClass, Flags) == 0x140);
	static_assert(offsetof(HouseClass, Defeated) == 0x1F5);
	// 568300 reads the legal area from MapRect.Width and Height at 0xF4 and 0xF8.
	static_assert(offsetof(MapClass, MapRect) + offsetof(RectangleStruct, Width) == 0xF4);
	static_assert(offsetof(ABuffer, BufferPosition) == 0x10 && offsetof(ABuffer, Width) == 0x28);
	static_assert(sizeof(SHPStruct) == 8 && sizeof(SHPFrame) == 0x18);

	// One cell 4801F0 draws. Frame[1] is -1 where the darken pass does not run. For the trace: the
	// neighbour mask, the cell's own flags and 6D8700's result of each pass, and the results 4801F0
	// stored in Visibility and Foggedness when it last drew the cell.
	struct Write
	{
		int X, Y;
		Rect Clip;
		int CellX, CellY;
		int Frame[Passes], Mask[Passes], Occlusion[Passes], Stored[Passes];
		std::uint32_t Flags[Passes];
	};

	// A copied frame of a shape: the header's bounds and its raw bytes.
	struct FrameCopy
	{
		bool Tried = false, Ready = false;
		int Left = 0, Top = 0, Width = 0, Height = 0;
		std::vector<std::uint8_t> Pixels;
	};

	struct Shape
	{
		// The pointer the global holds and the file data behind it.
		const SHPStruct* Pointer = nullptr;
		const SHPStruct* Data = nullptr;
		std::vector<FrameCopy> Frames;
	};

	struct Snapshot
	{
		const char* Reason = "none";
		RectangleStruct View {};
		bool Fog = false, Defeated = false, Window = false;
		std::array<Shape, Passes> Shapes;
		std::vector<Write> Writes;
		// Covered and Uncovered count the fallback frames 15 and 0 of the first pass, Edges the others;
		// Invalid counts the lookups that fell back to the shared InvalidCell. Stale counts the cells whose
		// stored results differ from the current ones: the native buffer has not redrawn them yet. The
		// model has no alpha shapes; AlphaShapes counts the native ones.
		int Visited = 0, Legal = 0, Covered = 0, Uncovered = 0, Edges = 0, Invalid = 0, Missing = 0, Stale = 0, AlphaShapes = 0;
		std::uint64_t Hash = 0;
	};

	// The file data behind a shape, without loading a reference that is not loaded yet.
	inline const SHPStruct* ShapeData(const SHPStruct* shape)
	{
		if (!CursorSnapshot::Readable(shape, sizeof(SHPStruct))) return nullptr;
		if (!shape->IsReference()) return shape;
		const auto* reference = shape->AsReference();
		if (!CursorSnapshot::Readable(reference, sizeof(SHPReference)) || !reference->Loaded) return nullptr;
		const auto* data = reference->Data;
		return CursorSnapshot::Readable(data, sizeof(SHPStruct)) && !data->IsReference() ? data : nullptr;
	}

	// 69E7E0 and 69E740 without their loading: the frame's bounds and Width*Height bytes at its offset.
	inline const FrameCopy* FrameAt(Shape& shape, int index)
	{
		if (index < 0 || size_t(index) >= shape.Frames.size()) return nullptr;
		auto& copy = shape.Frames[index];
		if (copy.Tried) return copy.Ready ? &copy : nullptr;
		copy.Tried = true;
		const auto base = reinterpret_cast<std::uintptr_t>(shape.Data);
		const auto* header = reinterpret_cast<const SHPFrame*>(base + sizeof(SHPStruct)) + index;
		if (!CursorSnapshot::Readable(header, sizeof(*header)) || header->Offset <= 0 || header->Width <= 0 || header->Height <= 0)
			return nullptr;
		const auto size = size_t(header->Width) * size_t(header->Height);
		const auto* pixels = reinterpret_cast<const std::uint8_t*>(base + std::uint32_t(header->Offset));
		if (!CursorSnapshot::Readable(pixels, size)) return nullptr;
		copy.Ready = true;
		copy.Left = header->Left;
		copy.Top = header->Top;
		copy.Width = header->Width;
		copy.Height = header->Height;
		copy.Pixels.assign(pixels, pixels + size);
		return &copy;
	}

	inline bool BindShape(Shape& shape, std::uintptr_t global)
	{
		shape.Pointer = *reinterpret_cast<const SHPStruct* const*>(global);
		shape.Data = ShapeData(shape.Pointer);
		if (!shape.Data || shape.Data->Frames <= 0) return false;
		shape.Frames.resize(size_t(shape.Data->Frames));
		return true;
	}

	// 5657A0: a cell outside the storage falls back to the shared InvalidCell, whose flags are read
	// as they are; its coordinates, which the native lookup overwrites, are not used here.
	inline const CellClass* CellAt(std::int16_t x, std::int16_t y, int& invalid)
	{
		if (const auto* cell = MapClass::Instance.TryGetCellAt(CellStruct {x, y})) return cell;
		++invalid;
		return &MapClass::InvalidCell;
	}

	// 6D71E0 over ViewBounds: every legal cell of the scan whose box meets the view, in scan order.
	inline Snapshot Capture(int scanMargin)
	{
		using namespace TerrainImage;
		Snapshot snapshot;
		const auto* tactical = TacticalClass::Instance;
		const auto& view = DSurface::ViewBounds;
		snapshot.View = view;
		const auto* scenario = ScenarioClass::Instance;
		if (!tactical || !CursorSnapshot::Readable(scenario, sizeof(ScenarioClass))) { snapshot.Reason = "ineligible"; return snapshot; }
		snapshot.Fog = *reinterpret_cast<const std::uint32_t*>(&scenario->SpecialFlags) & FogOfWarFlag;
		if (snapshot.Fog)
		{
			const auto* player = HouseClass::CurrentPlayer;
			if (!CursorSnapshot::Readable(player, sizeof(HouseClass))) { snapshot.Reason = "player"; return snapshot; }
			snapshot.Defeated = player->Defeated;
		}
		snapshot.Window = Game::hWnd != nullptr;
		snapshot.AlphaShapes = AlphaShapeClass::Array.Count;
		const bool darken = snapshot.Fog && !snapshot.Defeated;
		// The writers address the buffer by x without ViewBounds.X; the model covers ViewBounds at 0.
		if (view.X != 0 || view.Width <= 0 || view.Height <= 0) { snapshot.Reason = "view"; return snapshot; }
		if (*reinterpret_cast<const std::uint32_t*>(TransparentValue) != Transparent) { snapshot.Reason = "transparent"; return snapshot; }
		// A shape the game has not bound yet would be loaded by the writer; the check waits for it.
		if (!*reinterpret_cast<const bool*>(OverwriteShapesReady) || (darken && !*reinterpret_cast<const bool*>(DarkenShapeReady))
			|| !BindShape(snapshot.Shapes[0], snapshot.Fog ? FogShape : ShroudShape) || (darken && !BindShape(snapshot.Shapes[1], DarkenShape)))
		{
			snapshot.Reason = "shape";
			return snapshot;
		}
		const auto* table = reinterpret_cast<const std::int8_t*>(OcclusionTable);
		const Rect viewRect {view.X, view.Y, view.Width, view.Height};
		const auto tacticalX = tactical->TacticalPos.X, tacticalY = tactical->TacticalPos.Y;
		const auto anchor = ScanAnchor(tacticalX, tacticalY);
		const int rows = ScanRows(view.Height), columns = ScanColumns(view.Width);
		const int state[] {darken, snapshot.AlphaShapes};
		std::uint64_t hash = SidebarSnapshot::Hash(state, sizeof(state));
		// 6D71E0 runs its cell loop only with a window.
		for (int r = -scanMargin; snapshot.Window && r < rows + scanMargin; ++r)
			for (int c = -scanMargin; c < columns + scanMargin; ++c)
			{
				const auto [cellX, cellY] = ScanCell(anchor, r, c);
				if (cellX < INT16_MIN || cellX > INT16_MAX || cellY < INT16_MIN || cellY > INT16_MAX) continue;
				++snapshot.Visited;
				const CellStruct coords {std::int16_t(cellX), std::int16_t(cellY)};
				if (!MapClass::Instance.CoordinatesLegal(coords)) continue;
				++snapshot.Legal;
				const auto box = CellBox(cellX, cellY, tacticalX, tacticalY, view.Y);
				const auto clip = Intersect(box, viewRect);
				if (clip.Width <= 0) continue;
				const auto* cell = CellAt(coords.X, coords.Y, snapshot.Invalid);
				std::uint8_t alt[8], flags[8];
				for (int i = 0; i < 8; ++i)
				{
					const auto* neighbour = CellAt(std::int16_t(coords.X + Neighbours[i].X), std::int16_t(coords.Y + Neighbours[i].Y), snapshot.Invalid);
					alt[i] = std::uint8_t(std::uint32_t(neighbour->AltFlags));
					flags[i] = std::uint8_t(std::uint32_t(neighbour->Flags));
				}
				Write write {box.X + view.X, box.Y, clip, cellX, cellY};
				write.Flags[0] = std::uint32_t(cell->AltFlags);
				write.Flags[1] = std::uint32_t(cell->Flags);
				write.Mask[0] = OcclusionMask(alt, MappedBit);
				write.Mask[1] = OcclusionMask(flags, EdgeRevealedBit);
				write.Occlusion[0] = Occlusion(write.Flags[0], alt, false, table);
				write.Occlusion[1] = Occlusion(write.Flags[1], flags, true, table);
				write.Stored[0] = cell->Visibility;
				write.Stored[1] = cell->Foggedness;
				write.Frame[0] = FrameOf(write.Occlusion[0]);
				write.Frame[1] = darken ? FrameOf(write.Occlusion[1]) : -1;
				if (write.Stored[0] != write.Occlusion[0] || write.Stored[1] != write.Occlusion[1]) ++snapshot.Stale;
				if (write.Frame[0] == CoveredFrame) ++snapshot.Covered;
				else if (write.Frame[0] == UncoveredFrame) ++snapshot.Uncovered;
				else ++snapshot.Edges;
				for (int pass = 0; pass < Passes; ++pass)
					if (write.Frame[pass] >= 0 && !FrameAt(snapshot.Shapes[pass], write.Frame[pass])) ++snapshot.Missing;
				const int key[] {cellX, cellY, write.Frame[0], write.Frame[1]};
				hash = SidebarSnapshot::Hash(key, sizeof(key), hash);
				snapshot.Writes.push_back(write);
			}
		snapshot.Hash = hash;
		snapshot.Reason = "ready";
		return snapshot;
	}

	// Trace only: the snapshot drawn over a cleared view against the native words at the same
	// logical position. Owner is the write whose active pixel covers the first mismatch.
	struct Check
	{
		int Compared = 0, Mismatched = 0, Unsupported = 0, ModelShaded = 0, NativeShaded = 0;
		int Left = INT_MAX, Top = INT_MAX, Right = INT_MIN, Bottom = INT_MIN;
		int FirstX = -1, FirstY = -1, Native = 0, Model = 0, Owner = -1;
	};

	inline Check CheckNative(Snapshot& snapshot, ABuffer* buffer)
	{
		Check check;
		const auto& view = snapshot.View;
		const Rect area {view.X, view.Y, view.Width, view.Height};
		std::vector<std::uint16_t> model(size_t(view.Width) * view.Height, ClearValue);
		std::vector<int> owner(model.size(), -1);
		for (size_t index = 0; index < snapshot.Writes.size(); ++index)
		{
			const auto& write = snapshot.Writes[index];
			for (int pass = 0; pass < Passes; ++pass)
			{
				if (write.Frame[pass] < 0) continue;
				const auto* copy = FrameAt(snapshot.Shapes[pass], write.Frame[pass]);
				if (!copy) continue;
				const Frame frame {copy->Left, copy->Top, copy->Width, copy->Height, copy->Pixels.data()};
				const auto drawn = Draw(model.data(), area, write.X, write.Y, write.Clip, frame, pass ? Pass::Darken : Pass::Overwrite);
				if (drawn == Drawn::Unsupported) ++check.Unsupported;
				if (drawn != Drawn::Pixels) continue;
				const auto span = Intersect(write.Clip, {write.X + frame.Left, write.Y + frame.Top, frame.Width, frame.Height});
				for (int y = span.Y; y < span.Y + span.Height; ++y)
					for (int x = span.X; x < span.X + span.Width; ++x)
					{
						const auto value = frame.Pixels[size_t(y - write.Y - frame.Top) * frame.Width + size_t(x - write.X - frame.Left)];
						if (pass ? value <= MaxDarken : value != Transparent)
							owner[size_t(y - area.Y) * area.Width + size_t(x - area.X)] = int(index);
					}
			}
		}
		for (int row = 0; row < view.Height; ++row)
		{
			// 47EFE0 and 47F250 address the ring buffer by x and the row below ViewBounds.Y.
			auto* native = static_cast<std::uint16_t*>(buffer->GetBuffer(view.X, row));
			for (int column = 0; column < view.Width; ++column, ++native)
			{
				buffer->AdjustPointer(native);
				const auto at = size_t(row) * view.Width + column;
				const auto expected = model[at];
				++check.Compared;
				if (expected != ClearValue) ++check.ModelShaded;
				if (*native != ClearValue) ++check.NativeShaded;
				if (*native == expected) continue;
				if (!check.Mismatched++)
				{
					check.FirstX = column;
					check.FirstY = row;
					check.Native = *native;
					check.Model = expected;
					check.Owner = owner[at];
				}
				check.Left = std::min(check.Left, column); check.Right = std::max(check.Right, column);
				check.Top = std::min(check.Top, row); check.Bottom = std::max(check.Bottom, row);
			}
		}
		return check;
	}

	inline void Trace(Snapshot& snapshot, unsigned sample)
	{
		auto* buffer = ABuffer::Instance;
		const bool readable = CursorSnapshot::Readable(buffer, sizeof(ABuffer));
		const auto& map = MapClass::Instance;
		// The first cell that draws anything but the plain frame 0, to explain a shaded edge.
		const auto shaded = std::find_if(snapshot.Writes.begin(), snapshot.Writes.end(),
			[](const Write& write) { return write.Frame[0] != UncoveredFrame || write.Frame[1] > UncoveredFrame; });
		const Write none {};
		const auto& edge = shaded == snapshot.Writes.end() ? none : *shaded;
		Debug::Log("[TerrainGPU] abuffer sample=%u reason=%s fog=%d defeated=%d window=%d shapes=%p/%p,%p/%p frames=%d,%d alpha=%d writes=%zu "
			"visited=%d legal=%d covered=%d uncovered=%d edges=%d invalid=%d missing=%d stale=%d map=%d,%d,%d,%d visible=%d,%d,%d,%d "
			"buffer=%p bounds=%d,%d,%d,%d width=%d height=%d position=%d size=%d shaded=%d,%d:flags=%X/%X:mask=%02X/%02X:frame=%d/%d\n",
			sample, snapshot.Reason, snapshot.Fog, snapshot.Defeated, snapshot.Window, snapshot.Shapes[0].Pointer, snapshot.Shapes[0].Data,
			snapshot.Shapes[1].Pointer, snapshot.Shapes[1].Data, int(snapshot.Shapes[0].Frames.size()), int(snapshot.Shapes[1].Frames.size()),
			snapshot.AlphaShapes, snapshot.Writes.size(), snapshot.Visited, snapshot.Legal, snapshot.Covered, snapshot.Uncovered, snapshot.Edges, snapshot.Invalid,
			snapshot.Missing, snapshot.Stale, map.MapRect.X, map.MapRect.Y, map.MapRect.Width, map.MapRect.Height, map.VisibleRect.X, map.VisibleRect.Y,
			map.VisibleRect.Width, map.VisibleRect.Height, buffer, readable ? buffer->Bounds.X : 0, readable ? buffer->Bounds.Y : 0,
			readable ? buffer->Bounds.Width : 0, readable ? buffer->Bounds.Height : 0, readable ? buffer->Width : 0, readable ? buffer->Height : 0,
			readable ? buffer->BufferPosition : 0, readable ? buffer->BufferSize : 0, edge.CellX, edge.CellY, edge.Flags[0], edge.Flags[1],
			edge.Mask[0], edge.Mask[1], edge.Frame[0], edge.Frame[1]);
		if (std::strcmp(snapshot.Reason, "ready") || !readable || buffer->Width < snapshot.View.Width || buffer->Height < snapshot.View.Height) return;
		const auto check = CheckNative(snapshot, buffer);
		const auto& owner = check.Owner >= 0 ? snapshot.Writes[size_t(check.Owner)] : none;
		Debug::Log("[TerrainGPU] abuffercheck sample=%u compared=%d mismatched=%d unsupported=%d modelShaded=%d nativeShaded=%d box=%d,%d,%d,%d "
			"first=%d,%d:native=%d:model=%d:cell=%d,%d:flags=%X/%X:mask=%02X/%02X:frame=%d/%d:occlusion=%d/%d:stored=%d/%d\n",
			sample, check.Compared, check.Mismatched, check.Unsupported, check.ModelShaded, check.NativeShaded,
			check.Mismatched ? check.Left : 0, check.Mismatched ? check.Top : 0, check.Mismatched ? check.Right : 0, check.Mismatched ? check.Bottom : 0,
			check.FirstX, check.FirstY, check.Native, check.Model, owner.CellX, owner.CellY, owner.Flags[0], owner.Flags[1],
			owner.Mask[0], owner.Mask[1], owner.Frame[0], owner.Frame[1], owner.Occlusion[0], owner.Occlusion[1], owner.Stored[0], owner.Stored[1]);
	}
}
