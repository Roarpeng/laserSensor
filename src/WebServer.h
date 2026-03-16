#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

typedef void (*ShieldingChangeCallback)(uint8_t deviceAddr, uint8_t inputNum, bool state);
typedef void (*ClearShieldingCallback)();
typedef void (*TriggerFilterCallback)(int threshold);

class LaserWebServer {
private:
  WiFiServer server;
  WiFiClient clients[4];
  bool isSSEClient[4];
  int clientCount;
  bool isWebServerRunning;
  unsigned long lastUpdateTime;
  unsigned long baselineDelay;
  int triggerFilterThreshold;
  
  uint8_t deviceStates[4][48];
  uint8_t shieldMask[4][48];
  
  ShieldingChangeCallback shieldingChangeCallback;
  ClearShieldingCallback clearShieldingCallback;
  TriggerFilterCallback triggerFilterCallback;
  
  String getHTTPResponse(const String &contentType, const String &content);
  void handleHTTPRequest(WiFiClient &client, int slotIndex);
  void sendWebSocketUpdate(WiFiClient &client, const String &data);
  String getDeviceStatesJSON();
  String getShieldMaskJSON();
  String getHTMLPage();
  String getBaselineDelayJSON();
  String getTriggerFilterJSON();

public:
  LaserWebServer();
  void begin();
  void handleClient();
  void updateDeviceState(uint8_t deviceAddr, uint8_t inputNum, bool state);
  void updateAllDeviceStates(uint8_t deviceAddr, uint8_t *states);
  void broadcastStates();
  
  void setBaselineDelay(unsigned long delay);
  unsigned long getBaselineDelay();
  
  void setShieldState(uint8_t deviceAddr, uint8_t inputNum, bool state);
  bool getShieldState(uint8_t deviceAddr, uint8_t inputNum);
  void loadShielding(uint8_t shielding[4][48]);
  void clearShielding();
  
  void setShieldingChangeCallback(ShieldingChangeCallback callback);
  void setClearShieldingCallback(ClearShieldingCallback callback);
  
  void setTriggerFilterThreshold(int threshold);
  int getTriggerFilterThreshold();
  void setTriggerFilterCallback(TriggerFilterCallback callback);
};

#endif
