#pragma once

#include <Windows.h>
#include <Utilities/Debug.h>
#include <array>
#include <cstdint>

// Opt-in diagnostic. Wall time and CPU cycles are separate; cycles are NOT milliseconds.
namespace MainThreadProfile
{
	enum class Phase { MainLoop, NativeLayers, GpuPresent, GpuPrepare, TerrainCapture, ObjectCapture, CompositeCopy, SidebarCapture, Count };
	inline constexpr std::array<const char*, size_t(Phase::Count)> Names {
		"main_loop_other", "native_layers", "gpu_present", "gpu_prepare", "terrain_capture", "object_capture", "composite_copy", "sidebar_capture" };
	struct Clock { std::uint64_t Ticks = 0, Cycles = 0; };
	inline Clock Now()
	{
		LARGE_INTEGER ticks {}; ULONG64 cycles = 0;
		QueryPerformanceCounter(&ticks); QueryThreadCycleTime(GetCurrentThread(), &cycles);
		return {std::uint64_t(ticks.QuadPart), cycles};
	}
	inline std::uint64_t CpuTime()
	{
		FILETIME created {}, exited {}, kernel {}, user {};
		GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
		return (std::uint64_t(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime
			+ (std::uint64_t(user.dwHighDateTime) << 32) + user.dwLowDateTime;
	}
	struct Profile
	{
		struct Entry { Phase Kind {}; Clock Start {}, Children {}; };
		struct Metric { std::uint64_t Calls = 0, Ticks = 0, Cycles = 0, InclusiveTicks = 0; };
		bool Enabled = false;
		DWORD Owner = GetCurrentThreadId();
		unsigned Depth = 0, Frames = 0, Windows = 0, Errors = 0;
		std::array<Entry, 32> Stack {};
		std::array<Metric, size_t(Phase::Count)> Metrics {};
		Clock Start {};
		std::uint64_t CpuStart = 0, Frequency = 1;
		Profile()
		{
			char value[2] {};
			Enabled = GetEnvironmentVariableA("PHOBOS_MAIN_THREAD_PROFILE", value, sizeof(value)) == 1 && value[0] == '1';
			LARGE_INTEGER frequency {}; QueryPerformanceFrequency(&frequency); Frequency = frequency.QuadPart;
			Start = Now(); CpuStart = CpuTime();
		}
		bool Active() const { return Enabled && Windows < 64 && Owner == GetCurrentThreadId(); }
		void Enter(Phase kind)
		{
			if (!Active()) return;
			if (Depth == Stack.size()) { ++Errors; return; }
			Stack[Depth++] = {kind, Now(), {}};
		}
		void Exit(Phase kind)
		{
			if (!Active()) return;
			if (!Depth || Stack[Depth - 1].Kind != kind) { ++Errors; return; }
			const auto end = Now(); const auto entry = Stack[--Depth];
			const Clock elapsed {end.Ticks - entry.Start.Ticks, end.Cycles - entry.Start.Cycles};
			auto& metric = Metrics[size_t(kind)]; ++metric.Calls;
			metric.Ticks += elapsed.Ticks - entry.Children.Ticks;
			metric.Cycles += elapsed.Cycles - entry.Children.Cycles;
			metric.InclusiveTicks += elapsed.Ticks;
			if (Depth) { Stack[Depth - 1].Children.Ticks += elapsed.Ticks; Stack[Depth - 1].Children.Cycles += elapsed.Cycles; }
		}
		void Frame(bool complete)
		{
			if (!Active() || !complete || ++Frames < 120) return;
			if (Depth) { ++Errors; return; }
			const auto end = Now(); const auto cpu = CpuTime();
			const double us = 1000000.0 / double(Frequency);
			Debug::Log("[MainThreadProfile] window=%u frames=%u wallUs=%.3f cpuUs=%.3f cycles=%llu errors=%u tid=%lu beginSeconds=%.9f endSeconds=%.9f\n",
				++Windows, Frames, (end.Ticks - Start.Ticks) * us, (cpu - CpuStart) / 10.0, end.Cycles - Start.Cycles, Errors, Owner,
				double(Start.Ticks)/Frequency, double(end.Ticks)/Frequency);
			std::uint64_t accountedTicks = 0, accountedCycles = 0;
			for (size_t i = 0; i < Metrics.size(); ++i)
			{
				const auto& metric = Metrics[i]; accountedTicks += metric.Ticks; accountedCycles += metric.Cycles;
				Debug::Log("[MainThreadProfile] window=%u phase=%s calls=%llu exclusiveUs=%.3f inclusiveUs=%.3f cycles=%llu\n",
					Windows, Names[i], metric.Calls, metric.Ticks * us, metric.InclusiveTicks * us, metric.Cycles);
			}
			Debug::Log("[MainThreadProfile] window=%u phase=outside_scopes calls=0 exclusiveUs=%.3f inclusiveUs=0 cycles=%llu\n",
				Windows, (end.Ticks - Start.Ticks - accountedTicks) * us, end.Cycles - Start.Cycles - accountedCycles);
			Metrics = {}; Frames = 0; Start = Now(); CpuStart = CpuTime();
		}
	};
	inline Profile& Get() { static Profile profile; return profile; }
	inline void Enter(Phase phase) { Get().Enter(phase); }
	inline void Exit(Phase phase) { Get().Exit(phase); }
	struct Scope
	{
		Phase Kind;
		explicit Scope(Phase phase) : Kind(phase) { Enter(Kind); }
		~Scope() { Exit(Kind); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};
}
