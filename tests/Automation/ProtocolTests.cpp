#include <Automation/MCPProtocol.h>
#include <cassert>
#include <iostream>

using namespace Automation;

int __cdecl main()
{
	const std::string token(32, 'x');
	MCPProtocol protocol(18742, token, "test-instance", [] { return Json{{"process_id", 42}}; });
	HTTPRequest r {"POST", "", {{"host", "127.0.0.1:18742"}, {"authorization", "Bearer " + token},
		{"content-type", "application/json"}, {"accept", "application/json, text/event-stream"},
		{"mcp-protocol-version", "2026-07-28"}, {"mcp-method", "server/discover"}}};
	Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "server/discover"}, {"params", {{"_meta", {
		{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
		{"io.modelcontextprotocol/clientInfo", {{"name", "test"}, {"version", "1"}}},
		{"io.modelcontextprotocol/clientCapabilities", Json::object()}}}}}};
	auto call = [&] { r.Body = request.dump(); return protocol.Handle(r); };
	assert(call().Body["result"]["resultType"] == "complete");
	r.Headers["origin"] = "http://evil.invalid";
	assert(call().Status == 403);
	r.Headers.erase("origin");
	r.Headers["authorization"] = "invalid";
	assert(call().Status == 401);
	r.Headers["authorization"] = "Bearer " + token;
	r.Headers["mcp-method"] = "tools/list";
	assert(call().Body["error"]["code"] == -32020);
	request["method"] = "tools/list";
	assert(call().Body["result"]["tools"].size() == 1);
	request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
	r.Headers["mcp-protocol-version"] = "2099-01-01";
	assert(call().Body["error"]["code"] == -32022);
	request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";
	r.Headers["mcp-protocol-version"] = "2026-07-28";
	r.Headers["mcp-method"] = "tools/call";
	r.Headers["mcp-name"] = "=?base64?Z2FtZV9zdGF0dXM=?=";
	request["method"] = "tools/call";
	request["params"]["name"] = "game_status";
	request["params"]["arguments"] = Json::object();
	assert(call().Body["result"]["structuredContent"]["process_id"] == 42);
	request["params"]["arguments"] = {{"unexpected", 1}};
	assert(call().Body["error"]["code"] == -32602);
	request["params"]["name"] = "unknown";
	r.Headers["mcp-name"] = "unknown";
	assert(call().Body["error"]["code"] == -32602);
	r.Body = "{";
	assert(protocol.Handle(r).Body["error"]["code"] == -32700);
	r.Body = std::string(MCPProtocol::MaxBodyBytes + 1, ' ');
	assert(protocol.Handle(r).Status == 413);
	r.Body = std::string(33, '[') + std::string(33, ']');
	assert(protocol.Handle(r).Status == 400);
	request = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "initialize"}, {"params", {
		{"protocolVersion", "2025-11-25"}, {"clientInfo", {{"name", "test"}, {"version", "1"}}}, {"capabilities", Json::object()}}}};
	r.Headers.erase("mcp-protocol-version");
	auto initialized = call();
	assert(initialized.Body["result"]["protocolVersion"] == "2025-11-25");
	r.Headers["mcp-session-id"] = initialized.Headers.at("Mcp-Session-Id");
	r.Headers["mcp-protocol-version"] = "2025-11-25";
	request = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}};
	assert(call().Status == 400);
	request = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
	assert(call().Status == 202);
	request = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/list"}};
	assert(!call().Body["result"].contains("resultType"));
	r.Method = "DELETE";
	assert(call().Status == 204);
	r.Method = "POST";
	assert(call().Status == 404);
	std::cout << "Protocol tests passed\n";
}
