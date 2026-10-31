#pragma once

#include "BlitFrameSource.h"
#include "BuildingImage.h"
#include "GlyphCache.h"
#include "OverlayImage.h"
#include "ShapeDepth.h"
#include "TerrainObjectImage.h"
#include "TerrainSnapshot.h"
#include <BuildingClass.h>
#include <ColorScheme.h>
#include <FileSystem.h>
#include <HouseClass.h>
#include <OverlayTypeClass.h>
#include <TerrainClass.h>
#include <TiberiumClass.h>
#include <Ext/OverlayType/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/Tiberium/Body.h>

// The overlay layer of the tactical Tile pass (gamemd-reverse #18): 6D6D10 draws every overlay
// body (47F6A0), then every overlay shadow (47F510), each through 4AED70 with a Z test, after the
// TMP tiles and smudges TerrainSnapshot captures. Captured in the same 4F4583 callback for a full
// redraw of the visible cells. The static Terrain objects with their own shadows (6D97D0, 71C1B0)
// and the cliff shadows (6D7C00, 547230) of gamemd-reverse #19 follow in the same list, in the native
// order (C6). TerrainPass draws the list after the tiles (C5.2c); the trace replays the captured
// tiles, smudges and draws in software and compares the result with the native Tile and ZBuffer
// (C5.2b). Rubble and the deferred records of 6D3470 other than buildings are absent. The live
// buildings of step 8 (gamemd-reverse #22) follow the cliff shadows in the Tile list (D1); the building
// records of 4D1890 (step 4) come first, before the overlays (D1c). What RenderLayers (6D8DB0) draws on
// Composite in mode 2 through 4AED70 (animated Terrain, anims, units, bullets and their extras,
// gamemd-reverse #23, #26, #27) is recorded call by call as it draws, with the voxels' blits of 4373B0 and
// 437A10 (D3a-3), its lines and fills (4BFD30, 7BA610, 4BB620) and its text's glyphs (434120), and turned into a
// list of its own in the same callback, which the trace replays over CompositeSnapshot's copies (D3); the lines
// and glyphs only the GPU draws.
namespace OverlaySnapshot
{
	// 817710: the Z gradient records; AA105C: the slope Z shapes; 81CC30: the bridge pattern.
	constexpr std::uintptr_t Gradients = 0x817710, SlopeShapes = 0xAA105C, BridgePattern = 0x81CC30;
	constexpr int GradientCount = 4;
	constexpr std::uint32_t BridgeFlag = 0x80;
	// 4AED70's brightness for Tiberium and shadows.
	constexpr int FullBrightness = 1000;
	// 47F6A0's ZAdjust is -2 - height; an overlay that is not flat or a rock sits 15 nearer.
	constexpr int BodyDepth = -2, RaisedDepth = -15;
	// 47F6A0 draws a veins slope's Z shape moved by this.
	constexpr int VeinsZShapeX = 30, VeinsZShapeY = -2;
	// The SHP frame flag of an RLE frame.
	constexpr unsigned CompressedFrame = 2;
	// 545150's C_SHADOW.SHP; 547230's special range starts, their records, the starts of the regular
	// ranges and their records.
	constexpr std::uintptr_t CliffShadows = 0xABC554, CliffSpecialBase = 0xABC1F8, CliffFallbackBase = 0xAA1098,
		CliffSpecialRecords = 0xABC210, CliffRangeStarts = 0xAA102C, CliffRecords = 0xABC2D0;
	// 6D20E0 (screen height of a Z) and 6D2140 (client position of coordinates), which 71C1B0 and
	// 71CC50 call.
	constexpr std::uintptr_t AdjustForZ = 0x6D20E0, CoordsToClient = 0x6D2140;
	// 6D97D0 draws the objects of this layer.
	constexpr int GroundLayer = 2;
	// 4D1890's FoggedObject records: the (key, record) pairs, their count and whether they are sorted
	// (else it sorts them with 4D2ED0 first), and a record's owner, kind, saved Location, rectangle,
	// parts, part count and enabled byte. Only the building records (kind 6) are drawn (D1c); with
	// FogOfWar on, the copy makes no overlay (14h), smudge (1Dh) or Terrain (24h) record, so the trace
	// only counts them.
	constexpr std::uintptr_t FoggedPairs = 0x8B3CC0, FoggedCount = 0x8B3CC4, FoggedSorted = 0x8B3CCC, FoggedCompare = 0x4D2ED0;
	constexpr size_t FoggedOwner = 0x28, FoggedKind = 0x30, FoggedLocation = 0x34, FoggedBounds = 0x40, FoggedParts = 0x60, FoggedPartCount = 0x6C,
		FoggedEnabled = 0x74, FoggedRecordSize = 0x78;
	constexpr int FoggedBuilding = 6;
	// A record's part: the building type and its frame, then each anim's type, frame and ZAdjust; the
	// flag marks a laser fence or firestorm body.
	struct FoggedPart
	{
		void* Type;
		int Frame;
		bool Flat;
		int ZAdjust;
	};
	static_assert(sizeof(FoggedPart) == 0x10 && offsetof(FoggedPart, ZAdjust) == 0xC);
	// 6D8DB0 walks the layers at 8A0360.
	constexpr int LayerCount = 5;
	// 43D290's BUILDNGZ.SHA (frame 0, already less 41h), the projection of a Foundation corner
	// (6D1FE0), and the Foundation width and height (45EC90, 45ECA0, which Ares takes over), read
	// as the whole EAX.
	constexpr std::uintptr_t BuildingZShape = 0x89DDBC, ZShapeMove = 0x6D1FE0, FoundationWidth = 0x45EC90, FoundationHeight = 0x45ECA0;
	// 4AED70's flags: Darken (a shadow), the translucency bits, Centered, 400h (which every caller sets),
	// Alpha, the Z test and the Z write. A recorded call with any other flag, a remap, a Z shape frame
	// other than 0 or without Centered is not modeled.
	constexpr std::uint32_t ShadowFlag = 0x1, TranslucencyMask = 0x6, CenteredFlag = 0x200, AlphaFlag = 0x800, ZReadMask = 0x3000,
		ZReadFlag = 0x2000, ZWriteFlag = 0x4000, ModeledFlags = ShadowFlag | TranslucencyMask | CenteredFlag | 0x400 | AlphaFlag | ZReadMask | ZWriteFlag;
	// 4AED70's own blits, which its record covers.
	constexpr std::uintptr_t DrawShapeBegin = 0x4AED70, DrawShapeEnd = 0x4AF295;
	// ConvertClass' Blitters and RLEBlitters tables.
	constexpr size_t PlainBlitters = 50, RLEBlitters = 39;

