#include "ABufferSnapshot.h"

// The single definition of the state ABufferSnapshot.h declares.
namespace ABufferSnapshot
{
	FrameCache::Cache Frames({4 * 1024 * 1024, 1024, 16});
	std::unordered_map<std::uintptr_t, std::uint64_t> HeaderHashes;
	Snapshot Current;
}
