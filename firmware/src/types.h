#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

// 優先度定義
enum PlayPriority {
    PRIORITY_LOW    = 1,
    PRIORITY_NORMAL = 5,
    PRIORITY_HIGH   = 10
};

// 音声タスク構造体
struct AudioTask {
    String voice_id;
    String voice_url;
    PlayPriority priority;
    
    // priority_queue用の比較演算子
    bool operator<(const AudioTask& other) const {
        return priority < other.priority;
    }
};

#endif