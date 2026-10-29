"""SDK 2.3.0 game-input evidence collector, using an isolated game directory.

Pillow desktop images are diagnostic evidence only, NOT game_screenshot output.
No installed game files or saves are modified. No test is marked consumed based
solely on queue drain; inspect the captured menu states separately.
"""
import asyncio
import base64
import io
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
from PIL import Image, ImageChops, ImageGrab

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / "Debug/AutomationGame"
SCENARIO = os.environ.get("PHOBOS_TEST_SCENARIO") == "1"
GPU = os.environ.get("PHOBOS_TEST_GPU") == "1"
CAPTURE = os.environ.get("PHOBOS_TEST_CAPTURE") == "1"
DRIVER = os.environ.get("PHOBOS_TEST_DRIVER", "")
assert DRIVER in ("", "direct3d12", "vulkan")
MIRROR = os.environ.get("PHOBOS_GPU_MIRROR") == "1"
OUT = ROOT / ("Debug/AutomationTests/game-input-scenario" if SCENARIO else "Debug/AutomationTests/game-input-native")
if GPU:
    OUT = OUT.with_name(OUT.name + "-gpu")
if DRIVER:
    OUT = OUT.with_name(OUT.name + "-" + DRIVER)
if MIRROR:
    OUT = OUT.with_name(OUT.name + "-mirror")
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
kernel.WritePrivateProfileStringW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR]
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
            async def screenshot(source, tag, **options):
                result = await client.call_tool("game_screenshot", {"source": source, "max_edge": 0, **options},
                    meta={"com.phobos/instance_id": instance})
                data = result.model_dump(by_alias=True)
                assert not data["isError"], data
                images = [item for item in result.content if isinstance(item, types.ImageContent)]
                assert len(images) == 1 and images[0].mime_type == "image/png"
                png = base64.b64decode(images[0].data, validate=True)
                decoded = Image.open(io.BytesIO(png)).convert("RGBA")
                metadata = data["structuredContent"]
                assert decoded.size == (metadata["width"], metadata["height"])
                assert metadata["source"] == source and metadata["valid_capture"]
                assert metadata["timestamp_unix_ms"] > 0
                if source == "ddraw" or tag != "menu":
                    assert len(decoded.getcolors(decoded.width * decoded.height) or []) > 8, "capture has no meaningful pixel variation"
                assert decoded.getchannel("A").getextrema() == (255,255)
                (OUT / f"native-{tag}-{source}.png").write_bytes(png)
                evidence.append({"native_capture": metadata, "png_bytes": len(png)})
                (OUT / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
                return decoded, metadata
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
            if CAPTURE:
                await screenshot("ddraw", "menu")
                if GPU:
                    await screenshot("gpu", "menu")
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
                if CAPTURE:
                    ddraw, ddraw_meta = await screenshot("ddraw", "paused")
                    cropped, _ = await screenshot("ddraw", "crop", crop={"x": 220, "y": 210, "width": 400, "height": 240}, max_edge=200)
                    assert cropped.size == (200, 120)
                    if GPU:
                        gpu_image, gpu_meta = await screenshot("gpu", "paused")
                        assert gpu_meta["simulation_frame"] == ddraw_meta["simulation_frame"]
                        assert gpu_meta["output_mode"] == ("mirror" if MIRROR else "independent")
                        if DRIVER:
                            assert gpu_meta["backend"] == {"vulkan": "Vulkan", "direct3d12": "D3D12"}[DRIVER]
                        core = (220, 210, 620, 450)
                        if MIRROR:
                            difference = ImageChops.difference(ddraw.crop(core).convert("RGB"), gpu_image.crop(core).convert("RGB"))
                            # UNORM conversion can differ by one; render-only particles can
                            # change between two calls even while simulation is paused.
                            changed = sum(max(pixel) > 1 for pixel in difference.getdata())
                            fraction = changed / (difference.width * difference.height)
                            evidence.append({"mirror_core_fraction_over_one": fraction, "synchronization": "best_effort"})
                            assert fraction <= .001, "mirror differs beyond UNORM rounding and sparse render-only updates"
                        else:
                            assert gpu_image.crop(core).convert("RGB").getbbox() is None, "DDraw map leaked into independent output"
                            assert ddraw.crop(core).convert("RGB").getbbox() is not None
                        previous = gpu_meta["gpu_present_id"]
                        count = min(50, max(1, int(os.environ.get("PHOBOS_TEST_CAPTURE_COUNT", "5"))))
                        durations = []
                        for index in range(count):
                            before = time.monotonic()
                            _, metadata = await screenshot("gpu", f"repeat-{index}", max_edge=640)
                            durations.append(time.monotonic() - before)
                            assert metadata["gpu_present_id"] > previous
                            previous = metadata["gpu_present_id"]
                        evidence.append({"capture_repeat_seconds": durations})
                        gpu_hwnd = (await status())["gpu_window_hwnd"]
                        assert user.SetWindowPos(gpu_hwnd, None, 0, 0, 900, 700, 0x16) # NOACTIVATE, NOMOVE, NOZORDER
                        await asyncio.sleep(.3)
                        resized, metadata = await screenshot("gpu", "resized")
                        rect = wintypes.RECT(); assert user.GetClientRect(gpu_hwnd, ctypes.byref(rect))
                        assert resized.size == (rect.right, rect.bottom)
                        assert any(value > 0 for value in metadata["letterbox"]), metadata
                        if metadata["letterbox"][1] >= 1:
                            assert resized.crop((0,0,resized.width,1)).convert("RGB").getbbox() is None
                        user.ShowWindow(gpu_hwnd, 7) # SW_SHOWMINNOACTIVE
                        await asyncio.sleep(.2)
                        before = time.monotonic()
                        timed = (await client.call_tool("game_screenshot", {"source":"gpu", "timeout_ms":200},
                            meta={"com.phobos/instance_id":instance})).model_dump(by_alias=True)
                        assert time.monotonic() - before < 2, timed["structuredContent"]
                        # Some drivers keep supplying a swapchain image after minimization.
                        # A fresh completed capture is valid; do not manufacture a timeout.
                        if not timed["isError"]:
                            assert timed["structuredContent"]["gpu_present_id"] > metadata["gpu_present_id"]
                        evidence.append({"minimize_request_iconic": bool(user.IsIconic(gpu_hwnd)),
                            "minimize_capture_is_error": timed["isError"], "minimize_capture": timed["structuredContent"]})
                        user.ShowWindow(gpu_hwnd, 4) # SW_SHOWNOACTIVATE
                        await asyncio.sleep(.3)
                        await screenshot("gpu", "after-restore")
                        # Close only the test's SDL presentation window. DDraw capture must survive.
                        user.PostMessageW(gpu_hwnd, 0x0010, 0, 0)
                        await asyncio.sleep(.3)
                        hidden = (await client.call_tool("game_screenshot", {"source":"gpu", "timeout_ms":500},
                            meta={"com.phobos/instance_id":instance})).model_dump(by_alias=True)
                        assert hidden["isError"] and hidden["structuredContent"]["reason"] == "gpu_renderer_not_running", hidden
                        await screenshot("ddraw", "gpu-hidden")
                        evidence.append({"hidden_gpu_rejected": True, "ddraw_after_gpu_hidden": True})
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
    ini = GAME / "RA2MD.INI"
    original_ini = ini.read_bytes() if DRIVER else None
    if DRIVER:
        assert kernel.WritePrivateProfileStringW("Phobos", "GPURender.Driver", DRIVER, str(ini))
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
        if original_ini is not None:
            ini.write_bytes(original_ini)
