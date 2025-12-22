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

// ============== Modbus 设备设置 ==============
#define BAUD_RATE 115200
#define NUM_DEVICES 4
#define NUM_INPUTS_PER_DEVICE 48

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

// ============== 硬件映射表 ==============
// struct HardwareCoord {
//     uint8_t row; // 行
//     uint8_t col; // 列
// };

// const HardwareCoord INDEX_MAP[144] = {
//     {6,0},{6,1},{6,2},{6,3},{6,4},{6,5},{7,0},{7,1},{7,2},{7,3},{7,4},{7,5},
//     {8,0},{8,1},{8,2},{8,3},{8,4},{8,5},{9,0},{9,1},{9,2},{9,3},{9,4},{9,5},
//     {10,1},{10,0},{10,2},{10,3},{10,4},{10,5},{11,0},{11,1},{11,2},{11,3},{11,4},{11,5},
//     {0,3},{0,1},{0,2},{0,0},{0,4},{0,5},{1,0},{1,1},{1,2},{1,3},{1,4},{1,5},
//     {2,0},{2,1},{2,2},{2,3},{2,4},{2,5},{3,0},{3,1},{3,2},{3,4},{3,3},{3,5},
//     {4,0},{4,1},{4,2},{4,3},{4,4},{4,5},{5,0},{5,1},{5,2},{5,3},{5,4},{5,5},
//     {6,11},{6,10},{6,9},{6,8},{6,7},{6,6},{7,11},{7,10},{7,9},{7,8},{7,7},{7,6},
//     {8,11},{8,10},{8,9},{8,8},{8,7},{8,6},{9,11},{9,10},{9,9},{9,8},{9,7},{9,6},
//     {10,11},{10,10},{10,9},{10,8},{10,7},{10,6},{11,11},{11,10},{11,9},{11,8},{11,7},{11,6},
//     {0,11},{0,10},{0,9},{0,7},{0,8},{0,6},{1,11},{1,10},{1,9},{1,8},{1,7},{1,6},
//     {2,11},{2,10},{2,9},{2,8},{2,7},{2,6},{3,11},{3,10},{3,9},{3,8},{3,7},{3,6},
//     {4,11},{4,10},{4,9},{4,8},{4,7},{4,6},{5,11},{5,10},{5,9},{5,8},{5,7},{5,6}
// };

// ============== 全局对象 ==============
HardwareSerial rs485Serial(1);
WiFiClient espClient;
PubSubClient client(espClient);
LaserWebServer webServer;

// ============== 计时变量 ==============
unsigned long lastTriggerTime = 0;
unsigned long lastReadFailTime = 0;
unsigned long lastScanTime = 0;
int currentDevice = 1;

// ============== 基线变量 ==============
unsigned long baselineSetTime = 0;
unsigned long lastBaselineCheck = 0;

