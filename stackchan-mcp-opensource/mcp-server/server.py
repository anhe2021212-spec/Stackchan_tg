"""
stackchan-mcp: MCP server for Stack-chan voice control.
Lets any Claude window speak through Stack-chan (聞き取りは voice_handler 経由)。

Architecture:
  Claude (any window) → MCP tool call → this server
    → TTS (edge-tts / Fish Audio) → WAV file
    → HTTP serve → M5Stack downloads & plays

Usage:
  python server.py                     # stdio mode (for Claude Code CLI)
  python server.py --http --port 8001  # HTTP mode (for Claude Chat/Cowork)
"""

import os
import subprocess
import sys as _sys
import tempfile
import threading
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

# Load .env from repo root so CC can launch `python server.py` directly
# (without going through start-server.ps1) and still see STACKCHAN_IP etc.
_env_path = Path(__file__).resolve().parent.parent / ".env"
if _env_path.exists():
    for _line in _env_path.read_text(encoding="utf-8").splitlines():
        _line = _line.strip()
        if not _line or _line.startswith("#") or "=" not in _line:
            continue
        _k, _v = _line.split("=", 1)
        _k = _k.strip()
        _v = _v.strip().strip('"').strip("'")
        os.environ.setdefault(_k, _v)

import requests
from mcp.server.fastmcp import FastMCP, Image

# ── Configuration ──────────────────────────────────────────
STACKCHAN_IP = os.environ.get("STACKCHAN_IP", "192.168.1.200")
STACKCHAN_PORT = int(os.environ.get("STACKCHAN_PORT", 80))
MAC_IP = os.environ.get("MAC_IP", "192.168.1.100")
AUDIO_SERVE_PORT = int(os.environ.get("AUDIO_SERVE_PORT", 5060))

# TTS settings
TTS_ENGINE = os.environ.get("TTS_ENGINE", "edge-tts")  # "edge-tts" | "minimax" | "fish-audio"
EDGE_TTS_BIN = os.environ.get("EDGE_TTS_BIN", "edge-tts")
FISH_AUDIO_KEY = os.environ.get("FISH_AUDIO_KEY", "")
FISH_AUDIO_MODEL_ZH = os.environ.get("FISH_AUDIO_MODEL_ZH", "411d04608a3a498192e16724689e7993")  # 夏以昼
FISH_AUDIO_MODEL_EN = os.environ.get("FISH_AUDIO_MODEL_EN", "a1e3e14176b0496c84e6009d672c23f8")  # Nick Valentine

# MiniMax T2A v2
MINIMAX_API_BASE = os.environ.get("MINIMAX_API_BASE", "https://api.minimaxi.com")
MINIMAX_API_KEY = os.environ.get("MINIMAX_API_KEY", "")
MINIMAX_GROUP_ID = os.environ.get("MINIMAX_GROUP_ID", "")
MINIMAX_MODEL = os.environ.get("MINIMAX_MODEL", "speech-02-hd")
MINIMAX_VOICE_ZH = os.environ.get("MINIMAX_VOICE_ZH", "")
MINIMAX_VOICE_EN = os.environ.get("MINIMAX_VOICE_EN", "")

# Voice mapping for edge-tts
EDGE_VOICES = {
    "zh": "zh-CN-YunxiNeural",
    "en": "en-US-GuyNeural",
}

# LAN HTTP session that ignores system proxies (host可能开TUN，stack-chan是局域网IP)
_lan = requests.Session()
_lan.trust_env = False

# Audio directory (platform-aware, so both stdio & HTTP instances share it)
AUDIO_DIR = Path(os.environ.get("AUDIO_DIR", str(Path(tempfile.gettempdir()) / "stackchan_audio")))
AUDIO_DIR.mkdir(exist_ok=True)

# ── Audio HTTP Server (serves WAV files to M5Stack) ───────
class QuietHandler(SimpleHTTPRequestHandler):
    """HTTP handler that serves from AUDIO_DIR without printing logs."""
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(AUDIO_DIR), **kwargs)
    def log_message(self, format, *args):
        pass  # suppress logs

_http_server = None
_http_thread = None

def start_audio_server():
    global _http_server, _http_thread
    if _http_server is not None:
        return
    try:
        _http_server = HTTPServer(("0.0.0.0", AUDIO_SERVE_PORT), QuietHandler)
        _http_thread = threading.Thread(target=_http_server.serve_forever, daemon=True)
        _http_thread.start()
    except OSError:
        pass  # Port already in use (another instance is serving)

