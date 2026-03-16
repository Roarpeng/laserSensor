#include "WebServer.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstring>

// ============== RS485 引脚定义 ==============
#define RS485_TX_PIN 17
#define RS485_RX_PIN 18
#define RS485_DE_RE_PIN 21

// ============== WiFi STA 凭据 ==============
const char *ssid = "LC_01";
const char *password = "12345678";

// ============== 静态IP配置 ==============
IPAddress local_IP(192, 168, 10, 71);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(223, 5, 5, 5);
IPAddress secondaryDNS(223, 6, 6, 6);

// ============== MQTT 代理设置 ==============
const char *mqtt_server = "192.168.10.80";
const char *mqtt_client_id = "receiver";
const char *mqtt_topic = "receiver/triggered";
const char *changeState_topic = "changeState";
const char *btn_resetAll_topic = "btn/resetAll";
const char *debug_printBaseline_topic = "debug/printBaseline";

// ============== Modbus 设备设置 ==============
#define BAUD_RATE 115200
#define NUM_DEVICES 4
#define NUM_INPUTS_PER_DEVICE 48

// ==============================================================================
// ============== [核心配置区] 每个设备独立设置灵敏度和稳定性 ==============
// ==============================================================================

// 1. [独立容差] 缺失光束阈值 (Tolerance)
//    含义：该设备当前有效点数比基线少多少时，视为“异常”。
//    数组顺序：{设备1设置, 设备2设置, 设备3设置, 设备4设置}
//    建议：环境好的设备设为 1，灰尘多或不重要的设备设为 2 或 3。
const int DEVICE_TOLERANCE[NUM_DEVICES] = {1, 1, 1, 1};

// 2. [独立去抖] 连续确认次数 (Debounce)
//    含义：该设备必须“连续”多少次扫描都处于异常状态，才触发最终报警。
//    数组顺序：{设备1设置, 设备2设置, 设备3设置, 设备4设置}
//    建议：需要极快响应设为 1 或 2，需要极高抗干扰设为 3 到 5。
//    注意：扫描间隔约 30ms，设置为 3 大约意味着持续遮挡 90ms 才报警。
const int DEVICE_DEBOUNCE[NUM_DEVICES] = {2, 2, 2, 2};

unsigned long baselineDelay = 350;       // 基线设置延迟，单位毫秒
unsigned long scanInterval = 700;        // 扫描间隔，单位毫秒
unsigned long baselineScanInterval = 35; // 基线扫描间隔，单位毫秒
unsigned long baselineStableTime = 100;  // 基线稳定时间，单位毫秒
// ==============================================================================

