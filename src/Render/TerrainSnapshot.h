#pragma once

#include "SidebarSnapshot.h"
#include "TerrainBatch.h"
#include "TerrainImage.h"
#include <CellClass.h>
#include <ConvertClass.h>
#include <IsometricTileTypeClass.h>
#include <MapClass.h>
#include <TacticalClass.h>
#include <cstdio>
#include <unordered_map>

// The TMP tiles the terrain pass (6D2DE0 -> 6D7560 -> 480350 -> 547CF0) draws into
// DSurface::Tile, captured in the 4F4583 callback as a full redraw of the visible cells.
// Stage C2: main diamond and extra image in native order without the Z buffer, lit by
// Intensity_Terrain with the default ABuffer value. Smudges and every later Tile layer are absent.
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
	// Rows of the color table texture: 19 tables of 53 shades, or 37 of 27.
	constexpr std::uint32_t TableRows = 1024;
	static_assert(offsetof(ObjectTypeClass, Image) == 0xA4);
	static_assert(offsetof(IsometricTileTypeClass, unk_2BC) == 0x2BC);
	static_assert(offsetof(IsometricTileTypeClass, unk_2F0) == 0x2F0);
	static_assert(offsetof(CellClass, MapCoords) == 0x24);
	static_assert(offsetof(CellClass, LightConvert) == 0x34);
	static_assert(offsetof(CellClass, IsoTileTypeIndex) == 0x38);
	static_assert(offsetof(CellClass, Intensity_Terrain) == 0x10C);
	static_assert(offsetof(CellClass, Height) == 0x11A);
	static_assert(offsetof(CellClass, Level) == 0x11B);
	static_assert(offsetof(CellClass, Flags) == 0x140);
	static_assert(offsetof(ConvertClass, ShadeCount) == 0x16C);
	static_assert(offsetof(ConvertClass, FullColorData) == 0x170);

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

	struct Placement
	{
		FrameCache::FrameID ID;
		TerrainBatch::ColorTables::Handle Table;
		int X, Y, Width, Height;
		// 547CF0's row of the alpha lighting remap; the shader combines it with the ABuffer value.
		int LightRow;
		bool Diamond;
	};
	struct Frame
	{
		bool Visible = false;
		// Canvas X of Tile's origin and the native clip, ViewBounds, in Tile coordinates.
		int OriginX = 0;
		RectangleStruct Clip {};
		std::vector<Placement> Placements;
	};
	inline Frame Current;
	inline FrameCache::Cache Frames({64 * 1024 * 1024, 16384, 8192});
	inline TerrainBatch::ColorTables Tables(TableRows);
	inline std::unordered_map<std::uintptr_t, std::uint64_t> PieceHashes;
	// 0: not checked yet, 1: the template is the analytic diamond, 2: it is not.
	inline int TemplateState = 0;
	inline bool Trace = false;
	inline unsigned TraceCount = 0;
	// Capture cost and what the presents drew, logged every PerfWindow captures.
	constexpr unsigned PerfWindow = 600;
	struct Perf
	{
		std::uint64_t Captures = 0, Ticks = 0, MaxTicks = 0, Presents = 0, Draws = 0, Instances = 0;
		unsigned Lines = 0;
	};
	inline Perf Stats;
	inline std::uint64_t LastTraceHash = 0;

	inline void Invalidate()
	{
		Current.Visible = false;
		Current.Placements.clear();
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
		if (!CursorSnapshot::Readable(image, sizeof(*image))) return nullptr;
		const auto pieces = std::int32_t(std::uint32_t(image->GridWidth) * std::uint32_t(image->GridHeight));
		return pieces > 0 && CursorSnapshot::Readable(image + 1, size_t(pieces) * sizeof(void*)) ? image : nullptr;
	}

	inline int PieceCount(const TileImageHeader* image)
	{
		return std::int32_t(std::uint32_t(image->GridWidth) * std::uint32_t(image->GridHeight));
	}

	inline const Piece* PieceSlot(const TileImageHeader* image, int index)
	{
		return reinterpret_cast<const Piece* const*>(image + 1)[index];
	}

	struct PieceFrames { FrameCache::FrameID Main, Extra; const char* Reason = nullptr; };
	struct Counters { int Visited = 0, Legal = 0, Drawn = 0, Extras = 0, Tile = 0, Image = 0, Variant = 0, Empty = 0, Palette = 0, Frame = 0; };
	// The drawn cell nearest the view center, for the trace.
	struct Sample
	{
		int X = -1, Y = -1, Index = -1, Type = -1, Height = -1, Level = 0, Variants = 0, Variant = 0, Drawn = -1;
		int Intensity = 0, Shades = 0, Shade = -1;
		const void* Piece = nullptr;
	};

	// Copies the piece into the frame cache. A changed piece at the same address replaces the old frames.
	inline PieceFrames AcquirePiece(const Piece* piece)
	{
		const auto* packed = reinterpret_cast<const std::uint8_t*>(piece + 1);
		const std::uint8_t* extra = nullptr;
		const bool hasExtra = piece->Flags & HasExtra;
		if (hasExtra)
		{
			const auto base = reinterpret_cast<std::uintptr_t>(piece);
			if (piece->ExtraWidth <= 0 || piece->ExtraHeight <= 0 || piece->ExtraWidth > ShapeImage::MaxDimension
				|| piece->ExtraHeight > ShapeImage::MaxDimension || piece->ExtraData > UINTPTR_MAX - base) return {{}, {}, "extra"};
			extra = reinterpret_cast<const std::uint8_t*>(base + piece->ExtraData);
			if (!CursorSnapshot::Readable(extra, size_t(piece->ExtraWidth) * piece->ExtraHeight)) return {{}, {}, "extra"};
		}
		auto hash = SidebarSnapshot::Hash(piece, sizeof(*piece) + DiamondPixels);
		if (extra) hash = SidebarSnapshot::Hash(extra, size_t(piece->ExtraWidth) * piece->ExtraHeight, hash);
		const auto identity = reinterpret_cast<std::uintptr_t>(piece);
		auto resource = Frames.LookupResource(identity);
		if (const auto known = PieceHashes.find(identity); known != PieceHashes.end() && known->second != hash && resource)
		{
			Frames.Invalidate(resource);
			resource = {};
		}
		if (!resource)
		{
			resource = Frames.Register(identity, {TileWidth, DiamondRows, {{0, 0, TileWidth, DiamondRows},
				{0, 0, hasExtra ? piece->ExtraWidth : 0, hasExtra ? piece->ExtraHeight : 0}}});
			if (!resource)
			{
				// Over budget: start again from the pieces later captures still use. Pieces placed
				// earlier in this capture no longer resolve and are skipped for one frame.
				ClearPieces();
				return {{}, {}, "budget"};
			}
		}
		PieceHashes[identity] = hash;
		const auto main = Frames.GetOrDecode(resource, 0, [packed](const auto&, auto& indices)
		{
			indices.resize(size_t(TileWidth) * DiamondRows);
			ExpandDiamond(packed, indices.data());
			return FrameCache::Status::Ready;
		});
		if (main.State != FrameCache::Status::Ready) return {{}, {}, "frame"};
		PieceFrames result {main.Frame->ID};
		if (extra)
		{
			const auto copy = Frames.GetOrDecode(resource, 1, [extra](const auto& d, auto& indices)
			{
				indices.assign(extra, extra + size_t(d.Width) * d.Height);
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
		return CursorSnapshot::Readable(convert, sizeof(ConvertClass)) && convert->BytesPerPixel == 2
			&& convert->ShadeCount > 0 && convert->ShadeCount <= 256;
	}

	// The whole FullColorData of a LightConvert: every shade row, so the shader can pick any.
	inline TerrainBatch::ColorTables::Handle AcquireTable(const LightConvertClass* convert)
	{
		const auto* colors = static_cast<const std::uint16_t*>(convert->FullColorData);
		if (!CursorSnapshot::Readable(colors, size_t(convert->ShadeCount) * 256 * sizeof(std::uint16_t))) return {};
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

	inline void CaptureAfterDraw()
	{
		const CaptureTimer timer;
		Current.Visible = false;
		Current.Placements.clear();
		const char* reason = nullptr;
		const TacticalClass* tactical = TacticalClass::Instance;
		const auto& view = DSurface::ViewBounds;
		const auto& types = IsometricTileTypeClass::Array;
		if (!TemplateState)
			TemplateState = TemplateMatches(reinterpret_cast<const std::uint8_t*>(RowTemplate),
				reinterpret_cast<const std::int32_t*>(RowOffsets)) ? 1 : 2;
		if (!Eligible()) reason = "ineligible";
		else if (TemplateState != 1) reason = "template";
		else if (view.Width <= 0 || view.Height <= 0 || view.Width > SidebarSnapshot::MaxCanvasDimension
			|| view.Height > SidebarSnapshot::MaxCanvasDimension) reason = "view";
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
			// Each type, piece and color table is checked once per capture.
			std::unordered_map<const IsometricTileTypeClass*, const TileImageHeader*> images;
			std::unordered_map<const Piece*, bool> readablePieces;
			std::unordered_map<const LightConvertClass*, TerrainBatch::ColorTables::Handle> converts;
			std::unordered_map<const Piece*, PieceFrames> captured;
			const auto imageOf = [&](const IsometricTileTypeClass* type)
			{
				const auto [it, inserted] = images.try_emplace(type);
				if (inserted) it->second = ImageOf(type);
				return it->second;
			};
			const auto readable = [&](const Piece* piece)
			{
				const auto [it, inserted] = readablePieces.try_emplace(piece);
				if (inserted) it->second = CursorSnapshot::Readable(piece, sizeof(*piece) + DiamondPixels);
				return it->second;
			};
			for (int r = -ScanMargin; r < rows + ScanMargin; ++r)
				for (int c = -ScanMargin; c < columns + ScanMargin; ++c)
				{
					const int cellX = anchor.X - 2 + FloorHalf(r) + c, cellY = anchor.Y + FloorHalf(r + 1) - c;
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
						if (!basePiece || !readable(basePiece)) { ++count.Image; continue; }
						if (basePiece->Flags & HasExtra) rise = ExtraRise(basePiece->Y, basePiece->ExtraY);
					}
					if (!Intersects(Current.Clip, position.X, position.Y - rise, TileWidth, std::int64_t(CellHeight) + rise)) continue;
					// 480350: an alternate-variant piece (5471F0) takes the cell flag, otherwise 4814F0 picks.
					const int variants = type->unk_2F0;
					int variant = 0;
					bool pick = clear;
					if (!clear && variants > 1)
					{
						const auto* entry = PieceSlot(image, height % PieceCount(image));
						if (entry && !readable(entry)) { ++count.Image; continue; }
						if (entry && (entry->Flags & AlternateVariant))
							variant = (std::uint32_t(cell->Flags) & AlternateVariantFlag) ? 1 : 0;
						else pick = true;
					}
					if (pick)
					{
						if (variants > 4 && !table8Ready) { ++count.Variant; continue; }
						variant = PickVariant(coords.X, coords.Y, height, image->GridWidth, image->GridHeight, variants, table4, table8);
					}
					const int steps = VariantSteps(variant, variants);
					const IsometricTileTypeClass* drawn = type;
					for (int i = 0; i < steps && drawn; ++i)
						drawn = reinterpret_cast<const IsometricTileTypeClass*>(drawn->unk_2BC);
					if (steps < 0 || !drawn) { ++count.Variant; continue; }
					const auto* drawnImage = imageOf(drawn);
					if (!drawnImage) { ++count.Image; continue; }
					// 547CF0 draws nothing, extra image included, for an empty piece.
					const auto* piece = PieceSlot(drawnImage, (clear ? 0 : height) % PieceCount(drawnImage));
					if (!piece) { ++count.Empty; continue; }
					if (!readable(piece)) { ++count.Image; continue; }
					const auto* convert = cell->LightConvert;
					const auto [table, inserted] = converts.try_emplace(convert);
					if (inserted && ValidConvert(convert)) table->second = AcquireTable(convert);
					if (!table->second) { ++count.Palette; continue; }
					const int lightRow = LightRow(std::int16_t(cell->Intensity_Terrain));
					const int shade = ShadeRow(convert->ShadeCount, lightRow, DefaultAlpha);
					auto frames = captured.find(piece);
					if (frames == captured.end()) frames = captured.emplace(piece, AcquirePiece(piece)).first;
					if (frames->second.Reason) { ++count.Frame; continue; }
					if (position.X < INT_MIN || position.X > INT_MAX || position.Y < INT_MIN || position.Y > INT_MAX) continue;
					++count.Drawn;
					const auto dx = position.X + TileWidth / 2 - (std::int64_t(view.X) + view.Width / 2);
					const auto dy = position.Y + CellHeight / 2 - (std::int64_t(view.Y) + view.Height / 2);
					if (dx * dx + dy * dy < sampleDistance)
					{
						sampleDistance = dx * dx + dy * dy;
						sample = {cellX, cellY, cell->IsoTileTypeIndex, typeIndex, height, cell->Level, variants, variant,
							drawn->ArrayIndex, std::int16_t(cell->Intensity_Terrain), convert->ShadeCount, shade, piece};
					}
					Current.Placements.push_back({frames->second.Main, table->second, int(position.X), int(position.Y), TileWidth, DiamondRows, lightRow, true});
					if (frames->second.Extra)
					{
						const auto x = position.X + std::int64_t(piece->ExtraX) - piece->X;
						const auto y = position.Y + std::int64_t(piece->ExtraY) - piece->Y;
						if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX) continue;
						++count.Extras;
						Current.Placements.push_back({frames->second.Extra, table->second, int(x), int(y), piece->ExtraWidth, piece->ExtraHeight, lightRow, false});
					}
				}
			pieces = captured.size();
			tables = Tables.Count();
			Current.Visible = true;
			reason = "ready";
		}
		if (!Trace || TraceCount >= 64) return;
		const int state[] {Current.Visible, view.X, view.Y, view.Width, view.Height, Current.OriginX,
			tactical ? tactical->TacticalPos.X : 0, tactical ? tactical->TacticalPos.Y : 0, anchor.X, anchor.Y, rows, columns,
			count.Visited, count.Legal, count.Drawn, count.Extras, count.Tile, count.Image, count.Variant, count.Empty,
			count.Palette, count.Frame, int(pieces), int(tables), sample.X, sample.Y};
		auto hash = SidebarSnapshot::Hash(state, sizeof(state));
		hash = SidebarSnapshot::Hash(&reason, sizeof(reason), hash);
		if (hash == LastTraceHash) return;
		LastTraceHash = hash;
		++TraceCount;
		const auto& stats = Frames.GetStats();
		Debug::Log("[TerrainGPU] sample=%u ready=%d reason=%s view=%d,%d,%d,%d origin=%d tactical=%d,%d anchor=%d,%d scan=%dx%d+%d "
			"visited=%d legal=%d drawn=%d extras=%d tile=%d image=%d variant=%d empty=%d palette=%d frame=%d pieces=%zu tables=%zu rows=%u "
			"cache=%zu/%zu bytes=%zu center=%d,%d:index=%d:type=%d:sub=%d:level=%d:variants=%d:variant=%d:drawn=%d:piece=%p:intensity=%d:shade=%d/%d\n",
			TraceCount, Current.Visible, reason, view.X, view.Y, view.Width, view.Height, Current.OriginX,
			tactical ? tactical->TacticalPos.X : 0, tactical ? tactical->TacticalPos.Y : 0, anchor.X, anchor.Y, rows, columns, ScanMargin,
			count.Visited, count.Legal, count.Drawn, count.Extras, count.Tile, count.Image, count.Variant, count.Empty,
			count.Palette, count.Frame, pieces, tables, Tables.UsedRows(), stats.Resources, stats.Frames, stats.IndexBytes, sample.X, sample.Y, sample.Index,
			sample.Type, sample.Height, sample.Level, sample.Variants, sample.Variant, sample.Drawn, sample.Piece, sample.Intensity,
			sample.Shade, sample.Shades);
	}
}