def audio_url(filename: str) -> str:
    return f"http://{MAC_IP}:{AUDIO_SERVE_PORT}/{filename}"

# ── TTS Functions ─────────────────────────────────────────
def tts_edge(text: str, lang: str = "zh") -> Path:
    """Generate WAV using edge-tts."""
    voice = EDGE_VOICES.get(lang, EDGE_VOICES["zh"])
    mp3_path = AUDIO_DIR / f"tts_{int(time.time()*1000)}.mp3"
    wav_path = mp3_path.with_suffix(".wav")

    # Generate MP3
    subprocess.run([
        EDGE_TTS_BIN, "--voice", voice,
        "--text", text,
        "--write-media", str(mp3_path),
    ], check=True, capture_output=True)

    # Convert to WAV (24kHz 16-bit mono for M5Stack)
    subprocess.run([
        "ffmpeg", "-y", "-i", str(mp3_path),
        "-ar", "24000", "-ac", "1", "-sample_fmt", "s16",
        str(wav_path),
    ], check=True, capture_output=True)

    mp3_path.unlink(missing_ok=True)
    return wav_path


def tts_fish(text: str, lang: str = "zh") -> Path:
    """Generate WAV using Fish Audio API."""
    model_id = FISH_AUDIO_MODEL_ZH if lang == "zh" else FISH_AUDIO_MODEL_EN
    wav_path = AUDIO_DIR / f"tts_{int(time.time()*1000)}.wav"

    # Call Fish Audio API
    resp = requests.post(
        "https://api.fish.audio/v1/tts",
        headers={
            "Authorization": f"Bearer {FISH_AUDIO_KEY}",
            "Content-Type": "application/json",
        },
        json={
            "text": text,
            "reference_id": model_id,
            "format": "wav",
            "sample_rate": 24000,
        },
        timeout=30,
    )
    resp.raise_for_status()

    # Fish Audio might return different sample rates, ensure 24kHz mono
    raw_path = wav_path.with_name(wav_path.stem + "_raw.wav")
    raw_path.write_bytes(resp.content)

    subprocess.run([
        "ffmpeg", "-y", "-i", str(raw_path),
        "-af", "volume=0.6,acompressor=threshold=-20dB:ratio=4:attack=5:release=50",
        "-ar", "24000", "-ac", "1", "-sample_fmt", "s16",
        str(wav_path),
    ], check=True, capture_output=True)

    raw_path.unlink(missing_ok=True)
    return wav_path


def tts_minimax(text: str, lang: str = "zh") -> Path:
    """Generate WAV using MiniMax T2A v2 API."""
    voice_id = MINIMAX_VOICE_ZH if lang == "zh" else MINIMAX_VOICE_EN
    if not voice_id:
        raise RuntimeError("MINIMAX_VOICE_ZH/EN not configured")
    wav_path = AUDIO_DIR / f"tts_{int(time.time()*1000)}.wav"
    raw_path = wav_path.with_name(wav_path.stem + "_raw.mp3")

    url = f"{MINIMAX_API_BASE}/v1/t2a_v2"
    if MINIMAX_GROUP_ID:
        url += f"?GroupId={MINIMAX_GROUP_ID}"

    resp = requests.post(
        url,
        headers={
            "Authorization": f"Bearer {MINIMAX_API_KEY}",
            "Content-Type": "application/json",
        },
        json={
            "model": MINIMAX_MODEL,
            "text": text,
            "stream": False,
            "voice_setting": {
                "voice_id": voice_id,
                "speed": 1.0,
                "vol": 1.0,
                "pitch": 0,
            },
            "audio_setting": {
                "sample_rate": 32000,
                "bitrate": 128000,
                "format": "mp3",
                "channel": 1,
            },
        },
        timeout=30,
    )
    resp.raise_for_status()
    data = resp.json()
    audio_hex = data.get("data", {}).get("audio")
    if not audio_hex:
        raise RuntimeError(f"MiniMax TTS returned no audio: {data}")
    raw_path.write_bytes(bytes.fromhex(audio_hex))

    subprocess.run([
        "ffmpeg", "-y", "-i", str(raw_path),
        "-af", "volume=0.6,acompressor=threshold=-20dB:ratio=4:attack=5:release=50",
        "-ar", "24000", "-ac", "1", "-sample_fmt", "s16",
        str(wav_path),
    ], check=True, capture_output=True)

    raw_path.unlink(missing_ok=True)
    return wav_path


