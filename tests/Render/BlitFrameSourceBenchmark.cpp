// Optimized x86 microbenchmark of the existing acquisition paths, not game FPS.
#include <Render/BlitFrameSource.h>
#include <Render/VoxelIdentity.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace
{
	constexpr int PoseCount = 32, Samples = 120, Pitch = 256;
	volatile std::uint64_t Sink = 0;
	using Clock = std::chrono::steady_clock;

	void Run(int size, int draws, int path)
	{
		const bool encoded = path == 1, identity = path == 2;
		BlitFrameSource::Clear();
		VoxelIdentity::Validator identities;
		std::array<VoxelIdentity::Key, PoseCount> keys {};
		std::vector<std::vector<std::uint8_t>> sources;
		for (int pose = 0; pose < PoseCount; ++pose)
		{
			std::vector<std::uint8_t> bytes(size_t(encoded ? size + 2 : Pitch) * size);
			for (int y = 0; y < size; ++y)
			{
				const auto offset = size_t(y) * (encoded ? size + 2 : Pitch);
				if (encoded) { bytes[offset] = std::uint8_t(size + 2); bytes[offset + 1] = 0; }
				for (int x = 0; x < size; ++x)
					bytes[offset + (encoded ? 2 : 0) + x] = std::uint8_t(1 + (pose * 17 + x * 3 + y * 7) % 255);
			}
			sources.push_back(std::move(bytes));
		}
		const auto acquire = [&](int pose)
		{
			const auto& source = sources[size_t(pose)];
			return encoded ? BlitFrameSource::AcquireEncoded(source.data(), source.size(), size, size)
				: BlitFrameSource::AcquirePixels(source.data(), Pitch, size, size);
		};
		BlitFrameSource::BeginCapture();
		for (int pose = 0; pose < PoseCount; ++pose)
		{
			const auto id = acquire(pose);
			if (!id) std::exit(2);
			auto& key = keys[pose];
			key.Epoch = BlitFrameSource::Epoch; key.Count = 1;
			key.Parts[0].Payload = std::uintptr_t(sources[pose].data()); key.Region = {0, 0, size, size};
			if (identity) { identities.Observe(key, id, BlitFrameSource::Frames); identities.Observe(key, id, BlitFrameSource::Frames); }
		}
		const auto decoded = BlitFrameSource::Frames.GetStats().Decoded;
		std::vector<double> samples;
		for (int frame = 0; frame < Samples; ++frame)
		{
			BlitFrameSource::BeginCapture();
			std::uint64_t sum = 0;
			const auto start = Clock::now();
			for (int draw = 0; draw < draws; ++draw)
			{
				const auto pose = (draw + frame) % PoseCount;
				const auto id = identity ? identities.Lookup(keys[pose], BlitFrameSource::Frames) : acquire(pose);
				if (!id) std::exit(3);
				if (identity && !BlitFrameSource::Touch(id)) std::exit(5);
				sum += id.Value;
			}
			samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
			Sink = sum;
		}
		if (BlitFrameSource::Frames.GetStats().Decoded != decoded) std::exit(4);
		const auto average = std::accumulate(samples.begin(), samples.end(), 0.0) / Samples;
		std::sort(samples.begin(), samples.end());
		std::printf("%s,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%llu\n", identity ? "verified_identity_hit" : encoded ? "rle_address_hit" : "plain_content_hit",
			size, draws, Samples, average, samples[(Samples * 95 + 99) / 100 - 1],
			samples[(Samples * 99 + 99) / 100 - 1], samples.back(),
			encoded || identity ? 0ULL : static_cast<unsigned long long>(size) * size * draws);
	}
}

int main()
{
	static_assert(sizeof(void*) == 4, "Match the game's x86 target");
	std::puts("path,size,draws,samples,avg_us,p95_us,p99_us,max_us,copied_and_hashed_bytes_per_sample");
	for (const auto size : {32, 64, 128})
		for (const auto draws : {100, 500, 1000})
			for (const auto path : {0, 1, 2}) Run(size, draws, path);
}
