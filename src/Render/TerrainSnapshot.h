#pragma once

#include "ABufferSnapshot.h"
#include "BlitFrameSource.h"
#include "ShapeFrameSource.h"
#include "SidebarSnapshot.h"
#include "TerrainBatch.h"
#include "TerrainImage.h"
#include <CellClass.h>
#include <ConvertClass.h>
#include <Drawing.h>
#include <IsometricTileTypeClass.h>
#include <MapClass.h>
#include <SmudgeTypeClass.h>
#include <TacticalClass.h>
#include <cstdio>
#include <unordered_map>

// The TMP tiles the terrain pass (6D2DE0 -> 6D7560 -> 480350 -> 547CF0) draws into
// DSurface::Tile, captured in the 4F4583 callback as a full redraw of the visible cells.
// Stage C2: main diamond and extra image in native order, lit by Intensity_Terrain with the
// default ABuffer value. Stage C3 adds each image's Z plane and base depth, which the GPU tests and
// writes since stage C5.2a. Stage C4.2 captures the ABuffer's shroud and fog with each capture (ABufferSnapshot) for
// the shader's light. Stage C5.1 adds each cell's smudge (480350 -> 6B55F0) right after its tile.
// OverlaySnapshot captures the overlays after this capture; every later Tile layer is absent.
namespace TerrainSnapshot
{
	using namespace TerrainImage;
	// Cells the scan visits beyond the native rows and columns on every side.
	constexpr int ScanMargin = 2;
	constexpr int ClearTileSlot = 0xFFFF;
	constexpr std::uintptr_t ClearTile = 0xAA10B0;
	constexpr std::uintptr_t RowTemplate = 0x7EC4C8, RowOffsets = 0x7EC450;
	// 4814F0: the fixed 4x4 table and the 8x8 table with its one-time setup flag.
	constexpr std::uintptr_t VariantTable4 = 0x81CCA8, VariantTable8 = 0x89E620, VariantTable8Ready = 0x89E7C7;
	constexpr std::uint32_t AlternateVariantFlag = 1u << 13;
	// 547CF0 skips its Z loops for an accelerated blitter when this byte is set.
	constexpr std::uintptr_t AcceleratedDepth = 0xABC55C;
	// Rows of the color table texture: 19 tables of 53 shades, or 37 of 27.
	constexpr std::uint32_t TableRows = 1024;
	static_assert(offsetof(ObjectTypeClass, Image) == 0xA4);
	static_assert(offsetof(IsometricTileTypeClass, unk_2BC) == 0x2BC);
	static_assert(offsetof(IsometricTileTypeClass, unk_2F0) == 0x2F0);
	static_assert(offsetof(CellClass, MapCoords) == 0x24);
	static_assert(offsetof(CellClass, LightConvert) == 0x34);
	static_assert(offsetof(CellClass, IsoTileTypeIndex) == 0x38);
	static_assert(offsetof(CellClass, Intensity_Terrain) == 0x10C);
	static_assert(offsetof(CellClass, SmudgeTypeIndex) == 0x48);
	static_assert(offsetof(CellClass, SmudgeData) == 0x11F);
	static_assert(offsetof(SmudgeTypeClass, Width) == 0x298);
	static_assert(offsetof(CellClass, Height) == 0x11A);
	static_assert(offsetof(CellClass, Level) == 0x11B);
	static_assert(offsetof(CellClass, Flags) == 0x140);
	static_assert(offsetof(ConvertClass, ShadeCount) == 0x16C);
	static_assert(offsetof(ConvertClass, FullColorData) == 0x170);
	static_assert(offsetof(ZBuffer, Bounds) == 0x00);
	static_assert(offsetof(ZBuffer, MaxValue) == 0x24);

	// A loaded TMP: grid size and nominal pixel size, then one absolute piece pointer per cell.
	struct TileImageHeader { std::int32_t GridWidth, GridHeight, PixelWidth, PixelHeight; };
	// Piece offsets are relative to the piece; the packed 900-pixel diamond follows the header.
	struct Piece
	{
		std::int32_t X, Y;
		std::uint32_t ExtraData, Depth, ExtraDepth;
		std::int32_t ExtraX, ExtraY, ExtraWidth, ExtraHeight;
		std::uint8_t Flags, Unknown[0x0F];
	};
	static_assert(sizeof(Piece) == 0x34);
	enum PieceFlag : std::uint8_t { HasExtra = 1, HasDepth = 2, AlternateVariant = 4 };

