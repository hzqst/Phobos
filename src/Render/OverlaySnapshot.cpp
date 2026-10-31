#include "OverlaySnapshot.h"
#include "CompositeSnapshot.h"
#include "TerrainPass.h"

#include <AnimTypeClass.h>
#include <GameOptionsClass.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Misc/BlittersFix.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <variant>

// The single definition of the state OverlaySnapshot.h declares, the capture and the trace replay.
namespace OverlaySnapshot
{
	Frame Current;
	unsigned TraceCount = 0;
	bool RedrawRequested = false;
	DSurface* RecordingTarget = nullptr;
	bool RecordingAfter = false;

	namespace
	{
		using TerrainImage::Point;
		// The first shape that could not be acquired: overlay, frame, frames of the shape.
		std::array<int, 4> FirstFailure {};
		// The calls, blits and lines of the last recorded RenderLayers in their order, and the first call and
		// blit not modeled.
		std::vector<std::variant<RecordedCall, RecordedBlit, RecordedLine>> Recorded;
		RecordedCall FirstUnmodeled;
		RecordedBlit FirstUnmodeledBlit;

		// 490B90 and 490E50 pick a blitter from the convert's own tables by the flags. The flags each slot
		// stands for, found by asking them for the combinations the draws model (an Alpha body, a shadow);
		// and the convert and slot of each blitter of ConvertClass::Array, rebuilt at most once a capture
		// when a blitter is not found.
		std::array<std::uint32_t, PlainBlitters> PlainSlots {};
		std::array<std::uint32_t, RLEBlitters> RLESlots {};
		bool SlotsReady = false;
		struct BlitterOwner
		{
			const ConvertClass* Convert = nullptr;
			std::uint8_t Slot = 0;
			bool Compressed = false;
		};
		std::unordered_map<const void*, BlitterOwner> BlitterOwners;
		unsigned OwnersSerial = UINT_MAX;

		void FillSlots(const ConvertClass* convert)
		{
			for (const std::uint32_t kind : {AlphaFlag, AlphaFlag | 2u, AlphaFlag | 4u, AlphaFlag | 6u, ShadowFlag})
				for (const std::uint32_t depth : {0u, ZReadFlag, ZWriteFlag})
				{
					const auto flags = 0x400 | kind | depth;
					const auto* plain = convert->SelectPlainBlitter(BlitterFlags(flags));
					const auto* rle = convert->SelectRLEBlitter(BlitterFlags(flags));
					for (size_t i = 0; i < PlainBlitters; ++i)
						if (convert->Blitters[i] == plain && !PlainSlots[i]) PlainSlots[i] = flags;
					for (size_t i = 0; i < RLEBlitters; ++i)
						if (convert->RLEBlitters[i] == rle && !RLESlots[i]) RLESlots[i] = flags;
				}
			SlotsReady = true;
		}

		// The convert a blitter belongs to and the flags it stands for; no convert where it is not found.
		std::pair<const ConvertClass*, std::uint32_t> FindBlitter(const void* blitter, bool compressed)
		{
			const auto owned = [blitter, compressed](const BlitterOwner& owner)
			{
				if (!CursorSnapshot::Readable(owner.Convert, sizeof(ConvertClass)) || owner.Compressed != compressed) return false;
				return compressed ? owner.Convert->RLEBlitters[owner.Slot] == blitter : owner.Convert->Blitters[owner.Slot] == blitter;
			};
			auto found = BlitterOwners.find(blitter);
			if ((found == BlitterOwners.end() || !owned(found->second)) && OwnersSerial != BlitFrameSource::Serial)
			{
				OwnersSerial = BlitFrameSource::Serial;
				BlitterOwners.clear();
				for (const auto* convert : ConvertClass::Array)
				{
					if (!CursorSnapshot::Readable(convert, sizeof(ConvertClass))) continue;
					for (size_t i = 0; i < PlainBlitters; ++i)
						if (convert->Blitters[i]) BlitterOwners.try_emplace(convert->Blitters[i], BlitterOwner {convert, std::uint8_t(i), false});
					for (size_t i = 0; i < RLEBlitters; ++i)
						if (convert->RLEBlitters[i]) BlitterOwners.try_emplace(convert->RLEBlitters[i], BlitterOwner {convert, std::uint8_t(i), true});
				}
				found = BlitterOwners.find(blitter);
			}
			if (found == BlitterOwners.end() || !owned(found->second)) return {};
			const auto& owner = found->second;
			if (!SlotsReady) FillSlots(owner.Convert);
			return {owner.Convert, (compressed ? RLESlots[owner.Slot] : PlainSlots[owner.Slot])};
		}

		bool Representable(Point point) { return point.X >= INT_MIN && point.X <= INT_MAX && point.Y >= INT_MIN && point.Y <= INT_MAX; }

		// 6D2140: the client position of coordinates, false outside its margin.
		bool Project(const CoordStruct& coords, Point2D& client)
		{
			return reinterpret_cast<bool(__thiscall*)(const TacticalClass*, const CoordStruct*, Point2D*)>(CoordsToClient)(TacticalClass::Instance, &coords, &client);
		}

		// 6D20E0: the screen height of a Z.
		int AdjustZ(int z) { return reinterpret_cast<int(__fastcall*)(int)>(AdjustForZ)(z); }

		// What one overlay draws: a frame of an image, lit and Z-adjusted, with an optional Z shape.
		struct Call
		{
			const SHPStruct* Image = nullptr;
			int Frame = 0;
			const ConvertClass* Convert = nullptr;
			int Brightness = 0, ZAdjust = 0, Gradient = 0;
			const SHPStruct* ZShape = nullptr;
			int ZShapeX = 0, ZShapeY = 0;
			// The frame 47FB90 bounds the cell by, when it differs from the drawn one.
			int BoundsFrame = -1;
			// Whether 6D6D10's test of 47FB90's rectangle applies; 6D97D0 tests the object's own one
			// and 6D7C00 none.
			bool Bounded = true;
			// The Tile pass tests and writes Z; a recorded call may only test it (2000h) or do neither.
			bool TestsDepth = true, WritesDepth = true;
			// The translucency bits of a colored draw (2, 4, 6).
			std::uint8_t Translucency = 0;
			// 705E00's tint, which only an RLE frame's blitter applies.
			int Tint = 0;
			// 4D1890 cuts a fogged body's clip shorter.
			int ClipHeight = INT_MAX;
			// A recorded call's own clip, which its point is relative to, instead of the capture's.
			const RectangleStruct* Bounds = nullptr;
		};

		struct Capture
		{
			const RectangleStruct& Clip;
			int TacticalX, TacticalY, ViewX, ViewY;
			// The ZBuffer's MaxValue and Bounds.Y.
			int MaxValue, BoundsY;
			std::unordered_map<const ConvertClass*, TerrainBatch::ColorTables::Handle> Tables;
			// Where the draws and their counters go: the Tile pass, or 6D8DB0's on Composite.
			std::vector<Draw>* Target = &Current.Draws;
			Counters* Count = &Current.Count;

			TerrainBatch::ColorTables::Handle Table(const ConvertClass* convert)
			{
				const auto [it, inserted] = Tables.try_emplace(convert);
				const auto* light = static_cast<const LightConvertClass*>(convert);
				if (inserted && TerrainSnapshot::ValidConvert(light)) it->second = TerrainSnapshot::AcquireTable(light);
				return it->second;
			}

			// 4AED70 with a Z shape: Z shape pixel (funX + i, funY + j) lies under frame pixel (i, j), and
			// the frame is cut to the Z shape's frame (421B60), possibly to start inside it.
			struct Cut
			{
				ShapeDepth::Rect Part {};
				int FunX = 0, FunY = 0, Pitch = 0;
				// The Z shape's raw frame data: GetPixels (69E740) does not decode an RLE frame.
				const std::uint8_t* Pixels = nullptr;
			};

			static bool CutByZShape(const Call& call, const Shape& shape, Cut& cut)
			{
				const auto* zshape = ShapeData(call.ZShape);
				if (!zshape || zshape->Frames <= 0) return false;
				const auto base = reinterpret_cast<std::uintptr_t>(zshape);
				if (!CursorSnapshot::Readable(reinterpret_cast<const void*>(base + sizeof(SHPStruct)), sizeof(SHPFrame))) return false;
				const auto header = zshape->AsFile()->GetFrameHeader(0);
				if (header.Offset <= 0) return false;
				cut.FunX = call.ZShapeX - (shape.CanvasWidth / 2 - shape.Frame.Left) + header.Left;
				cut.FunY = call.ZShapeY - (shape.CanvasHeight / 2 - shape.Frame.Top) + header.Top;
				cut.Part = ShapeDepth::Intersect({0, 0, shape.Frame.Width, shape.Frame.Height}, {-cut.FunX, -cut.FunY, header.Width, header.Height});
				cut.Pitch = header.Width;
				cut.Pixels = reinterpret_cast<const std::uint8_t*>(base + header.Offset);
				return true;
			}

			// 437A10's Z adjust bytes: per drawn row the pointer starts at Z shape pixel (funX + FrameX,
			// funY + FrameY + row) and stays behind by the row's prefix overshoot, the pixels the
			// blitter's whole tokens skip beyond FrameX. False where the data cannot be read.
			static bool ReadZBytes(const Shape& shape, const Cut& cut, Draw& draw)
			{
				draw.ZBytes.assign(size_t(draw.Width) * draw.Height, 0);
				const auto* row = reinterpret_cast<const std::uint8_t*>(shape.Data + shape.Frame.Offset);
				const auto lineLength = [](const std::uint8_t* line) -> size_t
				{
					if (!CursorSnapshot::Readable(line, 2)) return 0;
					const size_t length = line[0] | (size_t(line[1]) << 8);
					return length >= 2 && CursorSnapshot::Readable(line, length) ? length : 0;
				};
				for (int y = 0; y < draw.FrameY; ++y)
				{
					const auto length = lineLength(row);
					if (!length) return false;
					row += length;
				}
				for (int r = 0; r < draw.Height; ++r)
				{
					const auto length = lineLength(row);
					if (!length) return false;
					int skip = draw.FrameX;
					size_t token = 2;
					while (skip > 0)
					{
						if (token >= length) return false;
						if (row[token++] == 0)
						{
							if (token >= length) return false;
							skip -= row[token++];
						}
						else --skip;
					}
					const int lag = -std::min(skip, 0);
					const auto* z = cut.Pixels + std::ptrdiff_t(cut.FunY + draw.FrameY + r) * cut.Pitch + cut.FunX + draw.FrameX;
					if (draw.Width > lag)
					{
						if (!CursorSnapshot::Readable(z, size_t(draw.Width - lag))) return false;
						std::memcpy(draw.ZBytes.data() + size_t(r) * draw.Width + lag, z, size_t(draw.Width - lag));
					}
					row += length;
				}
				return true;
			}

			// The blitter's part of a draw whose Compressed is set: a shadow or a body, its translucency, Z
			// test and write, and tint.
			static void Blend(Draw& draw, const ConvertClass* convert, bool shadow, std::uint8_t translucency, bool tests, bool writes, int tint)
			{
				draw.Shadow = shadow;
				draw.Mask = convert->HalfTranslucencyMask;
				draw.QuarterMask = convert->QuatTranslucencyMask;
				draw.Translucency = shadow ? 0 : translucency;
				draw.TestsDepth = tests || writes;
				draw.WritesDepth = writes;
				// 437A10 takes an RLE AlphaZRead blitter for a frame that reads Z without writing it.
				draw.Rounded = draw.Translucency && draw.Compressed && tests && !writes && BlittersFix::Applied;
				// Only the Alpha blitters that test Z OR the tint in: RLE AlphaZRead (498150) and AlphaZReadWrite
				// (499280), plain AlphaZRead (494C40); the others' tinted copies call Blit_Copy (#22 §7).
				draw.Tint = !shadow && !draw.Translucency && (tests || writes) && (draw.Compressed || !writes) ? std::uint16_t(tint) : 0;
			}

