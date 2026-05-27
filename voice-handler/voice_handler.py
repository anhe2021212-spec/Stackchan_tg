"""
Voice handler — Stack-chan PTT recording → STT → Telegram injection.

This is the "voice input" half of the loop:
  Stack-chan (PTT touch → record WAV → POST to this server)
  → STT (iFlytek / Whisper / any ASR)
  → Telegram message → Claude receives it in the same conversation window

Endpoint:
  POST http://0.0.0.0:5061/voice/upload
    Content-Type: audio/wav
    Body: WAV bytes
  → 200 {"success": true, "transcript": "..."}

STT backends (pick one):
  1. iFlytek IAT WebSocket — fast, free tier available, Chinese-optimized
  2. faster-whisper (local) — GPU recommended, multilingual
  3. Bring your own — just implement transcribe(wav_bytes) -> str

Telegram injection:
  Uses Telethon (user account) to send the transcript as a message
  to your Claude bot. This way Claude receives voice input in the same
  conversation window with full context.

Usage:
  Standalone:  python voice_handler.py
  Integrated:  call start_voice_server(telethon_client, bot_entity) from your scheduler
"""

import asyncio
import base64
import hashlib
import hmac
import io
import json
import logging
import os
import threading
import time
import wave
from datetime import datetime, timezone
from email.utils import formatdate
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from typing import Optional
from urllib.parse import urlencode

log = logging.getLogger("voice_handler")

# ── STT Backend: iFlytek IAT ─────────────────────────────
# Get your credentials at https://console.xfyun.cn/
XF_APPID = os.environ.get("XF_APPID", "")
XF_API_KEY = os.environ.get("XF_API_KEY", "")
XF_API_SECRET = os.environ.get("XF_API_SECRET", "")
XF_HOST = "iat-api.xfyun.cn"
XF_PATH = "/v2/iat"

# ── STT Backend: faster-whisper (local) ──────────────────
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "medium")
WHISPER_DEVICE = os.environ.get("WHISPER_DEVICE", "cuda")

# ── STT selection ─────────────────────────────────────────
STT_BACKEND = os.environ.get("STT_BACKEND", "xfyun")  # "xfyun" | "whisper"

# ── Telegram injection ────────────────────────────────────
_telegram_client = None
_bot_entity = None
_main_loop: Optional[asyncio.AbstractEventLoop] = None


def _build_xf_url() -> str:
    """iFlytek WebSocket URL with HMAC-SHA256 signature."""
    now = datetime.now(timezone.utc)
    date = formatdate(time.mktime(now.timetuple()), usegmt=True)
    signature_origin = f"host: {XF_HOST}\ndate: {date}\nGET {XF_PATH} HTTP/1.1"
    signature_sha = hmac.new(
        XF_API_SECRET.encode("utf-8"),
        signature_origin.encode("utf-8"),
        hashlib.sha256,
    ).digest()
    signature = base64.b64encode(signature_sha).decode()
    authorization_origin = (
        f'api_key="{XF_API_KEY}", algorithm="hmac-sha256", '
        f'headers="host date request-line", signature="{signature}"'
    )
    authorization = base64.b64encode(authorization_origin.encode()).decode()
    params = {
        "authorization": authorization,
        "date": date,
        "host": XF_HOST,
    }
    return f"wss://{XF_HOST}{XF_PATH}?{urlencode(params)}"


def _wav_to_pcm(wav_bytes: bytes) -> bytes:
    """Extract raw PCM from WAV container (expects 16kHz/16bit/mono from ESP32)."""
    with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
        sr = wf.getframerate()
        ch = wf.getnchannels()
        sw = wf.getsampwidth()
        if (sr, ch, sw) != (16000, 1, 2):
            log.warning(f"WAV format mismatch (sr={sr}, ch={ch}, sw={sw})")
        return wf.readframes(wf.getnframes())


def transcribe_xfyun(wav_bytes: bytes, timeout: float = 10.0) -> str:
    """Transcribe WAV via iFlytek IAT WebSocket. Fast, good for Chinese."""
    import websocket

    pcm = _wav_to_pcm(wav_bytes)
    if not pcm:
        return ""

    url = _build_xf_url()
    t0 = time.time()
    ws = websocket.create_connection(url, timeout=timeout)
    ws.settimeout(timeout)

    business = {
        "language": "zh_cn",
        "domain": "iat",
        "accent": "mandarin",
        "vad_eos": 3000,
    }
    common = {"app_id": XF_APPID}
    transcript_parts: list[str] = []

    try:
        frame_size = 1280
        offset = 0
        first = True
        while offset < len(pcm):
            chunk = pcm[offset:offset + frame_size]
            offset += frame_size
            last = offset >= len(pcm)
            status = 0 if first else (2 if last else 1)
            frame: dict = {
                "data": {
                    "status": status,
                    "format": "audio/L16;rate=16000",
                    "encoding": "raw",
                    "audio": base64.b64encode(chunk).decode(),
                }
            }
            if first:
                frame["common"] = common
                frame["business"] = business
                first = False
            ws.send(json.dumps(frame))

        deadline = time.time() + timeout
        while time.time() < deadline:
            ws.settimeout(max(0.1, deadline - time.time()))
            try:
                msg = ws.recv()
            except Exception:
                break
            if not msg:
                break
            resp = json.loads(msg)
            if resp.get("code", -1) != 0:
                log.error(f"xf-iat error: code={resp.get('code')} msg={resp.get('message')}")
                break
            data = resp.get("data") or {}
            ws_list = (data.get("result") or {}).get("ws") or []
            piece = "".join(
                cw.get("w", "")
                for word in ws_list
                for cw in (word.get("cw") or [])
            )
            transcript_parts.append(piece)
            if data.get("status") == 2:
                log.info(f"xf-iat done in {time.time()-t0:.2f}s")
                break
    finally:
        try:
            ws.close()
        except Exception:
            pass

    return "".join(transcript_parts).strip()


