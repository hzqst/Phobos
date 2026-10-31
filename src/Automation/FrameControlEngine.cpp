#include "AutomationFrameControl.h"
#include <Commands/FrameByFrame.h>
#include <Phobos.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Fundamentals.h>
#include <Utilities/MainThreadProfile.h>

namespace Automation
{
	namespace
	{
		bool Tracking = false, WasScenario = false, FrameEntered = false;
		uint32_t EntryFrame = 0, LastEngineFrame = 0;
		uint64_t Completed = 0, Barriers = 0, SceneID = 0, EntryScene = 0;
		void RefreshScene()
		{
			if (!Tracking) return;
			const bool scenario = Unsorted::ScenarioStarted;
			const uint32_t frame = Unsorted::CurrentFrame;
			if (scenario != WasScenario || (scenario && frame < LastEngineFrame && LastEngineFrame != UINT32_MAX))
			{
				++SceneID;
					FrameSceneChanged();
			}
			WasScenario = scenario; LastEngineFrame = frame;
		}
		class EngineFrameHost final : public FrameHost
		{
			bool Available() const override
			{
				return Tracking && Unsorted::ScenarioStarted && Game::IsActive && Game::IsFocused
					&& !Phobos::IsLoadingSaveGame && !Game::SpecialDialog && SessionClass::IsSingleplayer();
			}
			uint64_t Scene() const override { return SceneID; }
			uint64_t CompletedFrames() const override { return Completed; }
			uint64_t PauseBarriers() const override { return Barriers; }
			bool Paused() const override { return FrameByFrameCommandClass::FrameStep; }
			void SetPaused(bool paused) override { FrameByFrameCommandClass::FrameStep = paused; }
			size_t Pending() const override { return FrameByFrameCommandClass::FrameStepCount; }
			void SetPending(size_t frames) override { FrameByFrameCommandClass::FrameStepCount = frames; }
		};
	}
	FrameHost& GameFrameHost() { RefreshScene(); static EngineFrameHost host; return host; }
	void StartFrameTracking()
	{
		Tracking = true; WasScenario = false; FrameEntered = false;
		Completed = 0; Barriers = 0; SceneID = 0; LastEngineFrame = 0;
		RefreshScene();
	}
	void StopFrameTracking() { Tracking = false; FrameEntered = false; }
	void FrameLogicBegin()
	{
		// Includes native work; nested GPU/capture scopes account for their own exclusive time.
		MainThreadProfile::Enter(MainThreadProfile::Phase::MainLoop);
		RefreshScene();
		FrameEntered = Tracking && Unsorted::ScenarioStarted && Game::IsActive;
		EntryFrame = Unsorted::CurrentFrame; EntryScene = SceneID;
	}
	void FramePauseBarrier()
	{
		MainThreadProfile::Exit(MainThreadProfile::Phase::MainLoop);
		FrameEntered = false;
		if (Tracking) ++Barriers;
	}
	bool FrameLogicEnd()
	{
		// 55DE81 increments the engine frame only on the complete simulation path.
		// The existing return hooks also see early exits; those must not count.
		RefreshScene();
		const bool complete = FrameEntered && EntryScene == SceneID
			&& uint32_t(Unsorted::CurrentFrame) == EntryFrame + 1;
		if (complete) ++Completed;
		MainThreadProfile::Exit(MainThreadProfile::Phase::MainLoop);
		MainThreadProfile::Get().Frame(complete);
		FrameEntered = false;
		return complete;
	}
}
