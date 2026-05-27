#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "mic_service.h"
#include "queue_manager.h"
#include "globals.h"
#include "config.h"
#include "types.h"
#include "chat_service.h"
#include "face_service.h"
#include "http_server.h"

enum MicState {
    MIC_IDLE = 0,
    MIC_TRIGGERING,
    MIC_RECORDING,
    MIC_SENDING
};

#pragma pack(push, 1)
struct WAVHeader {
    char riff[4] = {'R','I','F','F'};
    uint32_t file_size;
    char wave[4] = {'W','A','V','E'};
    char fmt_[4] = {'f','m','t',' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;      // PCM
    uint16_t channels = 1;
    uint32_t sample_rate = MIC_SAMPLE_RATE;
    uint32_t byte_rate = MIC_SAMPLE_RATE * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;
    char data_[4] = {'d','a','t','a'};
    uint32_t data_size;
};
#pragma pack(pop)

static int16_t* record_buffer = nullptr;
static size_t max_samples = MIC_SAMPLE_RATE * MIC_MAX_RECORD_SECONDS;
static size_t recorded_samples = 0;
static MicState mic_state = MIC_IDLE;
static uint32_t trigger_start_ms = 0;
static uint32_t silence_start_ms = 0;

// プリトリガーリングバッファ
static int16_t pre_trigger_buf[PRE_TRIGGER_BUFFER_SAMPLES];
static size_t  pre_buf_write = 0;
static bool    pre_buf_full  = false;
static unsigned long micResumedAtMs = 0;

// listen on/off gate — default OFF so VAD doesn't hog PSRAM/HTTP
static bool s_listening = false;
// 触屏 PTT 状态：true 时绕过 VAD 沉默检测，等用户松开才停
static bool s_ptt_active = false;

static inline float calcRmsNorm(const int16_t* data, size_t n) {
    if (n == 0) return 0.0f;
    float sum = 0.0f; 
    for (size_t i = 0; i < n; ++i) {
        float x = (float)data[i] / 32768.0f;
        sum += x * x;
    }
    return sqrtf(sum / (float)n);       
}

static bool sendAudioToServer(int16_t* audio_data, size_t sample_count);
static bool sendAudioToVoiceEndpoint(int16_t* audio_data, size_t sample_count);

static bool isValidAudio(int16_t* audio_data, size_t sample_count) {
    if (sample_count < MIC_MIN_VALID_SAMPLES) {
        Serial.printf("[MIC] Too short (%u samples), discarding\n", (unsigned)sample_count);
        return false;
    }
    size_t check_samples = MIC_SAMPLE_RATE / 2;
    if (sample_count > check_samples) {
        float early_rms = calcRmsNorm(audio_data, check_samples);
        if (early_rms < MIC_VOICE_CONFIRM_RMS) {
            Serial.printf("[MIC] No voice (early RMS=%.3f), discarding\n", early_rms);
            return false;
        }
    }
    return true;
}

static uint8_t* buildWav(int16_t* audio_data, size_t sample_count, size_t& wav_size) {
    WAVHeader header;
    header.data_size = sample_count * 2;
    header.file_size = header.data_size + sizeof(WAVHeader) - 8;

    wav_size = sizeof(WAVHeader) + header.data_size;
    uint8_t* wav = (uint8_t*)ps_malloc(wav_size);
    if (!wav) {
        Serial.println("[MIC] WAV buffer alloc failed");
        return nullptr;
    }
    memcpy(wav, &header, sizeof(WAVHeader));
    memcpy(wav + sizeof(WAVHeader), audio_data, header.data_size);
    return wav;
}
static void applyMicConfig() {
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate        = MIC_SAMPLE_RATE;
    mic_cfg.stereo             = false;
    mic_cfg.magnification      = MIC_MAGNIFICATION;
//    mic_cfg.dma_buf_len        = MIC_DMA_BUF_LEN;
//    mic_cfg.dma_buf_count      = MIC_DMA_BUF_COUNT;
    mic_cfg.noise_filter_level = MIC_NOISE_FILTER_LEVEL;
    M5.Mic.config(mic_cfg);
}

bool initMicrophone() {
    Serial.println("[MIC] Initializing microphone...");

    // プリトリガーバッファをリセット（初回 & 再開時共通）
    memset(pre_trigger_buf, 0, sizeof(pre_trigger_buf));
    pre_buf_write = 0;
    pre_buf_full  = false;

    if (M5.Speaker.isRunning()) {
        M5.Speaker.end();
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
    
    applyMicConfig();

    if (!M5.Mic.begin()) {
        Serial.println("[MIC] Mic.begin failed");
        return false;
    }
    
    // 初回のみ確保（再開時はスキップ）
    if (!record_buffer) {
        record_buffer = (int16_t*)ps_malloc(max_samples * sizeof(int16_t));
        if (!record_buffer) {
            Serial.println("[MIC] Failed to allocate record buffer");
            return false;
        }
    }

    Serial.printf("[MIC] Ready sr=%d maxSec=%d maxSamples=%u\n",
                  MIC_SAMPLE_RATE, MIC_MAX_RECORD_SECONDS, (unsigned)max_samples);
    mic_state = MIC_IDLE;

    return true;
}

void updateMicrophone() {
    if (!s_listening) return;
    if (!M5.Mic.isEnabled()) return;
    if (isPlaying) return;

    static int16_t frame[MIC_FRAME_SAMPLES];
    if (!M5.Mic.record(frame, MIC_FRAME_SAMPLES, MIC_SAMPLE_RATE)) return;
    size_t got = MIC_FRAME_SAMPLES;

    float rms = calcRmsNorm(frame, got);
    uint32_t now = millis();

    if (mic_state == MIC_IDLE || mic_state == MIC_TRIGGERING) {
        for (size_t i = 0; i < got; i++) {
            pre_trigger_buf[pre_buf_write] = frame[i];
            pre_buf_write = (pre_buf_write + 1) % PRE_TRIGGER_BUFFER_SAMPLES;
            if (pre_buf_write == 0) pre_buf_full = true;
        }
    }

    switch (mic_state) {
        case MIC_IDLE:
            if (rms > MIC_TRIGGER_RMS) {
                trigger_start_ms = now;
                mic_state = MIC_TRIGGERING;
            }
            break;

        case MIC_TRIGGERING:
            if (rms > MIC_TRIGGER_RMS) {
                if (now - trigger_start_ms >= MIC_TRIGGER_HOLD_MS) {
                    if (pre_buf_full) {
                        size_t older = PRE_TRIGGER_BUFFER_SAMPLES - pre_buf_write;
                        memcpy(record_buffer,
                               pre_trigger_buf + pre_buf_write,
                               older * sizeof(int16_t));
                        memcpy(record_buffer + older,
                               pre_trigger_buf,
                               pre_buf_write * sizeof(int16_t));
                        recorded_samples = PRE_TRIGGER_BUFFER_SAMPLES;
                    } else {
                        memcpy(record_buffer,
                               pre_trigger_buf,
                               pre_buf_write * sizeof(int16_t));
                        recorded_samples = pre_buf_write;
                    }
                    pre_buf_write = 0;
                    pre_buf_full  = false;
                    silence_start_ms = 0;
                    mic_state = MIC_RECORDING;
                    setFaceExpression(FACE_LISTENING);
                    Serial.printf("[MIC] Triggered -> RECORDING (pre-buffer: %u samples)\n",
                                  (unsigned)recorded_samples);
                }
            } else {
                mic_state = MIC_IDLE;
            }
            break;

        case MIC_RECORDING: {
            size_t remain = max_samples - recorded_samples;
            size_t to_copy = (got < remain) ? got : remain;
            memcpy(record_buffer + recorded_samples, frame, to_copy * sizeof(int16_t));
            recorded_samples += to_copy;

            bool maxed = (recorded_samples >= max_samples);

            if (s_ptt_active) {
                // PTT 模式：跳过 VAD 沉默检测，只在 buffer 满时强停
                if (maxed) {
                    mic_state = MIC_SENDING;
                    Serial.printf("[MIC] PTT maxed: %u samples (forced stop)\n",
                                  (unsigned)recorded_samples);
                    setFaceExpression(FACE_THINKING);
                    bool ok = sendAudioToVoiceEndpoint(record_buffer, recorded_samples);
                    Serial.printf("[MIC] PTT auto-send: %s\n", ok ? "OK" : "NG");
                    if (!ok) setFaceExpression(FACE_IDLE);
                    s_ptt_active = false;
                    s_listening  = false;
                    if (M5.Mic.isRunning()) M5.Mic.end();
                    mic_state = MIC_IDLE;
                }
                break;
            }

            // VAD 模式：沉默检测
            if (rms < MIC_SILENCE_RMS) {
                if (silence_start_ms == 0) silence_start_ms = now;
            } else {
                silence_start_ms = 0;
            }

            bool silent_end = (silence_start_ms != 0 &&
                               (now - silence_start_ms) >= MIC_SILENCE_HOLD_MS);

            if (maxed || silent_end) {
                mic_state = MIC_SENDING;
                Serial.printf("[MIC] Record end: samples=%u reason=%s\n",
                              (unsigned)recorded_samples, maxed ? "max" : "silence");
                setFaceExpression(FACE_THINKING);

                bool ok = sendAudioToServer(record_buffer, recorded_samples);
                Serial.printf("[MIC] Send/Process result=%s\n", ok ? "OK" : "NG");
                if (!ok) setFaceExpression(FACE_IDLE);
                mic_state = MIC_IDLE;
            }
            break;
        }

        case MIC_SENDING:
            break;
    }
}

bool startListening() {
    if (s_listening) {
        Serial.println("[MIC] startListening: already on");
        return true;
    }
    if (!initMicrophone()) {
        Serial.println("[MIC] startListening: init failed");
        return false;
    }
    s_listening = true;
    Serial.println("[MIC] Listening ON");
    return true;
}

bool stopListening() {
    if (!s_listening) {
        Serial.println("[MIC] stopListening: already off");
        return true;
    }
    s_listening = false;
    if (M5.Mic.isRunning()) {
        M5.Mic.end();
    }
    mic_state = MIC_IDLE;
    recorded_samples = 0;
    silence_start_ms = 0;
    trigger_start_ms = 0;
    pre_buf_write = 0;
    pre_buf_full  = false;
    setFaceExpression(FACE_IDLE);
    Serial.println("[MIC] Listening OFF");
    return true;
}

bool isListening() {
    return s_listening;
}

// ───────────────────────────────────────────────────────────
// 触屏 PTT — 按下立即录音，松开立即上传到 voiceUploadUrl
// ───────────────────────────────────────────────────────────
bool startPttRecording() {
    if (s_ptt_active) {
        Serial.println("[MIC] PTT already active");
        return true;
    }
    if (!M5.Mic.isRunning()) {
        if (!initMicrophone()) {
            Serial.println("[MIC] PTT init failed");
            return false;
        }
    }
    s_ptt_active = true;
    s_listening  = true;
    mic_state    = MIC_RECORDING;     // 跳过 TRIGGERING
    recorded_samples = 0;
    silence_start_ms = 0;
    trigger_start_ms = 0;
    pre_buf_write    = 0;
    pre_buf_full     = false;
    setFaceExpression(FACE_LISTENING);
    Serial.println("[MIC] PTT started");
    return true;
}

bool stopPttRecording() {
    if (!s_ptt_active) {
        Serial.println("[MIC] PTT not active, ignore stop");
        return true;
    }
    s_ptt_active = false;
    bool ok = true;
    if (mic_state == MIC_RECORDING && recorded_samples > 0) {
        mic_state = MIC_SENDING;
        setFaceExpression(FACE_THINKING);
        ok = sendAudioToVoiceEndpoint(record_buffer, recorded_samples);
        Serial.printf("[MIC] PTT released, sent=%s samples=%u\n",
                      ok ? "OK" : "NG", (unsigned)recorded_samples);
        if (!ok) setFaceExpression(FACE_IDLE);
    } else {
        Serial.println("[MIC] PTT released with empty recording");
        setFaceExpression(FACE_IDLE);
    }
    s_listening = false;
    if (M5.Mic.isRunning()) M5.Mic.end();
    mic_state = MIC_IDLE;
    return ok;
}

bool isPttRecording() {
    return s_ptt_active;
}

// 把录音 WAV POST 到 PC 上的 voice_handler.py
static bool sendAudioToVoiceEndpoint(int16_t* audio_data, size_t sample_count) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[MIC] voice/upload: WiFi disconnected");
        return false;
    }
    if (voiceUploadUrl.length() == 0) {
        Serial.println("[MIC] voice/upload: voiceUploadUrl is empty");
        return false;
    }
    if (!isValidAudio(audio_data, sample_count)) return false;

