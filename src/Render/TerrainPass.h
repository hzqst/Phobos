#pragma once

#include "FrameAtlas.h"
#include "GPUShader.h"
#include "TerrainBatch.h"
#include "TerrainSnapshot.h"

#include <SDL3/SDL_gpu.h>

#include <Shaders/TerrainTile.ps.dxil.h>
#include <Shaders/TerrainTile.ps.spirv.h>
#include <Shaders/TerrainTile.vs.dxil.h>
#include <Shaders/TerrainTile.vs.spirv.h>

#include <Unsorted.h>

#include <cstring>
#include <vector>

// The instanced terrain draw: every clipped tile image of the snapshot is one instance, drawn
// in native order with one draw per run of the same atlas page. Colors come from the cells'
// whole LightConvert tables, packed into one texture the pixel shader indexes by shade row, and
// the light of each pixel from the ABuffer texture ABufferPass drew. Only GPURender.cpp includes
// this (with ABufferPass.h); it runs on the main thread like the rest of the render.
namespace TerrainPass
{
	constexpr std::uint32_t TableRows = 1024;
	constexpr Uint32 VerticesPerTile = 6;

	struct ViewUniforms { Uint32 View[4]; };
	// Matches cbuffer Decode in TerrainTile.ps.hlsl: Alpha holds the ABuffer's canvas position and size.
	struct DecodeUniforms { Uint32 Shift[4], Bits[4]; Sint32 Alpha[4]; };

	// Instances in a storage buffer, uploaded only when they changed; the capacity grows by doubling.
	struct InstanceBuffer
	{
		SDL_GPUBuffer* Buffer = nullptr;
		SDL_GPUTransferBuffer* Transfer = nullptr;
		std::uint32_t Capacity = 0;
		std::vector<TerrainBatch::Instance> Instances, Uploaded;
		bool Pending = false;

		// After Instances is filled: room for them and whether they need an upload.
		bool Prepare(SDL_GPUDevice* device)
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

		bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
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

		void Release(SDL_GPUDevice* device)
		{
			if (Buffer) SDL_ReleaseGPUBuffer(device, Buffer);
			if (Transfer) SDL_ReleaseGPUTransferBuffer(device, Transfer);
			Buffer = nullptr; Transfer = nullptr; Capacity = 0;
			Uploaded.clear();
			Pending = false;
		}
	};

