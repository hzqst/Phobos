#include "TerrainPass.h"

#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <Shaders/TerrainComposite.ps.dxil.h>
#include <Shaders/TerrainComposite.ps.spirv.h>
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
		SDL_GPUGraphicsPipeline* CompositePipeline = nullptr;
		SDL_GPUTextureFormat DepthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		SDL_GPUTexture* TableTexture = nullptr;
		SDL_GPUTransferBuffer* TableTransfer = nullptr;
		// The tactical target, ViewBounds' size.
		SDL_GPUTexture* ColorTexture = nullptr;
		SDL_GPUTexture* DepthTexture = nullptr;
		int Width = 0, Height = 0;
		// The tile images, then one instance that draws the tactical target onto the canvas.
		InstanceBuffer Tiles;
		std::vector<TerrainBatch::Run> Runs;
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
			ColorTexture = nullptr; DepthTexture = nullptr;
			Width = Height = 0;
		}

		// Released textures stay alive until the GPU no longer uses them.
		bool PrepareTargets(SDL_GPUDevice* device, int width, int height)
		{
			if (ColorTexture && DepthTexture && width == Width && height == Height) return true;
			ReleaseTargets(device);
			ColorTexture = CreateTarget(device, ColorFormat, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
			DepthTexture = CreateTarget(device, DepthFormat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, width, height);
			if (!ColorTexture || !DepthTexture) { ReleaseTargets(device); return false; }
			Width = width;
			Height = height;
			return true;
		}

		Uint32 ChannelBits(int shiftRight) { return Uint32(8 - shiftRight); }
	}

	SDL_GPUSampler* Sampler = nullptr;

	bool InstanceBuffer::Prepare(SDL_GPUDevice* device)
	{
		if (Instances.size() > Capacity)
		{
			// Released buffers stay alive until the GPU no longer uses them.
			Release(device);
			Capacity = std::max<std::uint32_t>(std::uint32_t(Instances.size()) * 2, 1024);
			SDL_GPUBufferCreateInfo buffer {};
			buffer.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			buffer.size = Capacity * sizeof(TerrainBatch::Instance);
			Buffer = SDL_CreateGPUBuffer(device, &buffer);
			SDL_GPUTransferBufferCreateInfo transfer {};
			transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer.size = buffer.size;
			Transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
			if (!Buffer || !Transfer) { Capacity = 0; return false; }
		}
		// Presents without a new GScreen frame reuse the uploaded instances.
		Pending = !Instances.empty() && (Instances.size() != Uploaded.size()
			|| std::memcmp(Instances.data(), Uploaded.data(), Instances.size() * sizeof(TerrainBatch::Instance)) != 0);
		return true;
	}

	bool InstanceBuffer::Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
	{
		if (!Pending) return true;
		const auto bytes = std::uint32_t(Instances.size() * sizeof(TerrainBatch::Instance));
		auto* mapped = SDL_MapGPUTransferBuffer(device, Transfer, true);
		if (!mapped) return false;
		std::memcpy(mapped, Instances.data(), bytes);
		SDL_UnmapGPUTransferBuffer(device, Transfer);
		const SDL_GPUTransferBufferLocation source {Transfer, 0};
		const SDL_GPUBufferRegion destination {Buffer, 0, bytes};
		SDL_UploadToGPUBuffer(pass, &source, &destination, true);
		Uploaded = Instances;
		Pending = false;
		return true;
	}

	void InstanceBuffer::Release(SDL_GPUDevice* device)
	{
		if (Buffer) SDL_ReleaseGPUBuffer(device, Buffer);
		if (Transfer) SDL_ReleaseGPUTransferBuffer(device, Transfer);
		Buffer = nullptr; Transfer = nullptr; Capacity = 0;
		Uploaded.clear();
		Pending = false;
	}

	SDL_GPUGraphicsPipeline* CreatePipeline(SDL_GPUDevice* device, const GPUShader::Code& pixel, Uint32 samplers,
		const SDL_GPUColorTargetDescription& color, const SDL_GPUDepthStencilState* depth, SDL_GPUTextureFormat depthFormat)
	{
		SDL_GPUGraphicsPipeline* pipeline = nullptr;
		auto* vertex = GPUShader::Create(device, GPUShader::Make(g_TerrainTile_vs_DXIL, g_TerrainTile_vs_SPIRV),
			SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
		auto* fragment = GPUShader::Create(device, pixel, SDL_GPU_SHADERSTAGE_FRAGMENT, samplers, 1);
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
		SDL_GPUColorTargetDescription window {};
		window.format = target;
		CompositePipeline = CreatePipeline(device, GPUShader::Make(g_TerrainComposite_ps_DXIL, g_TerrainComposite_ps_SPIRV), 1, window);
		if (!TilePipelines[0] || !TilePipelines[1] || !CompositePipeline) return false;

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
		if (CompositePipeline) SDL_ReleaseGPUGraphicsPipeline(device, CompositePipeline);
		if (Sampler) SDL_ReleaseGPUSampler(device, Sampler);
		if (TableTexture) SDL_ReleaseGPUTexture(device, TableTexture);
		if (TableTransfer) SDL_ReleaseGPUTransferBuffer(device, TableTransfer);
		ReleaseTargets(device);
		Tiles.Release(device);
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

	bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const TerrainSnapshot::Frame* frame)
	{
		auto& instances = Tiles.Instances;
		instances.clear();
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
			CompositeInstance = std::uint32_t(instances.size());
			instances.push_back({frame->OriginX + view.X, view.Y, TerrainBatch::Pack(std::uint32_t(view.Width), std::uint32_t(view.Height)),
				0, 0, 0, 0, 0});
			Visible = true;
		}
		else Runs.clear();
		if (!Tiles.Prepare(device)) return false;
		PendingTables = TerrainSnapshot::Tables.Generation() != UploadedTables && TerrainSnapshot::Tables.Count() != 0;
		return true;
	}

	bool HasUploads() { return Tiles.Pending || PendingTables; }

	bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
	{
		if (!Tiles.Upload(device, pass)) return false;
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
		SDL_GPUDepthStencilTargetInfo depth {};
		depth.texture = DepthTexture;
		depth.clear_depth = ClearDepth;
		depth.load_op = SDL_GPU_LOADOP_CLEAR;
		depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depth.cycle = true;
		auto* pass = SDL_BeginGPURenderPass(commands, &target, 1, &depth);
		if (!pass) return false;
		if (!Runs.empty() && Tiles.Buffer && abuffer)
		{
			const SDL_GPUViewport viewport {0.0f, 0.0f, float(Width), float(Height), 0.0f, 1.0f};
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

	size_t DrawCount() { return Runs.size(); }
	size_t InstanceCount() { return Visible ? Tiles.Instances.size() - 1 : 0; }
}
