// face_service.cpp
// Stack-chan native avatar (m5stack-avatar) — 暴躁的小灰方块表情
// 用 meganetaaan/m5stack-avatar 取代原仓库基于 PNG 的实现。

#include "face_service.h"
#include "globals.h"
#include <M5Unified.h>
#include <Avatar.h>

using namespace m5avatar;

static Avatar avatar;
static WhaleFace currentFace = WHALE_CALM;
static bool isTalking = false;

static Expression mapWhaleToExpression(WhaleFace face) {
    switch (face) {
        case WHALE_CALM:     return Expression::Neutral;
        case WHALE_THINKING: return Expression::Doubt;
        case WHALE_HAPPY:    return Expression::Happy;
        case WHALE_SLEEPY:   return Expression::Sleepy;
        case WHALE_SHY:      return Expression::Happy;
        case WHALE_SMUG:     return Expression::Happy;
        case WHALE_POUTY:    return Expression::Angry;
        default:             return Expression::Neutral;
    }
}

void initFace() {
    avatar.init();
    avatar.setExpression(Expression::Neutral);
    Serial.println("[FACE] Native avatar ready (m5stack-avatar)");
    Serial.printf("[FACE] Free heap: %u  Free PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
}

void setFaceExpression(FaceExpression expr) {
    WhaleFace target;

    switch (expr) {
        case FACE_IDLE:
            if (serverHour >= 19 || (serverHour >= 0 && serverHour < 7)) {
                target = WHALE_SLEEPY;
            } else {
                target = WHALE_CALM;
            }
            isTalking = false;
            break;

        case FACE_LISTENING:
            target = WHALE_THINKING;
            isTalking = false;
            break;

        case FACE_PLAYING:
            target = WHALE_HAPPY;
            isTalking = true;
            break;

        case FACE_THINKING:
            target = WHALE_THINKING;
            isTalking = false;
            break;

        case FACE_HAPPY:
            target = WHALE_HAPPY;
            isTalking = false;
            break;

        default:
            target = WHALE_CALM;
            isTalking = false;
            break;
    }

    if (target != currentFace) {
        currentFace = target;
        avatar.setExpression(mapWhaleToExpression(target));
    }
}

void setMouthOpen(float ratio) {
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    avatar.setMouthOpenRatio(ratio);
}

void setWhaleFace(WhaleFace face) {
    isTalking = false;
    currentFace = face;
    avatar.setExpression(mapWhaleToExpression(face));
}

const char* getCurrentFaceName() {
    switch (currentFace) {
        case WHALE_CALM:     return "calm";
        case WHALE_THINKING: return "thinking";
        case WHALE_HAPPY:    return "happy";
        case WHALE_SLEEPY:   return "sleepy";
        case WHALE_SHY:      return "shy";
        case WHALE_SMUG:     return "smug";
        case WHALE_POUTY:    return "pouty";
        default:             return "unknown";
    }
}
