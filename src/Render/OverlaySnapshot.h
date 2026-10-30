#pragma once

#include "BuildingImage.h"
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
// (C5.2b). Rubble and the deferred records of 6D3470 are absent. The animated or crumbling Terrain
// objects RenderLayers (6D8DB0) draws on Composite in mode 2 (gamemd-reverse #23) are captured in the
// same callback into a list of their own, which the trace replays over CompositeSnapshot's copies (D2).
// The live buildings of step 8 (gamemd-reverse #22) follow the cliff shadows in the Tile list (D1).
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
	// 4D1890's FoggedObject records: the (key, record) pairs and their count, and a record's kind, saved
	// rectangle and enabled byte. Of its kinds, buildings belong to stage D; with FogOfWar on, the copy
	// makes no overlay (14h), smudge (1Dh) or Terrain (24h) record, so none is drawn and the trace only
	// counts them.
	constexpr std::uintptr_t FoggedPairs = 0x8B3CC0, FoggedCount = 0x8B3CC4;
	constexpr size_t FoggedKind = 0x30, FoggedBounds = 0x40, FoggedEnabled = 0x74, FoggedRecordSize = 0x78;
	constexpr int FoggedBuilding = 6;
	// 6D8DB0 walks the layers at 8A0360 and bounds the Terrain objects it draws by Locations that
	// 5AFB80 (the matrix applied to a vector) gives for its corners.
	constexpr int LayerCount = 5;
	constexpr std::uintptr_t MatrixApply = 0x5AFB80;
	// 43D290's BUILDNGZ.SHA (frame 0, already less 41h), the projection of a Foundation corner
	// (6D1FE0), and the Foundation width and height (45EC90, 45ECA0, which Ares takes over), read
	// as the whole EAX.
	constexpr std::uintptr_t BuildingZShape = 0x89DDBC, ZShapeMove = 0x6D1FE0, FoundationWidth = 0x45EC90, FoundationHeight = 0x45ECA0;

	// What a draw belongs to: an overlay (6D6D10), a Terrain object (6D97D0), a cliff (6D7C00) or a
	// building (6D9920).
	enum class Layer : std::uint8_t { Overlay, Terrain, Cliff, Building };

	// One 4AED70 call: a frame of ShapeFrameSource's cache with the blitter's parameters, clipped
	// against the clip of the capture as 4373B0 / 437A10 do.
	struct Draw
	{
		FrameCache::FrameID ID;
		TerrainBatch::ColorTables::Handle Table;
		// The first drawn pixel on Tile and in the frame, and the drawn columns and rows.
		int X = 0, Y = 0, Width = 0, Height = 0, FrameX = 0, FrameY = 0;
		int LightRow = 0;
		// Drawn row j's depth base is ShapeDepth::RowBase(Rows, Gradient, j).
		ShapeDepth::Rows Rows;
		ShapeDepth::Gradient Gradient {};
		bool Compressed = false;
		// The shadow blitter (4601) halves the old color with Mask; the body one (4E00) draws.
		bool Shadow = false;
		std::uint32_t Mask = 0;
		// An RLE frame with a Z shape: the byte 437A10 subtracts from each drawn pixel's depth,
		// Width x Height.
		bool ZShaped = false;
		std::vector<std::uint8_t> ZBytes;
		// 4AED70's Z write: the Tile pass writes Z (4E00, 4601), mode 2's Terrain only tests it (2E00).
		bool WritesDepth = true;
		// The tint an RLE body ORs into each color (705E00's, 0 where the frame is plain).
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
		// meets the clip.
		int Fogged = 0, FoggedBuildings = 0, FoggedBuildingsInView = 0, FoggedOthersInView = 0;
		// 6D8DB0: the objects of its layers, the Terrain ones it selects (animated or crumbling), those
		// outside its view range or at a fogged Location, and the other objects with those in the range.
		int LayerObjects = 0, Animated = 0, OutOfRange = 0, LocationFogged = 0, Others = 0, OthersInRange = 0;
		// The other objects 6D8DB0 marked IsVisible (it clears the flag of each and sets it where it draws),
		// and those of them whose rectangle (GetRenderDimensions) is empty.
		int OthersVisible = 0, OthersUnbounded = 0;
		// 6D9920: the buildings of the ground layer, those fogged, off the clip, in a state the capture
		// does not draw (another visual, warping, disguised), gates moving, buildings unloading and the bibs
		// and doors drawn.
		int Buildings = 0, BuildingsFogged = 0, BuildingsOutside = 0, BuildingsUnsupported = 0, Gates = 0, Unloading = 0, Bibs = 0, Doors = 0;
	};
	struct Frame
	{
		bool Visible = false;
		std::vector<Draw> Draws;
		Counters Count;
		// 6D8DB0's draws on Composite, in its order, and their counters.
		std::vector<Draw> CompositeDraws;
		Counters CompositeCount;
		// For the trace: the rectangles of those other objects on the clip's surface, where the layers
		// drew what the D2 replay does not.
		std::vector<RectangleStruct> OtherBounds;
	};
	extern Frame Current;
	extern unsigned TraceCount;
	extern bool RedrawRequested;

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
