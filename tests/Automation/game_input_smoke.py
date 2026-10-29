"""SDK 2.3.0 game-input evidence collector, using an isolated game directory.

Pillow desktop images are diagnostic evidence only, NOT game_screenshot output.
No installed game files or saves are modified. No test is marked consumed based
solely on queue drain; inspect the captured menu states separately.
"""
import asyncio
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time

import httpx2
from mcp import Client, types
from mcp.client.streamable_http import streamable_http_client
from PIL import ImageGrab

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / "Debug/AutomationGame"
SCENARIO = os.environ.get("PHOBOS_TEST_SCENARIO") == "1"
GPU = os.environ.get("PHOBOS_TEST_GPU") == "1"
OUT = ROOT / ("Debug/AutomationTests/game-input-scenario" if SCENARIO else "Debug/AutomationTests/game-input-native")
if GPU:
    OUT = OUT.with_name(OUT.name + "-gpu")
URL = "http://127.0.0.1:18742/mcp"
user = ctypes.WinDLL("user32", use_last_error=True)
user.SetProcessDPIAware()
user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
user.GetForegroundWindow.restype = wintypes.HWND
user.SetForegroundWindow.argtypes = [wintypes.HWND]
user.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
user.CreateWindowExW.restype = wintypes.HWND
user.DestroyWindow.argtypes = [wintypes.HWND]
user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
user.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
user.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
ENUM = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user.EnumChildWindows.argtypes = [wintypes.HWND, ENUM, wintypes.LPARAM]
user.GetDlgCtrlID.argtypes = [wintypes.HWND]
user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user.IsWindowEnabled.argtypes = [wintypes.HWND]
user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user.GetAsyncKeyState.argtypes = [ctypes.c_int]
user.GetAsyncKeyState.restype = ctypes.c_short
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user.IsWindowVisible.argtypes = [wintypes.HWND]
user.IsIconic.argtypes = [wintypes.HWND]
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
OWN_PID = None
WITNESS = None
EXIT_REQUESTED = False


def pump_witness():
    message = wintypes.MSG()
    for _ in range(64):
        if not user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
            break
        user.DispatchMessageW(ctypes.byref(message))
    asyncio.get_running_loop().call_later(.05, pump_witness)


def background_witness():
    current_thread = kernel.GetCurrentThreadId()
    foreground_thread = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
    attached = user.AttachThreadInput(current_thread, foreground_thread, True)
    try:
        user.SetForegroundWindow(WITNESS)
    finally:
        if attached:
            user.AttachThreadInput(current_thread, foreground_thread, False)


def controls(dialog):
    result = []
    @ENUM
    def collect(hwnd, _):
        text = ctypes.create_unicode_buffer(256)
        classname = ctypes.create_unicode_buffer(64)
        user.GetWindowTextW(hwnd, text, 256)
        user.GetClassNameW(hwnd, classname, 64)
        result.append({"id": user.GetDlgCtrlID(hwnd), "text": text.value, "class": classname.value,
            "enabled": bool(user.IsWindowEnabled(hwnd)), "visible": bool(user.IsWindowVisible(hwnd))})
        return True
    if dialog:
        user.EnumChildWindows(dialog, collect, 0)
    return result


def picture(hwnd, name):
    rect, origin = wintypes.RECT(), wintypes.POINT()
    if not user.GetClientRect(hwnd, ctypes.byref(rect)) or not user.ClientToScreen(hwnd, ctypes.byref(origin)):
        raise RuntimeError("game client rectangle unavailable")
    # Diagnostic visibility only: SWP_NOACTIVATE preserves the background test.
    # This is not the native DDraw/GPU screenshot implementation.
    user.SetWindowPos(hwnd, wintypes.HWND(-1), 0, 0, 0, 0, 0x13)
    try:
        time.sleep(.2)
        ImageGrab.grab(bbox=(origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom), all_screens=True).save(OUT / f"{name}.png")
    finally:
        user.SetWindowPos(hwnd, wintypes.HWND(-2), 0, 0, 0, 0, 0x13)


