// wifi_manager.cpp

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "wifi_manager.h"
#include "globals.h"
#include "config.h"

// ネットワーク定義（config.h の定数を配列にまとめる）
struct NetworkConfig {
    const char* ssid;
    const char* password;
    const char* server_url;
    const char* voice_upload_url;
};

static const NetworkConfig NETWORKS[WIFI_NETWORK_COUNT] = {
    { WIFI_SSID_0, WIFI_PASSWORD_0, SERVER_URL_0, VOICE_UPLOAD_URL_0 },
    { WIFI_SSID_1, WIFI_PASSWORD_1, SERVER_URL_1, VOICE_UPLOAD_URL_1 },
};

void connectWiFi() {
    Serial.println("\nConnecting to WiFi...");
    WiFi.mode(WIFI_STA);

    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
        Serial.printf("[WIFI] 試行 %d/%d: %s\n", i + 1, WIFI_NETWORK_COUNT, NETWORKS[i].ssid);

        WiFi.begin(NETWORKS[i].ssid, NETWORKS[i].password);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            // 接続成功 → このネットワークのサーバーURLをセット
            serverUrl = String(NETWORKS[i].server_url);
            voiceUploadUrl = String(NETWORKS[i].voice_upload_url);

            Serial.printf("\n[WIFI] 接続成功: %s\n", NETWORKS[i].ssid);
            Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("[WIFI] サーバー: %s\n", serverUrl.c_str());
            Serial.printf("[WIFI] Voice upload: %s\n", voiceUploadUrl.c_str());

            esp_wifi_set_ps(WIFI_PS_NONE);
            Serial.println("[WIFI] Power save disabled");

            return;
        }

        // このネットワークに繋がらなかった → 次を試す
        WiFi.disconnect();
        Serial.printf("\n[WIFI] %s に接続できませんでした\n", NETWORKS[i].ssid);
    }

    // 全部失敗
    Serial.println("[WIFI] ❌ すべてのネットワークへの接続に失敗しました");
}