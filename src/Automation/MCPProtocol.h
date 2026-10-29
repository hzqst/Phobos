#pragma once

#include "Json.h"
#include <atomic>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace Automation
{
	struct HTTPRequest
	{
		std::string Method, Body;
		// The transport normalizes header names to lower case and rejects duplicates.
		std::map<std::string, std::string> Headers;
		std::shared_ptr<std::atomic<bool>> Cancel;
	};
	struct HTTPResponse
	{
		int Status = 200;
		Json Body;
		std::map<std::string, std::string> Headers;
	};

	class MCPProtocol
	{
	public:
		using StatusReader = std::function<Json()>;
		using ToolExecutor = std::function<Json(const std::string&, const Json&, const Json&, const std::shared_ptr<std::atomic<bool>>&)>;
		MCPProtocol(int port, std::string token, std::string instance, StatusReader reader);
		void ConfigureTools(Json tools, ToolExecutor executor);
		HTTPResponse Handle(const HTTPRequest& request);
		static constexpr size_t MaxBodyBytes = 64 * 1024;
		static Json ToolResult(Json value, bool error = false);
	private:
		int Port;
		std::string Token, Instance;
		StatusReader ReadStatus;
		Json ToolDefinitions;
		ToolExecutor Execute;
		std::mutex SessionMutex;
		struct Session { bool Ready; uint64_t LastUsed; };
		std::map<std::string, Session> Sessions;
		uint64_t NextSession = 0;
	};
}
