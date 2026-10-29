#include "AutomationFrameControl.h"

namespace Automation
{
	bool IsFrameOperation(const std::string& name)
	{
		return name == "game_pause" || name == "game_step" || name == "game_resume";
	}
	Json AutomationFrameControl::Result(const AutomationDispatcher::Command& command, FrameHost& host, const char* error)
	{
		const uint64_t start = command.Progress.value("start", host.CompletedFrames());
		Json result = {{"start_frame", start}, {"end_frame", host.CompletedFrames()},
			{"actual_frames", host.CompletedFrames() - start}, {"completed", error == nullptr},
			{"paused", host.Paused()}, {"frame_step_pending", host.Pending()}, {"scene_id", host.Scene()}};
		if (error) result["automation_error"] = error;
		return result;
	}
	Json AutomationFrameControl::Tick(AutomationDispatcher::Command& command, FrameHost& host, uint64_t now)
	{
		const auto& operation = command.Operation;
		if (!command.Progress.is_null() && ActiveID != command.ID) return Result(command, host, "frame_control_superseded");
		if (ActiveID && ActiveID != command.ID && operation != "game_resume")
			return AutomationDispatcher::Failure("frame_control_busy");
		if (command.Progress.is_null())
		{
			if (!host.Available()) return AutomationDispatcher::Failure("frame_control_requires_active_singleplayer_without_dialog");
			if (operation == "game_step" && (!host.Paused() || host.Pending()))
				return AutomationDispatcher::Failure("step_requires_paused_idle_engine");
			if (operation == "game_pause" && host.Pending()) return AutomationDispatcher::Failure("step_already_pending");
			command.Progress = {{"start", host.CompletedFrames()}, {"scene", host.Scene()},
				{"barrier", host.PauseBarriers()}, {"deadline", now + command.Arguments.value("timeout_ms", 5000)},
				{"requested", command.Arguments.value("frames", 0)}, {"previous_paused", host.Paused()}};
			ActiveID = command.ID;
			StepOwned = operation == "game_step";
			if (operation == "game_pause")
			{
				PauseOwned |= !host.Paused();
				host.SetPaused(true);
			}
			else if (operation == "game_step") host.SetPending(command.Arguments["frames"].get<size_t>());
			else { host.SetPending(0); host.SetPaused(false); PauseOwned = false; }
			return nullptr;
		}
		const uint64_t actual = host.CompletedFrames() - command.Progress["start"].get<uint64_t>();
		const char* error = nullptr;
		bool complete = false;
		if (host.Scene() != command.Progress["scene"].get<uint64_t>()) error = "scene_changed";
		else if (!host.Available()) error = "engine_no_longer_available";
		else if (operation == "game_pause") complete = host.Paused() && !host.Pending() && host.PauseBarriers() > command.Progress["barrier"].get<uint64_t>();
		else if (operation == "game_resume") complete = !host.Paused() && actual > 0;
		else
		{
			const uint64_t requested = command.Progress["requested"];
			if (!host.Paused()) error = "step_interrupted";
			else if (actual > requested) error = "step_overrun";
			else complete = actual == requested && !host.Pending() && host.PauseBarriers() > command.Progress["barrier"].get<uint64_t>();
		}
		if (!complete && !error && now >= command.Progress["deadline"].get<uint64_t>()) error = "frame_control_timeout";
		if (!complete && !error) return nullptr;
		if (error)
		{
			host.SetPending(0);
			if (operation == "game_pause" || host.Scene() != command.Progress["scene"].get<uint64_t>())
			{
				if (PauseOwned) host.SetPaused(false);
				PauseOwned = false;
			}
		}
		ActiveID = 0; StepOwned = false;
		return Result(command, host, error);
	}
	Json AutomationFrameControl::Cancel(AutomationDispatcher::Command& command, FrameHost& host)
	{
		if (ActiveID == command.ID)
		{
			host.SetPending(0);
			if (command.Operation == "game_pause" && PauseOwned) { host.SetPaused(false); PauseOwned = false; }
			ActiveID = 0; StepOwned = false;
		}
		return command.Progress.is_null() ? Json::object() : Result(command, host, "request_cancelled");
	}
	void AutomationFrameControl::Stop(FrameHost& host)
	{
		if (StepOwned) host.SetPending(0);
		if (PauseOwned) host.SetPaused(false);
		ActiveID = 0; StepOwned = false; PauseOwned = false;
	}
}
