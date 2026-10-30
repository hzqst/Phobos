#pragma once

#include "FrameAtlas.h"
#include "GPUShader.h"
#include "TerrainBatch.h"
#include "TerrainSnapshot.h"

#include <SDL3/SDL_gpu.h>

#include <vector>

// The instanced terrain draw: every clipped tile image of the snapshot is one instance, drawn
// in native order with one draw per run of the same atlas page. Colors come from the cells'
// whole LightConvert tables, packed into one texture the pixel shader indexes by shade row, and
// the light of each pixel from the ABuffer texture ABufferPass drew. It runs on the main thread
// like the rest of the render; the GPU resources and shaders live in TerrainPass.cpp.
namespace TerrainPass
{
	constexpr std::uint32_t TableRows = 1024;
	constexpr Uint32 VerticesPerTile = 6;

	struct ViewUniforms { Uint32 View[4]; };

	// Instances in a storage buffer, uploaded only when they changed; the capacity grows by doubling.
	struct InstanceBuffer
	{
		SDL_GPUBuffer* Buffer = nullptr;
		SDL_GPUTransferBuffer* Transfer = nullptr;
		std::uint32_t Capacity = 0;
		std::vector<TerrainBatch::Instance> Instances, Uploaded;
		bool Pending = false;

		// After Instances is filled: room for them and whether they need an upload.
		bool Prepare(SDL_GPUDevice* device);
		bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass);
		void Release(SDL_GPUDevice* device);
	};

	// A pipeline of TerrainTile.vs and the given pixel shader, which reads samplers textures.
	SDL_GPUGraphicsPipeline* CreatePipeline(SDL_GPUDevice* device, const GPUShader::Code& pixel, Uint32 samplers,
		const SDL_GPUColorTargetDescription& color);

	// Nearest, clamped; ABufferPass samples its frames with it too.
	extern SDL_GPUSampler* Sampler;

	bool Create(SDL_GPUDevice* device, SDL_GPUTextureFormat target);
	void Release(SDL_GPUDevice* device);

	FrameAtlas::Request Request(const TerrainSnapshot::Placement& placement);
	// After FrameAtlas::Begin accepted the requests: the instances, their runs and what to upload.
	bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const TerrainSnapshot::Frame* frame);
	bool HasUploads();
	bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass);
	// Inside the render pass, before the sprites; the viewport is the letterboxed canvas. abuffer is the
	// width x height ABuffer texture ABufferPass drew.
	void Draw(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* commands, const SDL_GPUViewport& viewport,
		int logicalWidth, int logicalHeight, SDL_GPUTexture* abuffer, int width, int height);

	size_t DrawCount();
	size_t InstanceCount();
}
