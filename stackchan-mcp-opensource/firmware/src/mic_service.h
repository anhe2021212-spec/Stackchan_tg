#pragma once
#include <stdint.h>

bool initMicrophone();
void updateMicrophone();

bool startListening();
bool stopListening();
bool isListening();

// 触屏 PTT：直接录音并上传到 voiceUploadUrl，绕过 VAD/MCP 模式。
// startPttRecording: 按下触屏时调用，立即进入 RECORDING（跳过 TRIGGERING）。
// stopPttRecording:  松开触屏时调用，停止录音并 POST 到 voiceUploadUrl。
bool startPttRecording();
bool stopPttRecording();
bool isPttRecording();
