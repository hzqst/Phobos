#pragma once
#include "FrameCache.h"
#include "PaletteCache.h"
#include <memory>
#include <span>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUCopyPass;
struct SDL_GPUTexture;

class FrameAtlas
{
public:
	struct Options
	{
		int PageSize = 2048;
		unsigned MaxPages = 16, MaxPalettes = 256;
		std::uint64_t PageBudget = 64 * 1024 * 1024;
		unsigned StagingBudget = 16 * 1024 * 1024;
		bool ForceR32 = false; // Test harness; not an INI setting.
	};
	struct Request { FrameCache::FrameID Frame; PaletteCache::Handle Palette; };
	struct View { SDL_GPUTexture* Texture = nullptr; SDL_GPUTexture* Palette = nullptr; int X = 0, Y = 0; };
	struct Statistics
	{
		std::uint64_t Submissions = 0, FrameUploads = 0, PaletteUploads = 0;
		std::uint64_t IndexBytes = 0, PaletteBytes = 0, TransferBytes = 0, Evictions = 0, BudgetMisses = 0, Busy = 0;
		std::uint64_t PageBytes = 0, OccupiedPixels = 0;
		unsigned Pages = 0, RetiringPages = 0, Palettes = 0, InFlight = 0, BytesPerPixel = 0;
	};
	enum class BeginResult { Ready, Busy, Failed };
	FrameAtlas();
	~FrameAtlas();
	bool Create(SDL_GPUDevice* device, Options options);
	void Release(); // Caller has waited for GPU idle (or is tearing down a failed device).
	BeginResult Begin(const FrameCache::Cache& cache, std::span<const Request> requests);
	View Resolve(const Request& request) const;
	bool HasUploads() const;
	void EncodeUploads(SDL_GPUCopyPass* pass);
	bool Submit(SDL_GPUCommandBuffer* commands); // Consumes command buffer, including on failure.
	void CancelBatch(); // Caller separately cancels an unsubmitted command buffer, if any.
	void Poll();
	Statistics GetStats() const;
private:
	struct Impl;
	std::unique_ptr<Impl> State;
};
