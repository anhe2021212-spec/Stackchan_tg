#pragma once
#include <stdint.h>
#include <stddef.h>

// HTTPサーバー初期化（setup()で呼ぶ）
void initHttpServer();

// HTTPリクエスト処理（loop()で呼ぶ）
void handleHttpServer();

// 録音済みWAVをバッファに保存（mic_service.cppから呼ぶ）
void storeLastRecording(const uint8_t* wav, size_t size);

// 現在MCPモードかどうか（mic_service.cppから参照）
bool isMcpMode();