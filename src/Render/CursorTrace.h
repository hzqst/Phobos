#pragma once

#include <Syringe.h>
#include <GeneralStructures.h>
#include <WWMouseClass.h>
#include <Utilities/Debug.h>

#include <atomic>
#include <cstdio>

// Diagnostic only. No engine locks, surface methods, resource ownership or GPU calls.
// Header-only so the probe can share the existing Render translation units.
namespace CursorTrace
{
	constexpr size_t Capacity = 16384;
	constexpr DWORD TraceDurationMS = 5000;
	constexpr int ToggleKey = VK_F8;

	struct Event
	{
		LONGLONG ticks;
		DWORD thread, address, frame, depth, mode;
		DWORD eax, ebx, ecx, edx, esi, edi, esp, ebp;
		DWORD object, target, source, flags;
		RectangleStruct destinationRect, sourceRect;
		DWORD background[3];
	};

	inline Event Events[Capacity] {};
	inline SRWLOCK BufferLock = SRWLOCK_INIT;
	inline std::atomic<bool> Recording { false };
	inline std::atomic<unsigned> Dropped { 0 };
	inline std::atomic<DWORD> NextFrame { 0 }, Mode { 0 }, Session { 0 };
	inline thread_local DWORD Frame = 0, Depth = 0, ThreadSession = 0;
	inline size_t Count = 0;
	inline bool Armed = false, KeyDown = false, Pending = false;
	inline DWORD Started = 0, CaptureNumber = 0;
	inline LARGE_INTEGER Frequency {};

