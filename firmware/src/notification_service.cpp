#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "notification_service.h"
#include "globals.h"
#include "config.h"

// push型移行後: 音声fetchは不要。
// Mac側からPOST /play でpushされるため、ここではserverHour同期のみ行う。
void checkAndEnqueueNotification() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String pendingUrl = serverUrl + "/notify/pending";

    http.begin(pendingUrl);
    http.setTimeout(HTTP_TIMEOUT_SHORT);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;

        if (deserializeJson(doc, payload) != DeserializationError::Ok) {
            http.end();
            return;
        }

        // サーバー時刻を同期
        if (!doc["server_hour"].isNull()) {
            serverHour = doc["server_hour"].as<int>();
        }
    }

    http.end();
}
