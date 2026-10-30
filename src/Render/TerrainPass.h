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
// whole LightConvert tables, packed into one texture the pixel shader indexes by shade row.
// Only GPURender.cpp includes this; it runs on the main thread like the rest of the render.
namespace TerrainPass
{
	constexpr std::uint32_t TableRows = 1024;
	constexpr Uint32 VerticesPerTile = 6;
	constexpr std::uint32_t DefaultAlpha = TerrainImage::DefaultAlpha;

	struct ViewUniforms { Uint32 View[4]; };
	// Matches cbuffer Decode in TerrainTile.ps.hlsl.
	struct DecodeUniforms { Uint32 Shift[4], Bits[4], Alpha[4]; };

	inline SDL_GPUGraphicsPipeline* Pipeline = nullptr;
	inline SDL_GPUSampler* Sampler = nullptr;
	inline SDL_GPUTexture* TableTexture = nullptr;
	inline SDL_GPUTransferBuffer* TableTransfer = nullptr;
	inline SDL_GPUBuffer* InstanceBuffer = nullptr;
	inline SDL_GPUTransferBuffer* InstanceTransfer = nullptr;
	inline std::uint32_t InstanceCapacity = 0;
	inline std::vector<TerrainBatch::Instance> Instances, Uploaded;
	inline std::vector<TerrainBatch::Run> Runs;
	inline std::uint64_t UploadedTables = UINT64_MAX;
	inline bool PendingInstances = false, PendingTables = false;

