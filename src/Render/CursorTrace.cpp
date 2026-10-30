#include "CursorTrace.h"

// The single definition of the state CursorTrace.h declares.
namespace CursorTrace
{
	Event Events[Capacity] {};
	SRWLOCK BufferLock = SRWLOCK_INIT;
	std::atomic<bool> Recording { false };
	std::atomic<unsigned> Dropped { 0 };
	std::atomic<DWORD> NextFrame { 0 }, Mode { 0 }, Session { 0 };
	thread_local DWORD Frame = 0, Depth = 0, ThreadSession = 0;
	size_t Count = 0;
	bool Armed = false, KeyDown = false, Pending = false;
	DWORD Started = 0, CaptureNumber = 0;
	LARGE_INTEGER Frequency {};
}