	extern FrameCache::Cache Frames;

	struct Placement
	{
		FrameCache::FrameID ID;
		TerrainBatch::ColorTables::Handle Table;
		int X, Y, Width, Height;
		// 547CF0's row of the alpha lighting remap; the shader combines it with the ABuffer value.
		int LightRow;
		bool Diamond;
		// The frame's Z plane lies below its index plane. Without it the image neither tests nor
		// writes the ZBuffer; with it each pixel's depth is Depth plus its Z byte.
		bool UsesZ = false;
		std::uint16_t Depth = 0;
		// Tile pieces live in Frames, smudge shapes in ShapeFrameSource's cache.
		const FrameCache::Cache* Storage = &Frames;
	};
	struct Frame
	{
		bool Visible = false;
		// Canvas X of Tile's origin and the native clip, ViewBounds, in Tile coordinates.
		int OriginX = 0;
		RectangleStruct Clip {};
		std::vector<Placement> Placements;
	};
	extern Frame Current;
	extern TerrainBatch::ColorTables Tables;
	extern std::unordered_map<std::uintptr_t, std::uint64_t> PieceHashes;
	// 0: not checked yet, 1: the template is the analytic diamond, 2: it is not.
	extern int TemplateState;
	extern bool Trace;
	extern unsigned TraceCount;
	// Capture cost and what the presents drew, logged every PerfWindow captures.
	constexpr unsigned PerfWindow = 600;
	struct Perf
	{
		std::uint64_t Captures = 0, Ticks = 0, MaxTicks = 0, Presents = 0, Draws = 0, Instances = 0;
		unsigned Lines = 0;
	};
	extern Perf Stats;
	extern std::uint64_t LastTraceHash;
	// The trace waits until the view stops scrolling, so its lines go to settled views.
	extern Point LastTactical;

	inline void Invalidate()
	{
		Current.Visible = false;
		Current.Placements.clear();
		ABufferSnapshot::Current = {};
		LastTraceHash = 0;
	}

	// Pieces are keyed by their address and checked by content on every capture.
	inline void ClearPieces()
	{
		Frames.Clear();
		PieceHashes.clear();
	}

	inline void Clear()
	{
		Invalidate();
		ClearPieces();
		Tables.Clear();
		ABufferSnapshot::Clear();
		BlitFrameSource::Clear();
	}

	inline void InitializeTrace()
	{
		char value[2] {};
		Trace = GetEnvironmentVariableA("PHOBOS_TERRAIN_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
		TraceCount = 0;
		LastTraceHash = 0;
	}

	inline bool Eligible()
	{
		return Game::IsActive && Unsorted::ScenarioStarted && TacticalClass::Instance;
	}

	inline const TileImageHeader* ImageOf(const IsometricTileTypeClass* type)
	{
		// Read the loaded image directly; GetImage (544CB0) would load a lazy TMP.
		const auto* image = reinterpret_cast<const TileImageHeader*>(type->Image);
		if (!image) return nullptr;
		const auto pieces = std::int32_t(std::uint32_t(image->GridWidth) * std::uint32_t(image->GridHeight));
		return pieces > 0 ? image : nullptr;
	}

	inline int PieceCount(const TileImageHeader* image)
	{
		return std::int32_t(std::uint32_t(image->GridWidth) * std::uint32_t(image->GridHeight));
	}

	inline const Piece* PieceSlot(const TileImageHeader* image, int index)
	{
		return reinterpret_cast<const Piece* const*>(image + 1)[index];
	}

	struct PieceFrames { FrameCache::FrameID Main, Extra; const char* Reason = nullptr; bool UsesZ = false; };
	// Frame 0 of a smudge type's shape for one capture, with the shape's canvas and the type's Width.
	// No ID and not Failed: the type has no shape and draws nothing.
	struct SmudgeShape
	{
		FrameCache::FrameID ID;
		FrameCache::Descriptor Frame;
		int CanvasWidth = 0, CanvasHeight = 0, Width = 0;
		bool Failed = false;
	};
	struct Counters { int Visited = 0, Legal = 0, Drawn = 0, Extras = 0, Tile = 0, Image = 0, Variant = 0, Empty = 0, Palette = 0, Frame = 0, NoDepth = 0, MaxLevel = 0,
		Smudges = 0, SmudgeMissing = 0; };
	// The drawn cell nearest the view center, for the trace.
	struct Sample
	{
		int X = -1, Y = -1, Index = -1, Type = -1, Height = -1, Level = 0, Variants = 0, Variant = 0, Drawn = -1;
		int Intensity = 0, Shades = 0, Shade = -1;
		const void* Piece = nullptr;
	};

	// Piece data at an offset relative to the piece, or null when the address overflows.
	inline const std::uint8_t* PieceData(const Piece* piece, std::uint32_t offset, size_t size)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(piece);
		if (offset > UINTPTR_MAX - base) return nullptr;
		const auto* data = reinterpret_cast<const std::uint8_t*>(base + offset);
		return data;
	}

