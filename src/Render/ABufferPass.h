#pragma once

#include "ABufferSnapshot.h"
#include "FrameAtlas.h"

#include <SDL3/SDL_gpu.h>

#include <vector>

// The ABuffer the terrain shader reads: an R8 texture of ViewBounds' size, cleared to 127, then the
// snapshot's shroud and fog frames in their own render pass before the present's. Every overwrite is
// drawn before every darken; the trace's split count checks that this equals the native order of both
// passes per cell. Words above 255, which only abnormal input could write, are not representable.
// The pipelines and instances live in ABufferPass.cpp.
namespace ABufferPass
{
	// The texture TerrainPass::Draw samples and its size, ViewBounds' when drawn.
	extern SDL_GPUTexture* Texture;
	extern int Width, Height;

	bool Create(SDL_GPUDevice* device);
	void Release(SDL_GPUDevice* device);
	void AppendRequests(const ABufferSnapshot::Snapshot& snapshot, std::vector<FrameAtlas::Request>& requests);
	// After FrameAtlas::Begin accepted the requests. width x height is ViewBounds' size; without a
	// ready snapshot of the same view the texture stays cleared to 127. A null snapshot draws nothing.
	bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const ABufferSnapshot::Snapshot* snapshot, int width, int height);
	bool HasUploads();
	bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass);
	// Its own render pass, after the uploads and before the present's render pass samples the texture.
	bool Render(SDL_GPUCommandBuffer* commands);
}
