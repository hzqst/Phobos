#include "TerrainSnapshot.h"

// The single definition of the state TerrainSnapshot.h declares.
namespace TerrainSnapshot
{
	FrameCache::Cache Frames({64 * 1024 * 1024, 16384, 8192});
	Frame Current;
	TerrainBatch::ColorTables Tables(TableRows);
	std::unordered_map<std::uintptr_t, std::uint64_t> PieceHashes;
	int TemplateState = 0;
	bool Trace = false;
	unsigned TraceCount = 0;
	Perf Stats;
	std::uint64_t LastTraceHash = 0;
	Point LastTactical {INT64_MIN, INT64_MIN};
}
