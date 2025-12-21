#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <cstring>
#include "WebServer.h"

// ============== RS485 PINS ==============
#define RS485_TX_PIN 17
#define RS485_RX_PIN 18
#define RS485_DE_RE_PIN 21

// ============== WiFi STA Credentials ==============
const char* ssid = "LC_01";
const char* password = "12345678";

// ============== MQTT Broker Settings ==============
const char* mqtt_server = "192.168.10.80";
const char* mqtt_client_id = "receiver";
const char* mqtt_topic = "receiver/triggered";
const char* changeState_topic = "changeState";
const char* btn_resetAll_topic = "btn/resetAll";

// ============== Modbus Device Settings ==============
#define BAUD_RATE 115200
#define NUM_DEVICES 4
#define NUM_INPUTS_PER_DEVICE 48

// ============== System State Machine ==============
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

// ============== Hardware Mapping Table ==============
// struct HardwareCoord {
//     uint8_t row;
//     uint8_t col;
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

// ============== Global Objects ==============
HardwareSerial rs485Serial(1);
WiFiClient espClient;
PubSubClient client(espClient);
LaserWebServer webServer;

// ============== Timing Variables ==============
unsigned long lastTriggerTime = 0;
unsigned long lastReadFailTime = 0;
unsigned long lastScanTime = 0;
int currentDevice = 1;

// ============== Baseline Variables ==============
unsigned long baselineSetTime = 0;
unsigned long lastBaselineCheck = 0;

