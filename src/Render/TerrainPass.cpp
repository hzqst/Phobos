#include "TerrainPass.h"

#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <Shaders/TerrainComposite.ps.dxil.h>
#include <Shaders/TerrainComposite.ps.spirv.h>
#include <Shaders/TerrainShape.ps.dxil.h>
#include <Shaders/TerrainShape.ps.spirv.h>
#include <Shaders/TerrainTile.ps.dxil.h>
#include <Shaders/TerrainTile.ps.spirv.h>
#include <Shaders/TerrainTile.vs.dxil.h>
#include <Shaders/TerrainTile.vs.spirv.h>

#include <algorithm>
#include <cstring>

namespace TerrainPass
{
	namespace
	{
		// Matches cbuffer Decode in TerrainTile.ps.hlsl: Alpha holds the ABuffer's target position and size.
		struct DecodeUniforms { Uint32 Shift[4], Bits[4]; Sint32 Alpha[4]; };
		// Matches cbuffer Decode in TerrainComposite.ps.hlsl.
		struct CompositeUniforms { Uint32 Bits[4]; };
		// Channel values up to 63 halve exactly any number of times, as the native darkening of
		// 16-bit colors will need.
		constexpr SDL_GPUTextureFormat ColorFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		// The cleared ZBuffer holds 0xFFFF, which the shader's z / 65535 maps to 1.
		constexpr float ClearDepth = 1.0f;

		// Images without Z data, which neither test nor write it, and images with it.
		SDL_GPUGraphicsPipeline* TilePipelines[2] {};
		// 4AED70's draws by kind (a body, a shadow, a translucent body) and Z mode (none, a test of strictly
		// smaller depths, the test with the write): the Tile pass's overlays, Terrain objects and
		// buildings test and write, mode 2's animated Terrain (2E00, 2E01) only tests, its anims any.
		enum ShapeKind : std::uint32_t { BodyKind, ShadowKind, TranslucentKind, ShapeKinds };
		enum DepthMode : std::uint32_t { NoDepth, TestDepth, WriteDepth, DepthModes };
		SDL_GPUGraphicsPipeline* ShapePipelines[ShapeKinds][DepthModes] {};
		// A run's Mode: the pipeline, and above it the number of the group of translucent draws, which
		// ends where one meets an earlier one of the group: each group reads its own copy of the target.
		constexpr std::uint32_t PipelineMask = 0xFF, GroupShift = 8;
		SDL_GPUGraphicsPipeline* CompositePipeline = nullptr;
		SDL_GPUTextureFormat DepthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		SDL_GPUTexture* TableTexture = nullptr;
		SDL_GPUTransferBuffer* TableTransfer = nullptr;
		// The tactical target, ViewBounds' size.
		SDL_GPUTexture* ColorTexture = nullptr;
		SDL_GPUTexture* DepthTexture = nullptr;
		// The target's colors before a run of translucent draws.
		SDL_GPUTexture* CopyTexture = nullptr;
		int Width = 0, Height = 0;
		// The tile images, then one instance that draws the tactical target onto the canvas.
		InstanceBuffer Tiles;
		std::vector<TerrainBatch::Run> Runs;
		// Overlay runs follow the tile runs, and 6D8DB0's runs on Composite follow them; their Mode
		// selects the pipeline.
		std::vector<TerrainBatch::Run> ShapeRuns, LayerRuns;
		StorageBuffer<TerrainBatch::Shape> Shapes;
		// The Z shape bytes of the overlays, four to a word.
		StorageBuffer<std::uint32_t> ZBytes;
		size_t Skipped = 0;
		std::uint32_t CompositeInstance = 0;
		bool Visible = false;
		std::uint64_t UploadedTables = UINT64_MAX;
		bool PendingTables = false;
		// Target position of the ABuffer's first word, which is Tile's (0, ViewBounds.Y).
		int AlphaX = 0, AlphaY = 0;