    size_t wav_size = 0;
    uint8_t* wav = buildWav(audio_data, sample_count, wav_size);
    if (!wav) return false;

    Serial.printf("[MIC] voice/upload -> %s (%u bytes)\n",
                  voiceUploadUrl.c_str(), (unsigned)wav_size);

    HTTPClient http;
    http.begin(voiceUploadUrl);
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(HTTP_TIMEOUT_VOICE);

    int code = http.sendRequest("POST", wav, wav_size);
    free(wav);

    if (code != HTTP_CODE_OK) {
        Serial.printf("[MIC] voice/upload HTTP=%d body=%s\n",
                      code, http.getString().c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();
    Serial.printf("[MIC] voice/upload OK: %s\n", payload.c_str());
    setFaceExpression(FACE_IDLE);
    return true;
}

static bool sendAudioToServer(int16_t* audio_data, size_t sample_count) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[MIC] WiFi disconnected");
        return false;
    }
    if (!isValidAudio(audio_data, sample_count)) return false;

    size_t wav_size = 0;
    uint8_t* wav = buildWav(audio_data, sample_count, wav_size);
    if (!wav) return false;

    Serial.printf("[MIC] WAV: samples=%u bytes=%u sr=%u\n",
                  (unsigned)sample_count, (unsigned)wav_size, (unsigned)MIC_SAMPLE_RATE);

    storeLastRecording(wav, wav_size);
    if (isMcpMode()) {
        Serial.println("[MIC] MCP mode: stored, auto-stop listening");
        free(wav);
        setFaceExpression(FACE_IDLE);
        stopListening();
        return true;
    }

    HTTPClient http;
    http.begin(serverUrl + "/speech/transcribe");
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(HTTP_TIMEOUT_STT);

    int code = http.sendRequest("POST", wav, wav_size);
    free(wav);

    if (code != HTTP_CODE_OK) {
        Serial.printf("[MIC] /speech/transcribe HTTP=%d body=%s\n",
                      code, http.getString().c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        Serial.println("[MIC] STT JSON parse error");
        return false;
    }

    const char* transcript = doc["transcript"] | "";
    if (!doc["success"] || strlen(transcript) == 0) return false;

    Serial.printf("[MIC] Transcript: %s\n", transcript);
    sendChatRequest(transcript);
    return true;
}
