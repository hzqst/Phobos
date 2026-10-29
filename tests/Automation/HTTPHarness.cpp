// Transport integration fixture only. The shipping server lives in Phobos.dll.
#include <Automation/AutomationServer.h>
#include <Automation/MCPTools.h>
#include <Automation/CaptureEncoding.h>
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
		}, Automation::InputToolDefinitions(), [](const std::string& name, const Automation::Json& args, const Automation::Json&, const auto&)
		{
			if (name != "game_screenshot") return Automation::MCPProtocol::ToolResult({{"instance_id", "protocol-test"}}, false);
			RenderCapture::Image image;
			image.Width = image.LogicalWidth = image.Height = image.LogicalHeight = 1;
			image.Source = "fixture"; image.Ready = true; image.Pixels = {255,0,0,255};
			return Automation::EncodeScreenshot(image, args, {{"capture_id", 1}});
		})) return 3;
	std::string stop;
	std::getline(std::cin, stop);
	server.Stop();
	return 0;
}