		SDL_GPUTexture* CreateTarget(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, int width, int height)
		{
			SDL_GPUTextureCreateInfo texture {};
			texture.type = SDL_GPU_TEXTURETYPE_2D;
			texture.format = format;
			texture.usage = usage;
			texture.width = Uint32(width);
			texture.height = Uint32(height);
			texture.layer_count_or_depth = 1;
			texture.num_levels = 1;
			return SDL_CreateGPUTexture(device, &texture);
		}

		void ReleaseTargets(SDL_GPUDevice* device)
		{
			if (ColorTexture) SDL_ReleaseGPUTexture(device, ColorTexture);
			if (DepthTexture) SDL_ReleaseGPUTexture(device, DepthTexture);
			if (CopyTexture) SDL_ReleaseGPUTexture(device, CopyTexture);
			ColorTexture = nullptr; DepthTexture = nullptr; CopyTexture = nullptr;
			Width = Height = 0;
		}

		// Released textures stay alive until the GPU no longer uses them.
		bool PrepareTargets(SDL_GPUDevice* device, int width, int height)
		{
			if (ColorTexture && DepthTexture && CopyTexture && width == Width && height == Height) return true;
			ReleaseTargets(device);
			ColorTexture = CreateTarget(device, ColorFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
			DepthTexture = CreateTarget(device, DepthFormat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, width, height);
			CopyTexture = CreateTarget(device, ColorFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
			if (!ColorTexture || !DepthTexture || !CopyTexture) { ReleaseTargets(device); return false; }
			Width = width;
			Height = height;
			return true;
		}

		Uint32 ChannelBits(int shiftRight) { return Uint32(8 - shiftRight); }
	}

	SDL_GPUSampler* Sampler = nullptr;

	SDL_GPUGraphicsPipeline* CreatePipeline(SDL_GPUDevice* device, const GPUShader::Code& pixel, Uint32 samplers,
		const SDL_GPUColorTargetDescription& color, const SDL_GPUDepthStencilState* depth, SDL_GPUTextureFormat depthFormat, Uint32 storageBuffers)
	{
		SDL_GPUGraphicsPipeline* pipeline = nullptr;
		auto* vertex = GPUShader::Create(device, GPUShader::Make(g_TerrainTile_vs_DXIL, g_TerrainTile_vs_SPIRV),
			SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
		auto* fragment = GPUShader::Create(device, pixel, SDL_GPU_SHADERSTAGE_FRAGMENT, samplers, 1, storageBuffers);
		if (vertex && fragment)
		{
			SDL_GPUGraphicsPipelineCreateInfo info {};
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.target_info.color_target_descriptions = &color;
			info.target_info.num_color_targets = 1;
			if (depth)
			{
				info.depth_stencil_state = *depth;
				info.target_info.depth_stencil_format = depthFormat;
				info.target_info.has_depth_stencil_target = true;
			}
			pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
		}
		if (vertex) SDL_ReleaseGPUShader(device, vertex);
		if (fragment) SDL_ReleaseGPUShader(device, fragment);
		return pipeline;
	}

	bool Create(SDL_GPUDevice* device, SDL_GPUTextureFormat target)
	{
		DepthFormat = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GPU_TEXTUREFORMAT_D16_UNORM;
		Debug::Log("[GPURender] Tactical depth buffer: %s.\n", DepthFormat == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ? "D32_FLOAT" : "D16_UNORM");

		// Pixels are opaque or discarded; the terrain is the bottom layer.
		SDL_GPUColorTargetDescription color {};
		color.format = ColorFormat;
		const auto tile = GPUShader::Make(g_TerrainTile_ps_DXIL, g_TerrainTile_ps_SPIRV);
		SDL_GPUDepthStencilState depth {};
		TilePipelines[0] = CreatePipeline(device, tile, 3, color, &depth, DepthFormat);
		// 547CF0 draws where the old Z is not smaller and writes the new one.
		depth.enable_depth_test = true;
		depth.enable_depth_write = true;
		depth.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		TilePipelines[1] = CreatePipeline(device, tile, 3, color, &depth, DepthFormat);
		// 4AED70's Z-testing blitters draw where the new depth is strictly smaller (and may write it); a
		// shadow multiplies the old color by the shader's alpha of 1/2; a translucent body mixes in the
		// shader and draws opaque.
		const auto shape = GPUShader::Make(g_TerrainShape_ps_DXIL, g_TerrainShape_ps_SPIRV);
		SDL_GPUColorTargetDescription darken = color;
		darken.blend_state.enable_blend = true;
		darken.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		darken.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		darken.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		darken.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
		darken.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		darken.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		bool shapes = true;
		for (std::uint32_t mode = 0; mode < DepthModes; ++mode)
		{
			SDL_GPUDepthStencilState test {};
			test.enable_depth_test = mode != NoDepth;
			test.enable_depth_write = mode == WriteDepth;
			test.compare_op = mode == NoDepth ? SDL_GPU_COMPAREOP_ALWAYS : SDL_GPU_COMPAREOP_LESS;
			for (std::uint32_t kind = 0; kind < ShapeKinds; ++kind)
			{
				ShapePipelines[kind][mode] = CreatePipeline(device, shape, 4, kind == ShadowKind ? darken : color, &test, DepthFormat, 2);
				shapes = shapes && ShapePipelines[kind][mode];
			}
		}
		SDL_GPUColorTargetDescription window {};
		window.format = target;
		CompositePipeline = CreatePipeline(device, GPUShader::Make(g_TerrainComposite_ps_DXIL, g_TerrainComposite_ps_SPIRV), 1, window);
		if (!TilePipelines[0] || !TilePipelines[1] || !shapes || !CompositePipeline) return false;

		SDL_GPUSamplerCreateInfo sampler {};
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		Sampler = SDL_CreateGPUSampler(device, &sampler);

		// R32_UINT like the atlas palettes: one 256-color shade row per texture row.
		SDL_GPUTextureCreateInfo texture {};
		texture.type = SDL_GPU_TEXTURETYPE_2D;
		texture.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
		texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		texture.width = 256;
		texture.height = TableRows;
		texture.layer_count_or_depth = 1;
		texture.num_levels = 1;
		TableTexture = SDL_CreateGPUTexture(device, &texture);

		SDL_GPUTransferBufferCreateInfo transfer {};
		transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transfer.size = 256 * TableRows * sizeof(std::uint32_t);
		TableTransfer = SDL_CreateGPUTransferBuffer(device, &transfer);
		UploadedTables = UINT64_MAX;
		return Sampler && TableTexture && TableTransfer;
	}

	void Release(SDL_GPUDevice* device)
	{
		for (auto*& pipeline : TilePipelines)
		{
			if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			pipeline = nullptr;
		}
		for (auto& pipelines : ShapePipelines)
			for (auto*& pipeline : pipelines)
			{
				if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
				pipeline = nullptr;
			}
		if (CompositePipeline) SDL_ReleaseGPUGraphicsPipeline(device, CompositePipeline);
		if (Sampler) SDL_ReleaseGPUSampler(device, Sampler);
		if (TableTexture) SDL_ReleaseGPUTexture(device, TableTexture);
		if (TableTransfer) SDL_ReleaseGPUTransferBuffer(device, TableTransfer);
		ReleaseTargets(device);
		Tiles.Release(device);
		Shapes.Release(device);
		ZBytes.Release(device);
		Shapes.Instances.clear(); ZBytes.Instances.clear(); ShapeRuns.clear(); LayerRuns.clear();
		CompositePipeline = nullptr; Sampler = nullptr; TableTexture = nullptr; TableTransfer = nullptr;
		Tiles.Instances.clear(); Runs.clear();
		Visible = false;
		UploadedTables = UINT64_MAX;
		PendingTables = false;
	}

	FrameAtlas::Request Request(const TerrainSnapshot::Placement& placement)
	{
		return {placement.ID, {}, FrameAtlas::SourceKind::Mask, placement.Storage};
	}

	FrameAtlas::Request Request(const OverlaySnapshot::Draw& draw)
	{
		return {draw.ID, {}, FrameAtlas::SourceKind::Mask, &ShapeFrameSource::Frames};
	}

	namespace
	{
		// The instances of a draw list after those already prepared, their shapes and Z bytes; returns
		// their runs.
		std::vector<TerrainBatch::Run> PrepareShapes(const FrameAtlas& atlas, const RectangleStruct& view, const std::vector<OverlaySnapshot::Draw>& draws,
			std::vector<std::uint8_t>& packed)
		{
			auto& instances = Tiles.Instances;
			const auto first = std::uint32_t(instances.size());
			std::vector<const void*> pages;
			std::vector<std::uint32_t> modes;
			const auto& pack = Unsorted::ColorPackData;
			const int shifts[3] {pack._R_SHL, pack._G_SHL, pack._B_SHL};
			const int bits[3] {int(ChannelBits(pack._R_SHR)), int(ChannelBits(pack._G_SHR)), int(ChannelBits(pack._B_SHR))};
			const auto mask = TerrainBatch::HalfMask(shifts, bits), quarter = TerrainBatch::QuarterMask(shifts, bits);
			// The translucent draws since the last copy of the target, and their group.
			std::vector<TerrainBatch::Rect> group;
			std::uint32_t groups = 0;
			for (const auto& draw : draws)
			{
				const auto atlasView = atlas.Resolve(Request(draw));
				TerrainBatch::Instance instance;
				// Halving or quartering every channel of the integer target needs the color pack's own masks;
				// the patched blitters' mask rounds per channel only where it clears the channels' low bits.
				if (!atlasView.Texture || !draw.Table || ((draw.Shadow || draw.Translucency) && (draw.Mask & 0xFFFF) != mask)
					|| (draw.Translucency && (draw.QuarterMask & 0xFFFF) != quarter)
					|| (draw.Rounded && ShapeDepth::RoundedMask >> 1 != mask)
					|| !TerrainBatch::PlaceShape(draw.X, draw.Y, draw.Width, draw.Height, -view.X, -view.Y,
						atlasView.X + draw.FrameX, atlasView.Y + draw.FrameY, draw.Table->Row, draw.Table->Shades, draw.LightRow,
						std::uint32_t(Shapes.Instances.size()), instance))
				{
					++Skipped;
					continue;
				}
				const auto& rows = draw.Rows;
				const std::uint32_t flags = (draw.Shadow ? TerrainBatch::ShadowShape : 0u) | (draw.Translucency & TerrainBatch::TranslucentShape)
					| (draw.TestsDepth ? 0u : TerrainBatch::UntestedShape) | (draw.Rounded ? TerrainBatch::RoundedShape : 0u);
				TerrainBatch::Shape shape {rows.Base, std::uint32_t(rows.Accumulator), rows.Steps ? std::uint32_t(draw.Gradient.RowsPerBand) : 0u,
					std::uint32_t(draw.Gradient.BandLimit), draw.Gradient.BandDelta, TerrainBatch::NoZ, flags, draw.Tint};
				if (draw.ZShaped)
				{
					shape.ZOffset = std::uint32_t(packed.size());
					packed.insert(packed.end(), draw.ZBytes.begin(), draw.ZBytes.end());
				}
				Shapes.Instances.push_back(shape);
				instances.push_back(instance);
				pages.push_back(atlasView.Texture);
				const auto kind = draw.Shadow ? ShadowKind : draw.Translucency ? TranslucentKind : BodyKind;
				const auto depth = draw.WritesDepth ? WriteDepth : draw.TestsDepth ? TestDepth : NoDepth;
				// Any other draw changes the target, and a translucent one over a pixel of its group would read
				// the color from before the group.
				const TerrainBatch::Rect rect {draw.X, draw.Y, draw.Width, draw.Height};
				const auto meets = [&rect](const TerrainBatch::Rect& other)
				{
					return rect.X < other.X + other.Width && other.X < rect.X + rect.Width && rect.Y < other.Y + other.Height && other.Y < rect.Y + rect.Height;
				};
				if (kind != TranslucentKind) group.clear();
				else if (std::any_of(group.begin(), group.end(), meets) || modes.empty() || (modes.back() & PipelineMask) / DepthModes != TranslucentKind)
				{
					group.assign(1, rect);
					++groups;
				}
				else group.push_back(rect);
				modes.push_back(std::uint32_t(kind) * DepthModes + std::uint32_t(depth) + (kind == TranslucentKind ? groups << GroupShift : 0u));
			}
			std::vector<TerrainBatch::Run> runs;
			for (std::uint32_t i = 0; i < pages.size(); ++i)
			{
				if (runs.empty() || runs.back().Page != pages[i] || runs.back().Mode != modes[i]) runs.push_back({pages[i], first + i, 1, false, modes[i]});
				else ++runs.back().Count;
			}
			return runs;
		}
	}

	bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const TerrainSnapshot::Frame* frame, const OverlaySnapshot::Frame* overlays)
	{
		auto& instances = Tiles.Instances;
		instances.clear();
		Shapes.Instances.clear();
		ShapeRuns.clear();
		LayerRuns.clear();
		Skipped = 0;
		Visible = false;
		std::vector<const void*> pages;
		std::vector<std::uint8_t> depths;
		if (frame)
		{
			const auto& view = frame->Clip;
			if (view.Width <= 0 || view.Height <= 0 || !PrepareTargets(device, view.Width, view.Height)) return false;
			// The target's first pixel is Tile's (Clip.X, Clip.Y), and Clip is ViewBounds.
			AlphaX = -view.X;
			AlphaY = 0;
			const TerrainBatch::Rect clip {view.X, 0, view.Width, view.Height};
			for (const auto& placement : frame->Placements)
			{
				const auto atlasView = atlas.Resolve(Request(placement));
				// The Z plane lies a whole image height below the indices.
				const auto depth = placement.UsesZ ? TerrainBatch::Pack(placement.Depth, std::uint32_t(placement.Height)) : 0u;
				TerrainBatch::Instance instance;
				if (!atlasView.Texture || !TerrainBatch::Place(placement.X, placement.Y - view.Y, placement.Width, placement.Height, -view.X,
					clip, atlasView.X, atlasView.Y, placement.Table->Row, placement.Table->Shades, placement.LightRow, placement.Diamond,
					instance, depth)) continue;
				instances.push_back(instance);
				pages.push_back(atlasView.Texture);
				depths.push_back(placement.UsesZ ? 1 : 0);
			}
			Runs = TerrainBatch::Split(pages, depths);
			if (overlays && overlays->Visible)
			{
				std::vector<std::uint8_t> packed;
				ShapeRuns = PrepareShapes(atlas, view, overlays->Draws, packed);
				LayerRuns = PrepareShapes(atlas, view, overlays->CompositeDraws, packed);
				// At least one word: the shader's buffer is bound whenever shapes draw.
				auto& bytes = ZBytes.Instances;
				bytes.assign((packed.size() + 3) / 4 + 1, 0);
				std::memcpy(bytes.data(), packed.data(), packed.size());
			}
			CompositeInstance = std::uint32_t(instances.size());
			instances.push_back({frame->OriginX + view.X, view.Y, TerrainBatch::Pack(std::uint32_t(view.Width), std::uint32_t(view.Height)),
				0, 0, 0, 0, 0});
			Visible = true;
		}
		else Runs.clear();
		if (!Tiles.Prepare(device) || !Shapes.Prepare(device) || !ZBytes.Prepare(device)) return false;
		PendingTables = TerrainSnapshot::Tables.Generation() != UploadedTables && TerrainSnapshot::Tables.Count() != 0;
		return true;
	}

	bool HasUploads() { return Tiles.Pending || Shapes.Pending || ZBytes.Pending || PendingTables; }

	bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
	{
		if (!Tiles.Upload(device, pass) || !Shapes.Upload(device, pass) || !ZBytes.Upload(device, pass)) return false;
		if (PendingTables)
		{
			// Cycling leaves the new texture undefined outside the upload, so every live table is uploaded.
			const auto rows = TerrainSnapshot::Tables.UsedRows();
			auto* mapped = static_cast<std::uint32_t*>(SDL_MapGPUTransferBuffer(device, TableTransfer, true));
			if (!mapped) return false;
			std::memset(mapped, 0, size_t(rows) * 256 * sizeof(std::uint32_t));
			TerrainSnapshot::Tables.ForEach([mapped](const TerrainBatch::ColorTables::Table& table)
			{
				for (size_t i = 0; i < table.Colors.size(); ++i) mapped[size_t(table.Row) * 256 + i] = table.Colors[i];
			});
			SDL_UnmapGPUTransferBuffer(device, TableTransfer);
			SDL_GPUTextureTransferInfo source {};
			source.transfer_buffer = TableTransfer;
			source.pixels_per_row = 256;
			source.rows_per_layer = rows;
			SDL_GPUTextureRegion destination {};
			destination.texture = TableTexture;
			destination.w = 256;
			destination.h = rows;
			destination.d = 1;
			SDL_UploadToGPUTexture(pass, &source, &destination, true);
			UploadedTables = TerrainSnapshot::Tables.Generation();
			PendingTables = false;
		}
		return true;
	}

	bool Render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* abuffer, int width, int height)
	{
		if (!Visible || !ColorTexture) return true;
		SDL_GPUColorTargetInfo target {};
		target.texture = ColorTexture;
		target.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.cycle = true;
		// The depth outlives a pass that a run of translucent draws ends to copy the colors.
		SDL_GPUDepthStencilTargetInfo depth {};
		depth.texture = DepthTexture;
		depth.clear_depth = ClearDepth;
		depth.load_op = SDL_GPU_LOADOP_CLEAR;
		depth.store_op = SDL_GPU_STOREOP_STORE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depth.cycle = true;
		auto* pass = SDL_BeginGPURenderPass(commands, &target, 1, &depth);
		if (!pass) return false;
		const SDL_GPUViewport viewport {0.0f, 0.0f, float(Width), float(Height), 0.0f, 1.0f};
		if (!Runs.empty() && Tiles.Buffer && abuffer)
		{
			SDL_SetGPUViewport(pass, &viewport);
			SDL_BindGPUVertexStorageBuffers(pass, 0, &Tiles.Buffer, 1);
			const auto& pack = Unsorted::ColorPackData;
			const DecodeUniforms decode {
				{Uint32(pack._R_SHL), Uint32(pack._G_SHL), Uint32(pack._B_SHL), 0},
				{ChannelBits(pack._R_SHR), ChannelBits(pack._G_SHR), ChannelBits(pack._B_SHR), 0},
				{AlphaX, AlphaY, width, height}};
			SDL_GPUGraphicsPipeline* bound = nullptr;
			for (const auto& run : Runs)
			{
				if (auto* pipeline = TilePipelines[run.Depth ? 1 : 0]; pipeline != bound)
				{
					SDL_BindGPUGraphicsPipeline(pass, pipeline);
					SDL_PushGPUFragmentUniformData(commands, 0, &decode, sizeof(decode));
					bound = pipeline;
				}
				const SDL_GPUTextureSamplerBinding bindings[3] {
					{static_cast<SDL_GPUTexture*>(const_cast<void*>(run.Page)), Sampler}, {TableTexture, Sampler}, {abuffer, Sampler}};
				SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
				const ViewUniforms view {{Uint32(Width), Uint32(Height), run.First, 0}};
				SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
				SDL_DrawGPUPrimitives(pass, run.Count * VerticesPerTile, 1, 0, 0);
			}
			if (!(ShapeRuns.empty() && LayerRuns.empty()) && Shapes.Buffer && ZBytes.Buffer)
			{
				SDL_GPUBuffer* buffers[2] {Shapes.Buffer, ZBytes.Buffer};
				// The Tile pass's shapes, then what RenderLayers draws over Tile's copy.
				std::vector<const TerrainBatch::Run*> ordered;
				for (const auto& run : ShapeRuns) ordered.push_back(&run);
				for (const auto& run : LayerRuns) ordered.push_back(&run);
				std::uint32_t copied = 0;
				for (const auto* shapeRun : ordered)
				{
					const auto& run = *shapeRun;
					const auto pipelineIndex = run.Mode & PipelineMask;
					// A new group of translucent draws reads the colors drawn so far from a copy.
					if (pipelineIndex / DepthModes == TranslucentKind && run.Mode >> GroupShift != copied)
					{
						copied = run.Mode >> GroupShift;
						SDL_EndGPURenderPass(pass);
						auto* copy = SDL_BeginGPUCopyPass(commands);
						if (!copy) return false;
						const SDL_GPUTextureLocation from {ColorTexture, 0, 0, 0, 0, 0};
						const SDL_GPUTextureLocation to {CopyTexture, 0, 0, 0, 0, 0};
						SDL_CopyGPUTextureToTexture(copy, &from, &to, Uint32(Width), Uint32(Height), 1, true);
						SDL_EndGPUCopyPass(copy);
						target.load_op = SDL_GPU_LOADOP_LOAD;
						target.cycle = false;
						depth.load_op = SDL_GPU_LOADOP_LOAD;
						depth.cycle = false;
						pass = SDL_BeginGPURenderPass(commands, &target, 1, &depth);
						if (!pass) return false;
						SDL_SetGPUViewport(pass, &viewport);
						SDL_BindGPUVertexStorageBuffers(pass, 0, &Tiles.Buffer, 1);
						bound = nullptr;
					}
					auto* pipeline = ShapePipelines[pipelineIndex / DepthModes][pipelineIndex % DepthModes];
					if (pipeline != bound)
					{
						SDL_BindGPUGraphicsPipeline(pass, pipeline);
						SDL_PushGPUFragmentUniformData(commands, 0, &decode, sizeof(decode));
						SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 2);
						bound = pipeline;
					}
					const SDL_GPUTextureSamplerBinding bindings[4] {
						{static_cast<SDL_GPUTexture*>(const_cast<void*>(run.Page)), Sampler}, {TableTexture, Sampler}, {abuffer, Sampler}, {CopyTexture, Sampler}};
					SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);
					const ViewUniforms view {{Uint32(Width), Uint32(Height), run.First, 0}};
					SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
					SDL_DrawGPUPrimitives(pass, run.Count * VerticesPerTile, 1, 0, 0);
				}
			}
		}
		SDL_EndGPURenderPass(pass);
		return true;
	}

	void Draw(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* commands, const SDL_GPUViewport& viewport,
		int logicalWidth, int logicalHeight)
	{
		if (!Visible || !CompositePipeline || !Tiles.Buffer || !ColorTexture || logicalWidth <= 0 || logicalHeight <= 0) return;
		SDL_BindGPUGraphicsPipeline(pass, CompositePipeline);
		SDL_SetGPUViewport(pass, &viewport);
		SDL_BindGPUVertexStorageBuffers(pass, 0, &Tiles.Buffer, 1);
		const auto& pack = Unsorted::ColorPackData;
		const CompositeUniforms decode {{ChannelBits(pack._R_SHR), ChannelBits(pack._G_SHR), ChannelBits(pack._B_SHR), 0}};
		SDL_PushGPUFragmentUniformData(commands, 0, &decode, sizeof(decode));
		const SDL_GPUTextureSamplerBinding binding {ColorTexture, Sampler};
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		const ViewUniforms view {{Uint32(logicalWidth), Uint32(logicalHeight), CompositeInstance, 0}};
		SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
		SDL_DrawGPUPrimitives(pass, VerticesPerTile, 1, 0, 0);
	}

	size_t DrawCount() { return Runs.size() + ShapeRuns.size() + LayerRuns.size(); }
	size_t ShapeCount() { return Shapes.Instances.size(); }
	size_t SkippedShapes() { return Skipped; }
	size_t InstanceCount() { return Visible ? Tiles.Instances.size() - 1 : 0; }
}
