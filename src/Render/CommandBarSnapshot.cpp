#include "CommandBarSnapshot.h"

// The single definition of the state CommandBarSnapshot.h declares.
namespace CommandBarSnapshot
{
	Frame Current;
	unsigned TraceCount = 0;
	std::uint64_t LastTraceHash = 0;
}