uint8_t baseline[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_0[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_1[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_2[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

uint8_t baselineMask[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

unsigned long baselineDelay = 200;       // 开始扫描前的等待时间 (200ms)
unsigned long scanInterval = 30;         // 监测扫描间隔
unsigned long baselineScanInterval = 20; // 基线扫描之间的间隔 (20ms)
unsigned long baselineStableTime = 50;   // 进入监测状态前的短暂等待

bool triggerSent = false;

// ============== 触发灵敏度调整参数 ==============
// 1. deviationThreshold (差异阈值):
//    - 含义: 当前扫描到的有效点数比基线少的数量。
//    - 调整: 如果系统过于灵敏（误触发），增加此值（例如改为 2 或 3）。
//    - 默认值: 1 (只要少 1 个点就触发)
const int deviationThreshold = 1;

// 2. consecutiveThreshold (连续确认次数):
//    - 含义: 连续多少次扫描检测到差异才触发报警。
//    - 调整: 增加此值可以过滤掉偶尔的通信错误或抖动。
//    - 默认值: 1 (立即触发)
const int consecutiveThreshold = 1;

int consecutiveCount = 0;
int baselineBitCount = 0;
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true); // 启用WiFi自动重连
  WiFi.persistent(true);       // 保存WiFi配置
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

    if (currentState == BASELINE_WAITING || currentState == BASELINE_INIT_0 ||
        currentState == BASELINE_INIT_1 || currentState == BASELINE_INIT_2 ||
        currentState == BASELINE_CALC) {
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

  if (now - lastReconnectAttempt > 5000) { // 3秒→5秒，减少重连频率
    lastReconnectAttempt = now;

    Serial.print("MQTT reconnecting... ");
    if (client.connect(mqtt_client_id)) {
      client.subscribe(changeState_topic);
      client.subscribe(btn_resetAll_topic);
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
  // 清空接收缓冲区，避免残留数据干扰
  while (rs485Serial.available()) {
    rs485Serial.read();
  }

  // 构建并发送Modbus请求
  uint8_t request[] = {deviceAddress, 0x02, 0x00,
                       0x00,          0x00, NUM_INPUTS_PER_DEVICE};
  uint16_t crc = crc16(request, sizeof(request));

  uint8_t full_request[sizeof(request) + 2];
  memcpy(full_request, request, sizeof(request));
  full_request[sizeof(request)] = crc & 0xFF;
  full_request[sizeof(request) + 1] = (crc >> 8) & 0xFF;

  rs485Serial.write(full_request, sizeof(full_request));

  // 计算期望的响应长度
  const int responseLength = 3 + (NUM_INPUTS_PER_DEVICE + 7) / 8 + 2;
  uint8_t response[responseLength];

  // @ 115200波特率，15ms完全够用（3-5倍安全余量）
  unsigned long startMicros = micros();
  const unsigned long timeoutMicros = 25000; // 为了稳定性增加到 25ms

  // 等待足够的响应数据
  while (rs485Serial.available() < responseLength) {
    if (micros() - startMicros > timeoutMicros) {
      // 超时，清空可能的部分数据
      while (rs485Serial.available()) {
        rs485Serial.read();
      }
      return false;
    }
    // 短暂让出CPU，减少轮询开销
    delayMicroseconds(50);
  }

  rs485Serial.readBytes(response, responseLength);

  // CRC校验
  uint16_t crc_rx =
      (response[responseLength - 1] << 8) | response[responseLength - 2];
  uint16_t crc_calc = crc16(response, responseLength - 2);

  if (crc_rx != crc_calc)
    return false;

  // 解析数据位（优化版：简洁的三元表达式）
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

bool scanBaseline(uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
  for (int d = 1; d <= NUM_DEVICES; d++) {
    if (!readInputStatus(d, arr[d - 1])) {
      Serial.printf("Error: Baseline scan failed at Device %d\n", d);
      return false;
    }
    delay(2); // Reduced delay for speed while maintaining stability
  }
  return true;
}

// ========== UPDATED calculateFinalBaseline() ==========
void calculateFinalBaseline() {
  // 预先清零数组（优化：使用memset批量操作）
  memset(baseline, 0, sizeof(baseline));
  memset(baselineMask, 0, sizeof(baselineMask));

  // 3次扫描: 与逻辑
  baselineBitCount = 0;
  for (int d = 0; d < NUM_DEVICES; d++) {
    for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {

      // 与逻辑: 3次扫描必须全为1
      if (init_0[d][i] && init_1[d][i] && init_2[d][i]) {
        baseline[d][i] = 1;
        baselineMask[d][i] = 1;
        baselineBitCount++;
      }
    }
  }

  Serial.printf("\nTotal baseline bits: %d / 192 (3-scan AND logic)\n",
                baselineBitCount);

  // 显示最终基线数据
  printDeviceData("FINAL BASELINE", baseline);

  Serial.println("\n✓✓✓ BASELINE ESTABLISHED ✓✓✓");
  Serial.printf("Monitoring active (scan interval: %lums)\n", scanInterval);

  currentState = BASELINE_ACTIVE;
  lastBaselineCheck = millis() + baselineStableTime;
  consecutiveCount = 0;
}
// ========== UPDATED checkForChanges() ==========
bool checkForChanges() {

  if (currentState != BASELINE_ACTIVE)
    return false;

  uint8_t currentScan[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
  int deviationCount = 0;

  // 扫描当前状态
  for (int d = 1; d <= NUM_DEVICES; d++) {
    if (!readInputStatus(d, currentScan[d - 1])) {
      Serial.printf("Monitor scan failed at Device %d\n", d);
      return false;
    }
    delay(2); // Reduced delay for speed
  }

  // 打印扫描结果 (调试)
  printDeviceData("MONITOR SCAN", currentScan);

  // 比较总点数 (抗抖动)
  int currentBitCount = countActiveBits(currentScan);
  int diff = baselineBitCount - currentBitCount;

  // 仅当有效点数少于基线时触发
  if (diff > 0) {
    deviationCount = diff;
  } else {
    deviationCount = 0;
  }

  if (deviationCount >= deviationThreshold) {

    consecutiveCount++;

    Serial.printf("Deviation %d / threshold %d (consecutive %d/%d)\n",
                  deviationCount, deviationThreshold, consecutiveCount,
                  consecutiveThreshold);

    if (consecutiveCount >= consecutiveThreshold) {
      Serial.println(">>> TRIGGER CONFIRMED <<<");
      consecutiveCount = 0;
      return true;
    }
  } else {
    consecutiveCount = 0;
  }

  return false;
}

// ========== trigger publish ==========
void handleTriggerDetected() {
  if (triggerSent)
    return;

  if (!client.connected()) {
    Serial.println("Trigger pending - MQTT disconnected");
    // 不要设置 triggerSent=true, 以便在下次循环重试
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

// ========== setup ==========
void setup() {
  Serial.begin(115200);

  rs485Serial.begin(BAUD_RATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  rs485Serial.setPins(-1, -1, -1, RS485_DE_RE_PIN);
  rs485Serial.setMode(UART_MODE_RS485_HALF_DUPLEX);

  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setBufferSize(1024);  // 增加 MQTT 缓冲区到 512 字节（默认 256）
  client.setKeepAlive(60);     // 设置 keepalive 为60秒
  client.setSocketTimeout(15); // 设置 socket 超时为15秒
  client.setCallback(callback);

  webServer.begin();

  currentState = ACTIVE;
  Serial.println("System ready.");
}

// ========== main loop ==========
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
        currentState = ACTIVE; // 返回 ACTIVE 状态以允许重试
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
    if (now - lastBaselineCheck >= scanInterval) {
      lastBaselineCheck = now;

      if (checkForChanges()) {
        handleTriggerDetected();
      }
    }
    break;
  }
}
