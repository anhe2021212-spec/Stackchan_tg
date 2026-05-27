#include <Arduino.h>

#include <M5Unified.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include "http_server.h"
#include "types.h"
#include "config.h"
#include "globals.h"
#include "queue_manager.h"
#include "mic_service.h"
#include "wifi_manager.h"
#include "notification_service.h"
#include "playback_service.h"
#include "chat_service.h"
#include "face_service.h"
#include "servo_service.h"
#include "camera_service.h"

void setup() {
    Serial.begin(115200);
    delay(1000);

    M5StackChan.begin();
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);

    initFace();

    Serial.println("\n=== Yuno v5.0 (Microphone) ===");

    auto spk_cfg = M5.Speaker.config();
    M5.Speaker.config(spk_cfg);
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    // Mic stays OFF at boot — opened on demand via /listen/start.
    // Always-on VAD was starving HTTP/PSRAM in MCP mode.

    if (!initServo()) {
        Serial.println("[WARN] Servo init failed - head movement disabled");
    }

    if (!initCamera()) {
        Serial.println("[WARN] Camera init failed - vision disabled");
    }

    connectWiFi();
    syncServerHour();
    checkAndEnqueueNotification();
    initPlayback();
    initHttpServer();

#ifdef TEST_MODE
    delay(3000);
    testChat("おはよう！");
#endif
}
void loop() {
    M5StackChan.update();

    // 触屏 PTT：按下 = 开始录音，松开 = 停止 + POST 到 voiceUploadUrl。
    // 绕过 VAD，意图明确（避免误触发）。完全独立于 MCP/API 模式。
    auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        Serial.println("[TOUCH] Pressed -> PTT start");
        startPttRecording();
    } else if (touch.wasReleased() && isPttRecording()) {
        Serial.println("[TOUCH] Released -> PTT stop");
        stopPttRecording();
    }

    handleHttpServer();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Disconnected. Reconnecting...");
        WiFi.reconnect();
        delay(5000);
    }

    checkPendingPlayback();
    updateLipSync();
    updateMicrophone();

        // 再生完了検知（マイク再開より後に置く）
    if (isPlaying &&
        (millis() - playbackStartMs > 1000) &&
        (!M5.Speaker.isPlaying() ||
        (playbackDeadlineMs != 0 && millis() > playbackDeadlineMs))) {
        if (playbackDeadlineMs != 0 && millis() > playbackDeadlineMs) {
            Serial.println("[PLAY] Playback timeout -> force stop");
            M5.Speaker.stop();
        }
        notifyPlaybackFinished();
    }

    // マイク再開（完了検知より前に置く）
    // Only resume if listening was explicitly requested via /listen/start.
    if (micResumeRequested && !isPlaying) {
        micResumeRequested = false;
        if (isListening() && !M5.Mic.isRunning()) {
            if (initMicrophone()) {
                Serial.println("[MIC] Mic resumed after playback");
            } else {
                Serial.println("[MIC] Mic resume failed");
            }
        }
    }

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > NOTIFICATION_CHECK_INTERVAL) {
        checkAndEnqueueNotification();
        lastCheck = millis();
    }

    delay(50);
}
