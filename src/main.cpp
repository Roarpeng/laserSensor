#include "WebServer.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstring>
#include <Preferences.h> 

// ============== RS485 引脚定义 ==============
#define RS485_TX_PIN 17
#define RS485_RX_PIN 18
#define RS485_DE_RE_PIN 21

// ============== WiFi STA 凭据 ==============
const char *ssid = "LC_01";
const char *password = "12345678";

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
const int DEVICE_TOLERANCE[NUM_DEVICES] = { 1, 1, 1, 1 }; 

// 2. [独立去抖] 连续确认次数 (Debounce)
//    含义：该设备必须“连续”多少次扫描都处于异常状态，才触发最终报警。
//    数组顺序：{设备1设置, 设备2设置, 设备3设置, 设备4设置}
//    建议：需要极快响应设为 1 或 2，需要极高抗干扰设为 3 到 5。
//    注意：扫描间隔约 30ms，设置为 3 大约意味着持续遮挡 90ms 才报警。
const int DEVICE_DEBOUNCE[NUM_DEVICES]  = { 2, 2, 2, 2 };

unsigned long baselineDelay = 200;  // 基线设定延时 (ms)       
unsigned long scanInterval = 30;    // 扫描间隔 (ms)        
unsigned long baselineScanInterval = 20;    // 基线扫描间隔 (ms)
unsigned long baselineStableTime = 50;      // 基线稳定时间 (ms) 

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



bool triggerSent = false;

// [新增] 加载/保存屏蔽配置
void loadShieldingConfig() {
  preferences.begin("shielding", false);
  size_t read = preferences.getBytes("mask", globalShielding, sizeof(globalShielding));
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
  Serial.println("Shielding config saved to Flash");
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
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
    if (currentState == IDLE) return;
    
    if (currentState >= BASELINE_WAITING && currentState <= BASELINE_CALC) {
      return;
    }

    Serial.println("\n=== START BASELINE SCANS ===");
    currentState = BASELINE_WAITING;
    baselineSetTime = millis() + baselineDelay;
    triggerSent = false;
    
    // 重置所有设备的计数器
    for(int i=0; i<NUM_DEVICES; i++) currentConsecutiveErrors[i] = 0;
    
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

  uint8_t request[] = {deviceAddress, 0x02, 0x00, 0x00, 0x00, NUM_INPUTS_PER_DEVICE};
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
      while (rs485Serial.available()) rs485Serial.read();
      return false;
    }
    delayMicroseconds(50);
  }

  rs485Serial.readBytes(response, responseLength);

  uint16_t crc_rx = (response[responseLength - 1] << 8) | response[responseLength - 2];
  uint16_t crc_calc = crc16(response, responseLength - 2);

  if (crc_rx != crc_calc) return false;

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

void printDeviceData(const char *label, uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
  Serial.printf("\n=== %s ===\n", label);
  for (int d = 1; d <= NUM_DEVICES; d++) {
    Serial.printf("Device %d: ", d);
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      Serial.print(arr[d - 1][i]);
    }
    Serial.println();
  }
}

int countActiveBits(uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
  int cnt = 0;
  for (int d = 0; d < NUM_DEVICES; d++)
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      if (globalShielding[d][i]) continue; // [新增]
      if (arr[d][i])
        cnt++;
    }
  return cnt;
}