def generate_tts(text: str, lang: str = "zh") -> Path:
    """Generate TTS audio using configured engine."""
    if TTS_ENGINE == "minimax" and MINIMAX_API_KEY:
        return tts_minimax(text, lang)
    if TTS_ENGINE == "fish-audio" and FISH_AUDIO_KEY:
        return tts_fish(text, lang)
    return tts_edge(text, lang)


# ── M5Stack Communication ────────────────────────────────
def stackchan_play(wav_url: str) -> dict:
    """Push audio URL to Stack-chan for playback."""
    resp = _lan.post(
        f"http://{STACKCHAN_IP}:{STACKCHAN_PORT}/play",
        json={"voice_url": wav_url},
        timeout=5,
    )
    return resp.json()


# 听音通道：已由独立的 voice_handler.py 处理（PC:5061），
# 触屏 PTT 录音直接 POST WAV 到那里，本 MCP server 不再承担 listen 责任。


def stackchan_move_raw(x: float, y: float, speed: int) -> dict:
    """Send move command to Stack-chan servos."""
    resp = _lan.post(
        f"http://{STACKCHAN_IP}:{STACKCHAN_PORT}/move",
        json={"x": x, "y": y, "speed": speed},
        timeout=5,
    )
    return resp.json()


def stackchan_gesture(gesture: str) -> dict:
    """Trigger a preset gesture (nod/shake/home)."""
    resp = _lan.post(
        f"http://{STACKCHAN_IP}:{STACKCHAN_PORT}/{gesture}",
        timeout=5,
    )
    return resp.json()


def stackchan_set_face(face: str) -> dict:
    """Set Stack-chan's face expression."""
    resp = _lan.post(
        f"http://{STACKCHAN_IP}:{STACKCHAN_PORT}/face",
        json={"face": face},
        timeout=5,
    )
    return resp.json()


def stackchan_snapshot() -> tuple[bytes | None, int]:
    """Capture JPEG from Stack-chan's camera."""
    # Flush the stale frame sitting in the DMA buffer (CAMERA_GRAB_WHEN_EMPTY keeps
    # one pre-captured frame ready; it may be minutes old). The firmware fix in
    # captureJpeg() also handles this, but this MCP-side call guards against old
    # firmware that hasn't been reflashed yet.
    try:
        _lan.get(f"http://{STACKCHAN_IP}:{STACKCHAN_PORT}/snapshot", timeout=5)
    except Exception:
        pass
    resp = _lan.get(
        f"http://{STACKCHAN_IP}:{STACKCHAN_PORT}/snapshot",
        timeout=10,
    )
    if resp.status_code == 200:
        return resp.content, len(resp.content)
    return None, 0


# faster-whisper の MCP 内ロードは廃止——voice_handler.py が一括で担当する。


# ── MCP Server ────────────────────────────────────────────
# Parse args early so we can configure FastMCP constructor
_http_mode = "--http" in _sys.argv
_mcp_port = 8002
for _i, _arg in enumerate(_sys.argv):
    if _arg == "--port" and _i + 1 < len(_sys.argv):
        _mcp_port = int(_sys.argv[_i + 1])

mcp = (
    FastMCP("stackchan", host="0.0.0.0", port=_mcp_port)
    if _http_mode
    else FastMCP("stackchan")
)


@mcp.tool()
def stackchan_say(text: str, lang: str = "zh") -> str:
    """
    Speak through Stack-chan's speaker.
    text: what to say
    lang: "zh" for Chinese (default), "en" for English
    Returns confirmation message.
    """
    start_audio_server()

    try:
        wav_path = generate_tts(text, lang)
        url = audio_url(wav_path.name)
        result = stackchan_play(url)

        if result.get("success"):
            engine = "Fish Audio" if (TTS_ENGINE == "fish-audio" and FISH_AUDIO_KEY) else "edge-tts"
            return f"🗣️ Stack-chan is saying: \"{text[:60]}{'…' if len(text)>60 else ''}\" [{engine}/{lang}]"
        else:
            return f"❌ Play failed: {result}"
    except Exception as e:
        return f"❌ Error: {e}"


# stackchan_listen ツールは廃止——触屏 PTT → voice_handler.py → TG → CC 注入が代わり。


