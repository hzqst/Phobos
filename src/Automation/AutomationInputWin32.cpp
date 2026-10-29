#include "AutomationInput.h"
#include "EngineInputQueue.h"
#include <Unsorted.h>
#include <Surface.h>
#include <Windows.h>
#include <WWKeyboardClass.h>
#include <WWMouseClass.h>
#include <SessionClass.h>
#include <UI.h>
#include <OwnerDraw.h>
#include <FootClass.h>
#include <Utilities/Macro.h>
#include <atomic>
#include <vector>
#include <algorithm>

namespace Automation
{
	namespace
	{
		std::atomic<bool> InputEnabled {false};
		std::atomic<bool> VirtualCursor {false};
		std::atomic<bool> Keys[256] {};
		HWND EngineInputWindow = nullptr;
		constexpr char InputWindowClass[] = "Phobos.Automation.Input";
		ATOM InputWindowClassAtom = 0;
		constexpr UINT ForwardEngineMouse = WM_APP + 0x1000;
		bool IsMouseMessage(UINT message)
		{
			return message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN
				|| message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MOUSEWHEEL;
		}
		LRESULT CALLBACK InputWindowProc(HWND hwnd, UINT message, WPARAM key, LPARAM position)
		{
			if (message >= ForwardEngineMouse && IsMouseMessage(message - ForwardEngineMouse))
			{
				if (InputEnabled && Game::hWnd)
				{
					// Verified gamemd WndProc: stdcall, RET 10h at 777624.
					// Dispatch inside the engine rather than through a wrapper's
					// focus-dependent OS mouse filtering. Existing Syringe patches
					// inside the engine function (e.g. wheel hooks) still execute.
					const auto engineProc = reinterpret_cast<WNDPROC>(0x7775C0);
					return engineProc(Game::hWnd, message - ForwardEngineMouse, key, position);
				}
				return 0;
			}
			return DefWindowProcA(hwnd, message, key, position);
		}
		DEFINE_REFERENCE(HANDLE, MouseMutex, 0xB78168u)
		// UI::BeginDialog (622650) pushes this HWND; UI::EndDialog (622720)
		// restores the previous one. OwnerDraw::CurrentDialogHwnd is a layout
		// cache and can still refer to the underlying main menu.
		DEFINE_REFERENCE(HWND, ActiveDialog, 0xB72F44u)
		bool LockMouse(HANDLE mutex)
		{
			if (!mutex) return false;
			const DWORD wait = WaitForSingleObject(mutex, 0);
			if (wait == WAIT_ABANDONED) ReleaseMutex(mutex);
			return wait == WAIT_OBJECT_0;
		}
		HWND Dialog()
		{
			const HWND dialog = ActiveDialog;
			return dialog && IsWindow(dialog) && IsWindowVisible(dialog)
				&& GetWindowThreadProcessId(dialog, nullptr) == GetCurrentThreadId() ? dialog : nullptr;
		}
		struct HitTest { POINT Position; HWND Target; };
		BOOL CALLBACK FindControl(HWND hwnd, LPARAM param)
		{
			auto& hit = *reinterpret_cast<HitTest*>(param);
			RECT rect {};
			if (IsWindowVisible(hwnd) && IsWindowEnabled(hwnd))
			{
				UI::GetDisplayRect(hwnd, &rect);
				if (PtInRect(&rect, hit.Position)) hit.Target = hwnd;
			}
			return TRUE;
		}
		BOOL CALLBACK FindButtons(HWND hwnd, LPARAM param)
		{
			char name[32] {};
			GetClassNameA(hwnd, name, sizeof(name));
			if (_stricmp(name, "Button") == 0 && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd))
				reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
			return TRUE;
		}
		std::vector<HWND> Buttons(HWND dialog)
		{
			std::vector<HWND> buttons;
			EnumChildWindows(dialog, FindButtons, reinterpret_cast<LPARAM>(&buttons));
			std::sort(buttons.begin(), buttons.end(), [](HWND a, HWND b)
			{
				RECT ra {}, rb {}; UI::GetDisplayRect(a, &ra); UI::GetDisplayRect(b, &rb);
				return ra.top != rb.top ? ra.top < rb.top : ra.left < rb.left;
			});
			return buttons;
		}
		int DialogID(HWND dialog)
		{
			if (!OwnerDraw::WinData.size()) return 0;
			const auto data = OwnerDraw::WinData.try_get(dialog);
			return data ? data->DialogID : 0;
		}
		constexpr int MainMenuDialog = 226, SingleplayerDialog = 256;
		constexpr int ExitGameButton = 1006, BackToMainMenuButton = 1670;

