#include <M5Unified.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "http_server.h"
#include "queue_manager.h"
#include "globals.h"
#include "types.h"
#include "servo_service.h"
#include "camera_service.h"
#include "face_service.h"
#include "mic_service.h"

static WebServer server(80);

// ── 録音バッファ（PSRAMに確保）
static uint8_t* s_wav_buf   = nullptr;
static size_t   s_wav_size  = 0;
static bool     s_wav_ready = false;

// ── モードフラグ（false=APIモード / true=MCPモード）
static bool s_mcp_mode = false;

// ────────────────────────────────────────────
// POST /play
// body: {"voice_url": "http://..."}
// → AudioTaskをキューに積んで再生
// ────────────────────────────────────────────
static void handlePlay() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"json parse error\"}");
        return;
    }

    const char* voice_url = doc["voice_url"] | "";
    if (strlen(voice_url) == 0) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"voice_url required\"}");
        return;
    }

    AudioTask task;
    task.voice_id  = String("mcp_") + String(millis());
    task.voice_url = String(voice_url);
    task.priority  = PRIORITY_NORMAL;
    enqueueAudioTask(task);

    Serial.printf("[HTTP] POST /play -> queued: %s\n", voice_url);
    server.send(200, "application/json", "{\"success\":true}");
}

// ────────────────────────────────────────────
// POST /mode
// body: {"mode": "mcp"} または {"mode": "api"}
// → MCPモード / APIモードを切り替える
// ────────────────────────────────────────────
static void handleMode() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"json parse error\"}");
        return;
    }

    const char* mode = doc["mode"] | "";
    if (strcmp(mode, "mcp") == 0) {
        s_mcp_mode = true;
        // 古い録音を完全クリア
        s_wav_ready = false;
        s_wav_size  = 0; 
        if (s_wav_buf) {
            free(s_wav_buf); 
            s_wav_buf = nullptr;
        }
        Serial.println("[HTTP] Mode -> MCP (buffer cleared)");
        server.send(200, "application/json", "{\"success\":true,\"mode\":\"mcp\"}");
    } else if (strcmp(mode, "api") == 0) {
        s_mcp_mode = false;
        Serial.println("[HTTP] Mode -> API");
        server.send(200, "application/json", "{\"success\":true,\"mode\":\"api\"}");
    } else {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"mode must be mcp or api\"}");
    }
}

// ────────────────────────────────────────────
// GET /audio/status
// → {"ready": true/false, "mode": "mcp"|"api", "listening": true/false}
// ────────────────────────────────────────────
static void handleAudioStatus() {
    String body = "{\"ready\":";
    body += s_wav_ready ? "true" : "false";
    body += ",\"mode\":\"";
    body += s_mcp_mode ? "mcp" : "api";
    body += "\",\"listening\":";
    body += isListening() ? "true" : "false";
    body += "}";
    server.send(200, "application/json", body);
}

// ────────────────────────────────────────────
// POST /listen/start
// → turn on microphone VAD
// ────────────────────────────────────────────
static void handleListenStart() {
    // clear stale recording so the next poll sees only fresh audio
    s_wav_ready = false;
    s_wav_size  = 0;
    if (s_wav_buf) {
        free(s_wav_buf);
        s_wav_buf = nullptr;
    }
    bool ok = startListening();
    Serial.printf("[HTTP] POST /listen/start -> %s\n", ok ? "OK" : "FAIL");
    if (ok) {
        server.send(200, "application/json", "{\"success\":true,\"listening\":true}");
    } else {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"start failed\"}");
    }
}

// ────────────────────────────────────────────
// POST /listen/stop
// → turn off microphone VAD
// ────────────────────────────────────────────
static void handleListenStop() {
    bool ok = stopListening();
    Serial.printf("[HTTP] POST /listen/stop -> %s\n", ok ? "OK" : "FAIL");
    server.send(200, "application/json", ok
        ? "{\"success\":true,\"listening\":false}"
        : "{\"success\":false,\"error\":\"stop failed\"}");
}

