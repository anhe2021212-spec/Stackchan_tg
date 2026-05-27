#include <M5Unified.h>
#include "queue_manager.h"
#include "globals.h"
#include "playback_service.h"
#include "face_service.h"

void enqueueAudioTask(const AudioTask& task) {
    // 再生中なら優先度キューに積んで完了後に再生
    if (isPlaying) {
        audioQueue.push(task);
        return;
    }
    // 再生中でなければ即座にダウンロードキューへ
    startPlayback(task);
    last_played_voice_id = task.voice_id;
}

void notifyPlaybackFinished() {
    isPlaying = false;
    if (currentWavData) {
        free(currentWavData);
        currentWavData = nullptr;
        currentWavSize = 0;
    }
    setMouthOpen(0.0f);
    processAudioQueue();  

    if (!isPlaying) {
        micResumeRequested = true;
    }
}