// ============== 系统状态机 ==============
enum SystemState {
  IDLE,
  ACTIVE,
  BASELINE_WAITING,
  BASELINE_INIT_0,
  BASELINE_INIT_1,
  BASELINE_INIT_2,
  BASELINE_CALC,
  BASELINE_ACTIVE
};
SystemState currentState = ACTIVE;
Preferences preferences;
uint8_t globalShielding[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

// ============== 全局对象 ==============
HardwareSerial rs485Serial(1);
WiFiClient espClient;
PubSubClient client(espClient);
LaserWebServer webServer;

// ============== 计时变量 ==============
unsigned long lastLogTime = 0;
unsigned long lastReadFailTime = 0;

// ============== 基线变量 ==============
unsigned long baselineSetTime = 0;
unsigned long lastBaselineCheck = 0;

uint8_t baseline[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_0[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_1[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_2[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

// 存储每个设备独立的基线总点数
int baselineDeviceCounts[NUM_DEVICES];

// [新增] 存储每个设备当前的连续异常计数
int currentConsecutiveErrors[NUM_DEVICES];

// [新增] 设备读取失败计数（策略C）
int deviceReadFailCount[NUM_DEVICES];
const int READ_FAIL_THRESHOLD = 3;  // 连续失败3次才认为设备异常

// [新增] 触发点过滤阈值（大于此数量的点同时触发则过滤）
int triggerFilterThreshold = 20;  // 默认20个点

bool triggerSent = false;

// [新增] 加载/保存屏蔽配置
void loadShieldingConfig() {
  preferences.begin("shielding", false);
  size_t read =
      preferences.getBytes("mask", globalShielding, sizeof(globalShielding));
  if (read != sizeof(globalShielding)) {
    memset(globalShielding, 0, sizeof(globalShielding));
    Serial.println("No shielding config found, initialized to 0");
  } else {
    Serial.println("Shielding config loaded from Flash");
  }
  preferences.end();
  webServer.loadShielding(globalShielding);
}

void saveShieldingConfig() {
  preferences.begin("shielding", false);
  preferences.putBytes("mask", globalShielding, sizeof(globalShielding));
  preferences.end();

  // Enhanced logging
  int totalShielded = 0;
  for (int d = 0; d < NUM_DEVICES; d++) {
    int deviceShielded = 0;
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      if (globalShielding[d][i])
        deviceShielded++;
    }
    totalShielded += deviceShielded;
    Serial.printf("Device %d: %d points shielded\n", d + 1, deviceShielded);
  }
  Serial.printf("Total: %d/192 points shielded, saved to Flash\n",
                totalShielded);
}

// [新增] 加载触发过滤阈值
void loadTriggerFilterThreshold() {
  preferences.begin("trigger", false);
  triggerFilterThreshold = preferences.getInt("filterThreshold", 20);
  preferences.end();
  Serial.printf("Trigger filter threshold loaded: %d\n", triggerFilterThreshold);
}

// [新增] 保存触发过滤阈值
void saveTriggerFilterThreshold(int threshold) {
  preferences.begin("trigger", false);
  preferences.putInt("filterThreshold", threshold);
  preferences.end();
  Serial.printf("Trigger filter threshold saved: %d\n", threshold);
}

// [新增] 设置触发过滤阈值的回调
void onTriggerFilterThresholdChanged(int threshold) {
  triggerFilterThreshold = threshold;
  saveTriggerFilterThreshold(threshold);
}

// [新增] 重新计算每个设备的有效基线点数
void recalculateBaselineCounts() {
  int totalBits = 0;
  for (int d = 0; d < NUM_DEVICES; d++) {
    int deviceBits = 0;
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      // 只有物理上是1且没被屏蔽的才算基线
      if (baseline[d][i] == 1 && !webServer.getShieldState(d + 1, i + 1)) {
        deviceBits++;
      }
    }
    baselineDeviceCounts[d] = deviceBits;
    totalBits += deviceBits;
    Serial.printf("Device %d Recalculated Baseline: %d\n", d + 1, deviceBits);
  }
  Serial.printf("Total Recalculated Baseline: %d / 192\n", totalBits);
}

// Callback handler for shielding changes from WebServer
void onShieldingChanged(uint8_t deviceAddr, uint8_t inputNum, bool state) {
  if (deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
    // Update global storage
    globalShielding[deviceAddr - 1][inputNum - 1] = state ? 1 : 0;

    // Save to Flash immediately
    saveShieldingConfig();

    // Sync back to WebServer (fix refresh issue)
    webServer.loadShielding(globalShielding);

    // [关键修复] 当屏蔽改变时，必须重新计算基线参考值，否则会触发误报
    recalculateBaselineCounts();

    Serial.printf("Shield updated: Device %d, Input %d -> %s\n", deviceAddr,
                  inputNum, state ? "SHIELDED" : "UNSHIELDED");
  }
}

// Callback handler for clearing all shielding from WebServer
void onClearShielding() {
  // Clear global storage
  memset(globalShielding, 0, sizeof(globalShielding));
  
  // Save to Flash
  saveShieldingConfig();
  
  // Recalculate baseline
  recalculateBaselineCounts();
  
  Serial.println("All shielding cleared from Flash, baseline recalculated");
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  // 配置静态IP
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure static IP");
  }
  
  WiFi.begin(ssid, password);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(500);
    Serial.print(".");
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed! Restarting...");
    ESP.restart();
  }
}

void callback(char *topic, byte *payload, unsigned int length) {
  Serial.printf("MQTT: [%s] %d bytes\n", topic, length);

  if (strcmp(topic, btn_resetAll_topic) == 0) {
    Serial.println("✓ btn/resetAll received, activating system");
    currentState = ACTIVE;
    return;
  }

  if (strcmp(topic, changeState_topic) == 0) {
    if (currentState == IDLE)
      return;

    if (currentState >= BASELINE_WAITING && currentState <= BASELINE_CALC) {
      return;
    }

    Serial.println("\n=== START BASELINE SCANS ===");
    currentState = BASELINE_WAITING;
    baselineSetTime = millis() + baselineDelay;
    triggerSent = false;

    // 重置所有设备的计数器
    for (int i = 0; i < NUM_DEVICES; i++)
      currentConsecutiveErrors[i] = 0;

    return;
  }
}

void reconnect() {
  static unsigned long lastReconnectAttempt = 0;
  unsigned long now = millis();

  if (now - lastReconnectAttempt > 5000) {
    lastReconnectAttempt = now;
    Serial.print("MQTT reconnecting... ");
    if (client.connect(mqtt_client_id)) {
      client.subscribe(changeState_topic);
      client.subscribe(btn_resetAll_topic);
      client.subscribe(debug_printBaseline_topic);
      Serial.println("connected + subscribed");
    } else {
      Serial.printf("failed, rc=%d\n", client.state());
    }
  }
}

uint16_t crc16(const uint8_t *data, uint8_t length) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

bool readInputStatus(uint8_t deviceAddress, uint8_t *status_array) {
  while (rs485Serial.available()) {
    rs485Serial.read();
  }

  uint8_t request[] = {deviceAddress, 0x02, 0x00,
                       0x00,          0x00, NUM_INPUTS_PER_DEVICE};
  uint16_t crc = crc16(request, sizeof(request));

  uint8_t full_request[sizeof(request) + 2];
  memcpy(full_request, request, sizeof(request));
  full_request[sizeof(request)] = crc & 0xFF;
  full_request[sizeof(request) + 1] = (crc >> 8) & 0xFF;

  rs485Serial.write(full_request, sizeof(full_request));

  const int responseLength = 3 + (NUM_INPUTS_PER_DEVICE + 7) / 8 + 2;
  uint8_t response[responseLength];

  unsigned long startMicros = micros();
  const unsigned long timeoutMicros = 50000;

  while (rs485Serial.available() < responseLength) {
    if (micros() - startMicros > timeoutMicros) {
      while (rs485Serial.available())
        rs485Serial.read();
      return false;
    }
    delayMicroseconds(50);
  }

  rs485Serial.readBytes(response, responseLength);

  uint16_t crc_rx =
      (response[responseLength - 1] << 8) | response[responseLength - 2];
  uint16_t crc_calc = crc16(response, responseLength - 2);

  if (crc_rx != crc_calc)
    return false;

  uint8_t byte_count = response[2];
  for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
    int byte_index = 3 + (i / 8);
    int bit_index = i % 8;
    status_array[i] = (byte_index < (3 + byte_count))
                          ? (response[byte_index] >> bit_index) & 0x01
                          : 0;
  }

  return true;
}

