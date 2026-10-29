#pragma once
#include "Json.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace Automation
{
	// The network owns no engine references. Commands and results own all data.
	class AutomationDispatcher
	{
	public:
		enum class State { Queued, Executing, Completed, Failed, Cancelled };
		struct Command
		{
			uint64_t ID;
			std::string Instance, Operation;
			Json Arguments;
			Json Progress; // Accessed only by the main-thread handler.
			std::chrono::steady_clock::time_point Deadline;
			std::mutex Mutex;
			std::condition_variable Complete;
			State Status = State::Queued;
			Json Result;
			std::atomic<bool> Cancel {false};
			std::shared_ptr<std::atomic<bool>> Disconnected;
		};
		using Ticket = std::shared_ptr<Command>;
		using Handler = std::function<Json(Command&)>;
		explicit AutomationDispatcher(std::string instance);
		Ticket Submit(std::string operation, Json arguments, std::chrono::milliseconds timeout,
			std::shared_ptr<std::atomic<bool>> disconnected = {});
		Json Wait(const Ticket& ticket);
		// Only the game thread calls Pump. Handler must not block or retain engine objects.
		// A null handler result keeps the command executing until a later pump.
		void Pump(const Handler& handler, const Handler& cancel = {});
		void Stop();
		static Json Failure(const char* message);
	private:
		std::string Instance;
		std::mutex Mutex;
		std::deque<Ticket> Queue;
		uint64_t NextID = 0;
		size_t Outstanding = 0;
		bool Stopped = false;
		static void Finish(const Ticket& ticket, State state, Json result);
	};
}