// ────────────────────────────────────────────
// GET /audio
// → 録音済みWAVをそのまま返す（1回読んだらクリア）
// ────────────────────────────────────────────
static void handleAudio() {
    if (!s_wav_ready || s_wav_buf == nullptr || s_wav_size == 0) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"no audio\"}");
        return;
    }

    Serial.printf("[HTTP] GET /audio -> %u bytes\n", (unsigned)s_wav_size);
    server.send_P(200, "audio/wav", (const char*)s_wav_buf, s_wav_size);

    // 読んだらクリア（1回限り）
    s_wav_ready = false;
}

// ────────────────────────────────────────────
// POST /move
// body: {"x": float, "y": float, "speed": int}
// → Servo move head (degrees)
// ────────────────────────────────────────────
static void handleMove() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"no body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"json parse error\"}");
        return;
    }

    float x = doc["x"] | 0.0f;
    float y = doc["y"] | 0.0f;
    int speed = doc["speed"] | 50;

    if (!isServoReady()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"servo not ready\"}");
        return;
    }
    bool ack = servoMove(x, y, speed);

    Serial.printf("[HTTP] POST /move -> x=%.1f y=%.1f speed=%d\n", x, y, speed);
    server.send(200, "application/json", ack ? "{\"success\":true,\"ack\":true}" : "{\"success\":true,\"ack\":false}");
}

// ────────────────────────────────────────────
// POST /home
// → Return head to center position
// ────────────────────────────────────────────
static void handleHome() {
    if (!isServoReady()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"servo not ready\"}");
        return;
    }
    bool ack = servoHome(50);
    Serial.println("[HTTP] POST /home");
    server.send(200, "application/json", ack ? "{\"success\":true,\"ack\":true}" : "{\"success\":true,\"ack\":false}");
}

// ────────────────────────────────────────────
// POST /nod
// → Nod "yes" gesture
// ────────────────────────────────────────────
static void handleNod() {
    if (!isServoReady()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"servo not ready\"}");
        return;
    }
    bool ack = servoNod();
    Serial.println("[HTTP] POST /nod");
    server.send(200, "application/json", ack ? "{\"success\":true,\"ack\":true}" : "{\"success\":true,\"ack\":false}");
}

// ────────────────────────────────────────────
// POST /shake
// → Shake "no" gesture
// ────────────────────────────────────────────
static void handleShake() {
    if (!isServoReady()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"servo not ready\"}");
        return;
    }
    bool ack = servoShake();
    Serial.println("[HTTP] POST /shake");
    server.send(200, "application/json", ack ? "{\"success\":true,\"ack\":true}" : "{\"success\":true,\"ack\":false}");
}

static void addFeedback(JsonObject obj, const ServoFeedback& fb) {
    obj["ok"] = fb.ok;
    obj["position"] = fb.position;
    obj["speed"] = fb.speed;
    obj["load"] = fb.load;
    obj["voltage"] = fb.voltage;
    obj["temperature"] = fb.temperature;
    obj["moving"] = fb.moving;
    obj["current"] = fb.current;
}

