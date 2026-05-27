#pragma once

// Expression types (state-based, used by mic/audio services)
enum FaceExpression {
    FACE_IDLE      = 0,  // Default (calm or sleepy based on time)
    FACE_LISTENING = 1,  // Listening to mic
    FACE_PLAYING   = 2,  // Speaking (happy/open mouth)
    FACE_THINKING  = 3,  // Processing
    FACE_HAPPY     = 4,  // Happy
};

// Whale face names (for HTTP/MCP direct control)
enum WhaleFace {
    WHALE_CALM     = 0,
    WHALE_THINKING = 1,
    WHALE_HAPPY    = 2,
    WHALE_SLEEPY   = 3,
    WHALE_SHY      = 4,
    WHALE_SMUG     = 5,
    WHALE_POUTY    = 6,
};

void initFace();
void setFaceExpression(FaceExpression expr);
void setMouthOpen(float ratio);  // 0.0~1.0 for lip sync
void setWhaleFace(WhaleFace face);  // Direct face control
const char* getCurrentFaceName();