			// 4AED70 with flag 200 for one call: the frame centered on center, cut by its Z shape, then
			// clipped and given its row depths by 4373B0 or 437A10.
			void Place(int cellX, int cellY, Layer source, int index, const Call& call, Point center, bool shadow)
			{
				const auto& clip = call.Bounds ? *call.Bounds : Clip;
				auto& count = *Count;
				// 4AED70 and 47FB90 draw nothing for a type without an image.
				if (!call.Image) { ++count.NoImage; return; }
				// The native draw ran first, so a reference still unloaded has no file to load.
				if (FrameCount(call.Image) == UnloadedShape) { ++count.Unloaded; return; }
				const auto shape = AcquireShape(call.Image, call.Frame);
				if (!shape.Ready)
				{
					if (!count.Frame++) { FirstFailure = {index, call.Frame, FrameCount(call.Image), int(source)}; }
					return;
				}
				// 6D6D10 draws only where 47FB90's rectangle meets the clip.
				auto bounds = shape;
				if (call.BoundsFrame >= 0 && call.BoundsFrame != call.Frame) bounds = AcquireShape(call.Image, call.BoundsFrame);
				if (!bounds.Ready) { ++count.Frame; return; }
				if (!bounds.ID || !shape.ID) { ++count.Empty; return; }
				const auto boundsAt = OverlayImage::FrameOrigin(center, bounds.CanvasWidth, bounds.CanvasHeight, bounds.Frame.Left, bounds.Frame.Top);
				if (call.Bounded && !TerrainSnapshot::Intersects(clip, boundsAt.X, boundsAt.Y, bounds.Frame.Width, bounds.Frame.Height)) { ++count.Outside; return; }
				const auto at = OverlayImage::FrameOrigin(center, shape.CanvasWidth, shape.CanvasHeight, shape.Frame.Left, shape.Frame.Top);
				if (shape.Frame.Width <= 0 || shape.Frame.Height <= 0) { ++count.Empty; return; }
				if (!Representable({at.X - clip.X, at.Y - clip.Y})) { ++count.Outside; return; }
				const bool compressed = (shape.Frame.Flags & CompressedFrame) != 0;
				Cut cut {{0, 0, shape.Frame.Width, shape.Frame.Height}};
				if (call.ZShape && !CutByZShape(call, shape, cut)) { ++count.ZShape; return; }
				if (!cut.Part.Width || !cut.Part.Height) { ++count.Empty; return; }
				// The surface rectangle keeps the frame's place; only its size follows the cut.
				const ShapeDepth::Rect dest {int(at.X - clip.X), int(at.Y - clip.Y), cut.Part.Width, cut.Part.Height};
				const auto clipped = ShapeDepth::Clip(dest, cut.Part, compressed ? cut.Part : ShapeDepth::Rect {0, 0, shape.Frame.Width, shape.Frame.Height},
					clip.Width, std::min(clip.Height, call.ClipHeight), compressed);
				if (!clipped.Drawn || clipped.Width <= 0 || clipped.Rows <= 0) { ++count.Outside; return; }
				if (clipped.SourceX < 0 || clipped.SourceY < 0 || clipped.SourceX + clipped.Width > shape.Frame.Width
					|| clipped.SourceY + clipped.Rows > shape.Frame.Height) { ++count.Unsupported; return; }
				const auto* gradients = reinterpret_cast<const ShapeDepth::Gradient*>(Gradients);
				if (call.Gradient < 0 || call.Gradient >= GradientCount || !ShapeDepth::Usable(gradients[call.Gradient])
					|| !ShapeDepth::ClosedForm(gradients[call.Gradient])) { ++count.Unsupported; return; }
				const bool zshaped = compressed && call.ZShape;
				// Without a Z shape 437A10 reads its default Z adjust row, which Ares' 437CCC replaces with
				// zeros as wide as the frame (gamemd-reverse #19 §8.3): no adjustment.
				const auto table = Table(call.Convert);
				if (!table) { ++count.Palette; return; }
				Draw draw;
				draw.ID = shape.ID;
				draw.Table = table;
				draw.X = clip.X + clipped.Dest.X;
				draw.Y = clip.Y + clipped.Dest.Y;
				draw.Width = clipped.Width;
				draw.Height = clipped.Rows;
				draw.FrameX = clipped.SourceX;
				draw.FrameY = clipped.SourceY;
				draw.LightRow = TerrainImage::LightRow(call.Brightness);
				draw.Gradient = gradients[call.Gradient];
				draw.Rows = compressed
					? ShapeDepth::RLE(draw.Gradient, MaxValue, BoundsY, clip.Y, dest, clipped.Dest, call.ZAdjust, zshaped)
					: ShapeDepth::Plain(draw.Gradient, MaxValue, BoundsY, clip.Y, clipped.Dest, call.ZAdjust);
				draw.Compressed = compressed;
				Blend(draw, call.Convert, shadow, call.Translucency, call.TestsDepth, call.WritesDepth, call.Tint);
				draw.ZShaped = zshaped;
				if (zshaped && !ReadZBytes(shape, cut, draw)) { ++count.ZShape; return; }
				draw.ZAdjust = call.ZAdjust;
				draw.GradientIndex = call.Gradient;
				draw.CellX = cellX;
				draw.CellY = cellY;
				draw.Source = source;
				draw.Index = index;
				draw.ShapeFrame = call.Frame;
				if (compressed) ++count.Compressed;
				Target->push_back(std::move(draw));
				++(shadow ? count.Shadows : count.Bodies);
			}

			// A recorded blit within its clip: 4373B0 reads the kept pixels from their start, 437A10 the frame
			// from the clipped part's offset, each with its own row depths.
			void PlaceBlit(const RecordedBlit& blit)
			{
				auto& count = *Count;
				const auto& clipped = blit.Clipped;
				if (!clipped.Drawn || clipped.Width <= 0 || clipped.Rows <= 0) { ++count.Empty; return; }
				const auto* gradients = reinterpret_cast<const ShapeDepth::Gradient*>(Gradients);
				if (blit.Gradient < 0 || blit.Gradient >= GradientCount || !ShapeDepth::Usable(gradients[blit.Gradient])
					|| !ShapeDepth::ClosedForm(gradients[blit.Gradient])) { ++count.Unsupported; return; }
				const auto* frame = BlitFrameSource::Frames.Resolve(blit.ID);
				if (!frame) { ++count.Frame; return; }
				const int frameX = blit.Compressed ? clipped.SourceX : 0, frameY = blit.Compressed ? clipped.SourceY : 0;
				if (frameX < 0 || frameY < 0 || frameX + clipped.Width > frame->Geometry.Width || frameY + clipped.Rows > frame->Geometry.Height)
				{
					++count.Unsupported;
					return;
				}
				const auto table = Table(blit.Convert);
				if (!table) { ++count.Palette; return; }
				const auto& bounds = blit.Bounds;
				Draw draw;
				draw.ID = blit.ID;
				draw.Storage = &BlitFrameSource::Frames;
				draw.Table = table;
				draw.X = bounds.X + clipped.Dest.X;
				draw.Y = bounds.Y + clipped.Dest.Y;
				draw.Width = clipped.Width;
				draw.Height = clipped.Rows;
				draw.FrameX = frameX;
				draw.FrameY = frameY;
				draw.LightRow = TerrainImage::LightRow(blit.Brightness);
				draw.Gradient = gradients[blit.Gradient];
				draw.Rows = blit.Compressed
					? ShapeDepth::RLE(draw.Gradient, MaxValue, BoundsY, bounds.Y, blit.Dest, clipped.Dest, blit.ZAdjust, false)
					: ShapeDepth::Plain(draw.Gradient, MaxValue, BoundsY, bounds.Y, clipped.Dest, blit.ZAdjust);
				draw.Compressed = blit.Compressed;
				const bool shadow = blit.Flags & ShadowFlag;
				Blend(draw, blit.Convert, shadow, std::uint8_t(blit.Flags & TranslucencyMask), blit.Flags & ZReadMask, blit.Flags & ZWriteFlag, blit.Tint);
				draw.ZAdjust = blit.ZAdjust;
				draw.GradientIndex = blit.Gradient;
				draw.Source = Layer::Composite;
				draw.Index = int(blit.Caller);
				draw.ShapeFrame = -1;
				if (blit.Compressed) ++count.Compressed;
				Target->push_back(std::move(draw));
				++(shadow ? count.Shadows : count.Bodies);
			}

			// 47F6A0.
			void Body(const CellClass* cell, int index, const OverlayTypeClass* type, Point tile)
			{
				auto& count = *Count;
				if (index == OverlayImage::NoBodyA || index == OverlayImage::NoBodyB) { ++count.NoBody; return; }
				const auto* ext = OverlayTypeExt::TryFetch(type);
				const int level = std::int8_t(cell->Level), data = cell->OverlayData;
				const bool bridge = std::uint32_t(cell->Flags) & BridgeFlag;
				const auto center = OverlayImage::Center(tile,
					OverlayImage::BaseOffset(type->Tiberium, type->Wall, type->Crate, type->ArrayIndex, int(type->LandType), index),
					OverlayImage::Lift(bridge, data, index));
				const int height = OverlayImage::HeightAdjust(level, bridge, ext ? ext->ZAdjust.Get() : 0);
				Call call;
				call.Image = type->Image;
				call.Frame = data;
				call.Convert = cell->LightConvert;
				call.Brightness = std::int16_t(cell->Intensity_Terrain);
				call.ZAdjust = BodyDepth - height;
				const int slope = cell->SlopeIndex;
				if (bridge)
				{
					// The redraw stamp only skips a second draw within one frame; a full redraw draws once.
					++count.Bridges;
					call.Frame = OverlayImage::BridgeFrame(data, cell->MapCoords.X, cell->MapCoords.Y,
						reinterpret_cast<const std::int32_t*>(BridgePattern));
					call.BoundsFrame = data;
					call.Brightness = std::int16_t(cell->Color1_Blue);
				}
				else if (type->Tiberium)
				{
					const int tiberiumIndex = TiberiumOf(index);
					if (tiberiumIndex == -1) return;
					if (tiberiumIndex < 0 || tiberiumIndex >= TiberiumClass::Array.Count) { ++count.Tiberium; return; }
					const auto* tiberium = TiberiumClass::Array.Items[tiberiumIndex];
					if (!tiberium->Image) { ++count.Tiberium; return; }
					const auto* tiberiumExt = TiberiumExt::TryFetch(tiberium);
					const bool ramp = OverlayImage::RampAllowed(slope, tiberiumExt && tiberiumExt->AllowRamps, HasRampArt(tiberium));
					if (ramp) ++count.Ramps;
					const int image = OverlayImage::TiberiumImage(cell->MapCoords.X, cell->MapCoords.Y, slope, ramp, tiberium->Image->ArrayIndex,
						tiberium->NumImages, ramp ? std::max(tiberium->NumSlopes, OverlayImage::RampImages) : tiberium->NumSlopes);
					if (image < 0 || image >= OverlayTypeClass::Array.Count) { ++count.Tiberium; return; }
					call.Image = OverlayTypeClass::Array.Items[image]->Image;
					call.Convert = FileSystem::x_PAL;
					call.Brightness = FullBrightness;
					if (slope) call.ZShape = reinterpret_cast<SHPStruct* const*>(SlopeShapes)[slope];
				}
				else if (type->Wall)
				{
					// Phobos' 47F974: the owner's color scheme, from the type's palette when it has one.
					++count.Walls;
					const int owner = cell->WallOwnerIndex;
					const auto* house = owner >= 0 && owner < HouseClass::Array.Count ? HouseClass::Array.Items[owner] : HouseClass::CurrentPlayer;
					const int scheme = house ? house->ColorSchemeIndex : -1;
					call.Convert = nullptr;
					if (ext && ext->Palette)
					{
						if (scheme >= 0 && scheme < ext->Palette->Count && ext->Palette->Items[scheme]) call.Convert = ext->Palette->Items[scheme]->LightConvert;
					}
					else call.Convert = SchemeConvert(scheme);
					call.Gradient = int(ZGradient::Deg90);
					call.Brightness = cell->Intensity_Normal;
				}
				else if (type->ArrayIndex == OverlayImage::Veins)
				{
					// Phobos' 47FA1F / 47FA5C give veins the player's color scheme.
					call.Convert = FileSystem::x_PAL;
					if (type->IsVeins && HouseClass::CurrentPlayer)
						if (const auto* scheme = SchemeConvert(HouseClass::CurrentPlayer->ColorSchemeIndex)) call.Convert = scheme;
					if (slope)
					{
						call.ZShape = reinterpret_cast<SHPStruct* const*>(SlopeShapes)[slope];
						call.ZShapeX = VeinsZShapeX;
						call.ZShapeY = VeinsZShapeY;
					}
				}
				else
				{
					if (type->IsRubble) { ++count.Rubble; return; }
					const int raised = type->DrawFlat || type->IsARock ? 0 : RaisedDepth;
					call.ZAdjust = raised - height + BodyDepth;
					call.Gradient = type->DrawFlat ? int(ZGradient::Ground) : int(ZGradient::Deg90);
					if (type->Crate) call.Frame = 0;
				}
				if (!call.Convert) { ++count.Palette; return; }
				Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Overlay, index, call, center, false);
			}

