#include "servo_service.h"
#include <Arduino.h>
#include <M5StackChan.h>

// StackChan-BSP uses 0.1 degree units for motion commands.
static constexpr float YAW_MIN_DEG = -128.0f;
static constexpr float YAW_MAX_DEG = 128.0f;
static constexpr float PITCH_MIN_DEG = 5.0f;
static constexpr float PITCH_MAX_DEG = 85.0f;

static bool servoReady = false;
static bool lastCommandOk = false;
static int lastYawAngle = 0;
static int lastPitchAngle = 0;
static int lastSpeed = 0;
static unsigned long lastCommandMs = 0;

bool isServoReady() { return servoReady; }

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int degToBspAngle(float deg) {
    return (int)roundf(deg * 10.0f);
}

static int speedToBspSpeed(int speedPct) {
    if (speedPct < 0) speedPct = 0;
    if (speedPct > 100) speedPct = 100;
    return speedPct * 10;
}

static void noteCommand(int yawAngle, int pitchAngle, int speed) {
    lastCommandOk = true;
    lastYawAngle = yawAngle;
    lastPitchAngle = pitchAngle;
    lastSpeed = speed;
    lastCommandMs = millis();
}

bool initServo() {
    // M5StackChan.begin() performs the official hardware initialization,
    // including servo VM_EN power control through the IO expander.
    M5StackChan.Motion.setAutoAngleSyncEnabled(false);
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
    M5StackChan.Motion.goHome(500);
    noteCommand(0, 0, 500);
    servoReady = true;
    Serial.println("[SERVO] Ready via StackChan-BSP Motion");
    return true;
}

bool servoMove(float yawDeg, float pitchDeg, int speedPct) {
    if (!servoReady) return false;

    yawDeg = clampf(yawDeg, YAW_MIN_DEG, YAW_MAX_DEG);
    pitchDeg = clampf(pitchDeg, PITCH_MIN_DEG, PITCH_MAX_DEG);

    int yawAngle = degToBspAngle(yawDeg);
    int pitchAngle = degToBspAngle(pitchDeg);
    int speed = speedToBspSpeed(speedPct);

    M5StackChan.Motion.move(yawAngle, pitchAngle, speed);
    noteCommand(yawAngle, pitchAngle, speed);

    Serial.printf("[SERVO] Move yaw=%.1f pitch=%.1f speed=%d%% (bsp: %d,%d speed=%d)\n",
                  yawDeg, pitchDeg, speedPct, yawAngle, pitchAngle, speed);
    return true;
}

bool servoHome(int speedPct) {
    if (!servoReady) return false;
    int speed = speedToBspSpeed(speedPct);
    M5StackChan.Motion.goHome(speed);
    noteCommand(0, 0, speed);
    Serial.println("[SERVO] Home");
    return true;
}

bool servoNod() {
    if (!servoReady) return false;
    Serial.println("[SERVO] Nod");
    M5StackChan.Motion.moveY(300, 500);
    delay(350);
    M5StackChan.Motion.moveY(50, 600);
    delay(350);
    M5StackChan.Motion.moveY(300, 500);
    delay(350);
    M5StackChan.Motion.goHome(500);
    noteCommand(0, 0, 500);
    return true;
}

bool servoShake() {
    if (!servoReady) return false;
    Serial.println("[SERVO] Shake");
    M5StackChan.Motion.moveX(-400, 600);
    delay(300);
    M5StackChan.Motion.moveX(400, 600);
    delay(300);
    M5StackChan.Motion.moveX(-400, 600);
    delay(300);
    M5StackChan.Motion.goHome(500);
    noteCommand(0, 0, 500);
    return true;
}

ServoStatus getServoStatus() {
    ServoStatus status;
    status.ready = servoReady;
    status.lastCommandOk = lastCommandOk;
    status.lastYawRaw = lastYawAngle;
    status.lastPitchRaw = lastPitchAngle;
    status.lastYawResult = 1;
    status.lastPitchResult = 1;
    status.lastCommandMs = lastCommandMs;

    if (servoReady) {
        status.yaw.ok = true;
        status.yaw.position = M5StackChan.Motion.getCurrentYawAngle();
        status.yaw.moving = M5StackChan.Motion.isYawMoving() ? 1 : 0;
        status.yaw.speed = lastSpeed;

        status.pitch.ok = true;
        status.pitch.position = M5StackChan.Motion.getCurrentPitchAngle();
        status.pitch.moving = M5StackChan.Motion.isPitchMoving() ? 1 : 0;
        status.pitch.speed = lastSpeed;
    }

    return status;
}
