#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>

class LaserWebServer {
private:
  WiFiServer server;
  WiFiClient clients[4];       // 支持最多4个客户端
  bool isSSEClient[4];         // 标记哪些是 SSE 长连接
  uint8_t deviceStates[4][48]; // 存储所有设备的状态
  uint8_t shieldMask[4][48];   // 屏蔽位掩码: 1=屏蔽, 0=正常
  unsigned long lastUpdateTime;
  bool isWebServerRunning;
  int clientCount;
  unsigned long baselineDelay; // 基线延迟设置

public:
  LaserWebServer();
  void begin();
  void handleClient();
  void updateDeviceState(uint8_t deviceAddr, uint8_t inputNum, bool state);
  void updateAllDeviceStates(uint8_t deviceAddr, uint8_t *states);
  void broadcastStates();

  // 屏蔽功能相关接口
  void setShieldState(uint8_t deviceAddr, uint8_t inputNum, bool state);
  bool getShieldState(uint8_t deviceAddr, uint8_t inputNum);
  String getShieldMaskJSON();
  void loadShieldMask(); // TODO: 从Flash加载
  void saveShieldMask(); // TODO: 保存到Flash
  String getHTMLPage();
  String getDeviceStatesJSON();
  void handleHTTPRequest(WiFiClient &client, int slotIndex);
  String getHTTPResponse(const String &contentType, const String &content);
  void sendWebSocketUpdate(WiFiClient &client, const String &data);
  void setBaselineDelay(unsigned long delay);
  unsigned long getBaselineDelay();
  String getBaselineDelayJSON();
};

#endif