	// What a draw belongs to: an overlay (6D6D10), a Terrain object (6D97D0), a cliff (6D7C00), a
	// building (6D9920), a fogged building record's building or anim part (4D1890), or a call or blit
	// recorded during RenderLayers (its Index the return address).
	enum class Layer : std::uint8_t { Overlay, Terrain, Cliff, Building, Fogged, FoggedAnim, Composite };

	// One 4AED70 call: a frame of ShapeFrameSource's cache with the blitter's parameters, clipped
	// against the clip of the capture as 4373B0 / 437A10 do.
	struct Draw
	{
		FrameCache::FrameID ID;
		// The cache the frame lives in: ShapeFrameSource's for a shape, BlitFrameSource's for a blit.
		const FrameCache::Cache* Storage = &ShapeFrameSource::Frames;
		TerrainBatch::ColorTables::Handle Table;
		// The first drawn pixel on Tile and in the frame, and the drawn columns and rows.
		int X = 0, Y = 0, Width = 0, Height = 0, FrameX = 0, FrameY = 0;
		int LightRow = 0;
		// Drawn row j's depth base is ShapeDepth::RowBase(Rows, Gradient, j).
		ShapeDepth::Rows Rows;
		ShapeDepth::Gradient Gradient {};
		bool Compressed = false;
		// The shadow blitter (4601) halves the old color with Mask; the body one (4E00) draws, mixed with
		// the old color by the translucency bits (2, 4, 6 of the flags) through Mask and QuarterMask.
		bool Shadow = false;
		std::uint32_t Mask = 0, QuarterMask = 0;
		std::uint8_t Translucency = 0;
		// The translucent draw goes through a blitter Phobos' FixTransparencyBlitters patched
		// (ShapeDepth::RoundedBlend).
		bool Rounded = false;
		// An RLE frame with a Z shape: the byte 437A10 subtracts from each drawn pixel's depth,
		// Width x Height.
		bool ZShaped = false;
		std::vector<std::uint8_t> ZBytes;
		// 4AED70's Z test and write: the Tile pass writes Z (4E00, 4601), mode 2's Terrain only tests it
		// (2E00), a standalone anim's shadow (E01) does neither.
		bool TestsDepth = true, WritesDepth = true;
		// The tint a body ORs into each color (705E00's); only the Alpha blitters that test Z apply it.
		std::uint16_t Tint = 0;
		// For the trace: the layer, the overlay, Terrain type or tile type index, and the shape's frame.
		Layer Source = Layer::Overlay;
		int ZAdjust = 0, GradientIndex = 0, CellX = 0, CellY = 0, Index = -1, ShapeFrame = 0;
	};
	struct Counters
	{
		int Visited = 0, Legal = 0, Overlays = 0, Bodies = 0, Shadows = 0, NoBody = 0, Outside = 0, Empty = 0, Rubble = 0,
			Tiberium = 0, Palette = 0, Frame = 0, ZShape = 0, Ramps = 0, Bridges = 0, Walls = 0, Compressed = 0, NoImage = 0, Unloaded = 0, Slopes = 0, Unsupported = 0;
		// Ground layer objects that are Terrain, those drawn later on Composite, those off the clip,
		// the static ones drawn with their damaged frame, and the cells whose tile casts a cliff shadow.
		int Terrains = 0, Dynamic = 0, TerrainOutside = 0, Damaged = 0, Casters = 0;
		// 4D1890's records, the building ones and the others, and the enabled ones whose saved rectangle
		// meets the clip; the anim parts of the building records drawn, those above the detail level, the
		// records of an InvisibleInGame type, and the captures where the records were not sorted.
		int Fogged = 0, FoggedBuildings = 0, FoggedBuildingsInView = 0, FoggedOthersInView = 0, FoggedAnims = 0, FoggedDetail = 0,
			FoggedInvisible = 0, FoggedUnsorted = 0;
		// 6D8DB0: the objects of its layers, and those it marked IsVisible (it clears the flag of each and
		// sets it where it draws) with those of them whose rectangle (GetRenderDimensions) is empty.
		int LayerObjects = 0, OthersVisible = 0, OthersUnbounded = 0;
		// The 4AED70 calls of RenderLayers: on its target, on another surface (a vehicle's scratch), not
		// modeled, and those translucent, writing Z and tinted among the draws made of them and the blits'.
		int Calls = 0, CallsElsewhere = 0, CallsUnmodeled = 0, CallsTranslucent = 0, CallsWriting = 0, CallsTinted = 0;
		// Its blits outside 4AED70 (4373B0, 437A10): on its target, elsewhere, and not modeled.
		int Blits = 0, BlitsElsewhere = 0, BlitsUnmodeled = 0;
		// Its lines and fills (4BFD30, 7BA610, 4BB620): on its target, elsewhere, and those on it that draw nothing;
		// its glyphs (434120): on its target, elsewhere, and those whose bitmap was not taken.
		int Lines = 0, LinesElsewhere = 0, LinesEmpty = 0, Glyphs = 0, GlyphsElsewhere = 0, GlyphsUnknown = 0;
		// 6D9920: the buildings of the ground layer, those fogged, off the clip, in a state the capture
		// does not draw (another visual, warping, disguised), gates moving, buildings unloading and the bibs
		// and doors drawn.
		int Buildings = 0, BuildingsFogged = 0, BuildingsOutside = 0, BuildingsUnsupported = 0, Gates = 0, Unloading = 0, Bibs = 0, Doors = 0;
	};
	// What a line or fill of RenderLayers draws (Primitive): a line tested against Z and lit by A (4BFD30), a
	// plain line (7BA610), a filled rectangle (4BB620), or a BitFont glyph (434120) in one color.
	enum class Primitive : std::uint8_t { Line, PlainLine, Fill, Glyph };
	// A line from (X0, Y0) to (X1, Y1) on the surface whose depth goes from Depth0 to Depth1 (4BFD30 only), or a
	// fill or glyph from its first pixel (X0, Y0) to the one past its last (X1, Y1), drawn only inside Clip; drawn before
	// CompositeDraws[Before]. The GPU draws it approximately (not pixel for pixel like the native) and the trace
	// does not replay it.
	struct Line
	{
		Primitive Kind = Primitive::Line;
		RectangleStruct Clip {};
		int X0 = 0, Y0 = 0, X1 = 0, Y1 = 0, Depth0 = 0, Depth1 = 0;
		// A glyph's 1-bit mask in SidebarText's GlyphCache.
		GlyphCache::GlyphID Glyph;
		std::uint16_t Color = 0;
		bool WritesDepth = false;
		size_t Before = 0;
		std::uintptr_t Caller = 0;
	};
	struct Frame
	{
		bool Visible = false;
		std::vector<Draw> Draws;
		Counters Count;
		// 6D8DB0's draws on Composite, in its order, its lines and fills among them, and their counters.
		std::vector<Draw> CompositeDraws;
		std::vector<Line> CompositeLines;
		Counters CompositeCount;
		// For the trace: the rectangles of those other objects on the clip's surface, where the layers
		// drew what the replay does not (voxels, lines).
		std::vector<RectangleStruct> OtherBounds;
	};
	extern Frame Current;
	extern unsigned TraceCount;
	extern bool RedrawRequested;