	// Copies the piece into the frame cache. A changed piece at the same address replaces the old frames.
	// A piece with depth data stores its Z bytes as a second plane below the indices of each frame:
	// the packed main Z expanded like the pixels, the extra Z as its rectangle.
	inline PieceFrames AcquirePiece(const Piece* piece)
	{
		const auto* packed = reinterpret_cast<const std::uint8_t*>(piece + 1);
		const std::uint8_t *extra = nullptr, *depth = nullptr, *extraDepth = nullptr;
		const bool hasExtra = piece->Flags & HasExtra, hasDepth = piece->Flags & HasDepth;
		size_t extraSize = 0;
		if (hasExtra)
		{
			if (piece->ExtraWidth <= 0 || piece->ExtraHeight <= 0 || piece->ExtraWidth > ShapeImage::MaxDimension
				|| piece->ExtraHeight > ShapeImage::MaxDimension) return {{}, {}, "extra"};
			extraSize = size_t(piece->ExtraWidth) * piece->ExtraHeight;
			extra = PieceData(piece, piece->ExtraData, extraSize);
			if (!extra) return {{}, {}, "extra"};
		}
		if (hasDepth)
		{
			depth = PieceData(piece, piece->Depth, DiamondPixels);
			if (hasExtra) extraDepth = PieceData(piece, piece->ExtraDepth, extraSize);
			if (!depth || (hasExtra && !extraDepth)) return {{}, {}, "depth"};
		}
		const int planes = hasDepth ? 2 : 1;
		auto hash = SidebarSnapshot::Hash(piece, sizeof(*piece) + DiamondPixels);
		if (extra) hash = SidebarSnapshot::Hash(extra, extraSize, hash);
		if (depth) hash = SidebarSnapshot::Hash(depth, DiamondPixels, hash);
		if (extraDepth) hash = SidebarSnapshot::Hash(extraDepth, extraSize, hash);
		const auto identity = reinterpret_cast<std::uintptr_t>(piece);
		auto resource = Frames.LookupResource(identity);
		if (const auto known = PieceHashes.find(identity); known != PieceHashes.end() && known->second != hash && resource)
		{
			Frames.Invalidate(resource);
			resource = {};
		}
		if (!resource)
		{
			resource = Frames.Register(identity, {TileWidth, DiamondRows * planes, {{0, 0, TileWidth, DiamondRows * planes},
				{0, 0, hasExtra ? piece->ExtraWidth : 0, hasExtra ? piece->ExtraHeight * planes : 0}}});
			if (!resource)
			{
				// Over budget: start again from the pieces later captures still use. Pieces placed
				// earlier in this capture no longer resolve and are skipped for one frame.
				ClearPieces();
				return {{}, {}, "budget"};
			}
		}
		PieceHashes[identity] = hash;
		const auto main = Frames.GetOrDecode(resource, 0, [packed, depth, planes](const auto&, auto& indices)
		{
			indices.resize(size_t(TileWidth) * DiamondRows * planes);
			ExpandDiamond(packed, indices.data());
			if (depth) ExpandDiamond(depth, indices.data() + size_t(TileWidth) * DiamondRows);
			return FrameCache::Status::Ready;
		});
		if (main.State != FrameCache::Status::Ready) return {{}, {}, "frame"};
		PieceFrames result {main.Frame->ID, {}, nullptr, hasDepth};
		if (extra)
		{
			const auto copy = Frames.GetOrDecode(resource, 1, [extra, extraDepth, extraSize](const auto&, auto& indices)
			{
				indices.assign(extra, extra + extraSize);
				if (extraDepth) indices.insert(indices.end(), extraDepth, extraDepth + extraSize);
				return FrameCache::Status::Ready;
			});
			if (copy.State != FrameCache::Status::Ready) return {{}, {}, "frame"};
			result.Extra = copy.Frame->ID;
		}
		return result;
	}

