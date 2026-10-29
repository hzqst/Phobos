#include "AutomationDispatcher.h"

namespace Automation
{
	AutomationDispatcher::AutomationDispatcher(std::string instance) : Instance(std::move(instance)) { }
	Json AutomationDispatcher::Failure(const char* message) { return {{"automation_error", message}}; }
	AutomationDispatcher::Ticket AutomationDispatcher::Submit(std::string operation, Json args, std::chrono::milliseconds timeout,
		std::shared_ptr<std::atomic<bool>> disconnected)
	{
		std::lock_guard lock(Mutex);
		if (Stopped || Outstanding >= 32 || timeout.count() < 1 || timeout > std::chrono::seconds(30)) return {};
		auto ticket = std::make_shared<Command>();
		ticket->ID = ++NextID;
		ticket->Instance = Instance;
		ticket->Operation = std::move(operation);
		ticket->Arguments = std::move(args);
		ticket->Disconnected = std::move(disconnected);
		ticket->Deadline = std::chrono::steady_clock::now() + timeout;
		Queue.push_back(ticket);
		++Outstanding;
		return ticket;
	}
	void AutomationDispatcher::Finish(const Ticket& ticket, State state, Json result)
	{
		std::lock_guard lock(ticket->Mutex);
		if (ticket->Status == State::Cancelled) return;
		ticket->Status = state;
		ticket->Result = std::move(result);
		ticket->Complete.notify_all();
	}
	Json AutomationDispatcher::Wait(const Ticket& ticket)
	{
		if (!ticket) return Failure("dispatcher_unavailable_or_full");
		std::unique_lock lock(ticket->Mutex);
		while (ticket->Status == State::Queued || ticket->Status == State::Executing)
		{
			if (std::chrono::steady_clock::now() >= ticket->Deadline || (ticket->Disconnected && *ticket->Disconnected))
			{
				ticket->Cancel = true;
				return Failure(ticket->Disconnected && *ticket->Disconnected ? "client_disconnected" : "main_thread_timeout");
			}
			ticket->Complete.wait_for(lock, std::chrono::milliseconds(10));
		}
		return ticket->Result;
	}
	void AutomationDispatcher::Pump(const Handler& handler, const Handler& cancel)
	{
		std::deque<Ticket> ready;
		{
			std::lock_guard lock(Mutex);
			ready.swap(Queue);
		}
		for (const auto& ticket : ready)
		{
			bool cancelled = false;
			{
				std::lock_guard lock(ticket->Mutex);
				if (ticket->Cancel || (ticket->Disconnected && *ticket->Disconnected) || std::chrono::steady_clock::now() >= ticket->Deadline)
				{
					ticket->Cancel = true;
					cancelled = true;
				}
				else ticket->Status = State::Executing;
			}
			// No dispatcher or command lock is held while accessing the game.
			if (cancelled)
			{
				if (cancel) cancel(*ticket);
				Finish(ticket, State::Cancelled, Failure("request_cancelled"));
			}
			else
			{
				auto result = handler(*ticket);
				if (result.is_null())
				{
					std::lock_guard lock(Mutex);
					Queue.push_back(ticket);
					continue;
				}
				const bool error = result.contains("automation_error");
				Finish(ticket, error ? State::Failed : State::Completed, std::move(result));
			}
			std::lock_guard lock(Mutex);
			--Outstanding;
		}
	}
	void AutomationDispatcher::Stop()
	{
		std::deque<Ticket> cancelled;
		{
			std::lock_guard lock(Mutex);
			Stopped = true;
			cancelled.swap(Queue);
			Outstanding -= cancelled.size();
		}
		for (const auto& ticket : cancelled)
		{
			ticket->Cancel = true;
			Finish(ticket, State::Cancelled, Failure("game_shutdown"));
		}
	}
}