			// 47F510: the second half of the type's own frames, darkening what lies below.
			void Shadow(const CellClass* cell, int index, const OverlayTypeClass* type, Point tile)
			{
				auto& count = *Count;
				if (type->IsRubble) { ++count.Rubble; return; }
				const int level = std::int8_t(cell->Level), data = cell->OverlayData;
				const bool bridge = std::uint32_t(cell->Flags) & BridgeFlag;
				const auto center = OverlayImage::ShadowCenter(OverlayImage::Center(tile,
					OverlayImage::BaseOffset(type->Tiberium, type->Wall, type->Crate, type->ArrayIndex, int(type->LandType), index),
					OverlayImage::Lift(bridge, data, index)), bridge, data);
				const auto* image = type->Image;
				if (!image) { ++count.NoImage; return; }
				const int frames = FrameCount(image);
				if (frames == UnloadedShape) { ++count.Unloaded; return; }
				if (frames < 0) { ++count.Frame; return; }
				Call call;
				call.Image = image;
				call.Frame = OverlayImage::ShadowFrame(std::int16_t(frames), data, type->Crate);
				call.Convert = cell->LightConvert;
				call.Brightness = FullBrightness;
				call.ZAdjust = BodyDepth - TerrainImage::LevelHeight * level;
				if (!call.Convert) { ++count.Palette; return; }
				Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Overlay, index, call, center, true);
			}