	// One FullColorData row of a cell's LightConvert, the shade the default ABuffer value selects.
	inline bool ValidConvert(const LightConvertClass* convert)
	{
		return convert && convert->BytesPerPixel == 2
			&& convert->ShadeCount > 0 && convert->ShadeCount <= 256;
	}

	// The whole FullColorData of a LightConvert: every shade row, so the shader can pick any.
	inline TerrainBatch::ColorTables::Handle AcquireTable(const LightConvertClass* convert)
	{
		const auto* colors = static_cast<const std::uint16_t*>(convert->FullColorData);
		if (!colors) return {};
		return Tables.Acquire(colors, std::uint32_t(convert->ShadeCount));
	}

	inline bool Intersects(const RectangleStruct& clip, std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height)
	{
		return width > 0 && height > 0 && x < std::int64_t(clip.X) + clip.Width && x + width > clip.X
			&& y < std::int64_t(clip.Y) + clip.Height && y + height > clip.Y;
	}

	inline void TracePerf()
	{
		if (!Trace || Stats.Lines >= 64 || Stats.Captures < PerfWindow) return;
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const auto micros = [&](std::uint64_t ticks) { return double(ticks) * 1e6 / double(frequency.QuadPart); };
		++Stats.Lines;
		Debug::Log("[TerrainGPU] perf captures=%llu captureAvgUs=%.1f captureMaxUs=%.1f presents=%llu drawsPerPresent=%.1f instancesPerPresent=%.1f\n",
			Stats.Captures, micros(Stats.Ticks) / double(Stats.Captures), micros(Stats.MaxTicks), Stats.Presents,
			Stats.Presents ? double(Stats.Draws) / double(Stats.Presents) : 0.0,
			Stats.Presents ? double(Stats.Instances) / double(Stats.Presents) : 0.0);
		const auto lines = Stats.Lines;
		Stats = {};
		Stats.Lines = lines;
	}

	// draws: GPU draw calls the terrain took in one present; instances: tile images drawn.
	inline void RecordPresent(std::uint64_t draws, std::uint64_t instances)
	{
		++Stats.Presents;
		Stats.Draws += draws;
		Stats.Instances += instances;
	}

	struct CaptureTimer
	{
		LARGE_INTEGER Start;
		CaptureTimer() { QueryPerformanceCounter(&Start); }
		~CaptureTimer()
		{
			LARGE_INTEGER end;
			QueryPerformanceCounter(&end);
			const auto ticks = std::uint64_t(end.QuadPart - Start.QuadPart);
			++Stats.Captures;
			Stats.Ticks += ticks;
			Stats.MaxTicks = std::max(Stats.MaxTicks, ticks);
			TracePerf();
		}
	};

	// Trace only: the software model of 547CF0's Z test over the snapshot, against the native
	// ZBuffer. Equal where Tile was redrawn and nothing later wrote Z; Rejected counts the pixels
	// whose color the Z test kept from being overwritten.
	struct DepthCheck
	{
		int Compared = 0, Mismatched = 0, Rejected = 0, Untouched = 0, Missing = 0;
		int Left = INT_MAX, Top = INT_MAX, Right = INT_MIN, Bottom = INT_MIN;
		int FirstX = -1, FirstY = -1, Native = 0, Model = 0;
	};