uint8_t baseline[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_0[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_1[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_2[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

uint8_t baselineMask[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

unsigned long baselineDelay = 200;        // 优化: 延时
unsigned long scanInterval = 300;        // 优化: 扫描间隔
unsigned long baselineStableTime = 100;   // 优化: 基线稳定时间

bool triggerSent = false;

// ======== NEW THRESHOLD PARAMETERS ========
const int deviationThreshold = 3;   // 偏差阈值
const int consecutiveThreshold = 2; // 连续次数阈值
int consecutiveCount = 0;
void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);  // 启用WiFi自动重连
    WiFi.persistent(true);         // 保存WiFi配置
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

void callback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("MQTT: [%s] %d bytes\n", topic, length);

    if (strcmp(topic, btn_resetAll_topic) == 0) {
        Serial.println("✓ btn/resetAll received, activating system");
        currentState = ACTIVE;
        return;
    }

    if (strcmp(topic, changeState_topic) == 0) {
        if (currentState == IDLE) return;

        if (currentState == BASELINE_WAITING ||
            currentState == BASELINE_INIT_0 ||
            currentState == BASELINE_INIT_1 ||
            currentState == BASELINE_INIT_2 ||
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

    if (now - lastReconnectAttempt > 5000) {  // 3秒→5秒，减少重连频率
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

bool readInputStatus(uint8_t deviceAddress, uint8_t* status_array) {
    // 清空接收缓冲区，避免残留数据干扰
    while(rs485Serial.available()) {
        rs485Serial.read();
    }

    // 构建并发送Modbus请求
    uint8_t request[] = {deviceAddress, 0x02, 0x00, 0x00, 0x00, NUM_INPUTS_PER_DEVICE};
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
    const unsigned long timeoutMicros = 15000;  // 15ms超时

    // 等待足够的响应数据
    while (rs485Serial.available() < responseLength) {
        if (micros() - startMicros > timeoutMicros) {
            // 超时，清空可能的部分数据
            while(rs485Serial.available()) {
                rs485Serial.read();
            }
            return false;
        }
        // 短暂让出CPU，减少轮询开销
        delayMicroseconds(50);
    }

    rs485Serial.readBytes(response, responseLength);

    // CRC校验
    uint16_t crc_rx = (response[responseLength - 1] << 8) | response[responseLength - 2];
    uint16_t crc_calc = crc16(response, responseLength - 2);

    if (crc_rx != crc_calc) return false;

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

void printDeviceData(const char* label, uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
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
            if (arr[d][i]) cnt++;
    return cnt;
}

void scanBaseline(uint8_t arr[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
    for (int d = 1; d <= NUM_DEVICES; d++) {
        readInputStatus(d, arr[d - 1]);
        // @ 115200波特率，无需设备间延迟（理论通信时间~5ms已足够）
    }
}

// ========== UPDATED calculateFinalBaseline() ==========
void calculateFinalBaseline() {
    // 预先清零数组（优化：使用memset批量操作）
    memset(baseline, 0, sizeof(baseline));
    memset(baselineMask, 0, sizeof(baselineMask));

    int totalBaseline = 0;
    int votingStats[4] = {0}; // 统计: [0票, 1票, 2票, 3票]

    // 优化：直接使用0-based索引，避免d-1转换
    for (int d = 0; d < NUM_DEVICES; d++) {
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {

            int sum = init_0[d][i] + init_1[d][i] + init_2[d][i];
            votingStats[sum]++;

            // 多数投票：≥2则视为基线
            if (sum >= 2) {
                baseline[d][i] = 1;
                baselineMask[d][i] = 1;
                totalBaseline++;
            }
            // else分支可省略，因memset已清零
        }
    }

    // 投票统计信息
    Serial.println("\n--- VOTING STATISTICS ---");
    Serial.printf("0 votes (all OFF):  %d bits\n", votingStats[0]);
    Serial.printf("1 vote  (unstable): %d bits\n", votingStats[1]);
    Serial.printf("2 votes (majority): %d bits → BASELINE\n", votingStats[2]);
    Serial.printf("3 votes (all ON):   %d bits → BASELINE\n", votingStats[3]);
    Serial.printf("\nTotal baseline bits: %d / 192\n", totalBaseline);

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

    if (currentState != BASELINE_ACTIVE) return false;

    uint8_t currentScan[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
    int deviationCount = 0;

    // 扫描当前状态
    for (int d = 1; d <= NUM_DEVICES; d++) {
        if (!readInputStatus(d, currentScan[d - 1])) {
            return false;
        }
    }

    // 与基线比较
    for (int d = 1; d <= NUM_DEVICES; d++) {
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {

            if (baselineMask[d - 1][i] &&
                currentScan[d - 1][i] != baseline[d - 1][i]) {
                deviationCount++;
            }
        }
    }

    if (deviationCount >= deviationThreshold) {

        consecutiveCount++;

        Serial.printf("Deviation %d / threshold %d (consecutive %d/%d)\n",
                      deviationCount, deviationThreshold,
                      consecutiveCount, consecutiveThreshold);

        if (consecutiveCount >= consecutiveThreshold) {
            Serial.println(">>> TRIGGER CONFIRMED <<<");
            consecutiveCount = 0;
            return true;
        }
    }
    else {
        consecutiveCount = 0;
    }

    return false;
}

// ========== trigger publish ==========
void handleTriggerDetected() {
    if (!triggerSent) {
        Serial.println("Publishing receiver/triggered");
        client.publish(mqtt_topic, "");
        triggerSent = true;
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
    client.setBufferSize(1024); // 增加 MQTT 缓冲区到 512 字节（默认 256）
    client.setKeepAlive(60); // 设置 keepalive 为60秒
    client.setSocketTimeout(15); // 设置 socket 超时为15秒
    client.setCallback(callback);

    webServer.begin();

    currentState = ACTIVE;
    Serial.println("System ready.");
}

// ========== main loop ==========
void loop() {

    if (!client.connected()) reconnect();
    else client.loop();

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
                baselineSetTime = millis() + 50;
            }
            break;

        case BASELINE_INIT_0:
            if (now >= baselineSetTime) {
                scanBaseline(init_0);
                Serial.printf("Scan #0 completed: %d active bits\n", countActiveBits(init_0));
                printDeviceData("SCAN #0 DATA", init_0);

                Serial.println("\n=== BASELINE SCAN #1 ===");
                currentState = BASELINE_INIT_1;
                baselineSetTime = millis() + 50;
            }
            break;

        case BASELINE_INIT_1:
            if (now >= baselineSetTime) {
                scanBaseline(init_1);
                Serial.printf("Scan #1 completed: %d active bits\n", countActiveBits(init_1));
                printDeviceData("SCAN #1 DATA", init_1);

                Serial.println("\n=== BASELINE SCAN #2 ===");
                currentState = BASELINE_INIT_2;
                baselineSetTime = millis() + 50;
            }
            break;

        case BASELINE_INIT_2:
            if (now >= baselineSetTime) {
                scanBaseline(init_2);
                Serial.printf("Scan #2 completed: %d active bits\n", countActiveBits(init_2));
                printDeviceData("SCAN #2 DATA", init_2);

                Serial.println("\n=== CALCULATING FINAL BASELINE (Majority Vote) ===");
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
