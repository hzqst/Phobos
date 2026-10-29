#pragma once

#include "MCPProtocol.h"
#include <memory>

namespace Automation
{
	// Owns transport threads only; the callback may only read synchronized owned data.
	class AutomationServer
	{
	public:
		AutomationServer();
		~AutomationServer();
		bool Start(int port, const std::string& token, const std::string& instance, MCPProtocol::StatusReader reader,
			Json tools = nullptr, MCPProtocol::ToolExecutor executor = {});
		void Stop();
	private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