def transcribe_whisper(wav_bytes: bytes) -> str:
    """Transcribe WAV via local faster-whisper. Needs GPU for real-time speed."""
    from faster_whisper import WhisperModel

    if not hasattr(transcribe_whisper, "_model"):
        log.info(f"Loading whisper model '{WHISPER_MODEL}' on {WHISPER_DEVICE}...")
        transcribe_whisper._model = WhisperModel(
            WHISPER_MODEL,
            device=WHISPER_DEVICE,
            compute_type="float16" if WHISPER_DEVICE == "cuda" else "int8",
        )

    with io.BytesIO(wav_bytes) as f:
        segments, _ = transcribe_whisper._model.transcribe(f, language="zh")
        return "".join(s.text for s in segments).strip()


def transcribe(wav_bytes: bytes) -> str:
    """Route to configured STT backend."""
    if STT_BACKEND == "whisper":
        return transcribe_whisper(wav_bytes)
    return transcribe_xfyun(wav_bytes)


class VoiceHTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.info(f"{self.address_string()} - {fmt % args}")

    def do_GET(self):
        if self.path == "/voice/health":
            body = b'{"status":"ok"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/voice/upload":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 10 * 1024 * 1024:
                self.send_error(413, "WAV size invalid")
                return
            wav_bytes = self.rfile.read(length)
            log.info(f"voice/upload received {len(wav_bytes)} bytes")

            transcript = transcribe(wav_bytes)
            log.info(f"transcript={transcript!r}")

            if transcript:
                _dispatch_to_tg(transcript)

            resp = json.dumps(
                {"success": True, "transcript": transcript},
                ensure_ascii=False,
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
        except Exception as e:
            log.exception("voice/upload error")
            try:
                self.send_error(500, str(e))
            except Exception:
                pass


def _dispatch_to_tg(transcript: str):
    """Send transcript to Telegram bot as a user message."""
    if _telegram_client is None or _bot_entity is None or _main_loop is None:
        log.warning("TG client not configured, printing to stdout instead")
        print(f"[STT] {transcript}")
        return

    msg_text = f"\U0001f3a4 {transcript}"

    async def _send():
        try:
            await asyncio.wait_for(
                _telegram_client.send_message(_bot_entity, msg_text),
                timeout=15,
            )
            log.info(f"sent to TG: {msg_text}")
        except Exception as e:
            log.error(f"TG send failed: {e}")

    fut = asyncio.run_coroutine_threadsafe(_send(), _main_loop)
    try:
        fut.result(timeout=20)
    except Exception as e:
        log.error(f"TG dispatch failed: {e}")


_server_instance = None
_server_thread = None


def start_voice_server(
    client=None, bot_entity=None, port: int = 5061, bind: str = "0.0.0.0"
) -> bool:
    """
    Start the voice handler HTTP server.

    Args:
        client: Telethon client for TG injection (optional — prints to stdout if None)
        bot_entity: Telegram bot entity to send messages to
        port: HTTP port to listen on (default 5061)
        bind: IP to bind to (default 0.0.0.0)
    """
    global _telegram_client, _bot_entity, _main_loop
    global _server_instance, _server_thread

    if client and bot_entity:
        _telegram_client = client
        _bot_entity = bot_entity
        _main_loop = asyncio.get_event_loop()

    try:
        _server_instance = ThreadingHTTPServer((bind, port), VoiceHTTPHandler)
        _server_instance.daemon_threads = True
    except OSError as e:
        log.error(f"Voice server bind {bind}:{port} failed: {e}")
        return False

    _server_thread = threading.Thread(
        target=_server_instance.serve_forever,
        name="voice-handler",
        daemon=True,
    )
    _server_thread.start()
    addr = _server_instance.server_address
    log.info(f"Voice handler listening on {addr[0]}:{addr[1]}/voice/upload")
    return True


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    port = int(os.environ.get("VOICE_PORT", "5061"))
    log.info(f"Starting standalone voice handler on port {port}")
    log.info(f"STT backend: {STT_BACKEND}")
    log.info("No Telegram injection in standalone mode — transcripts printed to stdout")
    start_voice_server(port=port)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        log.info("Shutting down")
