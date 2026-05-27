import importlib.util
import sys
import types
from pathlib import Path

import pytest


class FakeFastMCP:
    def __init__(self, *args, **kwargs):
        self.args = args
        self.kwargs = kwargs
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func

        return decorator

    def run(self, *args, **kwargs):
        return None


@pytest.fixture()
def server_module(monkeypatch):
    fake_mcp_package = types.ModuleType("mcp")
    fake_mcp_server = types.ModuleType("mcp.server")
    fake_fastmcp = types.ModuleType("mcp.server.fastmcp")
    fake_fastmcp.FastMCP = FakeFastMCP
    fake_fastmcp.Image = lambda data, format: {"data": data, "format": format}

    monkeypatch.setitem(sys.modules, "mcp", fake_mcp_package)
    monkeypatch.setitem(sys.modules, "mcp.server", fake_mcp_server)
    monkeypatch.setitem(sys.modules, "mcp.server.fastmcp", fake_fastmcp)
    monkeypatch.setattr(sys, "argv", ["server.py"])

    module_path = Path(__file__).resolve().parents[1] / "mcp-server" / "server.py"
    spec = importlib.util.spec_from_file_location("stackchan_mcp_server_under_test", module_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_expected_mcp_tools_are_registered(server_module):
    assert set(server_module.mcp.tools) == {
        "stackchan_say",
        "stackchan_listen",
        "stackchan_move",
        "stackchan_nod",
        "stackchan_shake",
        "stackchan_face",
        "stackchan_see",
        "stackchan_home",
        "stackchan_status",
    }


def test_audio_url_uses_configured_host_and_port(server_module):
    server_module.MAC_IP = "192.0.2.10"
    server_module.AUDIO_SERVE_PORT = 5099

    assert server_module.audio_url("hello.wav") == "http://192.0.2.10:5099/hello.wav"


def test_move_clamps_inputs_before_http_call(server_module, monkeypatch):
    calls = []

    def fake_move_raw(x, y, speed):
        calls.append((x, y, speed))
        return {"success": True}

    monkeypatch.setattr(server_module, "stackchan_move_raw", fake_move_raw)

    result = server_module.stackchan_move(x=999, y=-20, speed=250)

    assert calls == [(128, 0, 100)]
    assert "x=128" in result
    assert "y=0" in result
    assert "speed 100%" in result


def test_invalid_face_is_rejected_without_http_call(server_module, monkeypatch):
    def fail_if_called(_expression):
        raise AssertionError("HTTP face setter should not be called for invalid expressions")

    monkeypatch.setattr(server_module, "stackchan_set_face", fail_if_called)

    assert "Unknown expression" in server_module.stackchan_face("surprised")


def test_listen_does_not_consume_audio_when_not_ready(server_module, monkeypatch):
    monkeypatch.setattr(server_module, "stackchan_audio_status", lambda: {"ready": False})

    def fail_if_called():
        raise AssertionError("GET /audio consumes the device buffer and should not be called")

    monkeypatch.setattr(server_module, "stackchan_get_audio", fail_if_called)

    assert "No recording ready" in server_module.stackchan_listen()
