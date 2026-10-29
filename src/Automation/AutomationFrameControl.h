#pragma once
#include "AutomationDispatcher.h"

namespace Automation
{
	class FrameHost
	{
	public:
		virtual ~FrameHost() = default;
		virtual bool Available() const = 0;
		virtual uint64_t Scene() const = 0;
		virtual uint64_t CompletedFrames() const = 0;
		virtual uint64_t PauseBarriers() const = 0;
		virtual bool Paused() const = 0;
		virtual void SetPaused(bool paused) = 0;
		virtual size_t Pending() const = 0;
		virtual void SetPending(size_t frames) = 0;
	};
	class AutomationFrameControl
	{
	public:
		Json Tick(AutomationDispatcher::Command& command, FrameHost& host, uint64_t now);
		Json Cancel(AutomationDispatcher::Command& command, FrameHost& host);
		void Stop(FrameHost& host);
		bool OwnsStep() const { return StepOwned; }
		bool OwnsPause() const { return PauseOwned; }
	private:
		uint64_t ActiveID = 0;
		bool StepOwned = false, PauseOwned = false;
		Json Result(const AutomationDispatcher::Command& command, FrameHost& host, const char* error = nullptr);
	};
	bool IsFrameOperation(const std::string& name);
	FrameHost& GameFrameHost();
	void StartFrameTracking();
	void StopFrameTracking();
	void FrameLogicBegin();
	void FramePauseBarrier();
	bool FrameLogicEnd();
	bool AutomationOwnsStep();
	void FrameSceneChanged();
}
