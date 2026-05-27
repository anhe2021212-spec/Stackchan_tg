#ifndef GLOBALS_H
#define GLOBALS_H

#include <queue>
#include "types.h"

extern std::priority_queue<AudioTask> audioQueue;
extern bool isPlaying;
extern uint8_t* currentWavData;
extern size_t currentWavSize;
extern AudioTask currentTask;
extern unsigned long playbackDeadlineMs;
extern unsigned long playbackStartMs;
extern bool micResumeRequested;

extern String last_played_voice_id;
extern String interrupted_voice_id;
extern String interrupted_voice_url;

// サーバーから時刻をみる
extern int serverHour;
// 接続中ネットワークに対応したサーバーURL（wifi_manager で設定）
extern String serverUrl;
// 触屏 PTT 录音上传地址（wifi_manager で設定）
extern String voiceUploadUrl;
#endif