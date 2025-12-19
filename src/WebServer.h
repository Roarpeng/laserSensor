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
    uint8_t deviceStates[4][48]; // 存储所有设备的状态
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
    void handleHTTPRequest(WiFiClient& client);
    String getHTTPResponse(const String& contentType, const String& content);
    void sendWebSocketUpdate(WiFiClient& client, const String& data);
    void setBaselineDelay(unsigned long delay);
    unsigned long getBaselineDelay();
    String getBaselineDelayJSON();
};

#endif