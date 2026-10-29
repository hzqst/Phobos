"""Run against the native x86 fixture; requires mcp==2.3.0 (or 1.25.0 for legacy)."""
import asyncio
import importlib.metadata
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.request

URL = "http://127.0.0.1:18743/mcp"
TOKEN = secrets.token_hex(32)


async def sdk_tests():
    if importlib.metadata.version("mcp").startswith("2."):
        import httpx2
        from mcp import Client
        from mcp.client.streamable_http import streamable_http_client
        for mode, version in [("auto", "2026-07-28"), ("legacy", "2025-11-25")]:
            async with httpx2.AsyncClient(headers={"Authorization": "Bearer " + TOKEN}, timeout=5) as http:
                async with Client(streamable_http_client(URL, http_client=http), mode=mode) as client:
                    assert version == client.protocol_version
                    tools = await client.list_tools()
                    assert ["game_status"] == [t.name for t in tools.tools]
                    result = await client.call_tool("game_status", {})
                    data = result.model_dump(by_alias=True)
                    assert "protocol-test" == data["structuredContent"]["instance_id"]
                    assert "text" == data["content"][0]["type"]
            print(f"SDK 2.3.0 {version}: discover/initialize, list, call PASS")
    else:
        from mcp import ClientSession
        from mcp.client.streamable_http import streamablehttp_client
        async with streamablehttp_client(URL, headers={"Authorization": "Bearer " + TOKEN}) as (read, write, _):
            async with ClientSession(read, write) as client:
                init = await client.initialize()
                assert "2025-11-25" == init.protocolVersion
                assert ["game_status"] == [t.name for t in (await client.list_tools()).tools]
                result = await client.call_tool("game_status", {})
                assert "protocol-test" == result.structuredContent["instance_id"]
        print("SDK 1.25.0 2025-11-25 initialize, list, call PASS")


def negative_tests():
    headers = {"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json",
               "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2026-07-28",
               "Mcp-Method": "server/discover"}
    body = {"jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": {"_meta": {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientInfo": {"name": "wire-test", "version": "1"},
        "io.modelcontextprotocol/clientCapabilities": {}}}}

    def post(data=None, changes=None):
        req = urllib.request.Request(URL, data=data if data is not None else json.dumps(body).encode(),
                                     headers=headers | (changes or {}))
        try:
            with urllib.request.urlopen(req, timeout=5) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    assert 200 == post()[0]
    assert 401 == post(changes={"Authorization": "Bearer invalid"})[0]
    assert 403 == post(changes={"Origin": "http://evil.invalid"})[0]
    assert 403 == post(changes={"Host": "evil.invalid:18743"})[0]
    assert -32020 == json.loads(post(changes={"Mcp-Method": "tools/list"})[1])["error"]["code"]
    assert -32700 == json.loads(post(b"{")[1])["error"]["code"]
    assert 413 == post(b" " * 65537)[0]
    assert 400 == post(b"[" * 33 + b"]" * 33)[0]
    for _ in range(20):
        assert 200 == post()[0]
    with socket.create_connection(("127.0.0.1", 18743), timeout=5) as slow:
        slow.sendall(b"POST /mcp HTTP/1.1\r\n")
        assert b"" == slow.recv(1)  # Absolute socket deadline, including slow headers.
    print("HTTP auth/origin/host/mismatch/JSON/limits/repeat/deadline PASS")


if __name__ == "__main__":
    exe = Path(__file__).resolve().parents[2] / "Debug/AutomationTests/HTTPHarness.exe"
    process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, text=True, env=os.environ | {"PHOBOS_MCP_TOKEN": TOKEN})
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError(f"fixture exited: {process.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", 18743), timeout=.1):
                    break
            except OSError:
                time.sleep(.05)
        else:
            raise TimeoutError("fixture readiness timed out")
        asyncio.run(sdk_tests())
        negative_tests()
    finally:
        process.communicate("stop\n", timeout=5)
        assert 0 == process.returncode