		// All queue producers run on the game thread. Mouse drawing also reads XY1,
		// so only that short update needs the engine's mouse mutex.
		class EngineInputHost final : public InputHost
		{
			HWND ButtonTargets[5] {};
			HWND PostedTarget = nullptr;
			UINT PostedMessage = 0;
			HWND NavigationDialog = nullptr, NavigationControl = nullptr;
			const char* LastRoute = "wwkeyboard";
			const char* Route() const override { return LastRoute; }
			bool Post(HWND target, UINT message, WPARAM key, LPARAM position)
			{
				if (!target || GetWindowThreadProcessId(target, nullptr) != GetCurrentThreadId()
					|| !PostMessageA(target, message, key, position)) return false;
				PostedTarget = target; PostedMessage = message;
				LastRoute = "native_window_queue";
				return true;
			}
			bool PostEngineMouse(UINT message, WPARAM key, LPARAM position)
			{
				if (!IsMouseMessage(message)) return false;
				const bool posted = Post(EngineInputWindow, ForwardEngineMouse + message, key, position);
				LastRoute = "engine_window_queue";
				return posted;
			}
			bool Available() const override
			{
				// Engine activity is distinct from OS foreground focus. Wrappers which
				// keep the engine active support background automation without activation.
				return InputEnabled && Game::hWnd && IsWindow(Game::hWnd) && Game::IsFocused
					&& WWKeyboardClass::Instance && WWMouseClass::Instance
					&& (!Unsorted::ScenarioStarted || SessionClass::IsSingleplayer());
			}
			bool Position(int& x, int& y) const override
			{
				auto mouse = WWMouseClass::Instance;
				if (!mouse) return false;
				const HANDLE mutex = MouseMutex;
				if (!LockMouse(mutex)) return false;
				x = mouse->XY1.X; y = mouse->XY1.Y;
				if (mutex) ReleaseMutex(mutex);
				return true;
			}
			bool Move(int x, int y) override
			{
				if (!Available() || x < 0 || y < 0 || x >= DSurface::WindowBounds.Width || y >= DSurface::WindowBounds.Height) return false;
				const HANDLE mutex = MouseMutex;
				if (!LockMouse(mutex)) return false;
				WWMouseClass::Instance->SetCoords({x, y});
				LastRoute = "wwmouse";
				VirtualCursor = true;
				if (mutex) ReleaseMutex(mutex);
				return true;
			}
			bool Key(int key, bool down) override
			{
				if (key <= 0 || key >= 256) return false;
				const HWND dialog = Dialog();
				if (!dialog) return Enqueue(key, down, false);
				const bool previous = Keys[key];
				Keys[key] = down;
				const bool queued = DialogKey(dialog, key, down);
				if (!queued) Keys[key] = down ? previous : false;
				return queued;
			}
			bool DialogKey(HWND dialog, int key, bool down)
			{
				{
					const int dialogID = DialogID(dialog);
					const bool frontEndMenu = dialogID == MainMenuDialog || dialogID == SingleplayerDialog;
					// USER32 dialog navigation normally depends on the thread's focus
					// HWND, which is null after deactivation. Keep a private navigation
					// target and enqueue the same dialog/control commands instead.
					if (NavigationDialog != dialog || !NavigationControl || !IsChild(dialog, NavigationControl))
					{
						NavigationDialog = dialog;
						const auto buttons = Buttons(dialog);
						NavigationControl = buttons.empty() ? nullptr : buttons.front();
					}
					if (key == VK_ESCAPE && frontEndMenu)
					{
						if (!down) return true;
						// Verified resource IDs and handlers: 531F60 (main), 52D640
						// (singleplayer). Neither dialog implements IDCANCEL.
						const HWND cancel = GetDlgItem(dialog, dialogID == MainMenuDialog ? ExitGameButton : BackToMainMenuButton);
						return cancel && Post(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(cancel), BN_CLICKED), reinterpret_cast<LPARAM>(cancel));
					}
					if (frontEndMenu && (key == VK_TAB || key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT))
					{
						if (down)
						{
							const bool previous = key == VK_UP || key == VK_LEFT || (key == VK_TAB && Keys[VK_SHIFT]);
							const auto buttons = Buttons(dialog);
							if (buttons.empty()) return false;
							const auto found = std::find(buttons.begin(), buttons.end(), NavigationControl);
							const size_t index = found == buttons.end() ? 0 : size_t(found - buttons.begin());
							const HWND old = NavigationControl;
							NavigationControl = buttons[(index + buttons.size() + (previous ? -1 : 1)) % buttons.size()];
							if (old && IsWindow(old)) Post(old, WM_KILLFOCUS, reinterpret_cast<WPARAM>(NavigationControl), 0);
							if (!Post(NavigationControl, WM_SETFOCUS, reinterpret_cast<WPARAM>(old), 0)) return false;
						}
						LastRoute = "native_dialog_navigation";
						return true;
					}
					if (frontEndMenu && (key == VK_RETURN || key == VK_SPACE))
					{
						if (!down) return true;
						if (!NavigationControl || !IsWindowEnabled(NavigationControl)) return false;
						return Post(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(NavigationControl), BN_CLICKED), reinterpret_cast<LPARAM>(NavigationControl));
					}
					HWND target = GetFocus();
					if (!target || !IsChild(dialog, target)) target = dialog;
					MSG message {};
					message.hwnd = target;
					message.message = down ? WM_KEYDOWN : WM_KEYUP;
					message.wParam = key;
					message.lParam = 1 | (MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16) | (down ? 0 : 0xC0000000);
					// Queue for the engine's normal IsDialogMessage/accelerator path.
					// Calling a control directly could enter a nested modal loop in Poll.
					const bool posted = Post(target, message.message, message.wParam, message.lParam);
					if (!posted) Keys[key] = false;
					return posted;
				}
			}
			bool Button(int button, bool down) override
			{
				if (button != VK_LBUTTON && button != VK_RBUTTON && button != VK_MBUTTON) return false;
				const HWND dialog = Dialog();
				if (dialog || (!down && ButtonTargets[button]))
				{
					int x, y;
					if (!Position(x, y)) return false;
					HWND target = ButtonTargets[button];
					if (down)
					{
						HitTest hit {{x, y}, dialog};
						EnumChildWindows(dialog, FindControl, reinterpret_cast<LPARAM>(&hit));
						target = hit.Target;
						ButtonTargets[button] = target;
					}
					Keys[button] = down;
					if (!down) ButtonTargets[button] = nullptr;
					if (!IsWindow(target)) return !down;
					RECT rect {};
					UI::GetDisplayRect(target, &rect);
					const LPARAM position = MAKELPARAM(x - rect.left, y - rect.top);
					const UINT message = button == VK_LBUTTON ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP)
						: button == VK_RBUTTON ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
					const bool posted = Post(target, message, down ? (button == VK_LBUTTON ? MK_LBUTTON : button == VK_RBUTTON ? MK_RBUTTON : MK_MBUTTON) : 0, position);
					if (!posted) Keys[button] = false;
					return posted;
				}
				if (Unsorted::ScenarioStarted)
				{
					// Tactical input has an essential WndProc preprocessor (6930A0):
					// it begins/ends selection and drag state BEFORE the same event is
					// appended to WWKeyboard. Bypassing it silently loses map clicks.
					if (!down) Keys[button] = false;
					if (Unsorted::UserInputLocked) return !down;
					int x, y;
					if (!Position(x, y)) return false;
					Keys[button] = down;
					const UINT message = button == VK_LBUTTON ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP)
						: button == VK_RBUTTON ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
					const bool posted = PostEngineMouse(message, down ? (button == VK_LBUTTON ? MK_LBUTTON : button == VK_RBUTTON ? MK_RBUTTON : MK_MBUTTON) : 0, MAKELPARAM(x, y));
					if (!posted) Keys[button] = false;
					LastRoute = "engine_window_queue";
					return posted;
				}
				return Enqueue(button, down, true);
			}
			bool Wheel(int delta) override
			{
				if (!Available() || Unsorted::UserInputLocked) return false;
				// The original WndProc (including Phobos sidebar scrolling) handles
				// wheel commands directly; wheel events have no WWKeyboard encoding.
				return PostEngineMouse(WM_MOUSEWHEEL, MAKEWPARAM(0, SHORT(delta)), 0);
			}
			bool IsDown(int key) const override { return key >= 0 && key < 256 && Keys[key]; }
			bool Drained() const override
			{
				MSG message {};
				if (PostedTarget && IsWindow(PostedTarget) && PeekMessageA(&message, PostedTarget, PostedMessage, PostedMessage, PM_NOREMOVE)) return false;
				auto keyboard = WWKeyboardClass::Instance;
				return keyboard && keyboard->Head == keyboard->Tail;
			}
			bool Enqueue(int key, bool down, bool mouse)
			{
				if (key <= 0 || key >= 256) return false;
				if (!down) Keys[key] = false; // Even a lost scene must release held state.
				auto keyboard = WWKeyboardClass::Instance;
				if (!keyboard || (down && !Available())) return false;
				if (Unsorted::UserInputLocked && key != VK_ESCAPE) return !down;
				int x = 0, y = 0;
				if (mouse && !Position(x, y)) return false;
				const bool previous = Keys[key];
				Keys[key] = down;
				if (!EnqueueEngineInput(*keyboard, WORD(key), !down, mouse, WORD(x), WORD(y)))
				{ Keys[key] = previous; return false; }
				LastRoute = "wwkeyboard";
				return true;
			}
		};
	}
	InputHost& GameInputHost() { static EngineInputHost host; return host; }
	void StartGameInput()
	{
		// Message-only window: no focus, rendering, desktop input or extra thread.
		WNDCLASSEXA windowClass {};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = InputWindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = InputWindowClass;
		InputWindowClassAtom = RegisterClassExA(&windowClass);
		if (InputWindowClassAtom)
			EngineInputWindow = CreateWindowExA(0, InputWindowClass, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
		InputEnabled = EngineInputWindow != nullptr;
	}
	void StopGameInput()
	{
		InputEnabled = false;
		if (EngineInputWindow) { DestroyWindow(EngineInputWindow); EngineInputWindow = nullptr; }
		if (InputWindowClassAtom) { UnregisterClassA(InputWindowClass, GetModuleHandleW(nullptr)); InputWindowClassAtom = 0; }
		VirtualCursor = false;
		for (auto& key : Keys) key = false;
	}
	bool NativeKeyState(int key, unsigned& value)
	{
		if (!InputEnabled) return false;
		value = Keys[key & 255] ? 0x8000u : 0;
		return true;
	}
	bool NativeCursor() { return InputEnabled && VirtualCursor; }
	Json GameInputStatus()
	{
		Json held = Json::array();
		Json selectedTypes = Json::array();
		if (Unsorted::ScenarioStarted)
			for (int i = 0; i < std::min(16, ObjectClass::CurrentObjects.Count); ++i)
			{
				const auto object = ObjectClass::CurrentObjects[i];
				const auto type = object ? object->GetType() : nullptr;
				selectedTypes.push_back(type ? Json(type->ID) : Json());
			}
		auto keyboard = WWKeyboardClass::Instance;
		if (keyboard && InputEnabled)
			for (int key = 1; key < 256; ++key) if (keyboard->Down(key)) held.push_back(key);
		return {{"backend", "engine_queue"}, {"ready", GameInputHost().Available()},
			{"dialog_hwnd", reinterpret_cast<uintptr_t>(Dialog())},
			{"dialog_id", DialogID(Dialog())},
			{"selection_count", Unsorted::ScenarioStarted ? Json(ObjectClass::CurrentObjects.Count) : Json()},
			{"selection_types", std::move(selectedTypes)},
			{"tactical_active", bool(Unsorted::TacticalActive)},
			{"mouse_down", Unsorted::ScenarioStarted ? Json(MouseClass::Instance.IsMouseDown) : Json()},
			{"drag_rectangle", Unsorted::ScenarioStarted ? Json(DisplayClass::Instance.LeftPressAndDraggingRectangle) : Json()},
			{"drag_start", Unsorted::ScenarioStarted ? Json{{"x", DisplayClass::Instance.LeftDownPosition.X}, {"y", DisplayClass::Instance.LeftDownPosition.Y}} : Json()},
			{"os_foreground", GetForegroundWindow() == Game::hWnd}, {"held_keys", held},
			{"queue_pending", keyboard ? Json((keyboard->Tail - keyboard->Head + 256) % 256) : Json()},
			{"virtual_cursor", bool(VirtualCursor)}};
	}
}

// The engine's key-state wrappers use ECX (fastcall), with no stack arguments.
// Returning at their existing RET preserves the caller and all callee-saved registers.
DEFINE_HOOK(0x53EC70, Automation_KeyState, 0x5)
{
	unsigned value;
	if (!Automation::NativeKeyState(R->ECX(), value)) return 0;
	R->EAX(value);
	return 0x53EC80;
}

DEFINE_HOOK(0x53EC90, Automation_AsyncKeyState, 0x5)
{
	unsigned value;
	if (!Automation::NativeKeyState(R->ECX(), value)) return 0;
	R->EAX(value);
	return 0x53ECA0;
}

// MouseMutex is already held here. Reuse the branch that reads XY1 instead of
// overwriting it with the desktop cursor. Do not enable engine replay mode.
DEFINE_HOOK(0x7BA0D0, Automation_MouseCoordinates, 0x5)
{
	return Automation::NativeCursor() ? 0x7BA0D9 : 0;
}