async def exercise(token):
    global OWN_PID, WITNESS, EXIT_REQUESTED
    pump_witness()
    evidence = []
    async with httpx2.AsyncClient(headers={"Authorization": "Bearer " + token}, timeout=15) as http:
        for _ in range(100):
            try:
                await http.post(URL, content=b"{}", headers={"Accept": "application/json, text/event-stream", "Content-Type": "application/json"})
                break
            except httpx2.ConnectError:
                await asyncio.sleep(.1)
        else:
            raise TimeoutError("embedded server readiness")
        async with Client(streamable_http_client(URL, http_client=http)) as client:
            async def status():
                data = (await client.call_tool("game_status", {})).model_dump(by_alias=True)
                assert not data["isError"], data
                return data["structuredContent"]

            state = await status()
            OWN_PID = state["process_id"]
            hwnd = state["game_window_hwnd"]
            instance = state["instance_id"]
            assert state["automation_enabled"] and state["gpu_state"] in ({"Running", "Hidden"} if GPU else {"Disabled"}), state
            await asyncio.sleep(8)
            state = await status()
            hwnd = state["game_window_hwnd"]
            foreground_pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(user.GetForegroundWindow(), ctypes.byref(foreground_pid))
            evidence.append({"before_input": state, "foreground_hwnd": user.GetForegroundWindow(),
                "foreground_pid": foreground_pid.value, "game_visible": bool(user.IsWindowVisible(hwnd)), "game_iconic": bool(user.IsIconic(hwnd))})
            (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            state = await status()
            evidence.append({"initial_status": state})
            picture(hwnd, "00-menu")

            enforce_background = False
            async def act(name, args, request=None):
                if enforce_background:
                    background_witness()
                before = user.GetForegroundWindow()
                input_state = (await status())["input"]
                cursor_before = wintypes.POINT()
                user.GetCursorPos(ctypes.byref(cursor_before))
                if enforce_background:
                    assert before != hwnd, "could not establish background test condition"
                meta = {"com.phobos/instance_id": instance,
                    "com.phobos/request_id": request or secrets.token_hex(16)}
                data = (await client.call_tool(name, args, meta=meta)).model_dump(by_alias=True)
                evidence.append({"tool": name, "args": args, "result": data,
                    "input_before": input_state, "controls_before": controls(input_state["dialog_hwnd"]),
                    "foreground_before": before, "foreground_after": user.GetForegroundWindow(), "background_required": enforce_background})
                (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
                assert not data["isError"], data
                assert data["structuredContent"]["backend"] == "engine_queue"
                assert data["structuredContent"]["affects_desktop_input"] is False
                if name == "game_mouse_move":
                    cursor_after = wintypes.POINT()
                    user.GetCursorPos(ctypes.byref(cursor_after))
                    assert (cursor_before.x, cursor_before.y) == (cursor_after.x, cursor_after.y), "desktop cursor moved during native move test"
                return data["structuredContent"]

            for _ in range(3):
                if (await status())["input"]["dialog_hwnd"]:
                    break
                await act("game_key", {"key": "ESC", "action": "tap"})
                await asyncio.sleep(.5)
            picture(hwnd, "00-menu-after-intro")
            # A test-owned tiny window establishes a controlled background case.
            # Never activate the game or send any desktop input.
            WITNESS = user.CreateWindowExW(0, "STATIC", "Phobos background input test", 0x80000000,
                0, 0, 24, 24, None, None, None, None)
            assert WITNESS
            user.ShowWindow(WITNESS, 5)
            background_witness()
            assert user.GetForegroundWindow() != hwnd, "could not establish background test condition"
            enforce_background = True
            await act("game_key", {"key": "DOWN", "action": "tap"})
            await asyncio.sleep(.3)
            picture(hwnd, "01-down")
            await act("game_key", {"key": "UP", "action": "tap"})
            await act("game_key", {"key": "ENTER", "action": "tap"})
            await asyncio.sleep(3)
            picture(hwnd, "02-enter")
            assert (await status())["input"]["dialog_id"] == 256, "ENTER did not open the singleplayer menu"
            await act("game_key", {"key": "ESC", "action": "tap"})
            await asyncio.sleep(3)
            picture(hwnd, "03-escape")
            assert (await status())["input"]["dialog_id"] == 226, "ESC did not return to the main menu"
            await act("game_mouse_click", {"x": 718, "y": 220, "button": "left"})
            await asyncio.sleep(3)
            picture(hwnd, "04-click-singleplayer")
            assert (await status())["input"]["dialog_id"] == 256, "click did not open the singleplayer menu"
            if SCENARIO:
                await act("game_mouse_click", {"x": 718, "y": 303, "button": "left"})
                await asyncio.sleep(3)
                picture(hwnd, "06-skirmish-settings")
                await act("game_mouse_click", {"x": 718, "y": 261, "button": "left"})
                for _ in range(60):
                    await asyncio.sleep(1)
                    state = await status()
                    if state["game_phase"] == "in_game":
                        break
                picture(hwnd, "07-scenario")
                assert state["game_phase"] == "in_game" and state["singleplayer"], state
                evidence.append({"scenario_started": state})
                # KeyboardMD.ini: NextObject=78 (N), DeployObject=68 (D).
                # The isolated scenario starts with one MCV and no other units.
                await act("game_key", {"key": "N", "action": "tap"})
                for _ in range(40):
                    selected = (await status())["input"]
                    if selected["selection_count"]:
                        break
                    await asyncio.sleep(.05)
                picture(hwnd, "08-keyboard-selection")
                evidence.append({"selection_after_next_object_key": selected, "drag_test": "not_passed_separate_followup"})
                (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
                assert selected["selection_count"] == 1 and selected["selection_types"] == ["AMCV"], "N did not select the starting MCV"
                await act("game_key", {"key": "D", "action": "tap"})
                await asyncio.sleep(3)
                picture(hwnd, "08-deploy")
                await act("game_mouse_wheel", {"x": state["game_width"] - 60, "y": 500, "delta": -120})
                picture(hwnd, "09-wheel")
                async def frame_call(name, args):
                    meta = {"com.phobos/instance_id": instance, "com.phobos/request_id": secrets.token_hex(16)}
                    data = (await client.call_tool(name, args, meta=meta)).model_dump(by_alias=True)
                    evidence.append({"tool": name, "args": args, "result": data})
                    (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
                    assert not data["isError"], data
                    assert data["structuredContent"]["completed"], data
                    return data["structuredContent"]

                await frame_call("game_pause", {})
                stopped = (await status())["simulation_frame"]
                await asyncio.sleep(.3)
                assert stopped == (await status())["simulation_frame"], "pause did not stop simulation"
                await act("game_key", {"key": "N", "action": "tap"})
                assert stopped == (await status())["simulation_frame"], "input advanced paused simulation"
                for count in (1, 1, 1, 5):
                    stepped = await frame_call("game_step", {"frames": count, "timeout_ms": 5000})
                    assert count == stepped["actual_frames"]
                    assert count == stepped["end_frame"] - stepped["start_frame"]
                    stopped = (await status())["simulation_frame"]
                    await asyncio.sleep(.1)
                    assert stopped == (await status())["simulation_frame"]
                await frame_call("game_resume", {})
                resumed = (await status())["simulation_frame"]
                await asyncio.sleep(.3)
                assert resumed < (await status())["simulation_frame"]
                evidence.append({"frame_control_live_passed": True})
                await act("game_key", {"key": "ESC", "action": "tap"})
                await asyncio.sleep(1)
                picture(hwnd, "10-pause-menu")
                evidence.append({"scenario_injection_completed": True, "visual_effects_require_review": True,
                    "normal_exit_tested": False, "final_status": await status()})
                (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
                return OWN_PID
            await act("game_key", {"key": "ESC", "action": "tap"})
            await asyncio.sleep(3)
            request = secrets.token_hex(16)
            moved = await act("game_mouse_move", {"x": 200, "y": 150, "mode": "absolute"}, request)
            repeated = await act("game_mouse_move", {"x": 200, "y": 150, "mode": "absolute"}, request)
            assert moved["request_id"] == repeated["request_id"]
            assert abs(moved["x"] - 200) <= 1 and abs(moved["y"] - 150) <= 1
            await act("game_key", {"key": "SHIFT", "action": "press"})
            assert not (user.GetAsyncKeyState(0x10) & 0x8000), "desktop SHIFT was changed"
            assert 0x10 in (await status())["input"]["held_keys"]
            await act("game_key", {"key": "SHIFT", "action": "release"})
            assert not (user.GetAsyncKeyState(0x10) & 0x8000)
            assert 0x10 not in (await status())["input"]["held_keys"]
            holding = asyncio.create_task(act("game_key", {"key": "SHIFT", "action": "tap", "hold_ms": 700}))
            await asyncio.sleep(.1)
            query_start = time.monotonic()
            await status()
            query_elapsed = time.monotonic() - query_start
            assert query_elapsed < .4 and not holding.done(), "status blocked behind input command"
            await holding
            evidence.append({"status_during_input_seconds": query_elapsed})
            # Unknown instance must fail before any mutation.
            bad_meta = {"com.phobos/instance_id": "old-instance", "com.phobos/request_id": "stale"}
            stale = (await client.call_tool("game_key", {"key": "ENTER", "action": "tap"}, meta=bad_meta)).model_dump(by_alias=True)
            assert stale["isError"] and "instance_mismatch" in stale["structuredContent"]["automation_error"]
            evidence.append({"stale_instance_rejected": True, "final_status": await status()})
            (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            process_handle = kernel.OpenProcess(0x100000, False, state["process_id"])
            assert process_handle
            try:
                # The game ignores SC_CLOSE; WM_CLOSE only destroys its window.
                # Exercise the native Exit menu command instead.
                await act("game_mouse_click", {"x": 718, "y": 555, "button": "left"})
                await asyncio.sleep(2)
                if kernel.WaitForSingleObject(process_handle, 0) != 0:
                    picture(hwnd, "05-exit-confirm")
                    EXIT_REQUESTED = True
                    try:
                        await act("game_mouse_click", {"x": 520, "y": 406, "button": "left"})
                    except Exception:
                        # Exiting can cancel the tool before its release phase;
                        # only observed process termination establishes success.
                        pass
                exited = kernel.WaitForSingleObject(process_handle, 5000) == 0
                evidence.append({"normal_exit": exited})
                (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
                assert exited, "game did not exit through its menu"
            finally:
                kernel.CloseHandle(process_handle)
            return state["process_id"]


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    with socket.socket() as probe:
        if probe.connect_ex(("127.0.0.1", 18742)) == 0:
            raise RuntimeError("port 18742 already occupied; no game launched")
    token = secrets.token_hex(32)
    # Do not activate the game: native automation must not need foreground.
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
    arguments = "--args=-CD -LOG -PhobosAutomation" + ("" if GPU else " -NoGPURender")
    launcher = subprocess.Popen([str(GAME / "Syringe.exe"), "gamemd.exe", arguments],
        cwd=GAME, startupinfo=startup, env=os.environ | {"PHOBOS_MCP_TOKEN": token, "PHOBOS_MCP_PORT": "18742"})
    try:
        try:
            game_id = asyncio.run(exercise(token))
        except Exception:
            if not EXIT_REQUESTED:
                raise
            # SDK transport tasks may be cancelled when the embedded server
            # exits. A disconnected call is not proof of process termination.
            handle = kernel.OpenProcess(0x1000 | 0x100000, False, OWN_PID)
            if not handle:
                raise
            try:
                exited = kernel.WaitForSingleObject(handle, 15000) == 0
                code = wintypes.DWORD()
                assert exited and kernel.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value == 0, "normal exit not observed"
                saved = json.loads((OUT / "results.json").read_text(encoding="utf-8"))
                saved.append({"normal_exit": True, "exit_code": code.value, "transport_closed_during_exit": True})
                (OUT / "results.json").write_text(json.dumps(saved, indent=2), encoding="utf-8")
                game_id = OWN_PID
            finally:
                kernel.CloseHandle(handle)
        print(f"Input evidence collected from game PID {game_id}: {OUT}")
    finally:
        if WITNESS:
            user.DestroyWindow(WITNESS)
        if OWN_PID:
            handle = kernel.OpenProcess(0x100001, False, OWN_PID)
            if handle:
                try:
                    if kernel.WaitForSingleObject(handle, 0) != 0:
                        print("Test did not achieve normal exit; cleaning up only its authenticated game PID", OWN_PID)
                        kernel.TerminateProcess(handle, 1)
                finally:
                    kernel.CloseHandle(handle)
