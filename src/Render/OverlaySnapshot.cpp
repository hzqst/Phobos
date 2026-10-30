#include "OverlaySnapshot.h"
#include "TerrainPass.h"

#include <Utilities/Debug.h>

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
		};

		struct Capture
		{
			const RectangleStruct& Clip;
			int TacticalX, TacticalY, ViewY;
			// The ZBuffer's MaxValue and Bounds.Y, and how many of FlatZShape's first bytes are 0.
			int MaxValue, BoundsY, FlatZeros;
			std::unordered_map<const ConvertClass*, TerrainBatch::ColorTables::Handle> Tables;

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
			void Place(const CellClass* cell, int index, const Call& call, Point center, bool shadow)
			{
				auto& count = Current.Count;
				// 4AED70 and 47FB90 draw nothing for a type without an image.
				if (!call.Image) { ++count.NoImage; return; }
				// The native draw ran first, so a reference still unloaded has no file to load.
				if (FrameCount(call.Image) == UnloadedShape) { ++count.Unloaded; return; }
				const auto shape = AcquireShape(call.Image, call.Frame);
				if (!shape.Ready)
				{
					if (!count.Frame++) { FirstFailure = {index, call.Frame, FrameCount(call.Image), 0}; }
					return;
				}
				// 6D6D10 draws only where 47FB90's rectangle meets the clip.
				auto bounds = shape;
				if (call.BoundsFrame >= 0 && call.BoundsFrame != call.Frame) bounds = AcquireShape(call.Image, call.BoundsFrame);
				if (!bounds.Ready) { ++count.Frame; return; }
				if (!bounds.ID || !shape.ID) { ++count.Empty; return; }
				const auto boundsAt = OverlayImage::FrameOrigin(center, bounds.CanvasWidth, bounds.CanvasHeight, bounds.Frame.Left, bounds.Frame.Top);
				if (!TerrainSnapshot::Intersects(Clip, boundsAt.X, boundsAt.Y, bounds.Frame.Width, bounds.Frame.Height)) { ++count.Outside; return; }
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
				if (zshaped && !ReadZBytes(shape, cut, draw)) { ++count.ZShape; return; }
				draw.ZAdjust = call.ZAdjust;
				draw.GradientIndex = call.Gradient;
				draw.CellX = cell->MapCoords.X;
				draw.CellY = cell->MapCoords.Y;
				draw.Overlay = index;
				if (compressed) ++count.Compressed;
				Current.Draws.push_back(std::move(draw));
				++(shadow ? count.Shadows : count.Bodies);
			}

			// 47F6A0.
			void Body(const CellClass* cell, int index, const OverlayTypeClass* type, Point tile)
			{
				auto& count = Current.Count;
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
				Place(cell, index, call, center, false);
			}

			// 47F510: the second half of the type's own frames, darkening what lies below.
			void Shadow(const CellClass* cell, int index, const OverlayTypeClass* type, Point tile)
			{
				auto& count = Current.Count;
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
				Place(cell, index, call, center, true);
			}
		};

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

		TileCheck Replay(const RectangleStruct& clip)
		{
			TileCheck check;
			const auto size = size_t(clip.Width) * clip.Height;
			std::vector<std::uint16_t> color(size, 0), depth(size, 0xFFFF), alpha(size, TerrainImage::DefaultAlpha);
			std::vector<int> owner(size, NoOwner);
			std::vector<std::uint8_t> unknown(size, 0);
			auto* abuffer = ABuffer::Instance;
			if (CursorSnapshot::Readable(abuffer, sizeof(ABuffer)))
				for (int row = 0; row < clip.Height; ++row)
				{
					// 4373B0 and 437A10 address the ring buffer by the draw position less its Bounds.Y.
					auto* word = static_cast<std::uint16_t*>(abuffer->GetBuffer(clip.X, clip.Y + row - abuffer->Bounds.Y));
					for (int column = 0; column < clip.Width; ++column, ++word)
					{
						abuffer->AdjustPointer(word);
						alpha[size_t(row) * clip.Width + column] = *word;
					}
				}
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

			for (size_t drawIndex = 0; drawIndex < Current.Draws.size(); ++drawIndex)
			{
				const auto& draw = Current.Draws[drawIndex];
				const auto* frame = ShapeFrameSource::Frames.Resolve(draw.ID);
				if (!frame) { ++check.Missing; continue; }
				const int pitch = frame->Geometry.Width;
				if (frame->Indices.size() < size_t(pitch) * frame->Geometry.Height || draw.FrameX + draw.Width > pitch
					|| draw.FrameY + draw.Height > frame->Geometry.Height) { ++check.Missing; continue; }
				if (draw.ZShaped) ++check.ZShaped;
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
						if (!ShapeDepth::Passes(value, depth[at])) { ++check.Rejected; continue; }
						depth[at] = std::uint16_t(value);
						if (draw.Shadow)
						{
							if (owner[at] == NoOwner) unknown[at] = 1;
							color[at] = ShapeDepth::Darken(color[at], draw.Mask);
						}
						else
						{
							const int shade = TerrainImage::ShadeRow(int(draw.Table->Shades), draw.LightRow, alpha[at]);
							color[at] = colors[size_t(shade) * 256 + index];
							unknown[at] = 0;
						}
						owner[at] = int(drawIndex);
					}
				}
			}

			auto* zbuffer = ZBuffer::Instance;
			auto* tile = DSurface::Tile;
			const auto* pixels = static_cast<const std::uint8_t*>(tile->Lock(clip.X, clip.Y));
			if (!pixels) { check.Missing = -1; return check; }
			const int tilePitch = tile->GetPitch();
			for (int row = 0; row < clip.Height; ++row)
			{
				const auto* native = reinterpret_cast<const std::uint16_t*>(pixels + ptrdiff_t(row) * tilePitch);
				// 547CF0 and the shape blitters address the ring buffer by the draw position less ViewBounds.Y.
				auto* nativeDepth = static_cast<std::uint16_t*>(zbuffer->GetBuffer(clip.X, clip.Y + row - DSurface::ViewBounds.Y));
				for (int column = 0; column < clip.Width; ++column, ++nativeDepth)
				{
					zbuffer->AdjustPointer(nativeDepth);
					const auto at = size_t(row) * clip.Width + column;
					++check.Compared;
					if ((*nativeDepth != depth[at] || (owner[at] != NoOwner && !unknown[at] && native[column] != color[at])) && check.Samples.size() < 24)
						check.Samples.push_back({column, row, native[column], color[at], *nativeDepth, depth[at], owner[at]});
					if (*nativeDepth != depth[at] && !check.DepthMismatched++)
					{
						check.DepthX = column; check.DepthY = row; check.NativeDepth = *nativeDepth; check.ModelDepth = depth[at]; check.DepthOwner = owner[at];
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

		void Trace()
		{
			const auto& count = Current.Count;
			++TraceCount;
			Debug::Log("[TerrainGPU] overlay sample=%u ready=%d visited=%d legal=%d overlays=%d bodies=%d shadows=%d noBody=%d outside=%d empty=%d "
				"rubble=%d tiberium=%d palette=%d frame=%d zshape=%d ramps=%d bridges=%d walls=%d compressed=%d noImage=%d unloaded=%d slopes=%d unsupported=%d failure=%d:%d/%d gpu=%zu/%zu\n",
				TraceCount, Current.Visible, count.Visited, count.Legal, count.Overlays, count.Bodies, count.Shadows, count.NoBody, count.Outside,
				count.Empty, count.Rubble, count.Tiberium, count.Palette, count.Frame, count.ZShape, count.Ramps, count.Bridges, count.Walls, count.Compressed, count.NoImage, count.Unloaded, count.Slopes, count.Unsupported, FirstFailure[0], FirstFailure[1], FirstFailure[2],
				TerrainPass::ShapeCount(), TerrainPass::SkippedShapes());
			if (!Current.Visible) return;
			// Each overlay type drawn: bodies, shadows, RLE frames and Z shapes.
			std::map<int, std::array<int, 4>> types;
			for (const auto& draw : Current.Draws)
			{
				auto& type = types[draw.Overlay];
				++type[draw.Shadow ? 1 : 0];
				type[2] += draw.Compressed;
				type[3] += draw.ZShaped;
			}
			std::string list;
			for (const auto& [index, type] : types)
			{
				char item[48];
				std::snprintf(item, sizeof(item), " %d:%d/%d/%d/%d", index, type[0], type[1], type[2], type[3]);
				list += item;
			}
			Debug::Log("[TerrainGPU] overlaytypes sample=%u%s\n", TraceCount, list.c_str());
			const auto check = Replay(TerrainSnapshot::Current.Clip);
			const auto describe = [](int owner)
			{
				static char text[192];
				if (owner < 0) std::snprintf(text, sizeof(text), "%s", owner == TerrainOwner ? "terrain" : "none");
				else
				{
					const auto& draw = Current.Draws[size_t(owner)];
					std::snprintf(text, sizeof(text), "%s:%d,%d:overlay=%d:z=%d:g=%d:rle=%d:zshape=%d:rect=%d,%d,%d,%d:frame=%d,%d", draw.Shadow ? "shadow" : "body",
						draw.CellX, draw.CellY, draw.Overlay, draw.ZAdjust, draw.GradientIndex, draw.Compressed, draw.ZShaped, draw.X, draw.Y, draw.Width, draw.Height,
						draw.FrameX, draw.FrameY);
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
		}
	}

	void CaptureAfterDraw(bool trace)
	{
		Current.Visible = false;
		Current.Draws.clear();
		Current.Count = {};
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
			Capture capture {clip, tactical->TacticalPos.X, tactical->TacticalPos.Y, view.Y, ZBuffer::Instance->MaxValue, ZBuffer::Instance->Bounds.Y,
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