	inline void Initialize()
	{
		// Opt in for a diagnostic session; no INI or normal gameplay bindings change.
		char value[2] {};
		Armed = GetEnvironmentVariableA("PHOBOS_CURSOR_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
		QueryPerformanceFrequency(&Frequency);
	}

	inline void Record(REGISTERS* r)
	{
		if (!Recording.load(std::memory_order_relaxed))
			return;

		const DWORD lastError = GetLastError();
		if (ThreadSession != Session.load())
		{
			ThreadSession = Session.load();
			Frame = Depth = 0;
		}
		const DWORD address = r->Origin();
		if (address == 0x4F4583 || address == 0x4F4780)
		{
			if (!Depth)
				Frame = ++NextFrame;
			++Depth;
		}
		const DWORD eventDepth = Depth;
		const DWORD eventFrame = Depth ? Frame : 0;
		if ((address == 0x4F459D || address == 0x4F4BA4) && Depth)
			--Depth;
		if (address == 0x560BF0)
			++Mode;

		// A contended probe drops its event instead of waiting while the engine holds MouseMutex.
		if (!TryAcquireSRWLockExclusive(&BufferLock))
		{
			++Dropped;
			SetLastError(lastError);
			return;
		}

		if (!Recording.load(std::memory_order_relaxed) || Count == Capacity)
		{
			Recording.store(false, std::memory_order_relaxed);
			ReleaseSRWLockExclusive(&BufferLock);
			SetLastError(lastError);
			return;
		}

		auto& e = Events[Count++];
		e = {};
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		e.ticks = now.QuadPart;
		e.thread = GetCurrentThreadId();
		e.address = address;
		e.frame = eventFrame;
		e.depth = eventDepth;
		e.mode = Mode.load();
		e.eax = r->EAX(); e.ebx = r->EBX(); e.ecx = r->ECX(); e.edx = r->EDX();
		e.esi = r->ESI(); e.edi = r->EDI(); e.esp = r->ESP(); e.ebp = r->EBP();

		// Copy only arguments that the original instruction stream is about to consume.
		// These are requested rectangles, NOT proof of the effective clipped region.
		if (address == 0x4F4B75)
		{
			e.target = r->ECX(); e.source = r->ESI();
			e.destinationRect = r->Stack<RectangleStruct>(0x48);
			e.sourceRect = *r->Stack<RectangleStruct*>(0);
			e.flags = r->Stack8(8);
		}
		else if (address == 0x7B9022)
		{
			e.object = r->ESI(); e.target = r->ECX(); e.source = r->Stack32(0);
			e.destinationRect = r->Stack<RectangleStruct>(0x30);
			e.sourceRect = *r->Stack<RectangleStruct*>(4);
		}
		else if (address == 0x7B91FF || address == 0x7B928A)
		{
			auto* mouse = r->EDI<WWMouseClass*>();
			e.object = r->EDI(); e.source = r->EBP();
			e.background[0] = reinterpret_cast<DWORD>(mouse->field_44);
			e.background[1] = reinterpret_cast<DWORD>(mouse->field_58);
			e.background[2] = reinterpret_cast<DWORD>(mouse->field_6C);
			if (address == 0x7B91FF)
			{
				e.target = r->EBX();
				e.sourceRect = *r->ESI<RectangleStruct*>();
			}
			else
				e.target = e.background[0];
		}
		else if (address == 0x7B90C0 || address == 0x7B93F0)
		{
			e.object = r->ECX(); e.target = r->Stack32(4);
			e.flags = r->Stack32(8);
		}
		else if (address == 0x4F4780)
		{
			e.source = r->EDX(); e.flags = r->CL();
		}

		ReleaseSRWLockExclusive(&BufferLock);
		SetLastError(lastError);
	}

	inline void Flush()
	{
		// Called only from the main-thread timer or shutdown, never from a mouse probe.
		Recording.store(false);
		AcquireSRWLockExclusive(&BufferLock);
		ReleaseSRWLockExclusive(&BufferLock);
		if (!Pending)
			return;

		char path[MAX_PATH];
		sprintf_s(path, "CursorTrace-%lu-%lu.csv", GetCurrentProcessId(), ++CaptureNumber);
		FILE* file = nullptr;
		if (fopen_s(&file, path, "w") == 0)
		{
			fprintf(file, "# cursor-trace-v1,qpc_frequency=%lld,dropped=%u,count=%zu,capacity=%zu\n",
				Frequency.QuadPart, Dropped.load(), Count, Capacity);
			fprintf(file, "# rects=requested; background pointers are observations, not validity/generation proof; eax low byte is copy result only at 4f4b7e/7b91ff/7b928a\n");
			fprintf(file, "ticks,thread,address,frame,depth,mode,eax,ebx,ecx,edx,esi,edi,esp,ebp,object,target,source,flags,dx,dy,dw,dh,sx,sy,sw,sh,b0,b1,b2\n");
			for (size_t i = 0; i < Count; ++i)
			{
				const auto& e = Events[i];
				fprintf(file, "%lld,%lu,%08lx,%lu,%lu,%lu,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%d,%d,%d,%d,%d,%d,%d,%d,%08lx,%08lx,%08lx\n",
					e.ticks, e.thread, e.address, e.frame, e.depth, e.mode,
					e.eax, e.ebx, e.ecx, e.edx, e.esi, e.edi, e.esp, e.ebp,
					e.object, e.target, e.source, e.flags,
					e.destinationRect.X, e.destinationRect.Y, e.destinationRect.Width, e.destinationRect.Height,
					e.sourceRect.X, e.sourceRect.Y, e.sourceRect.Width, e.sourceRect.Height,
					e.background[0], e.background[1], e.background[2]);
			}
			fclose(file);
			Debug::Log("[CursorTrace] Wrote %zu events to %s (%u dropped).\n", Count, path, Dropped.load());
		}
		else
			Debug::Log("[CursorTrace] Could not write %s.\n", path);
		Pending = false;
	}

	inline void Poll()
	{
		if (!Armed)
			return;
		const bool down = (GetAsyncKeyState(ToggleKey) & 0x8000) != 0;
		if (Pending && (!Recording.load() || GetTickCount() - Started >= TraceDurationMS || (down && !KeyDown)))
			Flush();
		else if (down && !KeyDown && !Pending)
		{
			AcquireSRWLockExclusive(&BufferLock);
			Count = 0;
			Dropped = 0;
			++Session;
			// Reset the main thread's partial bracket when starting a new bounded capture.
			Frame = Depth = 0;
			Started = GetTickCount();
			Pending = true;
			Recording.store(true);
			ReleaseSRWLockExclusive(&BufferLock);
		}
		KeyDown = down;
	}
}