void printDeviceData(const char *label,
                     uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
  Serial.printf("\n=== %s ===\n", label);
  for (int d = 1; d <= NUM_DEVICES; d++) {
    // Print physical state
    Serial.printf("Device %d: ", d);
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      Serial.print(arr[d - 1][i]);
    }
    Serial.println();

    // Print shielding mask (debug)
    Serial.printf("Shield %d: ", d);
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      Serial.print(webServer.getShieldState(d, i + 1) ? 'X' : '-');
    }
    Serial.println();
  }
}

int countActiveBits(uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
  int cnt = 0;
  for (int d = 0; d < NUM_DEVICES; d++) {
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      if (webServer.getShieldState(d + 1, i + 1))
        continue;
      if (arr[d][i])
        cnt++;
    }
  }
  return cnt;
}

int countSingleDeviceBits(int deviceIdx, uint8_t *deviceArr) {
  int cnt = 0;
  for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
    if (webServer.getShieldState(deviceIdx + 1, i + 1))
      continue;
    if (deviceArr[i])
      cnt++;
  }
  return cnt;
}

bool scanBaseline(uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
  for (int d = 1; d <= NUM_DEVICES; d++) {
    bool success = false;
    for (int retry = 0; retry < 3; retry++) {
      if (readInputStatus(d, arr[d - 1])) {
        success = true;
        break;
      }
      Serial.printf("Warning: Device %d read failed, retrying (%d/3)...\n", d,
                    retry + 1);
      delay(10);
    }

    if (!success) {
      Serial.printf("Error: Baseline scan failed PERMANENTLY at Device %d\n",
                    d);
      return false;
    }
    delay(3);
  }
  return true;
}

void calculateFinalBaseline() {
  memset(baseline, 0, sizeof(baseline));
  memset(baselineDeviceCounts, 0, sizeof(baselineDeviceCounts));

  // 清零状态计数器
  for (int i = 0; i < NUM_DEVICES; i++)
    currentConsecutiveErrors[i] = 0;

  int totalBits = 0;

  for (int d = 0; d < NUM_DEVICES; d++) {
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      // 记录物理基线（不管是否屏蔽）
      if (init_0[d][i] && init_1[d][i] && init_2[d][i]) {
        baseline[d][i] = 1;
      }
    }
  }

  // 使用统一函数计算带屏蔽的 baselineDeviceCounts
  recalculateBaselineCounts();

  printDeviceData("FINAL BASELINE", baseline);

  Serial.println("\n✓✓✓ BASELINE ESTABLISHED (Independent Config Mode) ✓✓✓");
  Serial.printf("Monitoring active (scan interval: %lums)\n", scanInterval);

  currentState = BASELINE_ACTIVE;
  lastBaselineCheck = millis() + baselineStableTime;
}

