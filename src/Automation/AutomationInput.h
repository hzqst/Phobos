#pragma once
#include "AutomationDispatcher.h"
#include <map>

namespace Automation
{
	class InputHost
	{
	public:
		virtual ~InputHost() = default;
		virtual bool Available() const = 0;
		virtual bool Position(int& x, int& y) const = 0;
		virtual bool Move(int x, int y) = 0;
		virtual bool Key(int key, bool down) = 0;
		virtual bool Button(int button, bool down) = 0;
		virtual bool Wheel(int delta) = 0;
		virtual bool IsDown(int key) const = 0;
		virtual bool Drained() const { return true; }
		virtual const char* Route() const { return "wwkeyboard"; }
	};
	class AutomationInput
	{
	public:
		Json Tick(AutomationDispatcher::Command& command, InputHost& host, int width, int height, uint64_t now);
		Json Cancel(AutomationDispatcher::Command& command, InputHost& host);
		void Maintain(InputHost& host, uint64_t now);
		void ReleaseAll(InputHost& host);
	private:
		static constexpr uint64_t LeaseMS = 10000, MessageGapMS = 32;
		struct Held { uint64_t Expires; bool Mouse; };
		std::map<int, Held> HeldInputs;
		struct Operation
		{
			uint64_t ID = 0, Due = 0;
			int Stage = 0, X = 0, Y = 0, StartX = 0, StartY = 0, EndX = 0, EndY = 0;
			int Key = 0, Button = 0, Clicks = 0, Step = 0, Steps = 0, Duration = 0, Hold = 0;
		} Active;
		bool Release(InputHost& host, int key);
		Json Done(InputHost& host);
		Json Fail(InputHost& host, const char* reason);
	};
	InputHost& GameInputHost();
	void StartGameInput();
	void StopGameInput();
	Json GameInputStatus();
}