int countSingleDeviceBits(int deviceIdx, uint8_t *deviceArr) {
  int cnt = 0;
  for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
    // [新增] 如果该点被屏蔽，不计入有效点数
    if (globalShielding[deviceIdx][i]) continue;
    if (deviceArr[i]) cnt++;
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
      Serial.printf("Warning: Device %d read failed, retrying (%d/3)...\n", d, retry + 1);
      delay(10); 
    }

    if (!success) {
      Serial.printf("Error: Baseline scan failed PERMANENTLY at Device %d\n", d);
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
  for(int i=0; i<NUM_DEVICES; i++) currentConsecutiveErrors[i] = 0;

  int totalBits = 0;

  for (int d = 0; d < NUM_DEVICES; d++) {
    int deviceBits = 0;
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      // [新增] 跳过屏蔽点
      if (globalShielding[d][i]) continue;
      
      if (init_0[d][i] && init_1[d][i] && init_2[d][i]) {
        baseline[d][i] = 1;
        deviceBits++;
      }
    }
    baselineDeviceCounts[d] = deviceBits;
    totalBits += deviceBits;
    
    Serial.printf("Device %d Baseline Bits: %d\n", d + 1, deviceBits);
  }

  Serial.printf("Total baseline bits (Global): %d / 192\n", totalBits);
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
  bool anyDeviceTriggered = false; // 只要有一个设备确认触发，就置为true

  // 1. 扫描所有设备
  for (int d = 1; d <= NUM_DEVICES; d++) {
    if (!readInputStatus(d, currentScan[d - 1])) {
      Serial.printf("Monitor scan failed at Device %d\n", d);
      lastReadFailTime = millis();
      return false;
    }
    delay(3);
  }

  // 2. 打印日志 (每200ms)
  if (millis() - lastLogTime > 200) {
    printDeviceData("MONITOR SCAN", currentScan);
    lastLogTime = millis();
    
    // [新增] 同步数据到 WebServer 并广播
    for(int d=1; d<=NUM_DEVICES; d++) {
      webServer.updateAllDeviceStates(d, currentScan[d-1]);
    }
    webServer.broadcastStates();
  }

  // 3. 逐个设备独立判断
  for (int d = 0; d < NUM_DEVICES; d++) {
    int currentCount = countSingleDeviceBits(d, currentScan[d]);
    int baselineCount = baselineDeviceCounts[d];
    
    // 计算缺失点数 (基线 - 当前)
    int diff = baselineCount - currentCount;

    // 获取该设备的配置参数
    int myTolerance = DEVICE_TOLERANCE[d];
    int myDebounceTarget = DEVICE_DEBOUNCE[d];

    // 判断逻辑：当前设备缺失数 >= 该设备的容差
    if (diff >= myTolerance) {
      // 增加该设备的连续异常计数
      currentConsecutiveErrors[d]++;

      Serial.printf(">> Dev %d ALARM: Missing %d (Thresh %d). Count %d/%d\n", 
                    d+1, diff, myTolerance, currentConsecutiveErrors[d], myDebounceTarget);

      // 检查是否达到该设备的确认次数
      if (currentConsecutiveErrors[d] >= myDebounceTarget) {
        anyDeviceTriggered = true; // 标记触发
        // 注意：这里不重置计数器，或者重置都可以。
        // 为了防止连续发MQTT，通常在 handleTriggerDetected 里控制
      }

    } else {
      // 如果该设备本次正常（或误差在容差内），重置该设备的计数器
      if (currentConsecutiveErrors[d] > 0) {
        Serial.printf("Dev %d recovered (Count reset)\n", d+1);
      }
      currentConsecutiveErrors[d] = 0;
    }
  }

  // 4. 如果有任意一个设备满足了触发条件
  if (anyDeviceTriggered) {
    Serial.println(">>> TRIGGER CONFIRMED (By at least one device) <<<");
    // 重置所有计数器？或者保留？
    // 这里选择重置，以便下一个周期重新检测
    for(int i=0; i<NUM_DEVICES; i++) currentConsecutiveErrors[i] = 0;
    return true;
  }

  return false;
}

void handleTriggerDetected() {
  if (triggerSent) return;

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

  loadShieldingConfig(); // [新增] 加载配置

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
  case IDLE: break;
  case ACTIVE: break;

  // [新增] 定时从 WebServer 同步并保存配置 (如果有更优雅的回调更好，这里采用轮询同步)
  static unsigned long lastSyncTime = 0;
  if (now - lastSyncTime > 1000) {
    bool changed = false;
    for(int d=0; d<NUM_DEVICES; d++) {
      for(int i=0; i<NUM_INPUTS_PER_DEVICE; i++) {
        uint8_t val = webServer.isShielded(d+1, i+1) ? 1 : 0;
        if (globalShielding[d][i] != val) {
          globalShielding[d][i] = val;
          changed = true;
        }
      }
    }
    if (changed) {
      saveShieldingConfig();
    }
    lastSyncTime = now;
  }

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
      Serial.printf("Scan #0 completed: %d active bits\n", countActiveBits(init_0));
      // [新增] 同步初始扫描到 WebServer
      for(int d=1; d<=NUM_DEVICES; d++) webServer.updateAllDeviceStates(d, init_0[d-1]);
      webServer.broadcastStates();
      
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
      Serial.printf("Scan #1 completed: %d active bits\n", countActiveBits(init_1));
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
      Serial.printf("Scan #2 completed: %d active bits\n", countActiveBits(init_2));
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