// ========== 核心监测逻辑 (独立设备、独立配置) ==========
bool checkForChanges() {
  if (currentState != BASELINE_ACTIVE)
    return false;

  uint8_t currentScan[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
  bool deviceReadSuccess[NUM_DEVICES] = {false, false, false, false};
  bool anyDeviceTriggered = false;
  int totalMissingBits = 0;  // 累计所有设备的缺失点数

  // 1. 扫描所有设备，记录读取成功/失败
  for (int d = 1; d <= NUM_DEVICES; d++) {
    if (readInputStatus(d, currentScan[d - 1])) {
      deviceReadSuccess[d - 1] = true;
      deviceReadFailCount[d - 1] = 0;  // 重置失败计数
      webServer.updateAllDeviceStates(d, currentScan[d - 1]);
    } else {
      deviceReadFailCount[d - 1]++;
      Serial.printf("Dev %d read failed (count: %d/%d)\n", d, 
                    deviceReadFailCount[d - 1], READ_FAIL_THRESHOLD);
      
      // 策略C：连续失败达到阈值才认为设备异常
      if (deviceReadFailCount[d - 1] >= READ_FAIL_THRESHOLD) {
        Serial.printf("Dev %d marked as OFFLINE after %d consecutive failures\n", 
                      d, READ_FAIL_THRESHOLD);
        // 设备离线，用上一次的数据或保持不动
        // 这里可以选择保持之前的扫描结果，或跳过
      }
      deviceReadSuccess[d - 1] = false;
    }
    delay(3);
  }

  // 2. 打印日志 (每200ms) 并广播到 WebServer
  if (millis() - lastLogTime > 200) {
    printDeviceData("MONITOR SCAN", currentScan);
    lastLogTime = millis();
    webServer.broadcastStates();
  }

  // 3. 逐个设备独立判断
  for (int d = 0; d < NUM_DEVICES; d++) {
    // 策略A：设备读取失败时跳过触发判断
    if (!deviceReadSuccess[d]) {
      Serial.printf("Dev %d: SKIPPED (read failed)\n", d + 1);
      continue;
    }

    // 逐位比较
    int missingBits = 0;
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      bool isShielded = webServer.getShieldState(d + 1, i + 1);
      uint8_t maskedBaseline = isShielded ? 0 : baseline[d][i];
      uint8_t maskedCurrent = isShielded ? 0 : currentScan[d][i];

      if (maskedBaseline == 1 && maskedCurrent == 0) {
        missingBits++;
      }
    }

    totalMissingBits += missingBits;

    // [调试日志] 每2秒打印一次
    static unsigned long lastDebugLog = 0;
    if (millis() - lastDebugLog > 2000) {
      Serial.printf("[DEBUG] Dev %d: MissingBits=%d (bitwise)\n", d + 1, missingBits);
    }

    int myTolerance = DEVICE_TOLERANCE[d];
    int myDebounceTarget = DEVICE_DEBOUNCE[d];

    if (missingBits >= myTolerance) {
      currentConsecutiveErrors[d]++;

      Serial.printf(">> Dev %d ALARM: Missing %d bits (Thresh %d). Count %d/%d\n", 
                    d + 1, missingBits, myTolerance, 
                    currentConsecutiveErrors[d], myDebounceTarget);

      if (currentConsecutiveErrors[d] == 1) {
        Serial.printf("   Missing positions: ");
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
          bool isShielded = webServer.getShieldState(d + 1, i + 1);
          uint8_t maskedBaseline = isShielded ? 0 : baseline[d][i];
          uint8_t maskedCurrent = isShielded ? 0 : currentScan[d][i];
          if (maskedBaseline == 1 && maskedCurrent == 0) {
            Serial.printf("%d ", i + 1);
          }
        }
        Serial.println();
      }

      if (currentConsecutiveErrors[d] >= myDebounceTarget) {
        anyDeviceTriggered = true;
      }

    } else {
      if (currentConsecutiveErrors[d] > 0) {
        Serial.printf("Dev %d recovered (Count reset)\n", d + 1);
      }
      currentConsecutiveErrors[d] = 0;
    }

    if (d == NUM_DEVICES - 1 && millis() - lastDebugLog > 2000) {
      lastDebugLog = millis();
    }
  }

  // 4. 触发判断 + 过滤阈值检查
  if (anyDeviceTriggered) {
    // [新功能] 触发点过滤：如果缺失点数超过阈值，认为是误触发
    if (triggerFilterThreshold > 0 && totalMissingBits >= triggerFilterThreshold) {
      Serial.printf(">>> TRIGGER FILTERED: TotalMissing=%d >= Threshold=%d <<<\n", 
                    totalMissingBits, triggerFilterThreshold);
      for (int i = 0; i < NUM_DEVICES; i++)
        currentConsecutiveErrors[i] = 0;
      return false;  // 过滤掉这次触发
    }
    
    Serial.printf(">>> TRIGGER CONFIRMED: TotalMissing=%d <<<\n", totalMissingBits);
    for (int i = 0; i < NUM_DEVICES; i++)
      currentConsecutiveErrors[i] = 0;
    return true;
  }

  return false;
}

