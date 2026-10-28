#include "PresentPass.h"

#include <Unsorted.h>

#include <SDL3/SDL_gpu.h>

#include <Shaders/DecodeRGB16.ps.dxil.h>
#include <Shaders/DecodeRGB16.ps.spirv.h>
#include <Shaders/Fullscreen.vs.dxil.h>
#include <Shaders/Fullscreen.vs.spirv.h>

#include <algorithm>
#include <cstddef>

SDL_GPUGraphicsPipeline* PresentPass::Pipeline = nullptr;
SDL_GPUSampler* PresentPass::Sampler = nullptr;

namespace
{
	constexpr SDL_FColor ClearColor { 0.0f, 0.0f, 0.0f, 1.0f };
	constexpr Uint32 FullscreenTriangleVertexCount = 3;

	// Matches cbuffer Decode in DecodeRGB16.ps.hlsl
	struct DecodeUniforms
	{
		Uint32 Shift[4];
		Uint32 Bits[4];
	};

	static_assert(sizeof(DecodeUniforms) == 32);

	struct ShaderCode
	{
		const unsigned char* pDXIL;
		size_t DXILSize;
		const unsigned char* pSPIRV;
		size_t SPIRVSize;
	};

	template <size_t DXILSize, size_t SPIRVSize>
	constexpr ShaderCode MakeShaderCode(const unsigned char (&dxil)[DXILSize], const unsigned char (&spirv)[SPIRVSize])
	{
		return { dxil, DXILSize, spirv, SPIRVSize };
	}

	SDL_GPUShader* CreateShader(SDL_GPUDevice* pDevice, const ShaderCode& code, SDL_GPUShaderStage stage,
		Uint32 samplerCount, Uint32 uniformBufferCount)
	{
		SDL_GPUShaderCreateInfo info {};
		info.entrypoint = "main";
		info.stage = stage;
		info.num_samplers = samplerCount;
		info.num_uniform_buffers = uniformBufferCount;

		if (SDL_GetGPUShaderFormats(pDevice) & SDL_GPU_SHADERFORMAT_DXIL)
		{
			info.format = SDL_GPU_SHADERFORMAT_DXIL;
			info.code = code.pDXIL;
			info.code_size = code.DXILSize;
		}
		else
		{
			info.format = SDL_GPU_SHADERFORMAT_SPIRV;
			info.code = code.pSPIRV;
			info.code_size = code.SPIRVSize;
		}

		return SDL_CreateGPUShader(pDevice, &info);
	}

	// The game packs 8 bit channels as (value >> SHR) << SHL, so a channel starts at bit SHL
	// and has 8 - SHR bits.
	DecodeUniforms GetDecodeUniforms()
	{
		const auto& pack = Unsorted::ColorPackData;

		return
		{
			{ static_cast<Uint32>(pack._R_SHL), static_cast<Uint32>(pack._G_SHL), static_cast<Uint32>(pack._B_SHL), 0 },
			{ static_cast<Uint32>(8 - pack._R_SHR), static_cast<Uint32>(8 - pack._G_SHR), static_cast<Uint32>(8 - pack._B_SHR), 0 },
		};
	}

	SDL_GPUViewport GetLetterboxViewport(unsigned int targetWidth, unsigned int targetHeight, int sourceWidth, int sourceHeight)
	{
		const float scale = std::min(
			static_cast<float>(targetWidth) / static_cast<float>(sourceWidth),
			static_cast<float>(targetHeight) / static_cast<float>(sourceHeight));

		SDL_GPUViewport viewport {};
		viewport.w = static_cast<float>(sourceWidth) * scale;
		viewport.h = static_cast<float>(sourceHeight) * scale;
		viewport.x = (static_cast<float>(targetWidth) - viewport.w) * 0.5f;
		viewport.y = (static_cast<float>(targetHeight) - viewport.h) * 0.5f;
		viewport.max_depth = 1.0f;
		return viewport;
	}
}

bool PresentPass::Create(SDL_GPUDevice* pDevice, SDL_Window* pWindow)
{
	SDL_GPUShader* pVertexShader = CreateShader(pDevice,
		MakeShaderCode(g_Fullscreen_vs_DXIL, g_Fullscreen_vs_SPIRV), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
	SDL_GPUShader* pFragmentShader = CreateShader(pDevice,
		MakeShaderCode(g_DecodeRGB16_ps_DXIL, g_DecodeRGB16_ps_SPIRV), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

	if (pVertexShader && pFragmentShader)
	{
		SDL_GPUColorTargetDescription colorTarget {};
		colorTarget.format = SDL_GetGPUSwapchainTextureFormat(pDevice, pWindow);

		SDL_GPUGraphicsPipelineCreateInfo info {};
		info.vertex_shader = pVertexShader;
		info.fragment_shader = pFragmentShader;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.target_info.color_target_descriptions = &colorTarget;
		info.target_info.num_color_targets = 1;

		Pipeline = SDL_CreateGPUGraphicsPipeline(pDevice, &info);
	}

	// The pipeline keeps what it needs from the shaders
	if (pVertexShader)
		SDL_ReleaseGPUShader(pDevice, pVertexShader);

	if (pFragmentShader)
		SDL_ReleaseGPUShader(pDevice, pFragmentShader);

	if (!Pipeline)
		return false;

	// Integer textures are only fetched, but SDL_GPU binds every sampled texture with a sampler
	SDL_GPUSamplerCreateInfo samplerInfo {};
	samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
	samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
	samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

	Sampler = SDL_CreateGPUSampler(pDevice, &samplerInfo);
	return Sampler != nullptr;
}

void PresentPass::Release(SDL_GPUDevice* pDevice)
{
	if (Sampler)
	{
		SDL_ReleaseGPUSampler(pDevice, Sampler);
		Sampler = nullptr;
	}

	if (Pipeline)
	{
		SDL_ReleaseGPUGraphicsPipeline(pDevice, Pipeline);
		Pipeline = nullptr;
	}
}

void PresentPass::Draw(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pTarget, unsigned int targetWidth,
	unsigned int targetHeight, SDL_GPUTexture* pSource, int sourceWidth, int sourceHeight)
{
	SDL_GPUColorTargetInfo target {};
	target.texture = pTarget;
	target.clear_color = ClearColor;
	target.load_op = SDL_GPU_LOADOP_CLEAR;
	target.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass* pRenderPass = SDL_BeginGPURenderPass(pCommandBuffer, &target, 1, nullptr);

	if (pSource && sourceWidth > 0 && sourceHeight > 0)
	{
		const SDL_GPUViewport viewport = GetLetterboxViewport(targetWidth, targetHeight, sourceWidth, sourceHeight);
		const DecodeUniforms uniforms = GetDecodeUniforms();

		SDL_GPUTextureSamplerBinding binding {};
		binding.texture = pSource;
		binding.sampler = Sampler;

		SDL_BindGPUGraphicsPipeline(pRenderPass, Pipeline);
		SDL_SetGPUViewport(pRenderPass, &viewport);
		SDL_BindGPUFragmentSamplers(pRenderPass, 0, &binding, 1);
		SDL_PushGPUFragmentUniformData(pCommandBuffer, 0, &uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(pRenderPass, FullscreenTriangleVertexCount, 1, 0, 0);
	}

	SDL_EndGPURenderPass(pRenderPass);
}