			// 6D97D0, 71CC50 and 71C1B0 (with Phobos' 71C1FE and 71C2BC) for an object of the ground
			// layer: the body, then its own shadow.
			void Terrain(TerrainClass* terrain)
			{
				auto& count = *Count;
				const auto* type = terrain->Type;
				if (!CursorSnapshot::Readable(type, sizeof(TerrainTypeClass))) { ++count.Frame; return; }
				++count.Terrains;
				// An animated or crumbling object draws on Composite (6D8DB0), and only there.
				if (type->IsAnimated || terrain->IsCrumbling) { ++count.Dynamic; return; }
				if (!terrain->IsOnMap || terrain->InLimbo) return;
				// 6D97D0 and 71CC50: the object's rectangle (71D160) must meet the clip.
				RectangleStruct dimensions {};
				terrain->GetRenderDimensions(&dimensions);
				const std::int64_t left = std::int64_t(dimensions.X) + ViewX, top = std::int64_t(dimensions.Y) + ViewY;
				if (left >= std::int64_t(Clip.X) + Clip.Width || top >= std::int64_t(Clip.Y) + Clip.Height
					|| left + dimensions.Width <= Clip.X || top + dimensions.Height <= Clip.Y) { ++count.TerrainOutside; return; }
				auto* cell = terrain->GetCell();
				if (!cell || !cell->LightConvert) { ++count.Palette; return; }
				const auto coords = terrain->GetRenderCoords();
				Point2D client {};
				reinterpret_cast<bool(__thiscall*)(const TacticalClass*, const CoordStruct*, Point2D*)>(CoordsToClient)(TacticalClass::Instance, &coords, &client);
				const int height = reinterpret_cast<int(__fastcall*)(int)>(AdjustForZ)(terrain->GetZ());
				const auto* ext = TerrainTypeExt::TryFetch(type);
				const bool damaged = ext && ext->HasDamagedFrames
					&& terrain->GetHealthPercentage() <= RulesExt::Global()->ConditionYellow_Terrain.Get(RulesClass::Instance->ConditionYellow);
				if (damaged) ++count.Damaged;
				// The clip is ViewBounds, which 71CC50's client position is relative to.
				Point center {std::int64_t(client.X) + ViewX, std::int64_t(client.Y) + ViewY};
				Call call;
				call.Image = type->Image;
				call.Frame = TerrainObjectImage::StaticFrame(damaged);
				call.Convert = cell->LightConvert;
				call.Brightness = std::int16_t(cell->Intensity_Terrain);
				call.ZAdjust = TerrainObjectImage::Depth(height, false);
				call.Gradient = int(ZGradient::Deg90);
				call.Bounded = false;
				if (type->SpawnsTiberium)
				{
					call.Convert = FileSystem::GRFTXT_TIBERIUM_PAL;
					call.Brightness = std::int16_t(cell->Intensity_Normal);
					center.Y -= TerrainObjectImage::TiberiumLift;
				}
				if (ext && ext->Palette)
				{
					// 71C2BC: the color scheme of the cell's wall owner, else of the player.
					const int owner = cell->WallOwnerIndex;
					const auto* house = owner >= 0 && owner < HouseClass::Array.Count ? HouseClass::Array.Items[owner] : HouseClass::CurrentPlayer;
					const int scheme = house ? house->ColorSchemeIndex : -1;
					call.Convert = scheme >= 0 && scheme < ext->Palette->Count && ext->Palette->Items[scheme] ? ext->Palette->Items[scheme]->LightConvert : nullptr;
					call.Brightness = cell->Intensity_Normal;
				}
				const int index = type->ArrayIndex;
				if (!call.Convert) ++count.Palette;
				else Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Terrain, index, call, center, false);
				if (!Game::bDrawShadow) return;
				const int frames = FrameCount(call.Image);
				if (frames == UnloadedShape) { ++count.Unloaded; return; }
				if (frames < 0) { ++count.Frame; return; }
				// The shadow keeps the cell's own palette and the Tiberium lift.
				call.Frame = TerrainObjectImage::ShadowFrame(call.Frame, std::int16_t(frames));
				call.Convert = cell->LightConvert;
				call.Brightness = FullBrightness;
				call.ZAdjust = TerrainObjectImage::Depth(height, true);
				call.Gradient = int(ZGradient::Ground);
				Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Terrain, index, call, center, true);
			}

			// 705E00's choices for a building that draws its normal visual (gamemd-reverse #22 §5, §7): the
			// palette, the tint and intensity with Phobos' 70632E and 706389, the shadow, and 43CEA0's point.
			struct Wrapper
			{
				BuildingClass* Building = nullptr;
				BuildingTypeClass* Type = nullptr;
				CellStruct MapCoords {};
				const CellClass* Cell = nullptr;
				const CellClass* CenterCell = nullptr;
				const ConvertClass* Convert = nullptr;
				int Height = 0;
				bool DrawShadow = false;
				Point Position {};

				int Intensity(int brightness) const
				{
					if (Type->TerrainPalette) brightness = std::int16_t(CenterCell->Intensity_Terrain);
					int value = Building->GetFlashingIntensity(brightness);
					if (Building->IsIronCurtained()) value = Building->GetInvulnerabilityTintIntensity(value);
					if (TechnoExt::Fetch(Building)->AirstrikeTargetingMe) value = Building->GetAirstrikeTintIntensity(value);
					return value + TechnoExt::GetCustomTintIntensity(Building);
				}
			};

			// 43CEA0's normal window path (force 1, the clip ViewBounds) up to its call: on the map, out of
			// limbo, its rectangle (455C20) meeting the clip, and drawn by 705E00 with these parameters.
			bool Wrap(BuildingClass* building, Wrapper& wrapper)
			{
				auto& count = *Count;
				if (!building->IsOnMap || building->InLimbo) return false;
				auto* type = building->Type;
				if (!CursorSnapshot::Readable(type, sizeof(BuildingTypeClass))) { ++count.Frame; return false; }
				RectangleStruct dimensions {};
				building->GetRenderDimensions(&dimensions);
				if (!TerrainSnapshot::Intersects(Clip, std::int64_t(dimensions.X) + ViewX, std::int64_t(dimensions.Y) + ViewY, dimensions.Width, dimensions.Height))
				{
					++count.BuildingsOutside;
					return false;
				}
				if (type->InvisibleInGame) return false;
				// 705E00 draws the normal visual of a building that neither warps nor hides behind a disguise
				// with these parameters; the others change its flags or palette.
				if (building->VisualCharacter(VARIANT_FALSE, nullptr) != VisualType::Normal || building->IsBeingWarpedOut() || building->IsWarpingIn()
					|| !building->IsClearlyVisibleTo(HouseClass::CurrentPlayer) || building->IsDisguised())
				{
					++count.BuildingsUnsupported;
					return false;
				}
				wrapper.Building = building;
				wrapper.Type = type;
				wrapper.MapCoords = building->GetMapCoords();
				wrapper.Cell = MapClass::Instance.GetCellAt(wrapper.MapCoords);
				// 705E00 reads the tint's shroud and the TerrainPalette from the center's cell.
				const auto center = building->GetCoords();
				wrapper.CenterCell = MapClass::Instance.GetCellAt(CellStruct {std::int16_t(center.X / 256), std::int16_t(center.Y / 256)});
				if (!wrapper.Cell || !wrapper.CenterCell) { ++count.Palette; return false; }
				wrapper.Convert = type->TerrainPalette ? wrapper.CenterCell->LightConvert : building->GetRemapColour();
				if (!wrapper.Convert) { ++count.Palette; return false; }
				wrapper.DrawShadow = Game::bDrawShadow && !type->NoShadow;
				wrapper.Height = AdjustZ(building->GetZ());
				const auto coords = building->GetRenderCoords();
				Point2D client {};
				Project(coords, client);
				wrapper.Position = {std::int64_t(client.X) + ViewX, std::int64_t(client.Y) + ViewY};
				return true;
			}

			// A part 705E00 draws, then its own shadow at half the frames, both testing Z and writing it; the
			// wrapper's tint is shroud-cleared.
			void WrapperPart(const Wrapper& wrapper, const SHPStruct* shape, int frame, int depth, int gradient, int brightness, int tint,
				const SHPStruct* zshape = nullptr, BuildingImage::Offset offset = {})
			{
				auto& count = *Count;
				Call call;
				call.Image = shape;
				call.Frame = frame;
				call.Convert = wrapper.Convert;
				call.Brightness = wrapper.Intensity(brightness);
				call.ZAdjust = depth;
				call.Gradient = gradient;
				call.Bounded = false;
				call.Tint = wrapper.CenterCell->IsShrouded() ? 0 : tint;
				call.ZShape = zshape;
				call.ZShapeX = offset.X;
				call.ZShapeY = offset.Y;
				const int index = wrapper.Type->ArrayIndex;
				Place(wrapper.MapCoords.X, wrapper.MapCoords.Y, Layer::Building, index, call, wrapper.Position, false);
				if (!wrapper.DrawShadow) return;
				const int frames = FrameCount(shape);
				if (frames == UnloadedShape) { ++count.Unloaded; return; }
				if (frames < 0) { ++count.Frame; return; }
				call.Frame = BuildingImage::Add32(std::int16_t(frames) / 2, frame);
				call.Brightness = BuildingImage::FullBrightness;
				call.ZAdjust = BuildingImage::ShadowDepth(wrapper.Height);
				call.Gradient = int(ZGradient::Ground);
				call.Tint = 0;
				call.ZShape = nullptr;
				Place(wrapper.MapCoords.X, wrapper.MapCoords.Y, Layer::Building, index, call, wrapper.Position, true);
			}

			// 6D9920 and 43CEA0 (force 1, the clip ViewBounds) and 43D290 for a building of the ground
			// layer, with Phobos' 43D363, 43D386, 43D6E5 and 43D874 and, through the native calls, Ares'
			// frame and Foundation hooks: each part through 705E00, which draws it and its own shadow.
			void Building(BuildingClass* building)
			{
				auto& count = *Count;
				++count.Buildings;
				if (building->IsFogged && !Unsorted::ArmageddonMode) { ++count.BuildingsFogged; return; }
				Wrapper wrapper;
				if (!Wrap(building, wrapper)) return;
				auto* type = wrapper.Type;
				auto* image = building->GetImage();
				if (!image) { ++count.NoImage; return; }
				const auto mission = building->GetCurrentMission();
				const auto* cell = wrapper.Cell;
				// Phobos' 43D386: the effect and custom tint.
				const int tint = TechnoExt::GetTintColor(building, true, true, false) | TechnoExt::GetCustomTintColor(building);
				const int height = wrapper.Height;
				const auto part = [&](const SHPStruct* shape, int frame, int depth, int gradient, int brightness, const SHPStruct* zshape, BuildingImage::Offset offset)
				{
					WrapperPart(wrapper, shape, frame, depth, gradient, brightness, tint, zshape, offset);
				};
				const auto healthy = building->GetHealthPercentage() > RulesClass::Instance->ConditionYellow;
				auto& timer = building->UnloadTimer;
				if (mission == Mission::Open && (timer.AreStates11() || timer.AreStates10() || timer.AreStates01()))
				{
					++count.Gates;
					const int frame = BuildingImage::GateFrame(int(timer.PercentageDone() * double(type->GateStages)), type->GateStages, timer.AreStates10(),
						timer.AreStates00(), timer.AreStates01(), !healthy);
					part(image, frame, BuildingImage::BodyDepth(type->NormalZAdjust, height), int(ZGradient::Deg90), std::int16_t(cell->Intensity_Normal), nullptr, {});
					return;
				}
				const int light = std::int16_t(cell->Intensity_Normal) + std::int16_t(type->ExtraLight);
				int normalZAdjust = type->NormalZAdjust;
				bool roof = false;
				if (mission == Mission::Unload)
				{
					++count.Unloading;
					// Phobos' 43D363: ExitThroughRoof, by default for jumpjets and balloons.
					if (const auto* linked = building->GetNthLink())
					{
						auto* linkedType = linked->GetTechnoType();
						const auto* linkedExt = TechnoTypeExt::TryFetch(linkedType);
						roof = linkedExt ? linkedExt->ExitThroughRoof.Get(linkedType->JumpJet || linkedType->BalloonHover) : linkedType->JumpJet || linkedType->BalloonHover;
					}
					if (roof && type->RoofDeployingAnim) { image = type->RoofDeployingAnim; normalZAdjust = BuildingImage::RoofZAdjust; }
					else if (!roof && type->DeployingAnim) { image = type->DeployingAnim; normalZAdjust = BuildingImage::DeployingZAdjust; }
				}
				const auto* typeExt = BuildingTypeExt::TryFetch(type);
				const bool pointMove = (mission != Mission::Construction && mission != Mission::Selling) || (typeExt && typeExt->ZShapePointMove_OnBuildup);
				const int width = reinterpret_cast<int(__thiscall*)(const BuildingTypeClass*)>(FoundationWidth)(type);
				const int depthCells = reinterpret_cast<int(__thiscall*)(const BuildingTypeClass*, bool)>(FoundationHeight)(type, false);
				const Point2D corner {BuildingImage::CornerLeptons(width), BuildingImage::CornerLeptons(depthCells)};
				Point2D move {};
				reinterpret_cast<Point2D*(__thiscall*)(const TacticalClass*, Point2D*, const Point2D*)>(ZShapeMove)(TacticalClass::Instance, &move, &corner);
				const auto offset = BuildingImage::ZShapeOffset(pointMove, type->ZShapePointMove.X, type->ZShapePointMove.Y, move.X, move.Y);
				const auto* zshape = width >= BuildingImage::NoZShapeWidth ? nullptr : *reinterpret_cast<SHPStruct* const*>(BuildingZShape);
				const int current = building->GetCurrentFrame();
				const int frames = FrameCount(image);
				if (frames < 0) { ++count.Frame; return; }
				part(image, BuildingImage::BodyFrame(current, std::int16_t(frames)), BuildingImage::BodyDepth(normalZAdjust, height), int(ZGradient::Deg90), light, zshape, offset);
				// Phobos' 43D874: no bib before the building is actually placed.
				if (type->BibShape && building->BState && building->ActuallyPlacedOnMap)
				{
					++count.Bibs;
					part(type->BibShape, current, BuildingImage::BibDepth(height), int(ZGradient::Ground), light, nullptr, {});
				}
				if (mission != Mission::Unload) return;
				if (const auto* door = roof ? type->UnderRoofDoorAnim : type->UnderDoorAnim)
				{
					++count.Doors;
					part(door, BuildingImage::DoorFrame(healthy), BuildingImage::DoorDepth(height), int(ZGradient::Ground), light, nullptr, {});
				}
			}

			// 4D1890's building branch (kind 6) for an enabled record whose saved rectangle meets the
			// clip: the saved parts in order, each straight through 4AED70 with the owner's current color
			// scheme, no flashing, tint or visual, and the shadow whatever bDrawShadow and NoShadow say.
			void FoggedBuilding(const std::uint8_t* record)
			{
				auto& count = *Count;
				const auto* owner = *reinterpret_cast<HouseClass* const*>(record + FoggedOwner);
				const auto& location = *reinterpret_cast<const CoordStruct*>(record + FoggedLocation);
				const auto* parts = *reinterpret_cast<const FoggedPart* const*>(record + FoggedParts);
				const int partCount = *reinterpret_cast<const int*>(record + FoggedPartCount);
				if (partCount <= 0) return;
				if (!CursorSnapshot::Readable(owner, sizeof(HouseClass)) || !CursorSnapshot::Readable(parts, size_t(partCount) * sizeof(FoggedPart)))
				{
					++count.Frame;
					return;
				}
				const auto* ownerConvert = SchemeConvert(owner->ColorSchemeIndex);
				const auto* cell = MapClass::Instance.TryGetCellAt(location);
				// The native draw ran first and made the cell's LightConvert where it was missing.
				if (!cell || !cell->LightConvert) { ++count.Palette; return; }
				const int height = reinterpret_cast<int(__fastcall*)(int)>(AdjustForZ)(location.Z);
				const CoordStruct origin {location.X - 128, location.Y - 128, location.Z};
				Point2D client {};
				reinterpret_cast<bool(__thiscall*)(const TacticalClass*, const CoordStruct*, Point2D*)>(CoordsToClient)(TacticalClass::Instance, &origin, &client);
				const Point position {std::int64_t(client.X) + ViewX, std::int64_t(client.Y) + ViewY};
				for (int i = 0; i < partCount; ++i)
				{
					const auto& part = parts[i];
					auto* object = static_cast<ObjectTypeClass*>(part.Type);
					// 4D204D: an anim part without a type draws nothing.
					if (!object && i) continue;
					if (!CursorSnapshot::Readable(object, sizeof(AbstractTypeClass))) { ++count.Frame; continue; }
					const auto* image = object->GetImage();
					if (i)
					{
						if (object->WhatAmI() != AbstractType::AnimType) continue;
						const auto* type = static_cast<const AnimTypeClass*>(object);
						if (type->DetailLevel > GameOptionsClass::Instance.DetailLevel) { ++count.FoggedDetail; continue; }
						if (!image) continue;
						++count.FoggedAnims;
						Call call;
						call.Image = image;
						call.Frame = part.Frame;
						call.Convert = ownerConvert && type->ShouldUseCellDrawer ? ownerConvert : FileSystem::ANIM_PAL;
						call.Brightness = type->UseNormalLight ? FullBrightness : std::int16_t(cell->Intensity_Normal);
						call.ZAdjust = BuildingImage::FoggedAnimDepth(part.ZAdjust, height);
						call.Gradient = int(type->Flat ? ZGradient::Ground : ZGradient::Deg90);
						call.Bounded = false;
						call.WritesDepth = false;
						Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::FoggedAnim, type->ArrayIndex, call, position, false);
						continue;
					}
					const auto* type = static_cast<const BuildingTypeClass*>(object);
					if (!CursorSnapshot::Readable(type, sizeof(BuildingTypeClass))) { ++count.Frame; return; }
					// 4D1D5F: the whole record, anims included.
					if (type->InvisibleInGame) { ++count.FoggedInvisible; return; }
					const int light = std::int16_t(cell->Intensity_Normal) + std::int16_t(type->ExtraLight);
					const ConvertClass* convert = type->TerrainPalette ? cell->LightConvert : ownerConvert;
					Call call;
					call.Image = image;
					call.Frame = part.Frame;
					call.Convert = convert;
					call.Brightness = light;
					call.Bounded = false;
					if (image && CursorSnapshot::Readable(image, sizeof(SHPStruct)))
					{
						// The body and its shadow are cut at the image's bottom; 4D1890 reads the shape's own
						// header, as for the shadow frame.
						call.ClipHeight = BuildingImage::FoggedClipHeight(Clip.Height, image->Height, int(position.Y - Clip.Y));
						if (call.ClipHeight > 0)
						{
							call.ZAdjust = BuildingImage::FoggedBodyDepth(type->NormalZAdjust, height, part.Flat);
							call.Gradient = int(part.Flat ? ZGradient::Ground : ZGradient::Deg90);
							if (!part.Flat)
							{
								const int width = reinterpret_cast<int(__thiscall*)(const BuildingTypeClass*)>(FoundationWidth)(type);
								const int depthCells = reinterpret_cast<int(__thiscall*)(const BuildingTypeClass*, bool)>(FoundationHeight)(type, false);
								const Point2D corner {BuildingImage::CornerLeptons(width), BuildingImage::CornerLeptons(depthCells)};
								Point2D move {};
								reinterpret_cast<Point2D*(__thiscall*)(const TacticalClass*, Point2D*, const Point2D*)>(ZShapeMove)(TacticalClass::Instance, &move, &corner);
								const auto offset = BuildingImage::FoggedZShapeOffset(type->ZShapePointMove.X, type->ZShapePointMove.Y, move.X, move.Y);
								call.ZShape = *reinterpret_cast<SHPStruct* const*>(BuildingZShape);
								call.ZShapeX = offset.X;
								call.ZShapeY = offset.Y;
							}
							Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Fogged, type->ArrayIndex, call, position, false);
							call.Frame = BuildingImage::Add32(image->Frames / 2, part.Frame);
							call.Brightness = FullBrightness;
							call.ZAdjust = BuildingImage::FoggedShadowDepth(height);
							call.Gradient = int(ZGradient::Ground);
							call.ZShape = nullptr;
							Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Fogged, type->ArrayIndex, call, position, true);
						}
					}
					else ++count.NoImage;
					if (!type->BibShape) continue;
					++count.Bibs;
					Call bib;
					bib.Image = type->BibShape;
					bib.Frame = part.Frame;
					bib.Convert = convert;
					bib.Brightness = light;
					bib.ZAdjust = BuildingImage::BibDepth(height);
					bib.Gradient = int(ZGradient::Ground);
					bib.Bounded = false;
					Place(cell->MapCoords.X, cell->MapCoords.Y, Layer::Fogged, type->ArrayIndex, bib, position, false);
				}
			}

			// 4802A0 and 547230 for a cell of 6D7C00's scan: the C_SHADOW.SHP frame its tile casts.
			void Cliff(const CellClass* cell, int cellX, int cellY)
			{
				auto& count = *Count;
				const auto& types = IsometricTileTypeClass::Array;
				const bool clear = cell->IsoTileTypeIndex == TerrainSnapshot::ClearTileSlot;
				const int typeIndex = clear ? *reinterpret_cast<const int*>(TerrainSnapshot::ClearTile) : cell->IsoTileTypeIndex;
				if (typeIndex < 0 || typeIndex >= types.Count) return;
				const auto* tile = types.Items[typeIndex];
				if (!CursorSnapshot::Readable(tile, sizeof(IsometricTileTypeClass))) { ++count.Frame; return; }
				if (!tile->ShadowCaster) return;
				++count.Casters;
				const auto record = TerrainObjectImage::SelectCliffShadow(tile->ArrayIndex, clear ? 0 : std::uint8_t(cell->Height),
					*reinterpret_cast<const int*>(CliffSpecialBase), *reinterpret_cast<const int*>(CliffFallbackBase),
					reinterpret_cast<const std::int32_t*>(CliffSpecialRecords), reinterpret_cast<const std::int32_t*>(CliffRangeStarts),
					reinterpret_cast<const std::int32_t*>(CliffRecords));
				if (!record.Drawn) return;
				const int level = std::int8_t(cell->Level);
				const auto tilePosition = TerrainImage::TilePosition(cellX, cellY, level, TacticalX, TacticalY, ViewY);
				Call call;
				call.Image = *reinterpret_cast<SHPStruct* const*>(CliffShadows);
				call.Frame = record.Frame;
				// The ISO palette's convert, not the cell's.
				call.Convert = LightConvertClass::Array.Count > 0 ? LightConvertClass::Array.Items[0] : nullptr;
				call.Brightness = FullBrightness;
				call.ZAdjust = TerrainObjectImage::CliffShadowDepth(level);
				call.Gradient = int(ZGradient::Ground);
				call.Bounded = false;
				if (!call.Convert) { ++count.Palette; return; }
				Place(cellX, cellY, Layer::Cliff, tile->ArrayIndex, call, TerrainObjectImage::CliffCenter(tilePosition, record.X, record.Y, ViewX), true);
			}
		};

		// 6D8DB0's draws on Composite: each recorded 4AED70 call in its order, placed as 4AED70 places it
		// within its clip, and each recorded blit; and the rectangles of the objects it marked IsVisible,
		// for the trace.
		void CaptureComposite(Capture& capture)
		{
			capture.Target = &Current.CompositeDraws;
			capture.Count = &Current.CompositeCount;
			auto& count = Current.CompositeCount;
			const auto counted = [&count](const std::vector<Draw>& draws, size_t drawn)
			{
				if (draws.size() == drawn) return;
				const auto& draw = draws.back();
				count.CallsTranslucent += draw.Translucency != 0;
				count.CallsWriting += draw.WritesDepth;
				count.CallsTinted += draw.Tint != 0;
			};
			for (const auto& item : Recorded)
			{
				if (const auto* line = std::get_if<RecordedLine>(&item))
				{
					const bool glyph = line->Kind == Primitive::Glyph;
					if (!line->OnTarget) { ++(glyph ? count.GlyphsElsewhere : count.LinesElsewhere); continue; }
					++(glyph ? count.Glyphs : count.Lines);
					if (glyph && !line->Glyph) { ++count.GlyphsUnknown; continue; }
					if (line->Clip.Width <= 0 || line->Clip.Height <= 0) { if (!glyph) ++count.LinesEmpty; continue; }
					count.LinesAfter += line->AfterLayers;
					// 4C0050's depth of a pixel: the ZBuffer's MaxValue and Bounds.Y, less the clip's Y and the pixel's,
					// plus the Z it steps to from the ends'.
					const int base = capture.MaxValue + capture.BoundsY - line->Clip.Y;
					Line placed;
					placed.Kind = line->Kind;
					placed.Clip = line->Clip;
					placed.X0 = line->From.X;
					placed.Y0 = line->From.Y;
					placed.X1 = line->To.X;
					placed.Y1 = line->To.Y;
					if (line->Kind == Primitive::Line)
					{
						placed.Depth0 = base - line->From.Y + line->ZFrom;
						placed.Depth1 = base - line->To.Y + line->ZTo;
					}
					placed.Color = line->Color;
					placed.WritesDepth = line->WritesDepth;
					placed.Glyph = line->Glyph;
					placed.AfterLayers = line->AfterLayers;
					if (line->Kind == Primitive::Dashed)
					{
						// 4C0750 steps from the end on the left; drawn from the right, its pattern runs down from the
						// offset plus the longer of the width and the height plus 1.
						const bool backward = line->From.X > line->To.X;
						const auto& first = backward ? line->To : line->From;
						const auto& last = backward ? line->From : line->To;
						placed.X0 = first.X; placed.Y0 = first.Y; placed.X1 = last.X; placed.Y1 = last.Y;
						const int steps = std::max(std::abs(last.X - first.X), std::abs(last.Y - first.Y) + 1);
						placed.Phase = (((backward ? steps + line->Offset : line->Offset) % 16) + 16) % 16;
						placed.Backward = backward;
						placed.Pattern = line->Pattern;
						placed.Shrouded = line->Shrouded;
					}
					placed.Before = capture.Target->size();
					placed.Caller = line->Caller;
					Current.CompositeLines.push_back(placed);
					continue;
				}
				if (const auto* blit = std::get_if<RecordedBlit>(&item))
				{
					if (!blit->OnTarget) { ++count.BlitsElsewhere; continue; }
					++count.Blits;
					if (blit->Issue != BlitIssue::None)
					{
						if (!count.BlitsUnmodeled++) FirstUnmodeledBlit = *blit;
						continue;
					}
					const auto drawn = capture.Target->size();
					capture.PlaceBlit(*blit);
					counted(*capture.Target, drawn);
					continue;
				}
				const auto& recorded = std::get<RecordedCall>(item);
				if (!recorded.OnTarget) { ++count.CallsElsewhere; continue; }
				++count.Calls;
				if (!(recorded.Flags & CenteredFlag) || (recorded.Flags & ~ModeledFlags) || recorded.Remap || (recorded.ZShape && recorded.ZShapeFrame))
				{
					if (!count.CallsUnmodeled++) FirstUnmodeled = recorded;
					continue;
				}
				Call call;
				call.Image = recorded.Image;
				call.Frame = recorded.Frame;
				call.Convert = recorded.Convert;
				call.Brightness = recorded.Brightness;
				call.ZAdjust = recorded.ZAdjust;
				call.Gradient = recorded.Gradient;
				call.ZShape = recorded.ZShape;
				call.ZShapeX = recorded.ZShapeX;
				call.ZShapeY = recorded.ZShapeY;
				call.Bounded = false;
				call.WritesDepth = recorded.Flags & ZWriteFlag;
				call.TestsDepth = call.WritesDepth || (recorded.Flags & ZReadMask);
				call.Translucency = std::uint8_t(recorded.Flags & TranslucencyMask);
				call.Tint = recorded.Tint;
				call.Bounds = &recorded.Bounds;
				if (!call.Convert) { ++count.Palette; continue; }
				const auto& bounds = recorded.Bounds;
				const auto drawn = capture.Target->size();
				capture.Place(0, 0, Layer::Composite, int(recorded.Caller), call,
					{std::int64_t(bounds.X) + recorded.Point.X, std::int64_t(bounds.Y) + recorded.Point.Y}, recorded.Flags & ShadowFlag);
				counted(*capture.Target, drawn);
			}
			for (int layer = 0; layer < LayerCount; ++layer)
			{
				const auto& items = MapClass::ObjectsInLayers[layer];
				for (int i = 0; i < items.Count; ++i)
				{
					auto* object = items.Items[i];
					if (!CursorSnapshot::Readable(object, sizeof(ObjectClass))) continue;
					++count.LayerObjects;
					if (!object->IsVisible || object->WhatAmI() == AbstractType::Terrain || object->WhatAmI() == AbstractType::Anim) continue;
					++count.OthersVisible;
					RectangleStruct dimensions {};
					object->GetRenderDimensions(&dimensions);
					// A voxel unit gives an empty rectangle: what it drew stays unexplained.
					if (dimensions.Width <= 0 || dimensions.Height <= 0) ++count.OthersUnbounded;
					dimensions.X += capture.ViewX;
					dimensions.Y += capture.ViewY;
					Current.OtherBounds.push_back(dimensions);
				}
			}
			capture.Target = &Current.Draws;
			capture.Count = &Current.Count;
		}

		// The software replay of the captured Tile pass against the native Tile and ZBuffer.
		struct TileCheck
		{
			int Compared = 0, ColorCompared = 0, ColorMismatched = 0, DepthMismatched = 0, TerrainColor = 0, OverlayColor = 0, ShadowColor = 0,
				Rejected = 0, Unknown = 0, Missing = 0, ZShaped = 0;
			int Left = INT_MAX, Top = INT_MAX, Right = INT_MIN, Bottom = INT_MIN;
			int FirstX = -1, FirstY = -1, NativeColor = 0, ModelColor = 0, Owner = -1;
			int DepthX = -1, DepthY = -1, NativeDepth = 0, ModelDepth = 0, DepthOwner = -1;
			// The first mismatched pixels: column, row, native and model color and depth, owner.
			std::vector<std::array<int, 7>> Samples;
		};

		// Owners of a model pixel: nothing drawn, a terrain image, or the index of an overlay draw.
		constexpr int NoOwner = -2, TerrainOwner = -1;

		// A software model of the clip: color, depth and ABuffer words, and the owner of each pixel.
		struct Model
		{
			std::vector<std::uint16_t> Color, Depth, Alpha;
			std::vector<int> Owner;
			// A shadow darkened a pixel whose color the model does not know.
			std::vector<std::uint8_t> Unknown;
			int Rejected = 0, Missing = 0, ZShaped = 0;
		};

		// The native ABuffer under the clip: 4373B0 and 437A10 address the ring buffer by the draw
		// position less its Bounds.Y.
		std::vector<std::uint16_t> NativeAlpha(const RectangleStruct& clip)
		{
			std::vector<std::uint16_t> alpha(size_t(clip.Width) * clip.Height, TerrainImage::DefaultAlpha);
			auto* abuffer = ABuffer::Instance;
			if (CursorSnapshot::Readable(abuffer, sizeof(ABuffer)))
				for (int row = 0; row < clip.Height; ++row)
				{
					auto* word = static_cast<std::uint16_t*>(abuffer->GetBuffer(clip.X, clip.Y + row - abuffer->Bounds.Y));
					for (int column = 0; column < clip.Width; ++column, ++word)
					{
						abuffer->AdjustPointer(word);
						alpha[size_t(row) * clip.Width + column] = *word;
					}
				}
			return alpha;
		}

		// The native ZBuffer under the clip: 547CF0 and the shape blitters address the ring buffer by
		// the draw position less ViewBounds.Y.
		std::vector<std::uint16_t> NativeDepth(const RectangleStruct& clip)
		{
			std::vector<std::uint16_t> depth(size_t(clip.Width) * clip.Height);
			auto* zbuffer = ZBuffer::Instance;
			for (int row = 0; row < clip.Height; ++row)
			{
				auto* word = static_cast<std::uint16_t*>(zbuffer->GetBuffer(clip.X, clip.Y + row - DSurface::ViewBounds.Y));
				for (int column = 0; column < clip.Width; ++column, ++word)
				{
					zbuffer->AdjustPointer(word);
					depth[size_t(row) * clip.Width + column] = *word;
				}
			}
			return depth;
		}

		// 4AED70's draws of a list in order: a pixel that passes the Z test takes the body's color or is
		// darkened by the shadow, and keeps the draw's depth where the draw writes Z.
		void ReplayDraws(const std::vector<Draw>& draws, const RectangleStruct& clip, Model& model)
		{
			for (size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex)
			{
				const auto& draw = draws[drawIndex];
				const auto* frame = draw.Storage->Resolve(draw.ID);
				if (!frame) { ++model.Missing; continue; }
				const int pitch = frame->Geometry.Width;
				if (frame->Indices.size() < size_t(pitch) * frame->Geometry.Height || draw.FrameX + draw.Width > pitch
					|| draw.FrameY + draw.Height > frame->Geometry.Height) { ++model.Missing; continue; }
				if (draw.ZShaped) ++model.ZShaped;
				const auto& colors = draw.Table->Colors;
				for (int j = 0; j < draw.Height; ++j)
				{
					const auto base = ShapeDepth::RowBase(draw.Rows, draw.Gradient, j);
					const auto* indices = frame->Indices.data() + size_t(draw.FrameY + j) * pitch + draw.FrameX;
					const auto* zrow = draw.ZShaped ? draw.ZBytes.data() + size_t(j) * draw.Width : nullptr;
					for (int i = 0; i < draw.Width; ++i)
					{
						const auto index = indices[i];
						if (!index) continue;
						const auto at = size_t(draw.Y - clip.Y + j) * clip.Width + size_t(draw.X - clip.X + i);
						const auto value = ShapeDepth::Depth(base, zrow ? zrow[i] : 0);
						if (draw.TestsDepth && !ShapeDepth::Passes(value, model.Depth[at])) { ++model.Rejected; continue; }
						if (draw.WritesDepth) model.Depth[at] = std::uint16_t(value);
						if (draw.Shadow)
						{
							if (model.Owner[at] == NoOwner) model.Unknown[at] = 1;
							model.Color[at] = ShapeDepth::Darken(model.Color[at], draw.Mask);
						}
						else
						{
							const int shade = TerrainImage::ShadeRow(int(draw.Table->Shades), draw.LightRow, model.Alpha[at]);
							const auto incoming = std::uint16_t(colors[size_t(shade) * 256 + index] | draw.Tint);
							// A translucent draw over a color the model does not know stays unknown.
							if (draw.Rounded) model.Color[at] = ShapeDepth::RoundedBlend(model.Color[at], incoming, draw.Translucency);
							else if (draw.Translucency) model.Color[at] = ShapeDepth::Blend(model.Color[at], incoming, draw.Translucency, draw.Mask, draw.QuarterMask);
							else
							{
								model.Color[at] = incoming;
								model.Unknown[at] = 0;
							}
						}
						model.Owner[at] = int(drawIndex);
					}
				}
			}
		}

		TileCheck Replay(const RectangleStruct& clip)
		{
			TileCheck check;
			const auto size = size_t(clip.Width) * clip.Height;
			Model model {std::vector<std::uint16_t>(size, 0), std::vector<std::uint16_t>(size, 0xFFFF), NativeAlpha(clip),
				std::vector<int>(size, NoOwner), std::vector<std::uint8_t>(size, 0)};
			auto& color = model.Color;
			auto& depth = model.Depth;
			const auto& alpha = model.Alpha;
			auto& owner = model.Owner;
			auto& unknown = model.Unknown;
			for (const auto& placement : TerrainSnapshot::Current.Placements)
			{
				const auto* frame = placement.Storage->Resolve(placement.ID);
				const auto plane = size_t(placement.Width) * placement.Height;
				if (!frame || frame->Indices.size() < plane * (placement.UsesZ ? 2 : 1) || !placement.Table) { ++check.Missing; continue; }
				const auto* indices = frame->Indices.data();
				const auto& colors = placement.Table->Colors;
				for (int row = std::max(0, clip.Y - placement.Y); row < placement.Height && placement.Y + row < clip.Y + clip.Height; ++row)
					for (int column = std::max(0, clip.X - placement.X); column < placement.Width && placement.X + column < clip.X + clip.Width; ++column)
					{
						const auto source = size_t(row) * placement.Width + column;
						const auto index = indices[source];
						if (placement.Diamond ? !TerrainImage::InsideDiamond(column, row) : index == 0) continue;
						const auto at = size_t(placement.Y + row - clip.Y) * clip.Width + size_t(placement.X + column - clip.X);
						if (placement.UsesZ)
						{
							// 547CF0 draws where the old depth is not smaller.
							const auto z = TerrainImage::PixelDepth(placement.Depth, indices[plane + source]);
							if (depth[at] < z) continue;
							depth[at] = z;
						}
						const int shade = TerrainImage::ShadeRow(int(placement.Table->Shades), placement.LightRow, alpha[at]);
						color[at] = colors[size_t(shade) * 256 + index];
						owner[at] = TerrainOwner;
						unknown[at] = 0;
					}
			}

			ReplayDraws(Current.Draws, clip, model);
			check.Rejected = model.Rejected;
			check.Missing += model.Missing;
			check.ZShaped = model.ZShaped;

			const auto nativeDepth = NativeDepth(clip);
			auto* tile = DSurface::Tile;
			const auto* pixels = static_cast<const std::uint8_t*>(tile->Lock(clip.X, clip.Y));
			if (!pixels) { check.Missing = -1; return check; }
			const int tilePitch = tile->GetPitch();
			for (int row = 0; row < clip.Height; ++row)
			{
				const auto* native = reinterpret_cast<const std::uint16_t*>(pixels + ptrdiff_t(row) * tilePitch);
				for (int column = 0; column < clip.Width; ++column)
				{
					const auto at = size_t(row) * clip.Width + column;
					++check.Compared;
					if ((nativeDepth[at] != depth[at] || (owner[at] != NoOwner && !unknown[at] && native[column] != color[at])) && check.Samples.size() < 24)
						check.Samples.push_back({column, row, native[column], color[at], nativeDepth[at], depth[at], owner[at]});
					if (nativeDepth[at] != depth[at] && !check.DepthMismatched++)
					{
						check.DepthX = column; check.DepthY = row; check.NativeDepth = nativeDepth[at]; check.ModelDepth = depth[at]; check.DepthOwner = owner[at];
					}
					if (owner[at] == NoOwner) continue;
					if (unknown[at]) { ++check.Unknown; continue; }
					++check.ColorCompared;
					if (native[column] == color[at]) continue;
					if (owner[at] == TerrainOwner) ++check.TerrainColor;
					else ++(Current.Draws[size_t(owner[at])].Shadow ? check.ShadowColor : check.OverlayColor);
					if (!check.ColorMismatched++)
					{
						check.FirstX = column; check.FirstY = row; check.NativeColor = native[column]; check.ModelColor = color[at]; check.Owner = owner[at];
					}
					check.Left = std::min(check.Left, column); check.Right = std::max(check.Right, column);
					check.Top = std::min(check.Top, row); check.Bottom = std::max(check.Bottom, row);
				}
			}
			tile->Unlock();
			return check;
		}

		// The replay of 6D8DB0's Terrain draws over the native Composite before the layers, against the
		// one after them (D2). Their input must be Tile's view: nothing else drew before the layers.
		// The pixels of the clip a recorded line, fill or glyph may have drawn: a fill's or glyph's rectangle, and each pixel a line
		// steps through with its neighbors, since the GPU's line and the native one may pick the next pixel.
		std::vector<std::uint8_t> LineCoverage(const std::vector<Line>& lines, const RectangleStruct& clip)
		{
			std::vector<std::uint8_t> covered(size_t(clip.Width) * clip.Height, 0);
			const auto mark = [&](std::int64_t x, std::int64_t y)
			{
				if (x >= clip.X && y >= clip.Y && x < std::int64_t(clip.X) + clip.Width && y < std::int64_t(clip.Y) + clip.Height)
					covered[size_t(y - clip.Y) * clip.Width + size_t(x - clip.X)] = 1;
			};
			for (const auto& line : lines)
			{
				if (line.AfterLayers) continue;
				if (line.Kind == Primitive::Fill || line.Kind == Primitive::Glyph)
				{
					const int left = std::max(line.X0, line.Clip.X), top = std::max(line.Y0, line.Clip.Y);
					const auto right = std::min<std::int64_t>(line.X1, std::int64_t(line.Clip.X) + line.Clip.Width);
					const auto bottom = std::min<std::int64_t>(line.Y1, std::int64_t(line.Clip.Y) + line.Clip.Height);
					for (int y = top; y < bottom; ++y)
						for (int x = left; x < right; ++x) mark(x, y);
					continue;
				}
				const std::int64_t dx = std::int64_t(line.X1) - line.X0, dy = std::int64_t(line.Y1) - line.Y0;
				const auto steps = std::max<std::int64_t>({dx < 0 ? -dx : dx, dy < 0 ? -dy : dy, 1});
				// A line far off the clip has no pixels to mark.
				if (steps > 4 * (std::int64_t(clip.Width) + clip.Height)) continue;
				for (std::int64_t i = 0; i <= steps; ++i)
				{
					const auto x = line.X0 + (dx * i + (dx >= 0 ? steps / 2 : -steps / 2)) / steps;
					const auto y = line.Y0 + (dy * i + (dy >= 0 ? steps / 2 : -steps / 2)) / steps;
					for (int ny = -1; ny <= 1; ++ny)
						for (int nx = -1; nx <= 1; ++nx) mark(x + nx, y + ny);
				}
			}
			return covered;
		}

		struct CompositeCheck
		{
			const char* Input = "", * Output = "";
			int InputCompared = 0, InputMismatched = 0, InputX = -1, InputY = -1, InputNative = 0, InputCopy = 0;
			// Mismatches by a recorded line or fill, which the replay does not draw; of the others, those untouched
			// inside another object's rectangle, and the rest.
			int Compared = 0, Drawn = 0, Mismatched = 0, Lines = 0, Untouched = 0, Others = 0, Unexplained = 0, Body = 0, Shadow = 0, Rejected = 0, Missing = 0;
			int Left = INT_MAX, Top = INT_MAX, Right = INT_MIN, Bottom = INT_MIN;
			int FirstX = -1, FirstY = -1, NativeColor = 0, ModelColor = 0, Owner = NoOwner;
			// The first mismatch on a pixel a draw of the list owns.
			int DrawnX = -1, DrawnY = -1, DrawnNative = 0, DrawnModel = 0, DrawnOwner = NoOwner;
			// The first mismatched pixels: column, row, native and model color, owner.
			std::vector<std::array<int, 5>> Samples;
		};

		// Ready where the copy holds this render's ViewBounds.
		const char* CopyState(const CompositeSnapshot::Copy& copy, const RectangleStruct& clip)
		{
			if (!copy.Valid) return copy.Error;
			if (copy.Bounds.X != clip.X || copy.Bounds.Y != clip.Y || copy.Bounds.Width != clip.Width || copy.Bounds.Height != clip.Height) return "bounds_differ";
			if (copy.Frame != Unsorted::CurrentFrame) return "stale";
			return nullptr;
		}

		CompositeCheck CompositeReplay(const RectangleStruct& clip)
		{
			CompositeCheck check;
			const auto& input = CompositeSnapshot::Input;
			const auto& output = CompositeSnapshot::Output;
			const auto* inputState = CopyState(input, clip);
			const auto* outputState = CopyState(output, clip);
			check.Input = inputState ? inputState : "ok";
			check.Output = outputState ? outputState : "ok";
			if (inputState || outputState) return check;
			// Tile's view, which mode 1 copied to Composite at 6D45D8.
			auto* tile = DSurface::Tile;
			if (const auto* pixels = static_cast<const std::uint8_t*>(tile->Lock(clip.X, clip.Y)))
			{
				const int pitch = tile->GetPitch();
				for (int row = 0; row < clip.Height; ++row)
				{
					const auto* native = reinterpret_cast<const std::uint16_t*>(pixels + ptrdiff_t(row) * pitch);
					for (int column = 0; column < clip.Width; ++column)
					{
						++check.InputCompared;
						const auto copied = input.Pixels[size_t(row) * clip.Width + column];
						if (native[column] == copied || check.InputMismatched++) continue;
						check.InputX = column; check.InputY = row; check.InputNative = native[column]; check.InputCopy = copied;
					}
				}
				tile->Unlock();
			}
			else check.InputCompared = -1;
			// The layers start from mode 1's A and Z (#23 §5); the Terrain draws write neither.
			const auto size = size_t(clip.Width) * clip.Height;
			Model model {input.Pixels, NativeDepth(clip), NativeAlpha(clip), std::vector<int>(size, NoOwner), std::vector<std::uint8_t>(size, 0)};
			ReplayDraws(Current.CompositeDraws, clip, model);
			check.Rejected = model.Rejected;
			check.Missing = model.Missing;
			const auto lines = LineCoverage(Current.CompositeLines, clip);
			for (int row = 0; row < clip.Height; ++row)
				for (int column = 0; column < clip.Width; ++column)
				{
					const auto at = size_t(row) * clip.Width + column;
					const int owner = model.Owner[at];
					++check.Compared;
					check.Drawn += owner != NoOwner;
					const auto native = output.Pixels[at], color = model.Color[at];
					if (native == color) continue;
					if (lines[at]) ++check.Lines;
					else if (owner == NoOwner)
					{
						++check.Untouched;
						const int x = clip.X + column, y = clip.Y + row;
						const bool other = std::any_of(Current.OtherBounds.begin(), Current.OtherBounds.end(), [x, y](const RectangleStruct& bounds)
						{
							return x >= bounds.X && y >= bounds.Y && x - bounds.X < bounds.Width && y - bounds.Y < bounds.Height;
						});
						++(other ? check.Others : check.Unexplained);
					}
					else
					{
						if (!check.Body && !check.Shadow)
						{
							check.DrawnX = column; check.DrawnY = row; check.DrawnNative = native; check.DrawnModel = color; check.DrawnOwner = owner;
						}
						++(Current.CompositeDraws[size_t(owner)].Shadow ? check.Shadow : check.Body);
					}
					if (check.Samples.size() < 24) check.Samples.push_back({column, row, native, color, owner});
					if (!check.Mismatched++)
					{
						check.FirstX = column; check.FirstY = row; check.NativeColor = native; check.ModelColor = color; check.Owner = owner;
					}
					check.Left = std::min(check.Left, column); check.Right = std::max(check.Right, column);
					check.Top = std::min(check.Top, row); check.Bottom = std::max(check.Bottom, row);
				}
			return check;
		}

		char LayerLetter(Layer layer)
		{
			switch (layer)
			{
			case Layer::Overlay: return 'o';
			case Layer::Terrain: return 't';
			case Layer::Cliff: return 'c';
			case Layer::Building: return 'b';
			case Layer::Fogged: return 'f';
			case Layer::Composite: return 'r';
			default: return 'a';
			}
		}

		void Trace()
		{
			const auto& count = Current.Count;
			++TraceCount;
			Debug::Log("[TerrainGPU] overlay sample=%u ready=%d visited=%d legal=%d overlays=%d bodies=%d shadows=%d noBody=%d outside=%d empty=%d "
				"rubble=%d tiberium=%d palette=%d frame=%d zshape=%d ramps=%d bridges=%d walls=%d compressed=%d noImage=%d unloaded=%d slopes=%d unsupported=%d failure=%d:%d/%d@%d "
				"terrains=%d dynamic=%d terrainOutside=%d damaged=%d casters=%d fogged=%d foggedBuildings=%d foggedInView=%d/%d gpu=%zu/%zu\n",
				TraceCount, Current.Visible, count.Visited, count.Legal, count.Overlays, count.Bodies, count.Shadows, count.NoBody, count.Outside,
				count.Empty, count.Rubble, count.Tiberium, count.Palette, count.Frame, count.ZShape, count.Ramps, count.Bridges, count.Walls, count.Compressed, count.NoImage, count.Unloaded, count.Slopes, count.Unsupported, FirstFailure[0], FirstFailure[1], FirstFailure[2],
				FirstFailure[3], count.Terrains, count.Dynamic, count.TerrainOutside, count.Damaged, count.Casters,
				count.Fogged, count.FoggedBuildings, count.FoggedBuildingsInView, count.FoggedOthersInView, TerrainPass::ShapeCount(), TerrainPass::SkippedShapes());
			Debug::Log("[TerrainGPU] buildings sample=%u buildings=%d fogged=%d outside=%d unsupported=%d gates=%d unloading=%d bibs=%d doors=%d "
				"foggedAnims=%d foggedDetail=%d foggedInvisible=%d foggedUnsorted=%d\n", TraceCount,
				count.Buildings, count.BuildingsFogged, count.BuildingsOutside, count.BuildingsUnsupported, count.Gates, count.Unloading, count.Bibs, count.Doors,
				count.FoggedAnims, count.FoggedDetail, count.FoggedInvisible, count.FoggedUnsorted);
			if (!Current.Visible) return;
			// Each overlay (o), Terrain (t), cliff tile (c), building (b), fogged building (f) and fogged anim (a) type drawn: bodies,
			// shadows, RLE frames and Z shapes.
			std::map<std::pair<Layer, int>, std::array<int, 4>> types;
			for (const auto& draw : Current.Draws)
			{
				auto& type = types[{draw.Source, draw.Index}];
				++type[draw.Shadow ? 1 : 0];
				type[2] += draw.Compressed;
				type[3] += draw.ZShaped;
			}
			std::string list;
			for (const auto& [key, type] : types)
			{
				char item[48];
				std::snprintf(item, sizeof(item), " %c%d:%d/%d/%d/%d", LayerLetter(key.first), key.second, type[0], type[1], type[2], type[3]);
				list += item;
			}
			Debug::Log("[TerrainGPU] overlaytypes sample=%u%s\n", TraceCount, list.c_str());
			// D1c: each draw of 4D1890's building records (f: building part, a: anim) with its Tile rectangle.
			std::string fogged;
			for (const auto& draw : Current.Draws)
			{
				if (draw.Source != Layer::Fogged && draw.Source != Layer::FoggedAnim) continue;
				char item[64];
				std::snprintf(item, sizeof(item), " %c%d:%d%s@%d,%d,%dx%d", LayerLetter(draw.Source), draw.Index, draw.ShapeFrame, draw.Shadow ? "s" : "",
					draw.X, draw.Y, draw.Width, draw.Height);
				fogged += item;
			}
			if (!fogged.empty()) Debug::Log("[TerrainGPU] foggeddraws sample=%u%s\n", TraceCount, fogged.c_str());
			const auto check = Replay(TerrainSnapshot::Current.Clip);
			const auto describe = [](int owner, const std::vector<Draw>& draws = Current.Draws)
			{
				static char text[192];
				if (owner < 0) std::snprintf(text, sizeof(text), "%s", owner == TerrainOwner ? "terrain" : "none");
				else
				{
					const auto& draw = draws[size_t(owner)];
					// A recorded call's index is its caller's address.
					std::snprintf(text, sizeof(text), draw.Source == Layer::Composite
						? "%s:%d,%d:%c%X:f%d:z=%d:g=%d:rle=%d:zshape=%d:rect=%d,%d,%d,%d:frame=%d,%d"
						: "%s:%d,%d:%c%d:f%d:z=%d:g=%d:rle=%d:zshape=%d:rect=%d,%d,%d,%d:frame=%d,%d", draw.Shadow ? "shadow" : "body",
						draw.CellX, draw.CellY, LayerLetter(draw.Source), draw.Index, draw.ShapeFrame, draw.ZAdjust, draw.GradientIndex, draw.Compressed, draw.ZShaped,
						draw.X, draw.Y, draw.Width, draw.Height, draw.FrameX, draw.FrameY);
				}
				return text;
			};
			Debug::Log("[TerrainGPU] tilecheck sample=%u compared=%d colorCompared=%d colorMismatched=%d terrain=%d body=%d shadow=%d unknown=%d "
				"depthMismatched=%d rejected=%d missing=%d zshaped=%d box=%d,%d,%d,%d first=%d,%d:native=%04X:model=%04X:%s\n",
				TraceCount, check.Compared, check.ColorCompared, check.ColorMismatched, check.TerrainColor, check.OverlayColor, check.ShadowColor,
				check.Unknown, check.DepthMismatched, check.Rejected, check.Missing, check.ZShaped,
				check.ColorMismatched ? check.Left : 0, check.ColorMismatched ? check.Top : 0, check.ColorMismatched ? check.Right : 0,
				check.ColorMismatched ? check.Bottom : 0, check.FirstX, check.FirstY, check.NativeColor, check.ModelColor, describe(check.Owner));
			std::string samples;
			for (const auto& sample : check.Samples)
			{
				char item[64];
				std::snprintf(item, sizeof(item), " %d,%d:%04X/%04X:%d/%d:%d", sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6]);
				samples += item;
			}
			if (!samples.empty()) Debug::Log("[TerrainGPU] tilesamples sample=%u%s\n", TraceCount, samples.c_str());
			Debug::Log("[TerrainGPU] tiledepth sample=%u first=%d,%d:native=%d:model=%d:%s\n", TraceCount, check.DepthX, check.DepthY,
				check.NativeDepth, check.ModelDepth, describe(check.DepthOwner));

			// D3: RenderLayers' recorded 4AED70 calls and blits on Composite, each draw in order (caller:frame,
			// -1 for a blit, s for a shadow, t translucent, w writing Z, n not testing it, @ its rectangle), and
			// their replay over the copies around the layers.
			const auto& composite = Current.CompositeCount;
			Debug::Log("[TerrainGPU] composite sample=%u layerObjects=%d othersVisible=%d unbounded=%d calls=%d elsewhere=%d unmodeled=%d "
				"firstUnmodeled=%X:flags=%X:remap=%d:zshapeFrame=%d bodies=%d shadows=%d compressed=%d translucent=%d writing=%d tinted=%d palette=%d frame=%d "
				"noImage=%d unloaded=%d empty=%d outside=%d unsupported=%d zshape=%d copyFailed=%u blits=%d blitsElsewhere=%d blitsUnmodeled=%d "
				"firstUnmodeledBlit=%X:issue=%d:rle=%d:flags=%X blitImages=%u blitBytes=%u epoch=%u lines=%d linesElsewhere=%d linesEmpty=%d "
				"glyphs=%d glyphsElsewhere=%d glyphsUnknown=%d linesAfter=%d\n",
				TraceCount, composite.LayerObjects, composite.OthersVisible, composite.OthersUnbounded, composite.Calls, composite.CallsElsewhere,
				composite.CallsUnmodeled, unsigned(FirstUnmodeled.Caller), FirstUnmodeled.Flags, FirstUnmodeled.Remap, FirstUnmodeled.ZShapeFrame,
				composite.Bodies, composite.Shadows, composite.Compressed, composite.CallsTranslucent, composite.CallsWriting, composite.CallsTinted,
				composite.Palette, composite.Frame, composite.NoImage, composite.Unloaded, composite.Empty, composite.Outside, composite.Unsupported,
				composite.ZShape, CompositeSnapshot::Failed, composite.Blits, composite.BlitsElsewhere, composite.BlitsUnmodeled,
				unsigned(FirstUnmodeledBlit.Caller), int(FirstUnmodeledBlit.Issue), FirstUnmodeledBlit.Compressed, FirstUnmodeledBlit.Flags,
				unsigned(BlitFrameSource::Frames.GetStats().Frames), unsigned(BlitFrameSource::Frames.GetStats().IndexBytes), BlitFrameSource::Epoch,
				composite.Lines, composite.LinesElsewhere, composite.LinesEmpty, composite.Glyphs, composite.GlyphsElsewhere, composite.GlyphsUnknown, composite.LinesAfter);
			std::string draws;
			for (const auto& draw : Current.CompositeDraws)
			{
				char item[80];
				std::snprintf(item, sizeof(item), " %X:%d%s%s%s@%d,%d,%dx%d", unsigned(draw.Index), draw.ShapeFrame, draw.Shadow ? "s" : "",
					draw.Translucency ? "t" : "", draw.WritesDepth ? "w" : (draw.TestsDepth ? "" : "n"), draw.X, draw.Y, draw.Width, draw.Height);
				draws += item;
			}
			if (!draws.empty()) Debug::Log("[TerrainGPU] compositedraws sample=%u%s\n", TraceCount, draws.c_str());
			// Each line (l), plain line (p), fill (f), glyph (g) and dashed line (d) in order: caller, the draw it
			// precedes, its ends (a fill's or glyph's corners), depths, color, w where it writes Z, a after RenderLayers.
			std::string lines;
			for (const auto& line : Current.CompositeLines)
			{
				char item[96];
				std::snprintf(item, sizeof(item), " %c%X:%u@%d,%d-%d,%d:z=%d/%d:%04X%s%s",
					"lpfgd"[size_t(line.Kind)], unsigned(line.Caller), unsigned(line.Before),
					line.X0, line.Y0, line.X1, line.Y1, line.Depth0, line.Depth1, line.Color, line.WritesDepth ? "w" : "", line.AfterLayers ? "a" : "");
				lines += item;
			}
			if (!lines.empty()) Debug::Log("[TerrainGPU] compositelines sample=%u%s\n", TraceCount, lines.c_str());
			const auto compositeCheck = CompositeReplay(TerrainSnapshot::Current.Clip);
			Debug::Log("[TerrainGPU] compositecheck sample=%u input=%s output=%s inputCompared=%d inputMismatched=%d inputFirst=%d,%d:tile=%04X:copy=%04X "
				"compared=%d drawn=%d mismatched=%d lines=%d untouched=%d others=%d unexplained=%d body=%d shadow=%d rejected=%d missing=%d box=%d,%d,%d,%d first=%d,%d:native=%04X:model=%04X:%s\n",
				TraceCount, compositeCheck.Input, compositeCheck.Output, compositeCheck.InputCompared, compositeCheck.InputMismatched, compositeCheck.InputX,
				compositeCheck.InputY, compositeCheck.InputNative, compositeCheck.InputCopy, compositeCheck.Compared, compositeCheck.Drawn, compositeCheck.Mismatched,
				compositeCheck.Lines, compositeCheck.Untouched, compositeCheck.Others, compositeCheck.Unexplained, compositeCheck.Body, compositeCheck.Shadow, compositeCheck.Rejected, compositeCheck.Missing,
				compositeCheck.Mismatched ? compositeCheck.Left : 0, compositeCheck.Mismatched ? compositeCheck.Top : 0,
				compositeCheck.Mismatched ? compositeCheck.Right : 0, compositeCheck.Mismatched ? compositeCheck.Bottom : 0,
				compositeCheck.FirstX, compositeCheck.FirstY, compositeCheck.NativeColor, compositeCheck.ModelColor,
				describe(compositeCheck.Owner, Current.CompositeDraws));
			if (compositeCheck.DrawnOwner != NoOwner)
				Debug::Log("[TerrainGPU] compositedrawn sample=%u first=%d,%d:native=%04X:model=%04X:%s\n", TraceCount, compositeCheck.DrawnX, compositeCheck.DrawnY,
					compositeCheck.DrawnNative, compositeCheck.DrawnModel, describe(compositeCheck.DrawnOwner, Current.CompositeDraws));
			samples.clear();
			for (const auto& sample : compositeCheck.Samples)
			{
				char item[48];
				std::snprintf(item, sizeof(item), " %d,%d:%04X/%04X:%d", sample[0], sample[1], sample[2], sample[3], sample[4]);
				samples += item;
			}
			if (!samples.empty()) Debug::Log("[TerrainGPU] compositesamples sample=%u%s\n", TraceCount, samples.c_str());
		}
	}

	void CaptureAfterDraw(bool trace)
	{
		Current.Visible = false;
		Current.Draws.clear();
		Current.Count = {};
		Current.CompositeDraws.clear();
		Current.CompositeLines.clear();
		Current.CompositeCount = {};
		Current.OtherBounds.clear();
		FirstFailure = {};
		const auto& terrain = TerrainSnapshot::Current;
		const TacticalClass* tactical = TacticalClass::Instance;
		if (terrain.Visible && tactical)
		{
			const auto& clip = terrain.Clip;
			const auto& view = DSurface::ViewBounds;
			Capture capture {clip, tactical->TacticalPos.X, tactical->TacticalPos.Y, view.X, view.Y, ZBuffer::Instance->MaxValue, ZBuffer::Instance->Bounds.Y, {}};
			const auto anchor = TerrainImage::ScanAnchor(capture.TacticalX + clip.X - view.X, capture.TacticalY + clip.Y - view.Y);
			const int rows = OverlayImage::ScanRows(clip.Height), columns = OverlayImage::ScanColumns(clip.Width);
			const int margin = TerrainSnapshot::ScanMargin;
			auto& count = Current.Count;
			// 4D1890 (step 4, before the overlays): the records in their sorted order, those enabled whose
			// saved rectangle, moved by the view, meets the clip.
			{
				const auto* pairs = *reinterpret_cast<const std::uint32_t* const*>(FoggedPairs);
				count.Fogged = *reinterpret_cast<const int*>(FoggedCount);
				std::vector<std::array<std::uint32_t, 2>> records;
				for (int i = 0; i < count.Fogged && CursorSnapshot::Readable(pairs + 2 * i, 2 * sizeof(std::uint32_t)); ++i)
					records.push_back({pairs[2 * i], pairs[2 * i + 1]});
				// A record made after this frame's draw unsorts them; 4D1890 sorts them by key next time.
				if (!*reinterpret_cast<const bool*>(FoggedSorted))
				{
					++count.FoggedUnsorted;
					std::qsort(records.data(), records.size(), sizeof(records[0]), reinterpret_cast<int(__cdecl*)(const void*, const void*)>(FoggedCompare));
				}
				for (const auto& [key, address] : records)
				{
					const auto* record = reinterpret_cast<const std::uint8_t*>(std::uintptr_t(address));
					if (!CursorSnapshot::Readable(record, FoggedRecordSize)) continue;
					const bool building = *reinterpret_cast<const int*>(record + FoggedKind) == FoggedBuilding;
					count.FoggedBuildings += building;
					const auto& bounds = *reinterpret_cast<const RectangleStruct*>(record + FoggedBounds);
					if (!record[FoggedEnabled] || !TerrainSnapshot::Intersects(clip, std::int64_t(bounds.X) - capture.TacticalX + capture.ViewX,
						std::int64_t(bounds.Y) - capture.TacticalY + capture.ViewY, bounds.Width, bounds.Height)) continue;
					++(building ? count.FoggedBuildingsInView : count.FoggedOthersInView);
					if (building) capture.FoggedBuilding(record);
				}
			}
			// 6D6D10: the bodies of all cells, then their shadows in the same order.
			for (int pass = 0; pass < 2; ++pass)
				for (int r = rows - 1 + margin; r >= -margin; --r)
					for (int c = -margin; c < columns + margin; ++c)
					{
						const auto [cellX, cellY] = OverlayImage::ScanCell(anchor, r, c);
						if (cellX < INT16_MIN || cellX > INT16_MAX || cellY < INT16_MIN || cellY > INT16_MAX) continue;
						if (!pass) ++count.Visited;
						const CellStruct coords {std::int16_t(cellX), std::int16_t(cellY)};
						if (!MapClass::Instance.CoordinatesLegal(coords)) continue;
						const auto* cell = MapClass::Instance.TryGetCellAt(coords);
						if (!cell) continue;
						if (!pass) ++count.Legal;
						if (!pass && cell->SlopeIndex >= 1 && cell->SlopeIndex <= OverlayImage::MaxCardinalRamp) ++count.Slopes;
						const int index = cell->OverlayTypeIndex;
						if (index < 0 || index >= OverlayTypeClass::Array.Count) continue;
						const auto* type = OverlayTypeClass::Array.Items[index];
						if (!CursorSnapshot::Readable(type, sizeof(OverlayTypeClass))) { ++count.Frame; continue; }
						if (!pass) ++count.Overlays;
						const auto tile = TerrainImage::TilePosition(cellX, cellY, cell->Level, capture.TacticalX, capture.TacticalY, capture.ViewY);
						if (pass) capture.Shadow(cell, index, type, tile);
						else capture.Body(cell, index, type, tile);
					}
			// 6D97D0: the static Terrain objects of the ground layer, last first.
			const auto& ground = MapClass::ObjectsInLayers[GroundLayer];
			for (int i = ground.Count - 1; i >= 0; --i)
			{
				auto* object = ground.Items[i];
				if (!CursorSnapshot::Readable(object, sizeof(TerrainClass)) || object->WhatAmI() != AbstractType::Terrain || !object->IsAlive) continue;
				capture.Terrain(static_cast<TerrainClass*>(object));
			}
			// 6D7C00: the cliff shadows, rows from far to near.
			const int cliffRows = TerrainObjectImage::CliffRows(clip.Height), cliffColumns = TerrainObjectImage::CliffColumns(clip.Width);
			for (int r = -margin; r < cliffRows + margin; ++r)
				for (int c = -margin; c < cliffColumns + margin; ++c)
				{
					const auto [cellX, cellY] = TerrainObjectImage::CliffCell(anchor, r, c);
					if (cellX < INT16_MIN || cellX > INT16_MAX || cellY < INT16_MIN || cellY > INT16_MAX) continue;
					const CellStruct coords {std::int16_t(cellX), std::int16_t(cellY)};
					if (!MapClass::Instance.CoordinatesLegal(coords)) continue;
					if (const auto* cell = MapClass::Instance.TryGetCellAt(coords)) capture.Cliff(cell, cellX, cellY);
				}
			// 6D9920: the buildings of the ground layer, first first.
			for (int i = 0; i < ground.Count; ++i)
			{
				auto* object = ground.Items[i];
				if (!CursorSnapshot::Readable(object, sizeof(BuildingClass)) || object->WhatAmI() != AbstractType::Building) continue;
				capture.Building(static_cast<BuildingClass*>(object));
			}
			CaptureComposite(capture);
			Current.Visible = true;
		}
		// The native Tile keeps what scrolling redrew in strips, and a Z-shaped draw's depth depends
		// on its clip's top row, so the trace (PHOBOS_TERRAIN_TRACE only) asks for a full redraw and
		// checks the capture after it.
		if (trace && Current.Visible)
		{
			MapClass::Instance.MarkNeedsRedraw(1);
			RedrawRequested = true;
		}
		else if (RedrawRequested)
		{
			RedrawRequested = false;
			Trace();
		}
		else if (trace) Trace();
	}

	void BeginRecording(DSurface* target)
	{
		Recorded.clear();
		RecordingTarget = target;
		RecordingAfter = false;
		if (target) BlitFrameSource::BeginCapture();
	}

	void RecordAfterLayers() { RecordingAfter = RecordingTarget != nullptr; }

	void EndRecording()
	{
		RecordingTarget = nullptr;
		RecordingAfter = false;
	}

	void RecordCall(RecordedCall call, DSurface* surface)
	{
		// 4AED70 cuts the clip to the surface (421B60) and draws nothing where they do not meet.
		auto& bounds = call.Bounds;
		const std::int64_t left = std::max(bounds.X, 0), top = std::max(bounds.Y, 0);
		const std::int64_t right = std::min(std::int64_t(bounds.X) + bounds.Width, std::int64_t(surface->Width));
		const std::int64_t bottom = std::min(std::int64_t(bounds.Y) + bounds.Height, std::int64_t(surface->Height));
		bounds = {int(left), int(top), int(std::max<std::int64_t>(right - left, 0)), int(std::max<std::int64_t>(bottom - top, 0))};
		call.OnTarget = surface == RecordingTarget;
		Recorded.push_back(call);
	}

	void RecordBlit(const BlitCall& call)
	{
		RecordedBlit blit;
		blit.Compressed = call.Compressed;
		blit.Bounds = *call.Bounds;
		blit.ZAdjust = call.ZAdjust;
		blit.Gradient = call.Gradient;
		blit.Brightness = call.Brightness;
		blit.Tint = call.Tint;
		blit.Caller = call.Caller;
		blit.OnTarget = call.Surface == RecordingTarget;
		if (!blit.OnTarget) { Recorded.push_back(blit); return; }
		const auto& dest = *call.Dest;
		const auto& limit = *call.Limit;
		const auto& part = *call.Part;
		blit.Dest = {dest.X, dest.Y, dest.Width, dest.Height};
		// 7BC040 and 437A10 draw nothing for an empty rectangle; 7BBE20 cuts the place by the clip and the
		// part by the source's rectangle.
		if (blit.Bounds.Width > 0 && blit.Bounds.Height > 0 && dest.Width > 0 && dest.Height > 0 && limit.Width > 0 && limit.Height > 0
			&& part.Width > 0 && part.Height > 0)
			blit.Clipped = ShapeDepth::Clip(blit.Dest, {part.X, part.Y, part.Width, part.Height}, {limit.X, limit.Y, limit.Width, limit.Height},
				blit.Bounds.Width, blit.Bounds.Height, call.Compressed);
		if (!blit.Clipped.Drawn || blit.Clipped.Width <= 0 || blit.Clipped.Rows <= 0) { Recorded.push_back(blit); return; }
		const auto [convert, flags] = FindBlitter(call.Blitter, call.Compressed);
		blit.Convert = convert;
		blit.Flags = flags;
		auto* source = static_cast<XSurface*>(call.Source);
		const auto& bounds = blit.Bounds;
		if (!convert || !flags) blit.Issue = BlitIssue::Blitter;
		else if (bounds.X < 0 || bounds.Y < 0 || std::int64_t(bounds.X) + bounds.Width > call.Surface->Width
			|| std::int64_t(bounds.Y) + bounds.Height > call.Surface->Height) blit.Issue = BlitIssue::Bounds;
		else if (!source || static_cast<Surface*>(call.Surface) == call.Source || source->BytesPerPixel != 1 || call.ZSurface
			|| (call.Compressed && (limit.X || limit.Y || part.X != limit.X || part.Y != limit.Y || part.Width != limit.Width || part.Height != limit.Height)))
			blit.Issue = BlitIssue::Source;
		else if (call.Compressed)
		{
			// 437A10 reads the frame's rows from the start of the source.
			if (const auto* data = static_cast<const std::uint8_t*>(source->Lock(0, 0)))
			{
				blit.ID = BlitFrameSource::AcquireEncoded(data, size_t(limit.Height) * (2 + 2 * size_t(limit.Width)), limit.Width, limit.Height);
				source->Unlock();
			}
		}
		else if (const auto* first = static_cast<const std::uint8_t*>(source->Lock(limit.X + blit.Clipped.SourceX, limit.Y + blit.Clipped.SourceY)))
		{
			// 7BC040 locks the source at the clipped part's place within the source's rectangle.
			blit.ID = BlitFrameSource::AcquirePixels(first, source->GetPitch(), blit.Clipped.Width, blit.Clipped.Rows);
			source->Unlock();
		}
		if (blit.Issue == BlitIssue::None && !blit.ID) blit.Issue = BlitIssue::Image;
		Recorded.push_back(blit);
	}

	void RecordLine(RecordedLine line, DSurface* surface)
	{
		line.OnTarget = surface == RecordingTarget;
		line.AfterLayers = RecordingAfter;
		if (line.OnTarget)
		{
			// Each cuts the clip to the surface (421B60) and draws nothing where they do not meet; a line's ends are
			// relative to the cut clip, a fill's rectangle to the clip as passed, then cut by the cut clip.
			const auto cut = [](const RectangleStruct& a, const RectangleStruct& b)
			{
				const std::int64_t left = std::max(a.X, b.X), top = std::max(a.Y, b.Y);
				const std::int64_t right = std::min(std::int64_t(a.X) + a.Width, std::int64_t(b.X) + b.Width);
				const std::int64_t bottom = std::min(std::int64_t(a.Y) + a.Height, std::int64_t(b.Y) + b.Height);
				return RectangleStruct {int(left), int(top), int(std::max<std::int64_t>(right - left, 0)), int(std::max<std::int64_t>(bottom - top, 0))};
			};
			// 4C0750 takes no clip: its caller cut the line to ViewBounds first (7BC2B0).
			if (line.Kind == Primitive::Dashed) line.Clip = {0, 0, surface->Width, surface->Height};
			const auto clip = cut(line.Clip, {0, 0, surface->Width, surface->Height});
			if (line.Kind == Primitive::Glyph || line.Kind == Primitive::Dashed)
			{
				// 434120 writes its rows and columns inside the font's bounds only; a dashed line's ends are on the surface.
				line.Clip = clip;
				if (line.Kind == Primitive::Glyph) line.To = {line.From.X + line.Fill.Width, line.From.Y + line.Fill.Height};
			}
			else if (line.Kind == Primitive::Fill)
			{
				const auto fill = cut({line.Clip.X + line.Fill.X, line.Clip.Y + line.Fill.Y, line.Fill.Width, line.Fill.Height}, clip);
				line.Clip = fill;
				line.From = {fill.X, fill.Y};
				line.To = {fill.X + fill.Width, fill.Y + fill.Height};
			}
			else
			{
				line.Clip = clip;
				line.From = {clip.X + line.From.X, clip.Y + line.From.Y};
				line.To = {clip.X + line.To.X, clip.Y + line.To.Y};
			}
		}
		Recorded.push_back(line);
	}
}
