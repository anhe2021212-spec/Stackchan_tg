#include "globals.h"

std::priority_queue<AudioTask> audioQueue;
bool isPlaying = false;
uint8_t* currentWavData = nullptr;
size_t currentWavSize = 0;
AudioTask currentTask;
unsigned long playbackDeadlineMs = 0;
unsigned long playbackStartMs = 0;
bool micResumeRequested = false;

String last_played_voice_id = "";
String interrupted_voice_id = "";
String interrupted_voice_url = "";

//　サーバー時刻
int serverHour = -1;  // -1 = 未取得
String serverUrl = "";  // wifi_manager.cpp の connectWiFi() でセット
String voiceUploadUrl = "";  // 同上、PTT 上传地址