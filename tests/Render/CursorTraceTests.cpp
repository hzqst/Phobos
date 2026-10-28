#include <Render/CursorTrace.h>

#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

// The standalone harness never loads the game or calls engine functions.
void Debug::Log(const char*, ...) {}

namespace
{
	REGISTERS MakeRegisters(DWORD origin)
	{
		REGISTERS registers {};
		// Syringe supplies origin as the first DWORD; the public API is read-only.
		std::memcpy(&registers, &origin, sizeof(origin));
		registers.EAX(0x12345601u);
		registers.EFLAGS(0x246u);
		return registers;
	}

	void Start()
	{
		CursorTrace::Count = 0;
		CursorTrace::Dropped = 0;
		++CursorTrace::Session;
		CursorTrace::Recording = true;
	}

	void Observe(DWORD origin)
	{
		auto registers = MakeRegisters(origin);
		const auto before = registers;
		SetLastError(ERROR_ACCESS_DENIED);
		CursorTrace::Record(&registers);
		assert(ERROR_ACCESS_DENIED == GetLastError());
		assert(0 == std::memcmp(&before, &registers, sizeof(registers)));
	}
}

int main()
{
	Start();
	Observe(0x4F4583);
	Observe(0x4F4780);
	Observe(0x4F4B7E);
	Observe(0x4F4BA4);
	Observe(0x4F459D);
	assert(5 == CursorTrace::Count);
	assert(0 == CursorTrace::Depth);
	assert(1 == CursorTrace::Events[0].depth);
	assert(2 == CursorTrace::Events[2].depth);
	assert(CursorTrace::Events[0].frame == CursorTrace::Events[4].frame);
	assert(0x12345601u == CursorTrace::Events[2].eax);

	// A rejected event must not leak a nesting level or change the machine state.
	AcquireSRWLockExclusive(&CursorTrace::BufferLock);
	std::thread contended([] { Observe(0x4F4780); Observe(0x4F4BA4); assert(0 == CursorTrace::Depth); });
	contended.join();
	ReleaseSRWLockExclusive(&CursorTrace::BufferLock);
	assert(2 == CursorTrace::Dropped.load());
	assert(5 == CursorTrace::Count);

	// Capture the copy arguments before the original LEA/PUSH execute.
	Start();
	RectangleStruct source { -3, 4, 25, 30 }, destination { 7, 8, 25, 30 };
	alignas(DWORD) unsigned char stack[128] {};
	auto* sourcePointer = &source;
	std::memcpy(stack, &sourcePointer, sizeof(sourcePointer));
	std::memcpy(stack + 0x48, &destination, sizeof(destination));
	stack[8] = 1;
	auto copy = MakeRegisters(0x4F4B75);
	copy.ESP(reinterpret_cast<DWORD>(stack));
	copy.ECX(0x1234u);
	copy.ESI(0x5678u);
	CursorTrace::Record(&copy);
	const auto& event = CursorTrace::Events[0];
	assert(-3 == event.sourceRect.X && 7 == event.destinationRect.X);
	assert(0x1234u == event.target && 0x5678u == event.source && 1 == event.flags);

	// Multiple producers may lose observations, but cannot overwrite each other's slots.
	Start();
	std::vector<std::thread> producers;
	for (int i = 0; i < 4; ++i)
		producers.emplace_back([] { for (int j = 0; j < 10000; ++j) Observe(0x4F4B7E); });
	for (auto& producer : producers)
		producer.join();
	assert(CursorTrace::Count <= CursorTrace::Capacity);
	for (size_t i = 0; i < CursorTrace::Count; ++i)
	{
		assert(0x4F4B7E == CursorTrace::Events[i].address);
		assert(0x12345601u == CursorTrace::Events[i].eax);
		assert(0 != CursorTrace::Events[i].thread);
	}
	while (CursorTrace::Recording.load())
		Observe(0x4F4B7E);
	assert(CursorTrace::Capacity == CursorTrace::Count);
	Observe(0x4F4B7E);
	assert(CursorTrace::Capacity == CursorTrace::Count);
	puts("CursorTrace: register preservation, nesting, contention, copy arguments and capacity passed.");
}
