#include "WebServer.h"
#include <Arduino.h>
#include <HardwareSerial.h>
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

// ============== 触发灵敏度与容差设置 (请在此处修改) ==============

// 1. [对比容差] 缺失光束阈值
//    含义：只有当总共缺失的光束数量 >= 此值时，才被视为一次“异常检测”。
//    举例：设置为 1，表示少 1 个点就算异常。
//    举例：设置为 3，表示允许少 1-2 个点(作为容差)，少 3 个点才算异常。
const int MISSING_BEAMS_THRESHOLD = 1; 

// 2. [检测次数] 连续确认次数
//    含义：连续多少次扫描都检测到“异常”，才真正触发 MQTT 报警。
//    作用：防止瞬间的电气干扰或飞虫误触发。
//    举例：设置为 2，表示需要连续 2 次扫描(约60ms)都缺失，才报警。
const int TRIGGER_CONFIRM_TIMES = 2;

// ==============================================================

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

// 存储每个设备独立的基线点数
int baselineDeviceCounts[NUM_DEVICES]; 

unsigned long baselineDelay = 200;       
unsigned long scanInterval = 30;         
unsigned long baselineScanInterval = 20; 
unsigned long baselineStableTime = 50;   

bool triggerSent = false;
int consecutiveCount = 0; // 当前连续异常计数

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
    
    // 如果正在建立基线，忽略新的请求
    if (currentState >= BASELINE_WAITING && currentState <= BASELINE_CALC) {
      return;
    }

    Serial.println("\n=== START BASELINE SCANS ===");
    currentState = BASELINE_WAITING;
    baselineSetTime = millis() + baselineDelay;
    triggerSent = false;
    consecutiveCount = 0;
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
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++)
      if (arr[d][i])
        cnt++;
  return cnt;
}

// 计算单个设备数组中的有效位
int countSingleDeviceBits(uint8_t *deviceArr) {
  int cnt = 0;
  for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
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

  int totalBits = 0;

  for (int d = 0; d < NUM_DEVICES; d++) {
    int deviceBits = 0;
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
      // 与逻辑: 3次扫描必须全为1
      if (init_0[d][i] && init_1[d][i] && init_2[d][i]) {
        baseline[d][i] = 1;
        deviceBits++;
      }
    }
    // 存储该设备独立的基线值
    baselineDeviceCounts[d] = deviceBits;
    totalBits += deviceBits;
    
    Serial.printf("Device %d Baseline Bits: %d\n", d + 1, deviceBits);
  }

  Serial.printf("Total baseline bits (Global): %d / 192\n", totalBits);
  printDeviceData("FINAL BASELINE", baseline);

  Serial.println("\n✓✓✓ BASELINE ESTABLISHED (Per-Device Mode) ✓✓✓");
  Serial.printf("Monitoring active (scan interval: %lums)\n", scanInterval);

  currentState = BASELINE_ACTIVE;
  lastBaselineCheck = millis() + baselineStableTime;
  consecutiveCount = 0;
}

// ========== 核心监测逻辑 (支持容差与去抖) ==========
bool checkForChanges() {
  if (currentState != BASELINE_ACTIVE)
    return false;

  uint8_t currentScan[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
  int totalMissingBeams = 0; // 本次扫描所有设备缺失光束的总和

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
    printDeviceData("MONITOR SCAN (Turbo)", currentScan);
    lastLogTime = millis();
  }

  // 3. 逐个设备对比，计算缺失量
  for (int d = 0; d < NUM_DEVICES; d++) {
    int currentCount = countSingleDeviceBits(currentScan[d]);
    int baselineCount = baselineDeviceCounts[d];
    
    // 计算差异: 基线 - 当前
    int diff = baselineCount - currentCount;

    // 逻辑：只有当 diff > 0 (即缺失) 时才计入。
    // 如果 diff < 0 (即当前点数 > 基线，说明有干扰亮点)，忽略，不计入缺失，也不抵消其他设备的缺失。
    if (diff > 0) {
      // 调试打印：仅当发现该设备有缺失时打印
      Serial.printf(">> Dev %d MISSING: Baseline=%d, Curr=%d, Missing=%d\n", 
                    d+1, baselineCount, currentCount, diff);
      totalMissingBeams += diff;
    }
  }

  // 4. 判断是否满足报警条件
  // 条件：(总缺失数 >= 容差阈值)
  if (totalMissingBeams >= MISSING_BEAMS_THRESHOLD) {
    consecutiveCount++; // 满足条件，计数器+1

    Serial.printf("Warning: Total Missing %d (Threshold %d). Consecutive Count: %d/%d\n",
                  totalMissingBeams, MISSING_BEAMS_THRESHOLD, consecutiveCount,
                  TRIGGER_CONFIRM_TIMES);

    // 判断是否连续满足次数
    if (consecutiveCount >= TRIGGER_CONFIRM_TIMES) {
      Serial.println(">>> TRIGGER CONFIRMED (Alarm) <<<");
      consecutiveCount = 0; // 触发后重置计数器 (或根据需求保持)
      return true;
    }
  } else {
    // 如果本次扫描正常（或者缺失数在容差范围内），重置连续计数器
    // 这实现了“必须连续N次”的逻辑，中间断一次就重来
    if (consecutiveCount > 0) {
       Serial.println("Warning cleared (Consecutive count reset).");
    }
    consecutiveCount = 0;
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