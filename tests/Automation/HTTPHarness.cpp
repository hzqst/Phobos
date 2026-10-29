// Transport integration fixture only. The shipping server lives in Phobos.dll.
#include <Automation/AutomationServer.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <iostream>

int __cdecl main()
{
	const auto token = std::getenv("PHOBOS_MCP_TOKEN");
	if (!token) return 2;
	Automation::AutomationServer server;
	if (!server.Start(18743, token, "protocol-test", [] {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		return Automation::Json{{"process_id", 0}, {"instance_id", "protocol-test"}};
	})) return 3;
	std::string stop;
	std::getline(std::cin, stop);
	server.Stop();
	return 0;
}