// ────────────────────────────────────────────
// GET /servo/status
// → Servo communication and feedback diagnostics
// ────────────────────────────────────────────
static void handleServoStatus() {
    ServoStatus status = getServoStatus();
    JsonDocument doc;
    doc["ready"] = status.ready;
    doc["last_command_ok"] = status.lastCommandOk;
    doc["last_yaw_raw"] = status.lastYawRaw;
    doc["last_pitch_raw"] = status.lastPitchRaw;
    doc["last_yaw_result"] = status.lastYawResult;
    doc["last_pitch_result"] = status.lastPitchResult;
    doc["last_command_ms"] = status.lastCommandMs;

    JsonObject yaw = doc["yaw"].to<JsonObject>();
    JsonObject pitch = doc["pitch"].to<JsonObject>();
    addFeedback(yaw, status.yaw);
    addFeedback(pitch, status.pitch);

    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

// ────────────────────────────────────────────
// POST /face
// body: {"face": "calm"|"thinking"|"happy"|"sleepy"}
// → Switch whale face expression
// ────────────────────────────────────────────
static void handleFace() {
    if (!server.hasArg("plain")) {
        // GET: return current face
        String body = "{\"face\":\"";
        body += getCurrentFaceName();
        body += "\"}";
        server.send(200, "application/json", body);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"json parse error\"}");
        return;
    }

    const char* face = doc["face"] | "";
    WhaleFace wf;

    if (strcmp(face, "calm") == 0)          wf = WHALE_CALM;
    else if (strcmp(face, "thinking") == 0)  wf = WHALE_THINKING;
    else if (strcmp(face, "happy") == 0)     wf = WHALE_HAPPY;
    else if (strcmp(face, "sleepy") == 0)    wf = WHALE_SLEEPY;
    else if (strcmp(face, "shy") == 0)       wf = WHALE_SHY;
    else if (strcmp(face, "smug") == 0)      wf = WHALE_SMUG;
    else if (strcmp(face, "pouty") == 0)     wf = WHALE_POUTY;
    else {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"face must be calm/thinking/happy/sleepy/shy/smug/pouty\"}");
        return;
    }

    setWhaleFace(wf);
    Serial.printf("[HTTP] POST /face -> %s\n", face);
    server.send(200, "application/json", "{\"success\":true}");
}

// ────────────────────────────────────────────
// GET /snapshot
// → Capture JPEG from camera and return it
// ────────────────────────────────────────────
static void handleSnapshot() {
    uint8_t* jpgBuf = nullptr;
    size_t jpgLen = 0;

    if (!captureJpeg(&jpgBuf, &jpgLen, 80)) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"capture failed\"}");
        return;
    }

    server.send_P(200, "image/jpeg", (const char*)jpgBuf, jpgLen);
    free(jpgBuf);
    Serial.printf("[HTTP] GET /snapshot -> %u bytes JPEG\n", (unsigned)jpgLen);
}

// ────────────────────────────────────────────
// 公開関数
// ────────────────────────────────────────────

bool isMcpMode() {
    return s_mcp_mode;
}

void storeLastRecording(const uint8_t* wav, size_t size) {
    if (s_wav_buf) {
        free(s_wav_buf);
        s_wav_buf = nullptr;
    }
    s_wav_buf = (uint8_t*)ps_malloc(size);
    if (!s_wav_buf) {
        Serial.println("[HTTP] WAV buffer alloc failed");
        s_wav_size  = 0;
        s_wav_ready = false;
        return;
    }
    memcpy(s_wav_buf, wav, size);
    s_wav_size  = size;
    s_wav_ready = true;
    Serial.printf("[HTTP] Stored recording: %u bytes\n", (unsigned)size);
}

void initHttpServer() {
    server.on("/play",         HTTP_POST, handlePlay);
    server.on("/mode",         HTTP_POST, handleMode);
    server.on("/audio/status", HTTP_GET,  handleAudioStatus);
    server.on("/audio",        HTTP_GET,  handleAudio);
    server.on("/listen/start", HTTP_POST, handleListenStart);
    server.on("/listen/stop",  HTTP_POST, handleListenStop);
    server.on("/move",         HTTP_POST, handleMove);
    server.on("/home",         HTTP_POST, handleHome);
    server.on("/nod",          HTTP_POST, handleNod);
    server.on("/shake",        HTTP_POST, handleShake);
    server.on("/servo/status", HTTP_GET,  handleServoStatus);
    server.on("/snapshot",     HTTP_GET,  handleSnapshot);
    server.on("/face",         HTTP_POST, handleFace);
    server.on("/face",         HTTP_GET,  handleFace);
    server.begin();
    Serial.println("[HTTP] Server started on port 80");
}

void handleHttpServer() {
    server.handleClient();
}