	inline bool Create(SDL_GPUDevice* device, SDL_GPUTextureFormat target)
	{
		auto* vertex = GPUShader::Create(device, GPUShader::Make(g_TerrainTile_vs_DXIL, g_TerrainTile_vs_SPIRV),
			SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
		auto* fragment = GPUShader::Create(device, GPUShader::Make(g_TerrainTile_ps_DXIL, g_TerrainTile_ps_SPIRV),
			SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
		if (vertex && fragment)
		{
			// Pixels are opaque or discarded; the terrain is the bottom layer.
			SDL_GPUColorTargetDescription color {};
			color.format = target;
			SDL_GPUGraphicsPipelineCreateInfo info {};
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.target_info.color_target_descriptions = &color;
			info.target_info.num_color_targets = 1;
			Pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
		}
		if (vertex) SDL_ReleaseGPUShader(device, vertex);
		if (fragment) SDL_ReleaseGPUShader(device, fragment);
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
		if (InstanceBuffer) SDL_ReleaseGPUBuffer(device, InstanceBuffer);
		if (InstanceTransfer) SDL_ReleaseGPUTransferBuffer(device, InstanceTransfer);
		Pipeline = nullptr; Sampler = nullptr; TableTexture = nullptr; TableTransfer = nullptr;
		InstanceBuffer = nullptr; InstanceTransfer = nullptr; InstanceCapacity = 0;
		Instances.clear(); Uploaded.clear(); Runs.clear();
		UploadedTables = UINT64_MAX;
		PendingInstances = PendingTables = false;
	}

	inline FrameAtlas::Request Request(const TerrainSnapshot::Placement& placement)
	{
		return {placement.ID, {}, FrameAtlas::SourceKind::Mask, &TerrainSnapshot::Frames};
	}

	// After FrameAtlas::Begin accepted the requests: the instances, their runs and what to upload.
	inline bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const TerrainSnapshot::Frame* frame)
	{
		Instances.clear();
		std::vector<const void*> pages;
		if (frame)
		{
			const TerrainBatch::Rect clip {frame->Clip.X, frame->Clip.Y, frame->Clip.Width, frame->Clip.Height};
			for (const auto& placement : frame->Placements)
			{
				const auto view = atlas.Resolve(Request(placement));
				TerrainBatch::Instance instance;
				if (!view.Texture || !TerrainBatch::Place(placement.X, placement.Y, placement.Width, placement.Height, frame->OriginX,
					clip, view.X, view.Y, placement.Table->Row, placement.Table->Shades, placement.LightRow, placement.Diamond, instance)) continue;
				Instances.push_back(instance);
				pages.push_back(view.Texture);
			}
		}
		Runs = TerrainBatch::Split(pages);
		if (Instances.size() > InstanceCapacity)
		{
			// Released buffers stay alive until the GPU no longer uses them.
			if (InstanceBuffer) SDL_ReleaseGPUBuffer(device, InstanceBuffer);
			if (InstanceTransfer) SDL_ReleaseGPUTransferBuffer(device, InstanceTransfer);
			InstanceCapacity = std::max<std::uint32_t>(std::uint32_t(Instances.size()) * 2, 1024);
			SDL_GPUBufferCreateInfo buffer {};
			buffer.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
			buffer.size = InstanceCapacity * sizeof(TerrainBatch::Instance);
			InstanceBuffer = SDL_CreateGPUBuffer(device, &buffer);
			SDL_GPUTransferBufferCreateInfo transfer {};
			transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			transfer.size = buffer.size;
			InstanceTransfer = SDL_CreateGPUTransferBuffer(device, &transfer);
			Uploaded.clear();
			if (!InstanceBuffer || !InstanceTransfer) { InstanceCapacity = 0; return false; }
		}
		// Presents without a new GScreen frame reuse the uploaded instances.
		PendingInstances = !Instances.empty() && (Instances.size() != Uploaded.size()
			|| std::memcmp(Instances.data(), Uploaded.data(), Instances.size() * sizeof(TerrainBatch::Instance)) != 0);
		PendingTables = TerrainSnapshot::Tables.Generation() != UploadedTables && TerrainSnapshot::Tables.Count() != 0;
		return true;
	}

	inline bool HasUploads() { return PendingInstances || PendingTables; }

	inline bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
	{
		if (PendingInstances)
		{
			const auto bytes = std::uint32_t(Instances.size() * sizeof(TerrainBatch::Instance));
			auto* mapped = SDL_MapGPUTransferBuffer(device, InstanceTransfer, true);
			if (!mapped) return false;
			std::memcpy(mapped, Instances.data(), bytes);
			SDL_UnmapGPUTransferBuffer(device, InstanceTransfer);
			const SDL_GPUTransferBufferLocation source {InstanceTransfer, 0};
			const SDL_GPUBufferRegion destination {InstanceBuffer, 0, bytes};
			SDL_UploadToGPUBuffer(pass, &source, &destination, true);
			Uploaded = Instances;
			PendingInstances = false;
		}
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

	// Inside the render pass, before the sprites; the viewport is the letterboxed canvas.
	inline void Draw(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* commands, const SDL_GPUViewport& viewport,
		int logicalWidth, int logicalHeight)
	{
		if (Runs.empty() || !Pipeline || !InstanceBuffer || logicalWidth <= 0 || logicalHeight <= 0) return;
		SDL_BindGPUGraphicsPipeline(pass, Pipeline);
		SDL_SetGPUViewport(pass, &viewport);
		SDL_BindGPUVertexStorageBuffers(pass, 0, &InstanceBuffer, 1);
		const auto& pack = Unsorted::ColorPackData;
		const DecodeUniforms decode {
			{Uint32(pack._R_SHL), Uint32(pack._G_SHL), Uint32(pack._B_SHL), 0},
			{Uint32(8 - pack._R_SHR), Uint32(8 - pack._G_SHR), Uint32(8 - pack._B_SHR), 0},
			{DefaultAlpha, 0, 0, 0}};
		SDL_PushGPUFragmentUniformData(commands, 0, &decode, sizeof(decode));
		for (const auto& run : Runs)
		{
			const SDL_GPUTextureSamplerBinding bindings[2] {
				{static_cast<SDL_GPUTexture*>(const_cast<void*>(run.Page)), Sampler}, {TableTexture, Sampler}};
			SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
			const ViewUniforms view {{Uint32(logicalWidth), Uint32(logicalHeight), run.First, 0}};
			SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
			SDL_DrawGPUPrimitives(pass, run.Count * VerticesPerTile, 1, 0, 0);
		}
	}

	inline size_t DrawCount() { return Runs.size(); }
	inline size_t InstanceCount() { return Instances.size(); }
}
