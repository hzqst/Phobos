#include "OverlaySnapshot.h"
#include "CompositeSnapshot.h"
#include "TerrainPass.h"

#include <Ext/Rules/Body.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>

// The single definition of the state OverlaySnapshot.h declares, the capture and the trace replay.
namespace OverlaySnapshot
{
	Frame Current;
	unsigned TraceCount = 0;
	bool RedrawRequested = false;

	namespace
	{
		using TerrainImage::Point;
		// The first shape that could not be acquired: overlay, frame, frames of the shape.
		std::array<int, 4> FirstFailure {};

		bool Representable(Point point) { return point.X >= INT_MIN && point.X <= INT_MAX && point.Y >= INT_MIN && point.Y <= INT_MAX; }

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
			// 2E00 and 2E01 (an animated or crumbling Terrain object) test Z without writing it.
			bool WritesDepth = true;
		};

		struct Capture
		{
			const RectangleStruct& Clip;
			int TacticalX, TacticalY, ViewX, ViewY;
			// The ZBuffer's MaxValue and Bounds.Y, and how many of FlatZShape's first bytes are 0.
			int MaxValue, BoundsY, FlatZeros;
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

			// 4AED70 with flag 200 for one call: the frame centered on center, cut by its Z shape, then
			// clipped and given its row depths by 4373B0 or 437A10.
			void Place(int cellX, int cellY, Layer source, int index, const Call& call, Point center, bool shadow)
			{
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
				if (call.Bounded && !TerrainSnapshot::Intersects(Clip, boundsAt.X, boundsAt.Y, bounds.Frame.Width, bounds.Frame.Height)) { ++count.Outside; return; }
				const auto at = OverlayImage::FrameOrigin(center, shape.CanvasWidth, shape.CanvasHeight, shape.Frame.Left, shape.Frame.Top);
				if (shape.Frame.Width <= 0 || shape.Frame.Height <= 0) { ++count.Empty; return; }
				if (!Representable({at.X - Clip.X, at.Y - Clip.Y})) { ++count.Outside; return; }
				const bool compressed = (shape.Frame.Flags & CompressedFrame) != 0;
				Cut cut {{0, 0, shape.Frame.Width, shape.Frame.Height}};
				if (call.ZShape && !CutByZShape(call, shape, cut)) { ++count.ZShape; return; }
				if (!cut.Part.Width || !cut.Part.Height) { ++count.Empty; return; }
				// The surface rectangle keeps the frame's place; only its size follows the cut.
				const ShapeDepth::Rect dest {int(at.X - Clip.X), int(at.Y - Clip.Y), cut.Part.Width, cut.Part.Height};
				const auto clipped = ShapeDepth::Clip(dest, cut.Part, compressed ? cut.Part : ShapeDepth::Rect {0, 0, shape.Frame.Width, shape.Frame.Height},
					Clip.Width, Clip.Height, compressed);
				if (!clipped.Drawn || clipped.Width <= 0 || clipped.Rows <= 0) { ++count.Outside; return; }
				if (clipped.SourceX < 0 || clipped.SourceY < 0 || clipped.SourceX + clipped.Width > shape.Frame.Width
					|| clipped.SourceY + clipped.Rows > shape.Frame.Height) { ++count.Unsupported; return; }
				const auto* gradients = reinterpret_cast<const ShapeDepth::Gradient*>(Gradients);
				if (call.Gradient < 0 || call.Gradient >= GradientCount || !ShapeDepth::Usable(gradients[call.Gradient])
					|| !ShapeDepth::ClosedForm(gradients[call.Gradient])) { ++count.Unsupported; return; }
				const bool zshaped = compressed && call.ZShape;
				// Without a Z shape 437A10 reads FlatZShape from each row's first drawn pixel on.
				if (compressed && !zshaped && clipped.Width > FlatZeros) { ++count.Unsupported; return; }
				const auto table = Table(call.Convert);
				if (!table) { ++count.Palette; return; }
				Draw draw;
				draw.ID = shape.ID;
				draw.Table = table;
				draw.X = Clip.X + clipped.Dest.X;
				draw.Y = Clip.Y + clipped.Dest.Y;
				draw.Width = clipped.Width;
				draw.Height = clipped.Rows;
				draw.FrameX = clipped.SourceX;
				draw.FrameY = clipped.SourceY;
				draw.LightRow = TerrainImage::LightRow(call.Brightness);
				draw.Gradient = gradients[call.Gradient];
				draw.Rows = compressed
					? ShapeDepth::RLE(draw.Gradient, MaxValue, BoundsY, Clip.Y, dest, clipped.Dest, call.ZAdjust, zshaped)
					: ShapeDepth::Plain(draw.Gradient, MaxValue, BoundsY, Clip.Y, clipped.Dest, call.ZAdjust);
				draw.Compressed = compressed;
				draw.Shadow = shadow;
				draw.Mask = call.Convert->HalfTranslucencyMask;
				draw.ZShaped = zshaped;
				draw.WritesDepth = call.WritesDepth;
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
			// layer: the body, then its own shadow. Dynamic for an object 6D8DB0 selected, which 71C1B0
			// draws with its animation's frame and without writing Z (2E00, 2E01).
			void Terrain(TerrainClass* terrain, bool dynamic)
			{
				auto& count = *Count;
				const auto* type = terrain->Type;
				if (!CursorSnapshot::Readable(type, sizeof(TerrainTypeClass))) { ++count.Frame; return; }
				++count.Terrains;
				// An animated or crumbling object draws on Composite (6D8DB0), and only there.
				const bool animated = type->IsAnimated, crumbling = terrain->IsCrumbling;
				if ((animated || crumbling) != dynamic) { ++count.Dynamic; return; }
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
				if (dynamic)
				{
					// 71C1FE reads the frames of the type's image as they are.
					const auto* image = type->Image;
					const bool damagedFrames = ext && ext->HasDamagedFrames;
					const int length = CursorSnapshot::Readable(image, sizeof(SHPStruct)) ? TerrainObjectImage::AnimationLength(ext && ext->AnimationLength.isset(),
						ext ? ext->AnimationLength.Get(0) : 0, image->Frames, damagedFrames) : 0;
					call.Frame = TerrainObjectImage::DynamicFrame(animated, crumbling, ext && ext->HasCrumblingFrames, damagedFrames, damaged,
						terrain->Animation.Value, length);
					call.WritesDepth = false;
				}
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

		// 6D8DB0's view range: IsoTransformMatrix applied by 5AFB80 to its probes, truncated toward
		// zero as 7C5F00 does.
		TerrainObjectImage::ViewRange ComputeViewRange(const TacticalClass& tactical)
		{
			using Apply = Vector3D<float>*(__fastcall*)(Vector3D<float>*, const Matrix3D*, const Vector3D<float>*);
			const auto apply = reinterpret_cast<Apply>(MatrixApply);
			const auto& view = DSurface::ViewBounds;
			const auto probes = TerrainObjectImage::RangeProbes(tactical.TacticalPos.X, tactical.TacticalPos.Y, view.Width, view.Height);
			const auto project = [&](TerrainObjectImage::ViewProbe probe)
			{
				const Vector3D<float> input {float(probe.X), float(probe.Y), 0.f};
				Vector3D<float> output {};
				return *apply(&output, &tactical.IsoTransformMatrix, &input);
			};
			const auto top = project(probes.Top), right = project(probes.Right), left = project(probes.Left);
			return TerrainObjectImage::MakeViewRange(int(top.X), int(top.Y), int(right.X), int(left.Y));
		}

		// 6D8DB0: every layer in order and each of its objects in place. Its Terrain branch (6D9224)
		// draws an animated or crumbling object whose Location is in the view range and not fogged
		// (5865E0); the other objects only count, for the trace's conditions.
		void CaptureComposite(Capture& capture, const TacticalClass& tactical)
		{
			capture.Target = &Current.CompositeDraws;
			capture.Count = &Current.CompositeCount;
			auto& count = Current.CompositeCount;
			const auto range = ComputeViewRange(tactical);
			for (int layer = 0; layer < LayerCount; ++layer)
			{
				const auto& items = MapClass::ObjectsInLayers[layer];
				for (int i = 0; i < items.Count; ++i)
				{
					auto* object = items.Items[i];
					if (!CursorSnapshot::Readable(object, sizeof(ObjectClass))) continue;
					++count.LayerObjects;
					const auto location = object->Location;
					const bool inRange = TerrainObjectImage::InViewRange(range, location.X, location.Y);
					if (object->WhatAmI() != AbstractType::Terrain)
					{
						++count.Others;
						count.OthersInRange += inRange;
						if (object->IsVisible)
						{
							++count.OthersVisible;
							RectangleStruct dimensions {};
							object->GetRenderDimensions(&dimensions);
							// A voxel unit gives an empty rectangle: what it drew stays unexplained.
							if (dimensions.Width <= 0 || dimensions.Height <= 0) ++count.OthersUnbounded;
							dimensions.X += capture.ViewX;
							dimensions.Y += capture.ViewY;
							Current.OtherBounds.push_back(dimensions);
						}
						continue;
					}
					auto* terrain = static_cast<TerrainClass*>(object);
					if (!CursorSnapshot::Readable(terrain, sizeof(TerrainClass)) || !CursorSnapshot::Readable(terrain->Type, sizeof(TerrainTypeClass)))
					{
						++count.Frame;
						continue;
					}
					if (!terrain->Type->IsAnimated && !terrain->IsCrumbling) continue;
					++count.Animated;
					if (!inRange) { ++count.OutOfRange; continue; }
					if (MapClass::Instance.IsLocationFogged(location)) { ++count.LocationFogged; continue; }
					capture.Terrain(terrain, true);
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
				const auto* frame = ShapeFrameSource::Frames.Resolve(draw.ID);
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
						if (!ShapeDepth::Passes(value, model.Depth[at])) { ++model.Rejected; continue; }
						if (draw.WritesDepth) model.Depth[at] = std::uint16_t(value);
						if (draw.Shadow)
						{
							if (model.Owner[at] == NoOwner) model.Unknown[at] = 1;
							model.Color[at] = ShapeDepth::Darken(model.Color[at], draw.Mask);
						}
						else
						{
							const int shade = TerrainImage::ShadeRow(int(draw.Table->Shades), draw.LightRow, model.Alpha[at]);
							model.Color[at] = colors[size_t(shade) * 256 + index];
							model.Unknown[at] = 0;
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
		struct CompositeCheck
		{
			const char* Input = "", * Output = "";
			int InputCompared = 0, InputMismatched = 0, InputX = -1, InputY = -1, InputNative = 0, InputCopy = 0;
			// Untouched mismatches inside another object's rectangle, and the rest.
			int Compared = 0, Drawn = 0, Mismatched = 0, Untouched = 0, Others = 0, Unexplained = 0, Body = 0, Shadow = 0, Rejected = 0, Missing = 0;
			int Left = INT_MAX, Top = INT_MAX, Right = INT_MIN, Bottom = INT_MIN;
			int FirstX = -1, FirstY = -1, NativeColor = 0, ModelColor = 0, Owner = NoOwner;
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
			for (int row = 0; row < clip.Height; ++row)
				for (int column = 0; column < clip.Width; ++column)
				{
					const auto at = size_t(row) * clip.Width + column;
					const int owner = model.Owner[at];
					++check.Compared;
					check.Drawn += owner != NoOwner;
					const auto native = output.Pixels[at], color = model.Color[at];
					if (native == color) continue;
					if (owner == NoOwner)
					{
						++check.Untouched;
						const int x = clip.X + column, y = clip.Y + row;
						const bool other = std::any_of(Current.OtherBounds.begin(), Current.OtherBounds.end(), [x, y](const RectangleStruct& bounds)
						{
							return x >= bounds.X && y >= bounds.Y && x - bounds.X < bounds.Width && y - bounds.Y < bounds.Height;
						});
						++(other ? check.Others : check.Unexplained);
					}
					else ++(Current.CompositeDraws[size_t(owner)].Shadow ? check.Shadow : check.Body);
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

		char LayerLetter(Layer layer) { return layer == Layer::Overlay ? 'o' : layer == Layer::Terrain ? 't' : 'c'; }

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
			if (!Current.Visible) return;
			// Each overlay (o), Terrain (t) and cliff tile (c) type drawn: bodies, shadows, RLE frames and Z shapes.
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
			const auto check = Replay(TerrainSnapshot::Current.Clip);
			const auto describe = [](int owner, const std::vector<Draw>& draws = Current.Draws)
			{
				static char text[192];
				if (owner < 0) std::snprintf(text, sizeof(text), "%s", owner == TerrainOwner ? "terrain" : "none");
				else
				{
					const auto& draw = draws[size_t(owner)];
					std::snprintf(text, sizeof(text), "%s:%d,%d:%c%d:f%d:z=%d:g=%d:rle=%d:zshape=%d:rect=%d,%d,%d,%d:frame=%d,%d", draw.Shadow ? "shadow" : "body",
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

			// D2: 6D8DB0's animated or crumbling Terrain on Composite, each draw in order (type@cell:frame,
			// s for a shadow), and its replay over the copies around the layers.
			const auto& composite = Current.CompositeCount;
			Debug::Log("[TerrainGPU] composite sample=%u layerObjects=%d others=%d othersInRange=%d othersVisible=%d unbounded=%d animated=%d outOfRange=%d locationFogged=%d "
				"terrainOutside=%d bodies=%d shadows=%d compressed=%d palette=%d frame=%d noImage=%d unloaded=%d empty=%d outside=%d unsupported=%d damaged=%d copyFailed=%u\n",
				TraceCount, composite.LayerObjects, composite.Others, composite.OthersInRange, composite.OthersVisible, composite.OthersUnbounded, composite.Animated, composite.OutOfRange, composite.LocationFogged,
				composite.TerrainOutside, composite.Bodies, composite.Shadows, composite.Compressed, composite.Palette, composite.Frame, composite.NoImage,
				composite.Unloaded, composite.Empty, composite.Outside, composite.Unsupported, composite.Damaged, CompositeSnapshot::Failed);
			std::string draws;
			for (const auto& draw : Current.CompositeDraws)
			{
				char item[48];
				std::snprintf(item, sizeof(item), " %d@%d,%d:%d%s", draw.Index, draw.CellX, draw.CellY, draw.ShapeFrame, draw.Shadow ? "s" : "");
				draws += item;
			}
			if (!draws.empty()) Debug::Log("[TerrainGPU] compositedraws sample=%u%s\n", TraceCount, draws.c_str());
			const auto compositeCheck = CompositeReplay(TerrainSnapshot::Current.Clip);
			Debug::Log("[TerrainGPU] compositecheck sample=%u input=%s output=%s inputCompared=%d inputMismatched=%d inputFirst=%d,%d:tile=%04X:copy=%04X "
				"compared=%d drawn=%d mismatched=%d untouched=%d others=%d unexplained=%d body=%d shadow=%d rejected=%d missing=%d box=%d,%d,%d,%d first=%d,%d:native=%04X:model=%04X:%s\n",
				TraceCount, compositeCheck.Input, compositeCheck.Output, compositeCheck.InputCompared, compositeCheck.InputMismatched, compositeCheck.InputX,
				compositeCheck.InputY, compositeCheck.InputNative, compositeCheck.InputCopy, compositeCheck.Compared, compositeCheck.Drawn, compositeCheck.Mismatched,
				compositeCheck.Untouched, compositeCheck.Others, compositeCheck.Unexplained, compositeCheck.Body, compositeCheck.Shadow, compositeCheck.Rejected, compositeCheck.Missing,
				compositeCheck.Mismatched ? compositeCheck.Left : 0, compositeCheck.Mismatched ? compositeCheck.Top : 0,
				compositeCheck.Mismatched ? compositeCheck.Right : 0, compositeCheck.Mismatched ? compositeCheck.Bottom : 0,
				compositeCheck.FirstX, compositeCheck.FirstY, compositeCheck.NativeColor, compositeCheck.ModelColor,
				describe(compositeCheck.Owner, Current.CompositeDraws));
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
		Current.CompositeCount = {};
		Current.OtherBounds.clear();
		FirstFailure = {};
		const auto& terrain = TerrainSnapshot::Current;
		const TacticalClass* tactical = TacticalClass::Instance;
		if (terrain.Visible && tactical)
		{
			const auto& clip = terrain.Clip;
			const auto& view = DSurface::ViewBounds;
			const auto* flat = reinterpret_cast<const std::uint8_t*>(FlatZShape);
			int zeros = 0;
			while (zeros < int(FlatZShapeChecked) && !flat[zeros]) ++zeros;
			Capture capture {clip, tactical->TacticalPos.X, tactical->TacticalPos.Y, view.X, view.Y, ZBuffer::Instance->MaxValue, ZBuffer::Instance->Bounds.Y,
				zeros, {}};
			const auto anchor = TerrainImage::ScanAnchor(capture.TacticalX + clip.X - view.X, capture.TacticalY + clip.Y - view.Y);
			const int rows = OverlayImage::ScanRows(clip.Height), columns = OverlayImage::ScanColumns(clip.Width);
			const int margin = TerrainSnapshot::ScanMargin;
			auto& count = Current.Count;
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
				capture.Terrain(static_cast<TerrainClass*>(object), false);
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
			// 4D1890 (drawn before the overlays): a record draws where it is enabled and its saved
			// rectangle, moved by the view, meets the clip.
			const auto* pairs = *reinterpret_cast<const std::uint32_t* const*>(FoggedPairs);
			count.Fogged = *reinterpret_cast<const int*>(FoggedCount);
			for (int i = 0; i < count.Fogged && CursorSnapshot::Readable(pairs + 2 * i, 2 * sizeof(std::uint32_t)); ++i)
			{
				const auto* record = reinterpret_cast<const std::uint8_t*>(std::uintptr_t(pairs[2 * i + 1]));
				if (!CursorSnapshot::Readable(record, FoggedRecordSize)) continue;
				const bool building = *reinterpret_cast<const int*>(record + FoggedKind) == FoggedBuilding;
				count.FoggedBuildings += building;
				const auto& bounds = *reinterpret_cast<const RectangleStruct*>(record + FoggedBounds);
				if (!record[FoggedEnabled] || !TerrainSnapshot::Intersects(clip, std::int64_t(bounds.X) - capture.TacticalX + capture.ViewX,
					std::int64_t(bounds.Y) - capture.TacticalY + capture.ViewY, bounds.Width, bounds.Height)) continue;
				++(building ? count.FoggedBuildingsInView : count.FoggedOthersInView);
			}
			CaptureComposite(capture, *tactical);
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
}