	// A pipeline of TerrainTile.vs and the given pixel shader, which reads samplers textures.
	inline SDL_GPUGraphicsPipeline* CreatePipeline(SDL_GPUDevice* device, const GPUShader::Code& pixel, Uint32 samplers,
		const SDL_GPUColorTargetDescription& color)
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
			pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
		}
		if (vertex) SDL_ReleaseGPUShader(device, vertex);
		if (fragment) SDL_ReleaseGPUShader(device, fragment);
		return pipeline;
	}

	inline SDL_GPUGraphicsPipeline* Pipeline = nullptr;
	inline SDL_GPUSampler* Sampler = nullptr;
	inline SDL_GPUTexture* TableTexture = nullptr;
	inline SDL_GPUTransferBuffer* TableTransfer = nullptr;
	inline InstanceBuffer Tiles;
	inline std::vector<TerrainBatch::Run> Runs;
	inline std::uint64_t UploadedTables = UINT64_MAX;
	inline bool PendingTables = false;
	// Canvas position of the ABuffer's first word: Tile's origin on the canvas and ViewBounds.Y.
	inline int AlphaX = 0, AlphaY = 0;

	inline bool Create(SDL_GPUDevice* device, SDL_GPUTextureFormat target)
	{
		// Pixels are opaque or discarded; the terrain is the bottom layer.
		SDL_GPUColorTargetDescription color {};
		color.format = target;
		Pipeline = CreatePipeline(device, GPUShader::Make(g_TerrainTile_ps_DXIL, g_TerrainTile_ps_SPIRV), 3, color);
		if (!Pipeline) return false;

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

	inline void Release(SDL_GPUDevice* device)
	{
		if (Pipeline) SDL_ReleaseGPUGraphicsPipeline(device, Pipeline);
		if (Sampler) SDL_ReleaseGPUSampler(device, Sampler);
		if (TableTexture) SDL_ReleaseGPUTexture(device, TableTexture);
		if (TableTransfer) SDL_ReleaseGPUTransferBuffer(device, TableTransfer);
		Tiles.Release(device);
		Pipeline = nullptr; Sampler = nullptr; TableTexture = nullptr; TableTransfer = nullptr;
		Tiles.Instances.clear(); Runs.clear();
		UploadedTables = UINT64_MAX;
		PendingTables = false;
	}

	inline FrameAtlas::Request Request(const TerrainSnapshot::Placement& placement)
	{
		return {placement.ID, {}, FrameAtlas::SourceKind::Mask, &TerrainSnapshot::Frames};
	}

	// After FrameAtlas::Begin accepted the requests: the instances, their runs and what to upload.
	inline bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const TerrainSnapshot::Frame* frame)
	{
		auto& instances = Tiles.Instances;
		instances.clear();
		std::vector<const void*> pages;
		if (frame)
		{
			AlphaX = frame->OriginX;
			AlphaY = frame->Clip.Y;
			const TerrainBatch::Rect clip {frame->Clip.X, frame->Clip.Y, frame->Clip.Width, frame->Clip.Height};
			for (const auto& placement : frame->Placements)
			{
				const auto view = atlas.Resolve(Request(placement));
				TerrainBatch::Instance instance;
				if (!view.Texture || !TerrainBatch::Place(placement.X, placement.Y, placement.Width, placement.Height, frame->OriginX,
					clip, view.X, view.Y, placement.Table->Row, placement.Table->Shades, placement.LightRow, placement.Diamond, instance)) continue;
				instances.push_back(instance);
				pages.push_back(view.Texture);
			}
		}
		Runs = TerrainBatch::Split(pages);
		if (!Tiles.Prepare(device)) return false;
		PendingTables = TerrainSnapshot::Tables.Generation() != UploadedTables && TerrainSnapshot::Tables.Count() != 0;
		return true;
	}

	inline bool HasUploads() { return Tiles.Pending || PendingTables; }

	inline bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
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

	// Inside the render pass, before the sprites; the viewport is the letterboxed canvas. abuffer is the
	// width x height ABuffer texture ABufferPass drew.
	inline void Draw(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* commands, const SDL_GPUViewport& viewport,
		int logicalWidth, int logicalHeight, SDL_GPUTexture* abuffer, int width, int height)
	{
		if (Runs.empty() || !Pipeline || !Tiles.Buffer || !abuffer || logicalWidth <= 0 || logicalHeight <= 0) return;
		SDL_BindGPUGraphicsPipeline(pass, Pipeline);
		SDL_SetGPUViewport(pass, &viewport);
		SDL_BindGPUVertexStorageBuffers(pass, 0, &Tiles.Buffer, 1);
		const auto& pack = Unsorted::ColorPackData;
		const DecodeUniforms decode {
			{Uint32(pack._R_SHL), Uint32(pack._G_SHL), Uint32(pack._B_SHL), 0},
			{Uint32(8 - pack._R_SHR), Uint32(8 - pack._G_SHR), Uint32(8 - pack._B_SHR), 0},
			{AlphaX, AlphaY, width, height}};
		SDL_PushGPUFragmentUniformData(commands, 0, &decode, sizeof(decode));
		for (const auto& run : Runs)
		{
			const SDL_GPUTextureSamplerBinding bindings[3] {
				{static_cast<SDL_GPUTexture*>(const_cast<void*>(run.Page)), Sampler}, {TableTexture, Sampler}, {abuffer, Sampler}};
			SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
			const ViewUniforms view {{Uint32(logicalWidth), Uint32(logicalHeight), run.First, 0}};
			SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
			SDL_DrawGPUPrimitives(pass, run.Count * VerticesPerTile, 1, 0, 0);
		}
	}

	inline size_t DrawCount() { return Runs.size(); }
	inline size_t InstanceCount() { return Tiles.Instances.size(); }
}