	// One 4AED70 call made during RenderLayers, as passed, with its clip already cut to the surface
	// (421B60), the caller's return address and whether it drew on RenderLayers' target.
	struct RecordedCall
	{
		const ConvertClass* Convert = nullptr;
		const SHPStruct* Image = nullptr;
		int Frame = 0;
		Point2D Point {};
		RectangleStruct Bounds {};
		std::uint32_t Flags = 0;
		int Remap = 0, ZAdjust = 0, Gradient = 0, Brightness = 0, Tint = 0;
		const SHPStruct* ZShape = nullptr;
		int ZShapeFrame = 0, ZShapeX = 0, ZShapeY = 0;
		std::uintptr_t Caller = 0;
		bool OnTarget = true;
	};
	// A blit of RenderLayers outside 4AED70 as 4373B0 and 437A10 take it (gamemd-reverse #26 §5, §9): the
	// clip on the destination, the place relative to it, the source surface, the source's rectangle (a
	// plain blit's surface part, an RLE blit's frame) and the part of it to draw, the blitter, Z, gradient,
	// brightness and tint; 437A10 may add a Z adjust surface.
	struct BlitCall
	{
		DSurface* Surface = nullptr;
		const RectangleStruct* Bounds = nullptr;
		const RectangleStruct* Dest = nullptr;
		::Surface* Source = nullptr;
		const RectangleStruct* Limit = nullptr;
		const RectangleStruct* Part = nullptr;
		const void* Blitter = nullptr;
		int ZAdjust = 0, Gradient = 0, Brightness = 0, Tint = 0;
		const void* ZSurface = nullptr;
		std::uintptr_t Caller = 0;
		bool Compressed = false;
	};
	// Why a recorded blit is not modeled: its blitter is no known convert's or stands for other flags,
	// its source is not an 8-bit surface apart from the target (or 437A10 cuts its frame or adjusts Z
	// by a surface), its clip leaves the surface, or its image could not be kept.
	enum class BlitIssue : std::uint8_t { None, Blitter, Source, Bounds, Image };
	// What a blit drew, taken at the call: the convert and the flags its blitter stands for, the image in
	// BlitFrameSource (a plain blit's drawn pixels, an RLE blit's whole frame), and the clipping 7BBE20
	// made of it.
	struct RecordedBlit
	{
		const ConvertClass* Convert = nullptr;
		std::uint32_t Flags = 0;
		FrameCache::FrameID ID;
		bool Compressed = false;
		RectangleStruct Bounds {};
		ShapeDepth::Rect Dest {};
		ShapeDepth::Clipped Clipped {};
		int ZAdjust = 0, Gradient = 0, Brightness = 0, Tint = 0;
		std::uintptr_t Caller = 0;
		bool OnTarget = true;
		BlitIssue Issue = BlitIssue::None;
	};

