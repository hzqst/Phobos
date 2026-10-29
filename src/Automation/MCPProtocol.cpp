#include "MCPProtocol.h"
#include "MCPTools.h"

#include <chrono>

namespace Automation
{
	namespace
	{
		constexpr auto Modern = "2026-07-28";
		constexpr auto Legacy = "2025-11-25";
		constexpr auto VersionKey = "io.modelcontextprotocol/protocolVersion";
		constexpr auto InfoKey = "io.modelcontextprotocol/clientInfo";
		constexpr auto CapsKey = "io.modelcontextprotocol/clientCapabilities";
		constexpr uint64_t SessionTTL = 30 * 60 * 1000;
		constexpr size_t MaxSessions = 32;

		uint64_t Now()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		std::string Header(const HTTPRequest& r, const char* key)
		{
			const auto i = r.Headers.find(key);
			return i == r.Headers.end() ? "" : i->second;
		}
		std::string String(const Json& j, const char* key)
		{
			const auto i = j.find(key);
			return i != j.end() && i->is_string() ? i->get<std::string>() : "";
		}
		HTTPResponse Error(int status, const Json& id, int code, const char* message, Json data = nullptr)
		{
			Json error = {{"code", code}, {"message", message}};
			if (!data.is_null()) error["data"] = std::move(data);
			return {status, {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}}, {}};
		}
		// Bound nesting before invoking the DOM parser; quoted brackets do not count.
		bool BoundedDepth(const std::string& text)
		{
			int depth = 0;
			bool quoted = false, escaped = false;
			for (char c : text)
			{
				if (quoted)
				{
					if (escaped) escaped = false;
					else if (c == '\\') escaped = true;
					else if (c == '"') quoted = false;
				}
				else if (c == '"') quoted = true;
				else if (c == '{' || c == '[') { if (++depth > 32) return false; }
				else if (c == '}' || c == ']') { if (--depth < 0) return false; }
			}
			return true;
		}
		bool EqualToken(const std::string& a, const std::string& b)
		{
			if (a.size() != b.size()) return false;
			unsigned char different = 0;
			for (size_t i = 0; i < a.size(); ++i) different |= a[i] ^ b[i];
			return different == 0;
		}
		bool DecodeName(std::string& value)
		{
			if (!value.starts_with("=?base64?") || !value.ends_with("?=")) return true;
			const std::string encoded = value.substr(9, value.size() - 11);
			if (encoded.empty() || encoded.size() % 4) return false;
			constexpr auto alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string decoded;
			unsigned int bits = 0; int count = 0; bool padding = false;
			for (char c : encoded)
			{
				if (c == '=') { padding = true; continue; }
				const auto p = std::string_view(alphabet).find(c);
				if (padding || p == std::string_view::npos) return false;
				bits = (bits << 6) | static_cast<unsigned int>(p); count += 6;
				if (count >= 8) { count -= 8; decoded += char((bits >> count) & 255); }
			}
			value = std::move(decoded);
			return true;
		}
		Json Tools()
		{
			return Json::array({{
				{"name", "game_status"}, {"description", "Read the last safe main-thread snapshot; unavailable fields are null."},
				{"inputSchema", {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}},
				{"outputSchema", {{"type", "object"}}},
				{"annotations", {{"readOnlyHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}}
			}});
		}
	}

	MCPProtocol::MCPProtocol(int port, std::string token, std::string instance, StatusReader reader)
		: Port(port), Token(std::move(token)), Instance(std::move(instance)), ReadStatus(std::move(reader)), ToolDefinitions(Tools()) { }

	void MCPProtocol::ConfigureTools(Json tools, ToolExecutor executor)
	{
		ToolDefinitions = std::move(tools);
		Execute = std::move(executor);
	}

	Json MCPProtocol::ToolResult(Json value, bool error)
	{
		return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})},
			{"structuredContent", std::move(value)}, {"isError", error}};
	}

	HTTPResponse MCPProtocol::Handle(const HTTPRequest& r)
	{
		const auto host = "127.0.0.1:" + std::to_string(Port);
		const auto localhost = "localhost:" + std::to_string(Port);
		const auto origin = Header(r, "origin");
		if ((Header(r, "host") != host && Header(r, "host") != localhost)
			|| (!origin.empty() && origin != "http://" + host && origin != "http://" + localhost))
			return Error(403, nullptr, -32600, "Invalid Host or Origin");
		if (Token.empty() || !EqualToken(Header(r, "authorization"), "Bearer " + Token))
			return Error(401, nullptr, -32600, "Authentication required");
		if (r.Method != "POST" && r.Method != "DELETE")
			return {405, nullptr, {{"Allow", "POST, DELETE"}}};
		if (r.Method == "DELETE")
		{
			std::lock_guard lock(SessionMutex);
			return {Sessions.erase(Header(r, "mcp-session-id")) ? 204 : 404, nullptr, {}};
		}
		if (r.Body.size() > MaxBodyBytes) return Error(413, nullptr, -32600, "Request too large");
		if (!Header(r, "content-type").starts_with("application/json"))
			return Error(415, nullptr, -32600, "Expected application/json");
		const auto accept = Header(r, "accept");
		if (accept.find("application/json") == std::string::npos || accept.find("text/event-stream") == std::string::npos)
			return Error(406, nullptr, -32600, "Accept must include application/json and text/event-stream");
		if (!BoundedDepth(r.Body)) return Error(400, nullptr, -32700, "Invalid JSON or nesting exceeds 32");
		const auto request = Json::parse(r.Body, nullptr, false);
		if (request.is_discarded()) return Error(400, nullptr, -32700, "Parse error");
		if (!request.is_object()) return Error(400, nullptr, -32600, "Expected a single JSON-RPC message");
		const Json id = request.contains("id") ? request["id"] : Json();
		if (String(request, "jsonrpc") != "2.0" || String(request, "method").empty()
			|| (request.contains("id") && !(id.is_string() || id.is_number_integer()))
			|| (request.contains("params") && !request["params"].is_object()))
			return Error(400, nullptr, -32600, "Invalid request");
		const auto method = String(request, "method");
		const Json params = request.contains("params") ? request["params"] : Json::object();
		const auto version = Header(r, "mcp-protocol-version");
		const bool modern = version != Legacy && method != "initialize";
		HTTPResponse response;
		Json result;
		if (modern)
		{
			const Json meta = params.contains("_meta") ? params["_meta"] : Json();
			if (version.empty() || Header(r, "mcp-method") != method || String(meta, VersionKey) != version)
				return Error(400, id, -32020, "HeaderMismatch");
			if (version != Modern)
				return Error(400, id, -32022, "Unsupported protocol version", {{"supported", {Modern, Legacy}}, {"requested", version}});
			if (!meta.is_object() || !meta.contains(InfoKey) || !meta[InfoKey].is_object()
				|| String(meta[InfoKey], "name").empty() || String(meta[InfoKey], "version").empty()
				|| !meta.contains(CapsKey) || !meta[CapsKey].is_object())
				return Error(400, id, -32602, "Required client metadata missing");
			if (method == "tools/call")
			{
				auto name = Header(r, "mcp-name");
				if (name.empty() || !DecodeName(name) || name != String(params, "name"))
					return Error(400, id, -32020, "HeaderMismatch");
			}
			if (!request.contains("id")) return Error(400, nullptr, -32600, "No modern notifications supported");
		}
		else
		{
			std::lock_guard lock(SessionMutex);
			const auto now = Now();
			std::erase_if(Sessions, [now](const auto& s) { return now - s.second.LastUsed > SessionTTL; });
			if (method == "initialize")
			{
				if (!request.contains("id") || String(params, "protocolVersion").empty()
					|| !params.contains("clientInfo") || !params["clientInfo"].is_object()
					|| !params.contains("capabilities") || !params["capabilities"].is_object())
					return Error(400, id, -32602, "Invalid initialize parameters");
				if (Sessions.size() >= MaxSessions) return Error(503, id, -32603, "Session limit reached");
				const auto session = Instance + "-" + std::to_string(++NextSession);
				Sessions.emplace(session, Session{false, now});
				response.Headers["Mcp-Session-Id"] = session;
				result = {{"protocolVersion", Legacy}, {"capabilities", {{"tools", Json::object()}}},
					{"serverInfo", {{"name", "Phobos"}, {"version", "0.1.0"}}}};
			}
			else
			{
				if (version != Legacy) return Error(400, id, -32600, "Invalid session protocol version");
				const auto session = Sessions.find(Header(r, "mcp-session-id"));
				if (session == Sessions.end()) return Error(404, id, -32600, "Session expired; initialize again");
				session->second.LastUsed = now;
				if (method == "notifications/initialized" && !request.contains("id"))
				{
					session->second.Ready = true;
					return {202, nullptr, {}};
				}
				if (method == "notifications/cancelled" && !request.contains("id")) return {202, nullptr, {}};
				if (!session->second.Ready && method != "ping") return Error(400, id, -32600, "Session not initialized");
				if (!request.contains("id")) return Error(400, nullptr, -32600, "Unknown notification");
			}
		}
		if (method == "server/discover" && modern)
			result = {{"supportedVersions", {Modern, Legacy}}, {"capabilities", {{"tools", Json::object()}}},
				{"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "Phobos"}, {"version", "0.1.0"}}}}},
				{"ttlMs", 0}, {"cacheScope", "private"}};
		else if (method == "tools/list")
		{
			if (params.contains("cursor")) return Error(400, id, -32602, "Pagination cursor is not supported");
			result = {{"tools", ToolDefinitions}};
			if (modern) { result["ttlMs"] = 0; result["cacheScope"] = "private"; }
		}
		else if (method == "tools/call")
		{
			const auto name = String(params, "name");
			const Json args = params.contains("arguments") ? params["arguments"] : Json::object();
			const auto definition = std::find_if(ToolDefinitions.begin(), ToolDefinitions.end(), [&](const Json& tool) { return tool["name"] == name; });
			if (definition == ToolDefinitions.end()) return Error(200, id, -32602, "Unknown tool");
			if (!ValidateArguments((*definition)["inputSchema"], args)) return Error(200, id, -32602, "Invalid tool arguments");
			if (Execute) result = Execute(name, args, params.value("_meta", Json::object()), r.Cancel);
			else
			{
				auto status = ReadStatus();
				const bool error = status.contains("automation_error");
				result = ToolResult(std::move(status), error);
			}
		}
		else if (method == "ping") result = Json::object();
		else if (method != "initialize" || modern) return Error(modern ? 404 : 200, id, -32601, "Method not found");
		if (modern) result["resultType"] = "complete";
		response.Body = {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
		return response;
	}
}
