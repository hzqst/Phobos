#include "SidebarSnapshot.h"

// The single definition of the state SidebarSnapshot.h declares.
namespace SidebarSnapshot
{
	Frame Current;
	unsigned Generation = 0;
	bool Trace = false;
	unsigned TraceCount = 0;
	std::uint64_t LastTraceHash = 0;
	bool LowerButtonsReady = false, RadarButtonsReady = false;
	unsigned ButtonTraceCount = 0;
	std::array<std::uint64_t, ButtonCount> ButtonTraceHashes {};
	std::uint64_t CaptureCount = 0, LastDecoded = 0;
	unsigned CacheTraceCount = 0;
	PaletteCache::Cache Colors;
	std::vector<std::pair<const ConvertClass*, PaletteCache::Handle>> CapturedColors;
	unsigned CameoTraceCount = 0;
	std::uint64_t CameoTraceHash = 0;
	unsigned CameoItemTraceCount = 0;
	std::array<std::uint64_t, ShapeImage::MaxCameoSlots> CameoItemHashes {};
	std::array<std::unordered_map<std::uint64_t, NativeCameoState>, 4> NativeCameoStates;
	unsigned EffectTraceCount = 0;
	std::array<std::uint64_t, ShapeImage::MaxCameoSlots> EffectHashes {};
	unsigned PowerTraceCount = 0;
	std::uint64_t PowerTraceHash = 0;
	std::vector<int> PowerSequence;
	unsigned RadarTraceCount = 0;
	std::uint64_t RadarTraceHash = 0;
}