	// A line or fill of RenderLayers as passed: the clip, the line's ends relative to it (a fill's rectangle
	// relative to it as X, Y, width and height), the 16-bit color, and for 4BFD30 the Z at each end and whether
	// it writes Z. A glyph instead has its place on the surface in From, its size in Fill's width and height and
	// the font's bounds as its clip, with no ID where its bitmap was not taken. Taken on the surface (OnTarget)
	// where it drew on RenderLayers' target.
	struct RecordedLine
	{
		Primitive Kind = Primitive::Line;
		RectangleStruct Clip {}, Fill {};
		Point2D From {}, To {};
		std::uint16_t Color = 0;
		int ZFrom = 0, ZTo = 0;
		bool WritesDepth = false;
		GlyphCache::GlyphID Glyph;
		std::uintptr_t Caller = 0;
		bool OnTarget = true;
	};

	// The surface RenderLayers draws on while its calls are recorded (between 6D465F and 6D4664 of a
	// capturing render), else null; the hooks at 4AED70, 4373B0 and 437A10 record through RecordCall and
	// RecordBlit, those at 4BFD30, 7BA610 and 4BB620 through RecordLine, the glyphs of 434120 too.
	extern DSurface* RecordingTarget;
	void BeginRecording(DSurface* target);
	void EndRecording();
	void RecordCall(RecordedCall call, DSurface* surface);
	void RecordBlit(const BlitCall& call);
	void RecordLine(RecordedLine line, DSurface* surface);

	inline void Invalidate() { Current = {}; }