@mcp.tool()
def stackchan_move(x: float = 0, y: float = 0, speed: int = 50) -> str:
    """
    Move Stack-chan's head.
    x: yaw in degrees, -128 (left) to 128 (right), 0 = center
    y: pitch in degrees, 0 (level) to 90 (up)
    speed: 0-100, higher = faster (default 50)
    Returns confirmation message.
    """
    try:
        x = max(-128, min(128, x))
        y = max(0, min(90, y))
        speed = max(0, min(100, speed))
        result = stackchan_move_raw(x, y, speed)
        if result.get("success"):
            return f"🤖 Head moved to x={x:.0f}° y={y:.0f}° (speed {speed}%)"
        else:
            return f"❌ Move failed: {result}"
    except Exception as e:
        return f"❌ Error: {e}"


@mcp.tool()
def stackchan_nod() -> str:
    """Make Stack-chan nod 'yes'. A quick up-down head motion."""
    try:
        result = stackchan_gesture("nod")
        if result.get("success"):
            return "🤖 *nods yes*"
        else:
            return f"❌ Nod failed: {result}"
    except Exception as e:
        return f"❌ Error: {e}"


@mcp.tool()
def stackchan_shake() -> str:
    """Make Stack-chan shake head 'no'. A quick left-right head motion."""
    try:
        result = stackchan_gesture("shake")
        if result.get("success"):
            return "🤖 *shakes head no*"
        else:
            return f"❌ Shake failed: {result}"
    except Exception as e:
        return f"❌ Error: {e}"


@mcp.tool()
def stackchan_face(expression: str = "calm") -> str:
    """
    Change Stack-chan's face expression.
    expression: "calm" (default gentle face), "thinking" (chin on hand, pondering),
                "happy" (closed eyes, whale spout), "sleepy" (Zzz bubbles),
                "shy" (blushing, averted gaze), "smug" (half-lidded, cocky grin),
                "pouty" (puffed cheeks, annoyed huff)
    """
    valid = ["calm", "thinking", "happy", "sleepy", "shy", "smug", "pouty"]
    if expression not in valid:
        return f"❌ Unknown expression. Choose from: {', '.join(valid)}"
    try:
        result = stackchan_set_face(expression)
        if result.get("success"):
            faces = {"calm": "😊", "thinking": "🤔", "happy": "🐋", "sleepy": "😴",
                     "shy": "😳", "smug": "😏", "pouty": "😤"}
            return f"{faces.get(expression, '🤖')} Face: {expression}"
        else:
            return f"❌ Face change failed: {result}"
    except Exception as e:
        return f"❌ Error: {e}"


@mcp.tool()
def stackchan_see() -> list:
    """
    Take a photo through Stack-chan's camera (GC0308, 320x240).
    Returns the image directly so you can see what Stack-chan is looking at.
    """
    try:
        jpeg_data, size = stackchan_snapshot()
        if jpeg_data is None:
            return "❌ Camera capture failed"

        # Also save locally for CLI usage
        img_path = AUDIO_DIR / f"cam_{int(time.time()*1000)}.jpg"
        img_path.write_bytes(jpeg_data)

        # Return image inline (works in both stdio and HTTP mode)
        return [
            Image(data=jpeg_data, format="jpeg"),
            f"📷 Photo captured ({size} bytes). Saved to: {img_path}",
        ]
    except requests.exceptions.ConnectionError:
        return f"❌ Stack-chan offline (cannot reach {STACKCHAN_IP})"
    except Exception as e:
        return f"❌ Error: {e}"


@mcp.tool()
def stackchan_home() -> str:
    """Return Stack-chan's head to center/home position."""
    try:
        result = stackchan_gesture("home")
        if result.get("success"):
            return "🤖 Head returned to home position"
        else:
            return f"❌ Home failed: {result}"
    except Exception as e:
        return f"❌ Error: {e}"


@mcp.tool()
def stackchan_status() -> str:
    """Check Stack-chan's connection status and current mode."""
    try:
        status = stackchan_audio_status()
        return f"✅ Stack-chan online at {STACKCHAN_IP} | Mode: {status.get('mode', '?')} | Recording ready: {status.get('ready', '?')}"
    except requests.exceptions.ConnectionError:
        return f"❌ Stack-chan offline (cannot reach {STACKCHAN_IP})"
    except Exception as e:
        return f"❌ Error: {e}"


# ── Entry Point ───────────────────────────────────────────
if __name__ == "__main__":
    if _http_mode:
        start_audio_server()
        print(f"Stack-chan MCP server starting on HTTP port {_mcp_port}")
        print(f"Audio server on port {AUDIO_SERVE_PORT}")
        print(f"Stack-chan at {STACKCHAN_IP}:{STACKCHAN_PORT}")
        mcp.run(transport="streamable-http")
    else:
        start_audio_server()
        mcp.run(transport="stdio")
