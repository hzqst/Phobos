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
#include <unordered_map>
#include <vector>

// The shroud and fog the terrain pass writes into the ABuffer (6D3660 -> 6D71E0 -> 4801F0) before
// 547CF0 reads it, rebuilt from the current cell flags as a full redraw of ViewBounds with each
// terrain capture; ABufferPass draws it for the terrain shader. The trace checks it in the 4F4583
// callback against the native ABuffer, which still holds what the tiles read while no alpha shape
// or later Alpha writer has touched it.
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

	// One cell 4801F0 draws. ID[1] is empty where the darken pass does not run. For the trace: the
	// frame, neighbour mask, the cell's own flags and 6D8700's result of each pass, and the results
	// 4801F0 stored in Visibility and Foggedness when it last drew the cell.
	struct Write
	{
		int X, Y;
		Rect Clip;
		int CellX, CellY;
		FrameCache::FrameID ID[Passes];
		int Frame[Passes], Mask[Passes], Occlusion[Passes], Stored[Passes];
		std::uint32_t Flags[Passes];
	};

	struct Shape
	{
		// The pointer the global holds and the file data behind it, registered by the data address.
		const SHPStruct* Pointer = nullptr;
		const SHPStruct* Data = nullptr;
		FrameCache::ResourceID Resource;
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
		bool Ready() const { return std::strcmp(Reason, "ready") == 0; }
	};

	// The frames of the shroud and fog shapes, each 60x30 here; checked by their headers on every capture.
	inline FrameCache::Cache Frames({4 * 1024 * 1024, 1024, 16});
	inline std::unordered_map<std::uintptr_t, std::uint64_t> HeaderHashes;
	inline Snapshot Current;

	inline void Clear()
	{
		Current = {};
		Frames.Clear();
		HeaderHashes.clear();
	}

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

	// 69E7E0 and 69E740 without their loading: each frame's bounds, and its Width*Height bytes at its
	// offset copied without RLE decoding. A frame without an offset or size has no data.
	inline bool BindShape(Shape& shape, std::uintptr_t global)
	{
		shape.Pointer = *reinterpret_cast<const SHPStruct* const*>(global);
		shape.Data = ShapeData(shape.Pointer);
		if (!shape.Data || shape.Data->Frames <= 0) return false;
		const auto headers = sizeof(SHPStruct) + size_t(shape.Data->Frames) * sizeof(SHPFrame);
		if (!CursorSnapshot::Readable(shape.Data, headers)) return false;
		const auto identity = reinterpret_cast<std::uintptr_t>(shape.Data);
		const auto hash = SidebarSnapshot::Hash(shape.Data, headers);
		shape.Resource = Frames.LookupResource(identity);
		if (const auto known = HeaderHashes.find(identity); known != HeaderHashes.end() && known->second != hash && shape.Resource)
		{
			Frames.Invalidate(shape.Resource);
			shape.Resource = {};
		}
		if (!shape.Resource)
		{
			FrameCache::ResourceInfo info {shape.Data->Width, shape.Data->Height, {}};
			for (int i = 0; i < shape.Data->Frames; ++i)
			{
				const auto& header = shape.Data->AsFile()->GetFrameHeader(i);
				const bool valid = header.Offset > 0 && header.Width > 0 && header.Height > 0;
				info.Frames.push_back({header.Left, header.Top, valid ? header.Width : 0, valid ? header.Height : 0, 0,
					valid ? size_t(header.Offset) : 0, 0});
			}
			shape.Resource = Frames.Register(identity, info);
			if (!shape.Resource) return false;
		}
		HeaderHashes[identity] = hash;
		return true;
	}

	inline int FrameCount(const Shape& shape)
	{
		const auto* info = Frames.Describe(shape.Resource);
		return info ? int(info->Frames.size()) : 0;
	}

	inline FrameCache::FrameID FrameAt(const Shape& shape, int index)
	{
		const auto* info = Frames.Describe(shape.Resource);
		if (!info || index < 0 || size_t(index) >= info->Frames.size()) return {};
		const auto base = reinterpret_cast<std::uintptr_t>(shape.Data);
		const auto result = Frames.GetOrDecode(shape.Resource, unsigned(index), [base](const FrameCache::Descriptor& d, auto& indices)
		{
			if (d.Width <= 0 || d.Height <= 0) return FrameCache::Status::InvalidData;
			const auto* pixels = reinterpret_cast<const std::uint8_t*>(base + d.Offset);
			const auto size = size_t(d.Width) * size_t(d.Height);
			if (!CursorSnapshot::Readable(pixels, size)) return FrameCache::Status::InvalidData;
			indices.assign(pixels, pixels + size);
			return FrameCache::Status::Ready;
		});
		return result.State == FrameCache::Status::Ready ? result.Frame->ID : FrameCache::FrameID {};
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
		// Each frame is looked up once per capture; most cells share a few frames.
		std::array<std::vector<FrameCache::FrameID>, Passes> ids;
		std::array<std::vector<bool>, Passes> looked;
		for (int pass = 0; pass < Passes; ++pass)
		{
			ids[pass].resize(size_t(FrameCount(snapshot.Shapes[pass])));
			looked[pass].resize(ids[pass].size());
		}
		const auto frameID = [&](int pass, int frame)
		{
			if (frame < 0 || size_t(frame) >= ids[pass].size()) return FrameCache::FrameID {};
			if (!looked[pass][frame]) { looked[pass][frame] = true; ids[pass][frame] = FrameAt(snapshot.Shapes[pass], frame); }
			return ids[pass][frame];
		};
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
					if (write.Frame[pass] >= 0 && !(write.ID[pass] = frameID(pass, write.Frame[pass]))) ++snapshot.Missing;
				const int key[] {cellX, cellY, write.Frame[0], write.Frame[1]};
				hash = SidebarSnapshot::Hash(key, sizeof(key), hash);
				snapshot.Writes.push_back(write);
			}
		snapshot.Hash = hash;
		snapshot.Reason = "ready";
		return snapshot;
	}

	// Trace only: the snapshot drawn over a cleared view against the native words at the same
	// logical position. Owner is the write whose active pixel covers the first mismatch. Split counts
	// the pixels where the GPU's order, every overwrite before every darken, differs from the native
	// order of both passes per cell.
	struct Check
	{
		int Compared = 0, Mismatched = 0, Unsupported = 0, ModelShaded = 0, NativeShaded = 0, Split = 0;
		int Left = INT_MAX, Top = INT_MAX, Right = INT_MIN, Bottom = INT_MIN;
		int FirstX = -1, FirstY = -1, Native = 0, Model = 0, Owner = -1;
	};

	// One pass of a write into buffer; owner, if given, takes index where the frame's pixel is active.
	inline Drawn DrawWrite(std::uint16_t* buffer, const Rect& area, const Write& write, int pass, int* owner, int index)
	{
		const auto* record = Frames.Resolve(write.ID[pass]);
		if (!record) return Drawn::Nothing;
		const auto& g = record->Geometry;
		const Frame frame {g.Left, g.Top, g.Width, g.Height, record->Indices.data()};
		const auto drawn = Draw(buffer, area, write.X, write.Y, write.Clip, frame, pass ? Pass::Darken : Pass::Overwrite);
		if (drawn != Drawn::Pixels || !owner) return drawn;
		const auto span = Intersect(write.Clip, {write.X + frame.Left, write.Y + frame.Top, frame.Width, frame.Height});
		for (int y = span.Y; y < span.Y + span.Height; ++y)
			for (int x = span.X; x < span.X + span.Width; ++x)
			{
				const auto value = frame.Pixels[size_t(y - write.Y - frame.Top) * frame.Width + size_t(x - write.X - frame.Left)];
				if (pass ? value <= MaxDarken : value != Transparent)
					owner[size_t(y - area.Y) * area.Width + size_t(x - area.X)] = index;
			}
		return drawn;
	}

	inline Check CheckNative(const Snapshot& snapshot, ABuffer* buffer)
	{
		Check check;
		const auto& view = snapshot.View;
		const Rect area {view.X, view.Y, view.Width, view.Height};
		std::vector<std::uint16_t> model(size_t(view.Width) * view.Height, ClearValue), split = model;
		std::vector<int> owner(model.size(), -1);
		for (size_t index = 0; index < snapshot.Writes.size(); ++index)
			for (int pass = 0; pass < Passes; ++pass)
				if (DrawWrite(model.data(), area, snapshot.Writes[index], pass, owner.data(), int(index)) == Drawn::Unsupported)
					++check.Unsupported;
		for (int pass = 0; pass < Passes; ++pass)
			for (const auto& write : snapshot.Writes)
				DrawWrite(split.data(), area, write, pass, nullptr, 0);
		for (size_t i = 0; i < model.size(); ++i) check.Split += model[i] != split[i];
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

	inline void Trace(const Snapshot& snapshot, unsigned sample)
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
			snapshot.Shapes[1].Pointer, snapshot.Shapes[1].Data, FrameCount(snapshot.Shapes[0]), FrameCount(snapshot.Shapes[1]),
			snapshot.AlphaShapes, snapshot.Writes.size(), snapshot.Visited, snapshot.Legal, snapshot.Covered, snapshot.Uncovered, snapshot.Edges, snapshot.Invalid,
			snapshot.Missing, snapshot.Stale, map.MapRect.X, map.MapRect.Y, map.MapRect.Width, map.MapRect.Height, map.VisibleRect.X, map.VisibleRect.Y,
			map.VisibleRect.Width, map.VisibleRect.Height, buffer, readable ? buffer->Bounds.X : 0, readable ? buffer->Bounds.Y : 0,
			readable ? buffer->Bounds.Width : 0, readable ? buffer->Bounds.Height : 0, readable ? buffer->Width : 0, readable ? buffer->Height : 0,
			readable ? buffer->BufferPosition : 0, readable ? buffer->BufferSize : 0, edge.CellX, edge.CellY, edge.Flags[0], edge.Flags[1],
			edge.Mask[0], edge.Mask[1], edge.Frame[0], edge.Frame[1]);
		if (!snapshot.Ready() || !readable || buffer->Width < snapshot.View.Width || buffer->Height < snapshot.View.Height) return;
		const auto check = CheckNative(snapshot, buffer);
		const auto& owner = check.Owner >= 0 ? snapshot.Writes[size_t(check.Owner)] : none;
		Debug::Log("[TerrainGPU] abuffercheck sample=%u compared=%d mismatched=%d unsupported=%d split=%d modelShaded=%d nativeShaded=%d box=%d,%d,%d,%d "
			"first=%d,%d:native=%d:model=%d:cell=%d,%d:flags=%X/%X:mask=%02X/%02X:frame=%d/%d:occlusion=%d/%d:stored=%d/%d\n",
			sample, check.Compared, check.Mismatched, check.Unsupported, check.Split, check.ModelShaded, check.NativeShaded,
			check.Mismatched ? check.Left : 0, check.Mismatched ? check.Top : 0, check.Mismatched ? check.Right : 0, check.Mismatched ? check.Bottom : 0,
			check.FirstX, check.FirstY, check.Native, check.Model, owner.CellX, owner.CellY, owner.Flags[0], owner.Flags[1],
			owner.Mask[0], owner.Mask[1], owner.Frame[0], owner.Frame[1], owner.Occlusion[0], owner.Occlusion[1], owner.Stored[0], owner.Stored[1]);
	}
}