	inline DepthCheck CheckDepth(ZBuffer* zbuffer, const RectangleStruct& view)
	{
		DepthCheck check;
		const auto& clip = Current.Clip;
		std::vector<std::uint16_t> model(size_t(clip.Width) * clip.Height, 0xFFFF);
		for (const auto& placement : Current.Placements)
		{
			const auto* frame = placement.Storage->Resolve(placement.ID);
			const auto plane = size_t(placement.Width) * placement.Height;
			if (!frame || frame->Indices.size() < plane * (placement.UsesZ ? 2 : 1)) { ++check.Missing; continue; }
			const auto* indices = frame->Indices.data();
			check.Rejected += DrawDepth(model.data(), clip.X, clip.Y, clip.Width, clip.Height, {placement.X, placement.Y,
				placement.Width, placement.Height, indices, placement.UsesZ ? indices + plane : nullptr, placement.Depth, placement.Diamond});
		}
		for (int row = 0; row < clip.Height; ++row)
		{
			// 547CF0 addresses the ring buffer by the draw position minus ViewBounds.Y.
			auto* native = static_cast<std::uint16_t*>(zbuffer->GetBuffer(clip.X, clip.Y + row - view.Y));
			for (int column = 0; column < clip.Width; ++column, ++native)
			{
				zbuffer->AdjustPointer(native);
				const auto expected = model[size_t(row) * clip.Width + column];
				++check.Compared;
				if (expected == 0xFFFF) ++check.Untouched;
				if (*native == expected) continue;
				if (!check.Mismatched++) { check.FirstX = column; check.FirstY = row; check.Native = *native; check.Model = expected; }
				check.Left = std::min(check.Left, column); check.Right = std::max(check.Right, column);
				check.Top = std::min(check.Top, row); check.Bottom = std::max(check.Bottom, row);
			}
		}
		return check;
	}