	// A frame of a shape for one capture, with the shape's canvas. Not Ready where the shape is not
	// loaded or the frame cannot be decoded.
	struct Shape
	{
		FrameCache::FrameID ID;
		FrameCache::Descriptor Frame;
		int CanvasWidth = 0, CanvasHeight = 0;
		bool Ready = false;
		// The shape's loaded data, which the frame's Offset is relative to.
		std::uintptr_t Data = 0;
	};

	// A shape reference whose file the native draw could not load: 4AED70 draws nothing for it.
	constexpr int UnloadedShape = -2;

	// The frames of a loaded shape, UnloadedShape for a reference that is not loaded and -1 where
	// the shape cannot be read.
	inline int FrameCount(const SHPStruct* image)
	{
		if (!CursorSnapshot::Readable(image, sizeof(SHPStruct))) return -1;
		if (image->IsReference())
		{
			const auto* reference = image->AsReference();
			if (!CursorSnapshot::Readable(reference, sizeof(*reference))) return -1;
			if (!reference->Loaded) return UnloadedShape;
			image = reference->Data;
			if (!CursorSnapshot::Readable(image, sizeof(SHPStruct))) return -1;
		}
		return image->Frames;
	}

	// The loaded data of a shape, null where it cannot be read.
	inline const SHPStruct* ShapeData(const SHPStruct* image)
	{
		if (!CursorSnapshot::Readable(image, sizeof(SHPStruct))) return nullptr;
		if (image->IsReference())
		{
			const auto* reference = image->AsReference();
			if (!CursorSnapshot::Readable(reference, sizeof(*reference)) || !reference->Loaded) return nullptr;
			image = reference->Data;
		}
		return CursorSnapshot::Readable(image, sizeof(SHPStruct)) && !image->IsReference() ? image : nullptr;
	}

	// 69E7E0 gives a frame outside the shape an empty rectangle, which draws nothing: such a frame
	// is Ready with no ID and no size.
	inline Shape AcquireShape(const SHPStruct* image, int frame)
	{
		if (!image) return {};
		const int frames = FrameCount(image);
		if (frames < 0) return {};
		if (std::uint32_t(frame) >= std::uint32_t(frames)) return {{}, {}, 0, 0, true};
		std::uintptr_t identity = 0;
		const auto result = ShapeFrameSource::Acquire(image, frame, identity);
		if (result.State != FrameCache::Status::Ready)
		{
			// An empty frame does not decode, and draws nothing either.
			const auto* shape = identity ? ShapeFrameSource::Frames.Describe(ShapeFrameSource::Frames.LookupResource(identity)) : nullptr;
			if (shape && size_t(frame) < shape->Frames.size() && (shape->Frames[frame].Width <= 0 || shape->Frames[frame].Height <= 0))
				return {{}, {}, 0, 0, true};
			return {};
		}
		const auto* info = ShapeFrameSource::Frames.Describe(result.Frame->Resource);
		if (!info) return {};
		return {result.Frame->ID, result.Frame->Geometry, info->Width, info->Height, true, identity};
	}

	// 5FDD20: the Tiberium an overlay type belongs to by its flat or slope images, -1 for an overlay
	// that is not Tiberium and 0 where no Tiberium claims it.
	inline int TiberiumOf(int index)
	{
		const auto* type = OverlayTypeClass::Array.Items[index];
		if (!type->Tiberium) return -1;
		for (const auto* tiberium : TiberiumClass::Array)
		{
			if (!CursorSnapshot::Readable(tiberium, sizeof(TiberiumClass)) || !tiberium->Image) return -1;
			const int first = tiberium->Image->ArrayIndex;
			if (index >= first && index < first + tiberium->NumImages) return tiberium->ArrayIndex;
			if (index >= first + tiberium->NumImages && index < first + tiberium->NumImages + tiberium->NumSlopes) return tiberium->ArrayIndex;
		}
		return 0;
	}

	// Phobos' HasRampArt: the overlay after the Tiberium's frames is Tiberium too.
	inline bool HasRampArt(const TiberiumClass* tiberium)
	{
		if (!tiberium->Image) return false;
		const int ramp = tiberium->Image->ArrayIndex + tiberium->NumFrames;
		if (ramp < 0 || ramp >= OverlayTypeClass::Array.Count) return false;
		const auto* type = OverlayTypeClass::Array.Items[ramp];
		return type && type->Tiberium;
	}

	inline const ConvertClass* SchemeConvert(int scheme)
	{
		if (scheme < 0 || scheme >= ColorScheme::Array.Count || !ColorScheme::Array.Items[scheme]) return nullptr;
		return ColorScheme::Array.Items[scheme]->LightConvert;
	}

	void CaptureAfterDraw(bool trace);
}
