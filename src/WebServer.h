#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>

class LaserWebServer {
private:
    WiFiServer server;
    WiFiClient clients[4]; // 支持最多4个客户端
    bool isSSEClient[4]; // 标记哪些是 SSE 长连接
    uint8_t deviceStates[4][48]; // 存储所有设备的状态
    uint8_t shieldedPoints[4][48]; // [新增] 屏蔽点位状态 (1为屏蔽, 0为正常)
    unsigned long lastUpdateTime;
    bool isWebServerRunning;
    int clientCount;
    unsigned long baselineDelay; // 基线延迟设置

public:
    LaserWebServer();
    void begin();
    void handleClient();
    void updateDeviceState(uint8_t deviceAddr, uint8_t inputNum, bool state);
    void updateAllDeviceStates(uint8_t deviceAddr, uint8_t* states);
    void broadcastStates();
    String getHTMLPage();
    String getDeviceStatesJSON();
    void handleHTTPRequest(WiFiClient& client, int slotIndex);
    String getHTTPResponse(const String& contentType, const String& content);
    void sendWebSocketUpdate(WiFiClient& client, const String& data);
    void setBaselineDelay(unsigned long delay);
    unsigned long getBaselineDelay();
    String getBaselineDelayJSON();

    // [新增] 屏蔽点位相关接口
    void setShielding(uint8_t deviceAddr, uint8_t inputNum, bool shielded);
    bool isShielded(uint8_t deviceAddr, uint8_t inputNum);
    String getShieldingJSON();
    void loadShielding(uint8_t shielding[4][48]); // 从main同步/加载
};

#endif