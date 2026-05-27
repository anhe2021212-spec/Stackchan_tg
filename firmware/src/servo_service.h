#ifndef SERVO_SERVICE_H
#define SERVO_SERVICE_H

#include <Arduino.h>

/**
 * Stack-chan Servo Control Service
 * Uses SCServo (SCSCL) bus servos via UART1 (GPIO 6 TX, GPIO 7 RX)
 * Yaw servo: ID 1, ±128 degrees
 * Pitch servo: ID 2, 0-90 degrees
 */

struct ServoFeedback {
    bool ok = false;
    int position = -1;
    int speed = -1;
    int load = -1;
    int voltage = -1;
    int temperature = -1;
    int moving = -1;
    int current = -1;
};

struct ServoStatus {
    bool ready = false;
    bool lastCommandOk = false;
    int lastYawRaw = -1;
    int lastPitchRaw = -1;
    int lastYawResult = 0;
    int lastPitchResult = 0;
    unsigned long lastCommandMs = 0;
    ServoFeedback yaw;
    ServoFeedback pitch;
};

// Initialize servo UART and set home position
bool initServo();

// Returns true if servo initialized successfully
bool isServoReady();

// Move head to position (degrees)
// yawDeg: -128 to 128 (left/right)
// pitchDeg: 0 to 90 (up)
// speedPct: 0-100 (movement speed, 100=fastest)
bool servoMove(float yawDeg, float pitchDeg, int speedPct = 50);

// Return to home position (center)
bool servoHome(int speedPct = 50);

// Nod "yes" gesture
bool servoNod();

// Shake "no" gesture
bool servoShake();

// Read current servo feedback for diagnostics
ServoStatus getServoStatus();

#endif
