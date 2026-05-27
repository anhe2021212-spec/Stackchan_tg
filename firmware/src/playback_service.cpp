#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include "playback_service.h"
#include "globals.h"
#include "config.h"
#include "face_service.h"

static size_t lipSyncOffset = 0;
static unsigned long lastLipMs = 0;

#define WAV_HEADER_SIZE       44
#define LIPSYNC_INTERVAL_MS   50
#define LIPSYNC_CHUNK_SAMPLES 1024
#define DOWNLOAD_TIMEOUT_MS   10000

// ── FreeRTOS: URLをCore 0に渡すキュー
//    StringはFreeRTOSキューに乗せられないのでchar配列で渡す
#define MAX_URL_LEN 256
static QueueHandle_t s_downloadQueue = nullptr;

// ── Core 0 → Core 1 へダウンロード完了を通知するフラグ
static volatile bool s_pendingReady = false;
static uint8_t*      s_pendingData  = nullptr;
static size_t        s_pendingSize  = 0;

// ════════════════════════════════════════
//  ダウンロードタスク（Core 0で動く）
//  loop()をブロックしないための分離
// ════════════════════════════════════════
static void downloadTask(void* arg) {
    char url[MAX_URL_LEN];
    for (;;) {
        if (xQueueReceive(s_downloadQueue, url, portMAX_DELAY) != pdTRUE) continue;

        uint8_t* data = nullptr;
        size_t   size = 0;

        if (downloadVoice(String(url), &data, &size)) {
            // 前の未処理データがあれば解放
            if (s_pendingData) {
                free(s_pendingData);
                s_pendingData = nullptr;
            }
            s_pendingData  = data;
            s_pendingSize  = size;
            s_pendingReady = true;  // Core 1に通知
            Serial.printf("[DOWNLOAD] Ready: %u bytes\n", (unsigned)size);
        } else {
            Serial.println("[DOWNLOAD] Failed");
//            setFaceExpression(FACE_IDLE);
        }
    }
}

// ════════════════════════════════════════
//  初期化（setup()から呼ぶ）
// ════════════════════════════════════════
void initPlayback() {
    s_downloadQueue = xQueueCreate(4, sizeof(char) * MAX_URL_LEN);
    xTaskCreatePinnedToCore(
        downloadTask,
        "downloadTask",
        8192,
        nullptr,
        1,
        nullptr,
        1   // Core 0（Arduinoのloop()はCore 1）
    );
    Serial.println("[PLAY] Download task started on Core 1");
}

// ════════════════════════════════════════
//  再生リクエスト受付（ノンブロッキング）
//  enqueueAudioTask()から呼ばれる
// ════════════════════════════════════════
void startPlayback(const AudioTask& task) {
    if (!s_downloadQueue) {
        Serial.println("[PLAY] Queue not initialized!");
        return;
    }
    char url[MAX_URL_LEN];
    task.voice_url.toCharArray(url, MAX_URL_LEN);
    xQueueSend(s_downloadQueue, url, 0);  // ノンブロッキング
    setFaceExpression(FACE_THINKING);
    Serial.printf("[PLAY] Queued for download: %s\n", url);
}

// ════════════════════════════════════════
//  ダウンロード完了チェック → Speaker起動
//  loop()から毎回呼ぶ（Core 1でSpeaker操作するために分離）
// ════════════════════════════════════════
void checkPendingPlayback() {
    if (!s_pendingReady) return;
    s_pendingReady = false;

    // 前の再生データを解放
    if (currentWavData) {
        free(currentWavData);
    }
    currentWavData = s_pendingData;
    currentWavSize = s_pendingSize;
    s_pendingData  = nullptr;
    s_pendingSize  = 0;

    // 再生時間 + 2秒のデッドライン
    const float bytes_per_sec = 24000.0f * 2.0f;
    playbackDeadlineMs = millis() +
        (unsigned long)((currentWavSize / bytes_per_sec) * 1000.0f) + 2000;

    // マイク停止 → スピーカー起動
    if (M5.Mic.isRunning()) {
        M5.Mic.end();
        vTaskDelay(pdMS_TO_TICKS(200));  // 固定200ms待機に戻す
    }
    if (!M5.Speaker.isRunning()) {
        M5.Speaker.begin();
    }

    Serial.println("[PLAY] Mic stopped");
    M5.Speaker.setVolume(SPEAKER_VOLUME);
    M5.Speaker.playWav(currentWavData, currentWavSize);
    setFaceExpression(FACE_PLAYING);

    lipSyncOffset = WAV_HEADER_SIZE;
    lastLipMs     = 0;
    isPlaying     = true;
    playbackStartMs  = millis();
    Serial.println("[PLAY] Speaker started");
}

// ════════════════════════════════════════
//  口パク更新（loop()から毎回呼ぶ）
// ════════════════════════════════════════
void updateLipSync() {
    if (!isPlaying || currentWavData == nullptr || currentWavSize == 0) return;

    unsigned long now = millis();
    if (now - lastLipMs < LIPSYNC_INTERVAL_MS) return;
    lastLipMs = now;

    if (lipSyncOffset < WAV_HEADER_SIZE) lipSyncOffset = WAV_HEADER_SIZE;
    if (lipSyncOffset >= currentWavSize) {
        setMouthOpen(0.0f);
        return;
    }

    int16_t* pcm = (int16_t*)(currentWavData + lipSyncOffset);
    size_t remainBytes = currentWavSize - lipSyncOffset;
    size_t samples = min((size_t)LIPSYNC_CHUNK_SAMPLES, remainBytes / sizeof(int16_t));
    if (samples == 0) {
        setMouthOpen(0.0f);
        return;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < samples; i++) {
        float v = (float)pcm[i] / 32768.0f;
        sum += v * v;
    }
    setMouthOpen(constrain(sqrtf(sum / samples) * 8.0f, 0.0f, 1.0f));
    lipSyncOffset += samples * sizeof(int16_t);
}

// ════════════════════════════════════════
//  音声ダウンロード（Core 0のタスクから呼ぶ）
// ════════════════════════════════════════
bool downloadVoice(const String& url, uint8_t** outData, size_t* outSize) {
    HTTPClient http;
    Serial.printf("[DOWNLOAD] URL: %s\n", url.c_str());

    http.begin(url);
    http.setTimeout(DOWNLOAD_TIMEOUT_MS);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[DOWNLOAD] HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }

    int len = http.getSize();
    uint8_t* wavData = (uint8_t*)ps_malloc(len);
    if (!wavData) {
        Serial.println("[DOWNLOAD] ps_malloc failed");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t bytesRead = 0;
    while (http.connected() && bytesRead < (size_t)len) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = min(available, (size_t)(len - bytesRead));
            stream->readBytes(wavData + bytesRead, toRead);
            bytesRead += toRead;
        }
        delay(1);
    }
    http.end();

    *outData = wavData;
    *outSize = (size_t)len;
    return true;
}

// ════════════════════════════════════════
//  再生完了後の次キュー処理
// ════════════════════════════════════════
void processAudioQueue() {
    if (isPlaying) return;

    setMouthOpen(0.0f);

    if (audioQueue.empty()) {
        setFaceExpression(FACE_IDLE);
        return;
    }

    AudioTask next = audioQueue.top();
    audioQueue.pop();
    startPlayback(next);
    last_played_voice_id = next.voice_id;
}