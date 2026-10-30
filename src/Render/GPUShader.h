#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstddef>

// Compiled shader blobs from the PhobosShader build step, for either backend.
namespace GPUShader
{
	struct Code
	{
		const unsigned char* pDXIL;
		size_t DXILSize;
		const unsigned char* pSPIRV;
		size_t SPIRVSize;
	};

	template <size_t DXILSize, size_t SPIRVSize>
	constexpr Code Make(const unsigned char (&dxil)[DXILSize], const unsigned char (&spirv)[SPIRVSize])
	{
		return { dxil, DXILSize, spirv, SPIRVSize };
	}

	inline SDL_GPUShader* Create(SDL_GPUDevice* pDevice, const Code& code, SDL_GPUShaderStage stage,
		Uint32 samplerCount, Uint32 uniformBufferCount, Uint32 storageBufferCount = 0)
	{
		SDL_GPUShaderCreateInfo info {};
		info.entrypoint = "main";
		info.stage = stage;
		info.num_samplers = samplerCount;
		info.num_uniform_buffers = uniformBufferCount;
		info.num_storage_buffers = storageBufferCount;

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
}
