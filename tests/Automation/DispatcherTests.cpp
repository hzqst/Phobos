#include <Automation/AutomationDispatcher.h>
#include <cassert>
#include <thread>
#include <iostream>

using namespace Automation;
using namespace std::chrono_literals;
int __cdecl main()
{
	AutomationDispatcher dispatcher("instance-one");
	auto command = dispatcher.Submit("status", Json::object(), 1s);
	assert(command && command->Instance == "instance-one");
	const auto owner = std::this_thread::get_id();
	Json result;
	std::thread network([&] { result = dispatcher.Wait(command); });
	dispatcher.Pump([&](const auto& c)
	{
		assert(owner == std::this_thread::get_id());
		assert(c.ID == command->ID);
		return Json{{"ok", true}};
	});
	network.join();
	assert(result["ok"] == true);
	auto expired = dispatcher.Submit("must_not_execute", {}, 1ms);
	assert(dispatcher.Wait(expired)["automation_error"] == "main_thread_timeout");
	bool called = false;
	dispatcher.Pump([&](const auto&) { called = true; return Json::object(); });
	assert(!called);
	for (int i = 0; i < 32; ++i) assert(dispatcher.Submit("queued", {}, 1s));
	assert(!dispatcher.Submit("overflow", {}, 1s));
	dispatcher.Stop();
	assert(!dispatcher.Submit("after_shutdown", {}, 1s));
	AutomationDispatcher second("instance-two");
	auto pending = second.Submit("pending", {}, 1s);
	std::thread waiter([&] { result = second.Wait(pending); });
	second.Stop();
	waiter.join();
	assert(result["automation_error"] == "game_shutdown");
	assert(pending->Instance != command->Instance);
	std::cout << "Dispatcher tests passed\n";
}
