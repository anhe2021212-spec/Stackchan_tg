#pragma once
#include "types.h"

void initPlayback();                 // setup()で呼ぶ
void startPlayback(const AudioTask& task);  // ダウンロードキューに積む
void checkPendingPlayback();         // loop()で呼ぶ（Speaker起動）
bool downloadVoice(const String& url, uint8_t** outData, size_t* outSize);
void processAudioQueue();
void updateLipSync();