void handleTriggerDetected() {
  if (triggerSent)
    return;

  if (!client.connected()) {
    Serial.println("Trigger pending - MQTT disconnected");
    return;
  }

  Serial.println("Publishing receiver/triggered");
  if (client.publish(mqtt_topic, "")) {
    triggerSent = true;
    Serial.println("Trigger sent successfully");
  } else {
    Serial.println("Trigger send failed");
  }
}

void setup() {
  Serial.begin(115200);

  rs485Serial.begin(BAUD_RATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  rs485Serial.setPins(-1, -1, -1, RS485_DE_RE_PIN);
  rs485Serial.setMode(UART_MODE_RS485_HALF_DUPLEX);

  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setBufferSize(1024);
  client.setKeepAlive(60);
  client.setSocketTimeout(15);
  client.setCallback(callback);

  webServer.begin();

  loadShieldingConfig();                                    // [新增] 加载配置
  webServer.loadShielding(globalShielding);                 // 同步到 WebServer
  webServer.setShieldingChangeCallback(onShieldingChanged); // 注册回调
  webServer.setClearShieldingCallback(onClearShielding);    // 注册清空回调

  loadTriggerFilterThreshold();                             // 加载过滤阈值
  webServer.setTriggerFilterThreshold(triggerFilterThreshold);  // 同步到 WebServer
  webServer.setTriggerFilterCallback(onTriggerFilterThresholdChanged);  // 注册回调

  currentState = ACTIVE;
  Serial.println("System ready.");
}

void loop() {
  if (!client.connected())
    reconnect();
  else
    client.loop();

  webServer.handleClient();

  unsigned long now = millis();

  switch (currentState) {
  case IDLE:
    break;
  case ACTIVE:
    break;

  case BASELINE_WAITING:
    if (now >= baselineSetTime) {
      Serial.println("\n=== BASELINE SCAN #0 ===");
      currentState = BASELINE_INIT_0;
      baselineSetTime = millis() + baselineScanInterval;
    }
    break;

  case BASELINE_INIT_0:
    if (now >= baselineSetTime) {
      if (!scanBaseline(init_0)) {
        Serial.println("Scan #0 FAILED - Aborting");
        currentState = ACTIVE;
        return;
      }
      Serial.printf("Scan #0 completed: %d active bits\n",
                    countActiveBits(init_0));
      Serial.println("\n=== BASELINE SCAN #1 ===");
      currentState = BASELINE_INIT_1;
      baselineSetTime = millis() + baselineScanInterval;
    }
    break;

  case BASELINE_INIT_1:
    if (now >= baselineSetTime) {
      if (!scanBaseline(init_1)) {
        Serial.println("Scan #1 FAILED - Aborting");
        currentState = ACTIVE;
        return;
      }
      Serial.printf("Scan #1 completed: %d active bits\n",
                    countActiveBits(init_1));
      Serial.println("\n=== BASELINE SCAN #2 ===");
      currentState = BASELINE_INIT_2;
      baselineSetTime = millis() + baselineScanInterval;
    }
    break;

  case BASELINE_INIT_2:
    if (now >= baselineSetTime) {
      if (!scanBaseline(init_2)) {
        Serial.println("Scan #2 FAILED - Aborting");
        currentState = ACTIVE;
        return;
      }
      Serial.printf("Scan #2 completed: %d active bits\n",
                    countActiveBits(init_2));
      Serial.println("\n=== CALCULATING FINAL BASELINE (AND Logic) ===");
      currentState = BASELINE_CALC;
    }
    break;

  case BASELINE_CALC:
    calculateFinalBaseline();
    break;

  case BASELINE_ACTIVE:
    if (checkForChanges()) {
      handleTriggerDetected();
    }
    break;
  }
}