	inline void CaptureAfterDraw()
	{
		const CaptureTimer timer;
		Current.Visible = false;
		Current.Placements.clear();
		const char* reason = nullptr;
		const TacticalClass* tactical = TacticalClass::Instance;
		const auto& view = DSurface::ViewBounds;
		const auto& types = IsometricTileTypeClass::Array;
		ZBuffer* zbuffer = ZBuffer::Instance;
		if (!TemplateState)
			TemplateState = TemplateMatches(reinterpret_cast<const std::uint8_t*>(RowTemplate),
				reinterpret_cast<const std::int32_t*>(RowOffsets)) ? 1 : 2;
		if (!Eligible()) reason = "ineligible";
		else if (TemplateState != 1) reason = "template";
		else if (view.Width <= 0 || view.Height <= 0 || view.Width > SidebarSnapshot::MaxCanvasDimension
			|| view.Height > SidebarSnapshot::MaxCanvasDimension) reason = "view";
		else if (!zbuffer) reason = "zbuffer";
		Counters count;
		Sample sample;
		std::int64_t sampleDistance = INT64_MAX;
		Cell anchor {};
		int rows = 0, columns = 0;
		size_t pieces = 0, tables = 0;
		if (!reason)
		{
			const auto& sidebar = DSurface::SidebarBounds;
			Current.OriginX = Tooltip::CanvasOrigin(Tooltip::Target::Composite, GameOptionsClass::Instance.SidebarMode,
				view.X + view.Width, sidebar.X + sidebar.Width);
			// A full redraw: the rectangle is ViewBounds itself (6D2DE0 with its redraw-all byte).
			Current.Clip = view;
			const auto tacticalX = tactical->TacticalPos.X, tacticalY = tactical->TacticalPos.Y;
			anchor = ScanAnchor(tacticalX + Current.Clip.X - view.X, tacticalY + Current.Clip.Y - view.Y);
			rows = ScanRows(Current.Clip.Height);
			columns = ScanColumns(Current.Clip.Width);
			const auto* table4 = reinterpret_cast<const std::int32_t*>(VariantTable4);
			const auto* table8 = reinterpret_cast<const std::int32_t*>(VariantTable8);
			const bool table8Ready = *reinterpret_cast<const bool*>(VariantTable8Ready);
			// Each type and color table is checked once per capture.
			std::unordered_map<const IsometricTileTypeClass*, const TileImageHeader*> images;
			std::unordered_map<const LightConvertClass*, TerrainBatch::ColorTables::Handle> converts;
			std::unordered_map<const Piece*, PieceFrames> captured;
			const auto imageOf = [&](const IsometricTileTypeClass* type)
			{
				const auto [it, inserted] = images.try_emplace(type);
				if (inserted) it->second = ImageOf(type);
				return it->second;
			};
			std::unordered_map<int, SmudgeShape> smudges;
			// Null for an index outside the smudge types, where the native lookup would read past the array.
			const auto smudgeShape = [&](int index) -> const SmudgeShape*
			{
				if (index < 0 || index >= SmudgeTypeClass::Array.Count) return nullptr;
				const auto [it, inserted] = smudges.try_emplace(index);
				auto& shape = it->second;
				if (!inserted) return &shape;
				const auto* smudgeType = SmudgeTypeClass::Array.Items[index];
				if (!smudgeType) { shape.Failed = true; return &shape; }
				shape.Width = smudgeType->Width;
				if (!smudgeType->Image) return &shape;
				// The loaded shape only: an unloaded reference stays NotReady instead of loading.
				std::uintptr_t identity = 0;
				const auto result = ShapeFrameSource::Acquire(smudgeType->Image, 0, identity);
				const auto* info = result.State == FrameCache::Status::Ready ? ShapeFrameSource::Frames.Describe(result.Frame->Resource) : nullptr;
				if (!info) { shape.Failed = true; return &shape; }
				shape = {result.Frame->ID, result.Frame->Geometry, info->Width, info->Height, smudgeType->Width};
				return &shape;
			};
			for (int r = -ScanMargin; r < rows + ScanMargin; ++r)
				for (int c = -ScanMargin; c < columns + ScanMargin; ++c)
				{
					const auto [cellX, cellY] = ScanCell(anchor, r, c);
					if (cellX < INT16_MIN || cellX > INT16_MAX || cellY < INT16_MIN || cellY > INT16_MAX) continue;
					++count.Visited;
					const CellStruct coords {std::int16_t(cellX), std::int16_t(cellY)};
					if (!MapClass::Instance.CoordinatesLegal(coords)) continue;
					const auto* cell = MapClass::Instance.TryGetCellAt(coords);
					if (!cell) continue;
					++count.Legal;
					const bool clear = cell->IsoTileTypeIndex == ClearTileSlot;
					const int typeIndex = clear ? *reinterpret_cast<const int*>(ClearTile) : cell->IsoTileTypeIndex;
					if (typeIndex < 0 || typeIndex >= types.Count) { ++count.Tile; continue; }
					const auto* type = types.Items[typeIndex];
					const auto* image = imageOf(type);
					if (!image) { ++count.Image; continue; }
					const int height = std::uint8_t(cell->Height);
					const auto position = TilePosition(cellX, cellY, cell->Level, tacticalX, tacticalY, view.Y);
					// 47FF80: the base tile's piece, clamped to the last, decides how far an extra image rises.
					int rise = 0;
					if (!clear)
					{
						const int last = PieceCount(image) - 1;
						const auto* basePiece = PieceSlot(image, height >= last ? last : height);
						if (!basePiece) { ++count.Image; continue; }
						if (basePiece->Flags & HasExtra) rise = ExtraRise(basePiece->Y, basePiece->ExtraY);
					}
					if (!Intersects(Current.Clip, position.X, position.Y - rise, TileWidth, std::int64_t(CellHeight) + rise)) continue;
					const auto* convert = cell->LightConvert;
					const auto [table, inserted] = converts.try_emplace(convert);
					if (inserted && ValidConvert(convert)) table->second = AcquireTable(convert);
					if (!table->second) { ++count.Palette; continue; }
					const int lightRow = LightRow(std::int16_t(cell->Intensity_Terrain));
					// 480350 draws the tile, then the cell's smudge even where the tile drew nothing.
					const auto placeTile = [&]
					{
						// 480350: an alternate-variant piece (5471F0) takes the cell flag, otherwise 4814F0 picks.
						const int variants = type->unk_2F0;
						int variant = 0;
						bool pick = clear;
						if (!clear && variants > 1)
						{
							const auto* entry = PieceSlot(image, height % PieceCount(image));
							if (entry && (entry->Flags & AlternateVariant))
								variant = (std::uint32_t(cell->Flags) & AlternateVariantFlag) ? 1 : 0;
							else pick = true;
						}
						if (pick)
						{
							if (variants > 4 && !table8Ready) { ++count.Variant; return; }
							variant = PickVariant(coords.X, coords.Y, height, image->GridWidth, image->GridHeight, variants, table4, table8);
						}
						const int steps = VariantSteps(variant, variants);
						const IsometricTileTypeClass* drawn = type;
						for (int i = 0; i < steps && drawn; ++i)
							drawn = reinterpret_cast<const IsometricTileTypeClass*>(drawn->unk_2BC);
						if (steps < 0 || !drawn) { ++count.Variant; return; }
						const auto* drawnImage = imageOf(drawn);
						if (!drawnImage) { ++count.Image; return; }
						// 547CF0 draws nothing, extra image included, for an empty piece.
						const auto* piece = PieceSlot(drawnImage, (clear ? 0 : height) % PieceCount(drawnImage));
						if (!piece) { ++count.Empty; return; }
						const int shade = ShadeRow(convert->ShadeCount, lightRow, DefaultAlpha);
						auto frames = captured.find(piece);
						if (frames == captured.end()) frames = captured.emplace(piece, AcquirePiece(piece)).first;
						if (frames->second.Reason) { ++count.Frame; return; }
						if (position.X < INT_MIN || position.X > INT_MAX || position.Y < INT_MIN || position.Y > INT_MAX) return;
						++count.Drawn;
						if (!frames->second.UsesZ) ++count.NoDepth;
						count.MaxLevel = std::max(count.MaxLevel, int(cell->Level));
						const auto dx = position.X + TileWidth / 2 - (std::int64_t(view.X) + view.Width / 2);
						const auto dy = position.Y + CellHeight / 2 - (std::int64_t(view.Y) + view.Height / 2);
						if (dx * dx + dy * dy < sampleDistance)
						{
							sampleDistance = dx * dx + dy * dy;
							sample = {cellX, cellY, cell->IsoTileTypeIndex, typeIndex, height, cell->Level, variants, variant,
								drawn->ArrayIndex, std::int16_t(cell->Intensity_Terrain), convert->ShadeCount, shade, piece};
						}
						const auto depth = BaseDepth(zbuffer->MaxValue, zbuffer->Bounds.Y, int(position.Y), drawnImage->PixelHeight, cell->Level);
						const bool usesZ = frames->second.UsesZ;
						Current.Placements.push_back({frames->second.Main, table->second, int(position.X), int(position.Y), TileWidth, DiamondRows,
							lightRow, true, usesZ, depth});
						if (frames->second.Extra)
						{
							const auto x = position.X + std::int64_t(piece->ExtraX) - piece->X;
							const auto y = position.Y + std::int64_t(piece->ExtraY) - piece->Y;
							if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) return;
							++count.Extras;
							Current.Placements.push_back({frames->second.Extra, table->second, int(x), int(y), piece->ExtraWidth, piece->ExtraHeight,
								lightRow, false, usesZ, depth});
						}
					};
					placeTile();
					if (cell->SmudgeTypeIndex == -1) continue;
					// 6B55F0: frame 0 of the type's shape, lit like the tile, neither testing nor writing Z.
					const auto* smudge = smudgeShape(cell->SmudgeTypeIndex);
					TerrainImage::Point at {};
					if (!smudge || !smudge->ID || !SmudgePosition(position, cell->SmudgeData, smudge->Width, smudge->CanvasWidth, smudge->CanvasHeight,
						smudge->Frame.Left, smudge->Frame.Top, at) || at.X < INT_MIN || at.X > INT_MAX || at.Y < INT_MIN || at.Y > INT_MAX)
					{
						// A type without a shape draws nothing natively; anything else is a capture failure.
						if (!smudge || smudge->ID || smudge->Failed) ++count.SmudgeMissing;
						continue;
					}
					++count.Smudges;
					Current.Placements.push_back({smudge->ID, table->second, int(at.X), int(at.Y), smudge->Frame.Width, smudge->Frame.Height,
						lightRow, false, false, 0, &ShapeFrameSource::Frames});
				}
			pieces = captured.size();
			tables = Tables.Count();
			Current.Visible = true;
			reason = "ready";
		}
		ABufferSnapshot::Current = ABufferSnapshot::Capture(ScanMargin);
		if (!Trace || TraceCount >= 64) return;
		const Point tacticalNow {tactical ? tactical->TacticalPos.X : 0, tactical ? tactical->TacticalPos.Y : 0};
		const bool settled = tacticalNow.X == LastTactical.X && tacticalNow.Y == LastTactical.Y;
		LastTactical = tacticalNow;
		if (!settled) return;
		const auto& abuffer = ABufferSnapshot::Current;
		// Stage C3 probe: which 547CF0 Z path runs and the values its base depth starts from.
		const int accelerated = *reinterpret_cast<const std::uint8_t*>(AcceleratedDepth);
		const bool zvalid = zbuffer != nullptr;
		const int state[] {accelerated, zvalid ? zbuffer->MaxValue : 0, zvalid ? zbuffer->Bounds.Y : 0, count.NoDepth, count.MaxLevel, Current.Visible, view.X, view.Y, view.Width, view.Height, Current.OriginX,
			tactical ? tactical->TacticalPos.X : 0, tactical ? tactical->TacticalPos.Y : 0, anchor.X, anchor.Y, rows, columns,
			count.Visited, count.Legal, count.Drawn, count.Extras, count.Tile, count.Image, count.Variant, count.Empty,
			count.Palette, count.Frame, int(pieces), int(tables), sample.X, sample.Y, count.Smudges, count.SmudgeMissing};
		auto hash = SidebarSnapshot::Hash(state, sizeof(state));
		hash = SidebarSnapshot::Hash(&reason, sizeof(reason), hash);
		hash = SidebarSnapshot::Hash(&abuffer.Hash, sizeof(abuffer.Hash), hash);
		if (hash == LastTraceHash) return;
		LastTraceHash = hash;
		++TraceCount;
		const auto& stats = Frames.GetStats();
		Debug::Log("[TerrainGPU] sample=%u ready=%d reason=%s view=%d,%d,%d,%d origin=%d tactical=%d,%d anchor=%d,%d scan=%dx%d+%d "
			"visited=%d legal=%d drawn=%d extras=%d tile=%d image=%d variant=%d empty=%d palette=%d frame=%d smudges=%d smudgeMissing=%d pieces=%zu tables=%zu rows=%u "
			"cache=%zu/%zu bytes=%zu center=%d,%d:index=%d:type=%d:sub=%d:level=%d:variants=%d:variant=%d:drawn=%d:piece=%p:intensity=%d:shade=%d/%d\n",
			TraceCount, Current.Visible, reason, view.X, view.Y, view.Width, view.Height, Current.OriginX,
			tactical ? tactical->TacticalPos.X : 0, tactical ? tactical->TacticalPos.Y : 0, anchor.X, anchor.Y, rows, columns, ScanMargin,
			count.Visited, count.Legal, count.Drawn, count.Extras, count.Tile, count.Image, count.Variant, count.Empty,
			count.Palette, count.Frame, count.Smudges, count.SmudgeMissing, pieces, tables, Tables.UsedRows(), stats.Resources, stats.Frames, stats.IndexBytes, sample.X, sample.Y, sample.Index,
			sample.Type, sample.Height, sample.Level, sample.Variants, sample.Variant, sample.Drawn, sample.Piece, sample.Intensity,
			sample.Shade, sample.Shades);
		Debug::Log("[TerrainGPU] depth sample=%u accelerated=%d zbuffer=%p bounds=%d,%d,%d,%d maxValue=%d width=%d height=%d noDepth=%d maxLevel=%d\n",
			TraceCount, accelerated, zbuffer, zvalid ? zbuffer->Bounds.X : 0, zvalid ? zbuffer->Bounds.Y : 0,
			zvalid ? zbuffer->Bounds.Width : 0, zvalid ? zbuffer->Bounds.Height : 0, zvalid ? zbuffer->MaxValue : 0,
			zvalid ? zbuffer->Width : 0, zvalid ? zbuffer->Height : 0, count.NoDepth, count.MaxLevel);
		ABufferSnapshot::Trace(abuffer, TraceCount);
		if (!Current.Visible) return;
		const auto check = CheckDepth(zbuffer, view);
		Debug::Log("[TerrainGPU] depthcheck sample=%u compared=%d mismatched=%d rejected=%d untouched=%d missing=%d box=%d,%d,%d,%d first=%d,%d:native=%d:model=%d\n",
			TraceCount, check.Compared, check.Mismatched, check.Rejected, check.Untouched, check.Missing,
			check.Mismatched ? check.Left : 0, check.Mismatched ? check.Top : 0, check.Mismatched ? check.Right : 0, check.Mismatched ? check.Bottom : 0,
			check.FirstX, check.FirstY, check.Native, check.Model);
	}
}
