#include "Automation.h"
#include "AutomationServer.h"
#include "AutomationDispatcher.h"
#include "AutomationInput.h"
#include "AutomationFrameControl.h"
#include "AutomationCapture.h"
#include "CaptureEncoding.h"
#include "MCPTools.h"

#include <Commands/FrameByFrame.h>
#include <Render/GPURender.h>
#include <Surface.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>
#include <Phobos.version.h>
#include <GameOptionsClass.h>
#include <SessionClass.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <charconv>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace Automation
{
	namespace
	{
		bool Enabled = false, Started = false;
		UINT_PTR Timer = 0;
		std::mutex SnapshotMutex;
		Json Snapshot;
		// Explicit lifecycle; never destroy transport threads under the loader lock.
		AutomationServer* Server = nullptr;
		std::string Instance;
		std::unique_ptr<AutomationDispatcher> Dispatcher;
		AutomationInput Input;
		AutomationFrameControl Frames;
		int LastWidth = 0, LastHeight = 0;
		bool CursorResetPending = false;
		std::atomic<bool> CaptureWorkerBusy {false};
		struct CachedRequest
		{
			std::string Signature;
			AutomationDispatcher::Ticket Ticket;
			uint64_t Created;
		};
		std::mutex CacheMutex;
		std::map<std::string, CachedRequest> Requests;

		Json Execute(const std::string& name, const Json& args, const Json& meta, const std::shared_ptr<std::atomic<bool>>& disconnected)
		{
			AutomationDispatcher::Ticket ticket;
			const bool screenshot = name == "game_screenshot";
			// Bound CPU pixels and WIC scratch memory across capture and encoding.
			struct CaptureLease
			{
				bool Owned = false;
				~CaptureLease() { if (Owned) CaptureWorkerBusy = false; }
			} captureLease;
			if (screenshot)
			{
				if (!meta.is_object() || meta.value("com.phobos/instance_id", Json()) != Instance)
					return MCPProtocol::ToolResult(AutomationDispatcher::Failure("instance_mismatch_get_game_status_first"), true);
				if (CaptureWorkerBusy.exchange(true)) return MCPProtocol::ToolResult(AutomationDispatcher::Failure("capture_busy"), true);
				captureLease.Owned = true;
				ticket = Dispatcher->Submit(name, args, std::chrono::milliseconds(args.value("timeout_ms", 5000)), disconnected);
			}
			else if (name != "game_status")
			{
				if (!meta.is_object() || !meta.contains("com.phobos/instance_id") || meta["com.phobos/instance_id"] != Instance)
					return MCPProtocol::ToolResult(AutomationDispatcher::Failure("instance_mismatch_get_game_status_first"), true);
				if (!meta.contains("com.phobos/request_id") || !meta["com.phobos/request_id"].is_string())
					return MCPProtocol::ToolResult(AutomationDispatcher::Failure("request_id_required"), true);
				const auto key = meta["com.phobos/request_id"].get<std::string>();
				if (key.empty() || key.size() > 128) return MCPProtocol::ToolResult(AutomationDispatcher::Failure("invalid_request_id"), true);
				const auto signature = name + args.dump();
				std::lock_guard lock(CacheMutex);
				const auto found = Requests.find(key);
				if (found != Requests.end())
				{
					if (found->second.Signature != signature) return MCPProtocol::ToolResult(AutomationDispatcher::Failure("request_id_reused_with_different_arguments"), true);
					ticket = found->second.Ticket;
				}
				else
				{
					if (Requests.size() >= 128)
					{
						auto oldest = Requests.end();
						for (auto i = Requests.begin(); i != Requests.end(); ++i)
						{
							std::lock_guard commandLock(i->second.Ticket->Mutex);
							if (i->second.Ticket->Status == AutomationDispatcher::State::Queued || i->second.Ticket->Status == AutomationDispatcher::State::Executing) continue;
							if (oldest == Requests.end() || i->second.Created < oldest->second.Created) oldest = i;
						}
						if (oldest == Requests.end()) return MCPProtocol::ToolResult(AutomationDispatcher::Failure("request_cache_busy"), true);
						Requests.erase(oldest);
					}
					const auto timeout = IsFrameOperation(name) ? std::chrono::milliseconds(args.value("timeout_ms", 5000) + 500) : std::chrono::milliseconds(10000);
					ticket = Dispatcher->Submit(name, args, timeout, disconnected);
					if (ticket) Requests.emplace(key, CachedRequest{signature, ticket, GetTickCount64()});
				}
			}
			else ticket = Dispatcher->Submit(name, args, std::chrono::seconds(1), disconnected);
			auto result = Dispatcher->Wait(ticket);
			result["instance_id"] = Instance;
			if (ticket) result["request_id"] = ticket->ID;
			const bool error = result.contains("automation_error");
			if (screenshot && !error && ticket && ticket->Image)
			{
				auto encoded = EncodeScreenshot(*ticket->Image, args, std::move(result));
				if (std::chrono::steady_clock::now() >= ticket->Deadline || (disconnected && *disconnected))
					return MCPProtocol::ToolResult(AutomationDispatcher::Failure("capture_timeout_or_disconnected"), true);
				return encoded;
			}
			return MCPProtocol::ToolResult(std::move(result), error);
		}

		void CALLBACK Poll(HWND, UINT, UINT_PTR, DWORD)
		{
			auto& inputHost = GameInputHost();
			const int width = DSurface::WindowBounds.Width, height = DSurface::WindowBounds.Height;
			if (LastWidth != width || LastHeight != height)
			{
				Input.ReleaseAll(inputHost);
				// Video-mode changes recreate/reset WWMouse at (0,0). Keeping that
				// virtual position would continuously scroll the tactical map.
				CursorResetPending = true;
			}
			LastWidth = width; LastHeight = height;
			if (CursorResetPending && width > 0 && height > 0 && inputHost.Move(width / 2, height / 2)) CursorResetPending = false;
			Input.Maintain(inputHost, GetTickCount64());
			const auto gpu = GPURender::GetState();
			auto& frameHost = GameFrameHost();
			constexpr const char* states[] = {"Disabled", "Initializing", "Running", "Hidden", "Failed", "Shutdown"};
			Json status = {
				{"process_id", GetCurrentProcessId()}, {"instance_id", Instance},
				{"game_window_hwnd", reinterpret_cast<uintptr_t>(static_cast<HWND>(Game::hWnd))},
				{"gpu_window_hwnd", reinterpret_cast<uintptr_t>(GPURender::GetWindowHandle())}, {"game_phase", Phobos::IsLoadingSaveGame ? "loading" : Unsorted::ScenarioStarted ? "in_game" : "unknown"},
				{"game_active", bool(Game::IsActive)}, {"game_focused", bool(Game::IsFocused)},
				{"windowed", bool(GameOptionsClass::WindowedMode)}, {"singleplayer", Unsorted::ScenarioStarted ? Json(SessionClass::IsSingleplayer()) : Json()},
				{"game_width", DSurface::WindowBounds.Width}, {"game_height", DSurface::WindowBounds.Height},
				{"gpu_state", states[static_cast<size_t>(gpu)]}, {"gpu_backend", GPURender::GetBackend()},
				{"gpu_output_mode", GPURender::GetOutputMode()}, {"simulation_frame", frameHost.CompletedFrames()}, {"gpu_present_id", GPURender::GetPresentID()},
				{"scene_id", frameHost.Scene()}, {"engine_frame", Unsorted::ScenarioStarted ? Json(int(Unsorted::CurrentFrame)) : Json()},
				{"frame_step_enabled", FrameByFrameCommandClass::FrameStep},
				{"frame_step_pending", FrameByFrameCommandClass::FrameStepCount},
				{"automation_enabled", true}, {"version", PRODUCT_VERSION},
				{"snapshot_tick_ms", GetTickCount64()}
			};
			status["input"] = GameInputStatus();
#ifdef STR_GIT_COMMIT
			status["git_commit"] = STR_GIT_COMMIT;
#else
			status["git_commit"] = nullptr;
#endif
			{
				std::lock_guard lock(SnapshotMutex);
				Snapshot = std::move(status);
			}
			if (Dispatcher) Dispatcher->Pump([](AutomationDispatcher::Command& command)
			{
				if (IsFrameOperation(command.Operation)) return Frames.Tick(command, GameFrameHost(), GetTickCount64());
				if (command.Operation == "game_screenshot") return CaptureTick(command);
				if (command.Operation != "game_status")
				{
					if (CursorResetPending) return AutomationDispatcher::Failure("cursor_reset_pending");
					return Input.Tick(command, GameInputHost(), DSurface::WindowBounds.Width, DSurface::WindowBounds.Height, GetTickCount64());
				}
				std::lock_guard lock(SnapshotMutex);
				auto result = Snapshot;
				result["request_id"] = command.ID;
				result["snapshot_age_ms"] = GetTickCount64() - result["snapshot_tick_ms"].get<uint64_t>();
				return result;
			}, [](AutomationDispatcher::Command& command)
			{
				if (command.Operation == "game_screenshot") return CaptureCancel(command);
				return IsFrameOperation(command.Operation) ? Frames.Cancel(command, GameFrameHost()) : Input.Cancel(command, GameInputHost());
			});
		}
	}
	void Enable() { Enabled = true; }
	bool AutomationOwnsStep() { return Frames.OwnsStep(); }
	void FrameSceneChanged()
	{
		// Do not carry an automation-owned pause into the next scenario.
		if (Frames.OwnsStep() || Frames.OwnsPause())
		{
			FrameByFrameCommandClass::FrameStepCount = 0;
			FrameByFrameCommandClass::FrameStep = false;
		}
	}
	bool RequestedAtStartup()
	{
		// ExeRun precedes the engine's argument parsing hook. Use Windows argument
		// parsing here so quoted arguments and similarly named switches are safe.
		int count = 0;
		auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
		if (!arguments) return false;
		bool requested = false;
		for (int i = 1; i < count; ++i)
			if (_wcsicmp(arguments[i], L"-PhobosAutomation") == 0) requested = true;
		LocalFree(arguments);
		return requested;
	}
	void Start()
	{
		if (!Enabled || Started) return;
		Started = true;
		char token[257] {}, portText[16] {};
		const auto length = GetEnvironmentVariableA("PHOBOS_MCP_TOKEN", token, sizeof(token));
		if (length < 32 || length >= sizeof(token))
		{
			Debug::Log("[Automation] PHOBOS_MCP_TOKEN must contain 32-256 printable ASCII characters; server not started.\n");
			return;
		}
		for (size_t i = 0; i < length; ++i)
			if (token[i] < 33 || token[i] > 126) { SecureZeroMemory(token, sizeof(token)); return; }
		int port = 18742;
		const auto portLength = GetEnvironmentVariableA("PHOBOS_MCP_PORT", portText, sizeof(portText));
		if (portLength)
		{
			const auto parsed = std::from_chars(portText, portText + std::min<size_t>(portLength, sizeof(portText)), port);
			if (portLength >= sizeof(portText) || parsed.ec != std::errc() || parsed.ptr != portText + portLength || port < 1 || port > 65535)
			{
				SecureZeroMemory(token, sizeof(token));
				Debug::Log("[Automation] Invalid PHOBOS_MCP_PORT; server not started.\n");
				return;
			}
		}
		unsigned char random[16];
		if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		{
			SecureZeroMemory(token, sizeof(token));
			return;
		}
		constexpr char hex[] = "0123456789abcdef";
		for (auto byte : random) { Instance += hex[byte >> 4]; Instance += hex[byte & 15]; }
		Dispatcher = std::make_unique<AutomationDispatcher>(Instance);
		StartFrameTracking();
		StartGameInput();
		Poll(nullptr, 0, 0, 0);
		Timer = SetTimer(nullptr, 0, 16, Poll);
			if (!Timer) { SecureZeroMemory(token, sizeof(token)); Stop(); return; }
		Server = new AutomationServer;
		const bool listening = Server->Start(port, token, Instance, []
		{
			return Dispatcher->Wait(Dispatcher->Submit("game_status", Json::object(), std::chrono::milliseconds(1000)));
			}, InputToolDefinitions(), Execute);
		SecureZeroMemory(token, sizeof(token));
		if (!listening)
		{
			Stop();
			Debug::Log("[Automation] Could not listen on 127.0.0.1:%d.\n", port);
			return;
		}
		Debug::Log("[Automation] Listening on http://127.0.0.1:%d/mcp (authentication required).\n", port);
	}
	void Stop()
	{
		if (Timer) { KillTimer(nullptr, Timer); Timer = 0; }
		if (Dispatcher) Dispatcher->Stop();
		Input.ReleaseAll(GameInputHost());
		Frames.Stop(GameFrameHost());
		StopFrameTracking();
		StopGameInput();
		if (Server) { Server->Stop(); delete Server; Server = nullptr; }
		Dispatcher.reset();
		Requests.clear();
	}
}
