#pragma once

#include "FrameAtlas.h"
#include "GPUShader.h"
#include "OverlaySnapshot.h"
#include "TerrainBatch.h"
#include "TerrainSnapshot.h"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstring>
#include <vector>

// The instanced terrain draw: every clipped tile image of the snapshot is one instance, drawn
// in native order with one draw per run of the same atlas page and depth mode. Colors come from
// the cells' whole LightConvert tables, packed into one texture the pixel shader indexes by shade
// row, and the light of each pixel from the ABuffer texture ABufferPass drew. The images go into
// a tactical target of ViewBounds' size, one target pixel per Tile pixel: color channels as their
// integer values and a depth buffer holding the native Z, tested and written like 547CF0. The
// overlays of OverlaySnapshot follow, one instance per 4AED70 draw, with the strict depth test of the
// shape blitters and a blending pipeline for their shadows (C5.2c). The animated Terrain mode 2
// draws on Composite, Tile's copy, comes last with the same test and no depth write (D2b), so the
// target matches the native Composite after the layers. The present pass then draws that
// target onto the window. It runs on the main thread like the rest
// of the render; the GPU resources and shaders live in TerrainPass.cpp.
namespace TerrainPass
{
	constexpr std::uint32_t TableRows = 1024;
	constexpr Uint32 VerticesPerTile = 6;

	struct ViewUniforms { Uint32 View[4]; };

	// Elements in a storage buffer, uploaded only when they changed; the capacity grows by doubling.
	template<class T>
	struct StorageBuffer
	{
		SDL_GPUBuffer* Buffer = nullptr;
		SDL_GPUTransferBuffer* Transfer = nullptr;
		std::uint32_t Capacity = 0;
		std::vector<T> Instances, Uploaded;
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
				buffer.size = Capacity * sizeof(T);
				Buffer = SDL_CreateGPUBuffer(device, &buffer);
				SDL_GPUTransferBufferCreateInfo transfer {};
				transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				transfer.size = buffer.size;
				Transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
				if (!Buffer || !Transfer) { Capacity = 0; return false; }
			}
			// Presents without a new GScreen frame reuse the uploaded instances.
			Pending = !Instances.empty() && (Instances.size() != Uploaded.size()
				|| std::memcmp(Instances.data(), Uploaded.data(), Instances.size() * sizeof(T)) != 0);
			return true;
		}

		bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass)
		{
			if (!Pending) return true;
			const auto bytes = std::uint32_t(Instances.size() * sizeof(T));
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
	using InstanceBuffer = StorageBuffer<TerrainBatch::Instance>;

	// A pipeline of TerrainTile.vs and the given pixel shader, which reads samplers textures and
	// storageBuffers buffers, with a depth target of depthFormat when depth is given.
	SDL_GPUGraphicsPipeline* CreatePipeline(SDL_GPUDevice* device, const GPUShader::Code& pixel, Uint32 samplers,
		const SDL_GPUColorTargetDescription& color, const SDL_GPUDepthStencilState* depth = nullptr,
		SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID, Uint32 storageBuffers = 0);

	// Nearest, clamped; ABufferPass samples its frames with it too.
	extern SDL_GPUSampler* Sampler;

	// target is the window's format, which the tactical target is drawn onto.
	bool Create(SDL_GPUDevice* device, SDL_GPUTextureFormat target);
	void Release(SDL_GPUDevice* device);

	FrameAtlas::Request Request(const TerrainSnapshot::Placement& placement);
	FrameAtlas::Request Request(const OverlaySnapshot::Draw& draw);
	// After FrameAtlas::Begin accepted the requests: the tactical target's size, the instances, their
	// runs and what to upload. A null frame draws nothing; null overlays draw no overlays.
	bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const TerrainSnapshot::Frame* frame,
		const OverlaySnapshot::Frame* overlays);
	bool HasUploads();
	bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass);
	// The tactical target's own render pass, after the uploads and ABufferPass::Render. abuffer is the
	// width x height ABuffer texture ABufferPass drew.
	bool Render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* abuffer, int width, int height);
	// Inside the present's render pass, before the sprites: the tactical target onto the canvas, which
	// the letterboxed viewport maps to the window.
	void Draw(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* commands, const SDL_GPUViewport& viewport,
		int logicalWidth, int logicalHeight);

	size_t DrawCount();
	size_t InstanceCount();
	// Overlay draws prepared, and those skipped (no atlas frame, a value out of range, or a shadow
	// mask other than the color pack's).
	size_t ShapeCount();
	size_t SkippedShapes();
}
