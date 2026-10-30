#include "ShapeFrameSource.h"

// The single definition of the state ShapeFrameSource.h declares.
namespace ShapeFrameSource
{
	FrameCache::Cache Frames;
	std::unordered_map<std::uintptr_t, std::uintptr_t> Aliases;
}
