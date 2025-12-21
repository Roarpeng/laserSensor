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
const char* btn_resetAll_topic = "btn/resetAll";  // 系统激活主题

// ============== Modbus Device Settings ==============
#define BAUD_RATE 9600
#define NUM_DEVICES 4
#define NUM_INPUTS_PER_DEVICE 48

// ============== System State Machine ==============
enum SystemState {
    IDLE,              // 初始状态：等待 btn/resetAll 激活
    ACTIVE,            // 激活状态：等待 changeState 设置基线
    BASELINE_WAITING,  // 等待基线延迟
    BASELINE_INIT_0,   // 第一次基线扫描
    BASELINE_INIT_1,   // 第二次基线扫描
    BASELINE_CALC,     // 计算最终基线
    BASELINE_ACTIVE    // 基线监控中：检测与基线的差异
};
SystemState currentState = ACTIVE;

// ============== Hardware Mapping Table ==============
struct HardwareCoord {
    uint8_t row;
    uint8_t col;
};

const HardwareCoord INDEX_MAP[144] = {
    // --- Index 0 - 11 (对应 map 里的 Row 0) ---
    {6,0}, {6,1}, {6,2}, {6,3}, {6,4}, {6,5}, {7,0}, {7,1}, {7,2}, {7,3}, {7,4}, {7,5},
    // --- Index 12 - 23 ---
    {8,0}, {8,1}, {8,2}, {8,3}, {8,4}, {8,5}, {9,0}, {9,1}, {9,2}, {9,3}, {9,4}, {9,5},
    // --- Index 24 - 35 ---
    {10,1}, {10,0}, {10,2}, {10,3}, {10,4}, {10,5}, {11,0}, {11,1}, {11,2}, {11,3}, {11,4}, {11,5},
    // --- Index 36 - 47 
    {0,3}, {0,1}, {0,2}, {0,0}, {0,4}, {0,5}, {1,0}, {1,1}, {1,2}, {1,3}, {1,4}, {1,5},
    // --- Index 48 - 59 ---
    {2,0}, {2,1}, {2,2}, {2,3}, {2,4}, {2,5}, {3,0}, {3,1}, {3,2}, {3,4}, {3,3}, {3,5},
    // --- Index 60 - 71 ---
    {4,0}, {4,1}, {4,2}, {4,3}, {4,4}, {4,5}, {5,0}, {5,1}, {5,2}, {5,3}, {5,4}, {5,5},
    // --- Index 72 - 83 ---
    {6,11}, {6,10}, {6,9}, {6,8}, {6,7}, {6,6}, {7,11}, {7,10}, {7,9}, {7,8}, {7,7}, {7,6},
    // --- Index 84 - 95 ---
    {8,11}, {8,10}, {8,9}, {8,8}, {8,7}, {8,6}, {9,11}, {9,10}, {9,9}, {9,8}, {9,7}, {9,6},
    // --- Index 96 - 107 ---
    {10,11}, {10,10}, {10,9}, {10,8}, {10,7}, {10,6}, {11,11}, {11,10}, {11,9}, {11,8}, {11,7}, {11,6},
    // --- Index 108 - 119 ---
    {0,11}, {0,10}, {0,9}, {0,7}, {0,8}, {0,6}, {1,11}, {1,10}, {1,9}, {1,8}, {1,7}, {1,6},
    // --- Index 120 - 131 ---
    {2,11}, {2,10}, {2,9}, {2,8}, {2,7}, {2,6}, {3,11}, {3,10}, {3,9}, {3,8}, {3,7}, {3,6},
    // --- Index 132 - 143 ---
    {4,11}, {4,10}, {4,9}, {4,8}, {4,7}, {4,6}, {5,11}, {5,10}, {5,9}, {5,8}, {5,7}, {5,6}
};

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

// ============== Baseline State Variables ==============
unsigned long baselineSetTime = 0;
unsigned long lastBaselineCheck = 0;
uint8_t baseline[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
uint8_t init_0[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];  // 第一次扫描结果
uint8_t init_1[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];  // 第二次扫描结果
uint8_t baselineScanCount = 0;  // 基线扫描计数器
unsigned long baselineDelay = 100; // 基线设置延迟时间(毫秒) 
unsigned long scanInterval = 200; // 扫描周期(毫秒)
unsigned long baselineStableTime = 100; // 基线稳定时间(毫秒) 
bool triggerSent = false; // 标记是否已发送触发消息

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("MQTT: [%s] %d bytes\n", topic, length);

    // 处理 btn/resetAll 主题 - 激活系统
    if (strcmp(topic, btn_resetAll_topic) == 0) {
        Serial.println("✓ btn/resetAll received, activating system");
        currentState = ACTIVE;
        return;
    }

    // 处理 changeState 主题 - 设置基线
    if (strcmp(topic, changeState_topic) == 0) {
        if (currentState == IDLE) {
            Serial.println("⚠ System is IDLE, ignoring changeState message");
            return;
        }

        // 如果正在建立基线过程中，忽略新的changeState消息
        if (currentState == BASELINE_WAITING ||
            currentState == BASELINE_INIT_0 ||
            currentState == BASELINE_INIT_1 ||
            currentState == BASELINE_CALC) {
            Serial.println("⚠ Baseline initialization in progress, ignoring changeState message");
            return;
        }

        Serial.println("\n╔════════════════════════════════════════════╗");
        Serial.println("║      RECEIVED CHANGESTATE - START SCAN     ║");
        Serial.println("╚════════════════════════════════════════════╝");

        currentState = BASELINE_WAITING;
        baselineSetTime = millis() + baselineDelay;
        baselineScanCount = 0;  // 重置扫描计数器
        triggerSent = false; // 重置触发标志，允许下一次触发
        return;
    }

    // 忽略其他主题（如 receiver/changeState，这是其他设备的主题）
}

void reconnect() {
    // 使用静态变量记录重连时间，避免阻塞
    static unsigned long lastReconnectAttempt = 0;
    unsigned long currentTime = millis();

    if (currentTime - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = currentTime;
        Serial.print("Attempting MQTT connection...");

        if (client.connect(mqtt_client_id)) {
            Serial.println("connected");

            // 订阅主题（使用 QoS 1）
            client.subscribe(mqtt_topic, 1);
            client.subscribe(changeState_topic, 1);
            client.subscribe(btn_resetAll_topic, 1);
            Serial.println("✓ Subscribed to MQTT topics");
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" retry in 5s");
        }
    }
}
uint16_t crc16(const uint8_t *data, uint8_t length) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool readInputStatus(uint8_t deviceAddress, uint8_t* status_array) {
    uint8_t request[] = {deviceAddress, 0x02, 0x00, 0x00, 0x00, NUM_INPUTS_PER_DEVICE};
    uint16_t crc = crc16(request, sizeof(request));
    
    uint8_t full_request[sizeof(request) + 2];
    memcpy(full_request, request, sizeof(request));
    full_request[sizeof(request)] = crc & 0xFF;
    full_request[sizeof(request) + 1] = (crc >> 8) & 0xFF;

    rs485Serial.write(full_request, sizeof(full_request));
    
    // Response: Address(1) + Func(1) + ByteCount(1) + Data(ceil(36/8)=5) + CRC(2) = 9 bytes
    const int responseLength = 3 + (NUM_INPUTS_PER_DEVICE + 7) / 8 + 2;
    uint8_t response[responseLength];
    
    unsigned long startTime = millis();
    while (rs485Serial.available() < responseLength) {
        if (millis() - startTime > 1000) { // 1 second timeout
            Serial.printf("Modbus timeout for device %d\n", deviceAddress);
            // Clear buffer on timeout
            while(rs485Serial.available()) rs485Serial.read();
            return false;
        }
    }

    rs485Serial.readBytes(response, responseLength);

    if (response[0] != deviceAddress) {
        Serial.println("Modbus response from wrong device");
        return false;
    }
    
    uint16_t response_crc_received = (response[responseLength - 1] << 8) | response[responseLength - 2];
    uint16_t response_crc_calculated = crc16(response, responseLength - 2);

    if (response_crc_received == response_crc_calculated) {
        // Correctly unpack bits from response bytes
        uint8_t byte_count = response[2];
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; ++i) {
            if (i / 8 < byte_count) {
                int byte_index = 3 + (i / 8);
                int bit_index = i % 8;
                status_array[i] = (response[byte_index] >> bit_index) & 0x01;
            } else {
                status_array[i] = 0; // Should not happen if response is valid
            }
        }
        return true;
    } else {
         Serial.printf("Modbus response CRC error for device %d. Got %04X, expected %04X\n", deviceAddress, response_crc_received, response_crc_calculated);
    }
    return false;
}

// 打印设备状态数据（格式化为可读的网格布局）
void printDeviceData(const char* label, uint8_t deviceArray[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
    Serial.printf("\n========== %s ==========\n", label);
    for (int device = 1; device <= NUM_DEVICES; device++) {
        Serial.printf("Device %d: ", device);
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
            Serial.print(deviceArray[device - 1][i]);
            if ((i + 1) % 12 == 0 && i < NUM_INPUTS_PER_DEVICE - 1) {
                Serial.print(" | "); // 每12位添加分隔符
            }
        }
        Serial.println();
    }
    Serial.println("======================================");
}

// 统计状态数据中为1的位数
int countActiveBits(uint8_t deviceArray[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
    int count = 0;
    for (int device = 0; device < NUM_DEVICES; device++) {
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
            if (deviceArray[device][i] == 1) {
                count++;
            }
        }
    }
    return count;
}

// 执行一次基线扫描并存储到指定的数组
void scanBaseline(uint8_t targetArray[NUM_DEVICES][NUM_INPUTS_PER_DEVICE]) {
    Serial.println("\n>>> Starting baseline scan...");
    for (int device = 1; device <= NUM_DEVICES; device++) {
        bool readSuccess = readInputStatus(device, targetArray[device - 1]);
        if (!readSuccess) {
            Serial.printf("!!! Failed to read device %d for baseline scan\n", device);
            // 失败时保持之前的值
        } else {
            Serial.printf("✓ Device %d scanned successfully\n", device);
        }

        // 设备间延迟，避免 RS485 总线冲突 (优化为10ms)
        if (device < NUM_DEVICES) {
            delay(10);  // 10ms 延迟让设备有时间处理
        }
    }
    Serial.println(">>> Baseline scan completed");
}

// 添加基线掩码数组，用于标记哪些位应该参与后续比较
uint8_t baselineMask[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];

void calculateFinalBaseline() {
    Serial.println("\n╔════════════════════════════════════════════╗");
    Serial.println("║  CALCULATING FINAL BASELINE FROM 2 SCANS  ║");
    Serial.println("╚════════════════════════════════════════════╝");

    // 首先打印两次扫描的原始数据
    printDeviceData("SCAN #0 (init_0)", init_0);
    int count0 = countActiveBits(init_0);
    Serial.printf("Active bits in scan #0: %d / %d\n", count0, NUM_DEVICES * NUM_INPUTS_PER_DEVICE);

    printDeviceData("SCAN #1 (init_1)", init_1);
    int count1 = countActiveBits(init_1);
    Serial.printf("Active bits in scan #1: %d / %d\n", count1, NUM_DEVICES * NUM_INPUTS_PER_DEVICE);

    // 计算逻辑：只有两次都为真的位才会被设为基线
    Serial.println("\n>>> Applying AND logic (only bits that are 1 in BOTH scans)...");

    int totalMasked = 0; // 统计有多少位被启用
    int totalBaseline = 0; // 统计基线中有多少位为1

    for (int device = 1; device <= NUM_DEVICES; device++) {
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
            // 计算该位在两次扫描中为真的次数
            int trueCount = init_0[device - 1][i] + init_1[device - 1][i];

            // 只有两次都为真的位，才设置到基线中并启用掩码
            if (trueCount == 2) {
                baseline[device - 1][i] = 1;
                baselineMask[device - 1][i] = 1;  // 启用该位的比较
                totalMasked++;
                totalBaseline++;
            } else {
                baseline[device - 1][i] = 0;  // 基线中的该位设为0
                baselineMask[device - 1][i] = 0;  // 禁用该位的比较
            }
        }
    }

    // 输出基线计算结果
    printDeviceData("FINAL BASELINE (AND result)", baseline);
    Serial.printf("✓ Final baseline: %d bits set to 1 (will be monitored)\n", totalBaseline);

    printDeviceData("BASELINE MASK (monitored bits)", baselineMask);
    Serial.printf("✓ Monitoring mask: %d / %d bits enabled\n", totalMasked, NUM_DEVICES * NUM_INPUTS_PER_DEVICE);

    // 打印差异分析
    Serial.println("\n>>> Consistency Analysis:");
    Serial.printf("  - Bits lost from scan #0: %d\n", count0 - totalBaseline);
    Serial.printf("  - Bits lost from scan #1: %d\n", count1 - totalBaseline);
    Serial.printf("  - Consistency rate: %.1f%%\n",
                  (totalBaseline * 100.0) / max(count0, count1));

    Serial.println("\n╔════════════════════════════════════════════╗");
    Serial.printf("║  BASELINE READY - Waiting %lu ms for stable  ║\n", baselineStableTime);
    Serial.println("╚════════════════════════════════════════════╝");
    Serial.printf("\n✓ Entering BASELINE_ACTIVE monitoring mode (scanInterval=%lums)\n\n", scanInterval);

    currentState = BASELINE_ACTIVE;
    lastBaselineCheck = millis() + baselineStableTime; // 延迟开始检测，等待基线稳定
}

void setBaseline() {
    Serial.println("Setting baseline state...");
    for (int device = 1; device <= NUM_DEVICES; device++) {
        bool readSuccess = readInputStatus(device, baseline[device - 1]);
        if (!readSuccess) {
            Serial.printf("Failed to read device %d for baseline\n", device);
            // 失败时保持之前的基线值
        } else {
            Serial.printf("Baseline set for device %d\n", device);
        }

        // 设备间延迟，避免 RS485 总线冲突
        if (device < NUM_DEVICES) {
            delay(20);  // 20ms 延迟让设备有时间处理
        }
    }
    Serial.printf("Baseline setup completed, waiting %lu ms for stabilization...\n", baselineStableTime);
    currentState = BASELINE_ACTIVE;
    lastBaselineCheck = millis() + baselineStableTime; // 延迟开始检测，等待基线稳定
}

bool checkForChanges() {
    if (currentState != BASELINE_ACTIVE) return false;

    static unsigned long scanCount = 0; // 扫描计数器
    scanCount++;

    // 简短提示：每次扫描都打印（但不打印完整数据）
    Serial.printf("[Scan #%lu] ", scanCount);

    uint8_t currentScan[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
    bool anyDeviceFailed = false;
    int totalActiveBits = 0;

    // 读取所有设备的当前状态
    for (int device = 1; device <= NUM_DEVICES; device++) {
        bool readSuccess = readInputStatus(device, currentScan[device - 1]);

        if (readSuccess) {
            // 更新Web服务器的设备状态
            webServer.updateAllDeviceStates(device, currentScan[device - 1]);

            // 统计该设备的活跃位
            for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
                if (currentScan[device - 1][i] == 1) {
                    totalActiveBits++;
                }
            }
        } else {
            Serial.printf("⚠ Device %d read failed\n", device);
            anyDeviceFailed = true;
        }

        // 设备间延迟，避免 RS485 总线冲突 (优化为10ms)
        if (device < NUM_DEVICES) {
            delay(10);
        }
    }

    // 如果有设备读取失败，不进行比较
    if (anyDeviceFailed) {
        Serial.println("Read failed, skipping comparison");
        return false;
    }

    // 检查是否有变化，但只对掩码为1的位进行比较
    bool changeDetected = false;
    int totalDeviations = 0;

    for (int device = 1; device <= NUM_DEVICES; device++) {
        for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
            if (baselineMask[device - 1][i] == 1) {  // 只有当掩码为1时才比较
                if (currentScan[device - 1][i] != baseline[device - 1][i]) {
                    if (!changeDetected) {
                        Serial.println("\n\n╔════════════════════════════════════════════╗");
                        Serial.println("║          BASELINE DEVIATION DETECTED!      ║");
                        Serial.println("╚════════════════════════════════════════════╝");
                    }
                    changeDetected = true;
                    totalDeviations++;

                    // 打印详细的差异信息
                    HardwareCoord coord = INDEX_MAP[(device - 1) * NUM_INPUTS_PER_DEVICE + i];
                    Serial.printf("  Device %d, Input %d [Row %d, Col %d]: %d → %d\n",
                                  device, i, coord.row, coord.col,
                                  baseline[device - 1][i], currentScan[device - 1][i]);
                }
            }
        }
    }

    if (changeDetected) {
        Serial.printf("\n✗ Total deviations: %d\n", totalDeviations);
        printDeviceData("TRIGGERED SCAN DATA", currentScan);
    } else {
        // 正常情况：简短输出
        Serial.printf("OK - %d active bits (baseline stable)\n", totalActiveBits);
    }

    return changeDetected;
}

// 统一的触发处理函数
void handleTriggerDetected() {
    if (!triggerSent) {
        Serial.println("\n✓ Publishing MQTT trigger: receiver/triggered");
        client.publish(mqtt_topic, "");
        triggerSent = true;
    }
    // 不改变状态，继续监控等待下一个 changeState
}

// 串口命令处理函数
void handleSerialCommands() {
    if (Serial.available() > 0) {
        String command = Serial.readStringUntil('\n');
        command.trim(); // 去除首尾空白字符

        if (command.startsWith("bd=")) {
            // 设置基线延迟: bd=500
            unsigned long newDelay = command.substring(3).toInt();
            if (newDelay >= 0 && newDelay <= 5000) {
                baselineDelay = newDelay;
                webServer.setBaselineDelay(baselineDelay);
                Serial.printf("✓ Baseline delay set to %lu ms\n", baselineDelay);
            } else {
                Serial.println("✗ Invalid value. Range: 0-5000 ms");
            }
        }
        else if (command.startsWith("si=")) {
            // 设置扫描周期: si=300
            unsigned long newInterval = command.substring(3).toInt();
            if (newInterval >= 100 && newInterval <= 2000) {
                scanInterval = newInterval;
                Serial.printf("✓ Scan interval set to %lu ms\n", scanInterval);
            } else {
                Serial.println("✗ Invalid value. Range: 100-2000 ms");
            }
        }
        else if (command.startsWith("st=")) {
            // 设置基线稳定时间: st=1000
            unsigned long newStableTime = command.substring(3).toInt();
            if (newStableTime >= 0 && newStableTime <= 5000) {
                baselineStableTime = newStableTime;
                Serial.printf("✓ Baseline stable time set to %lu ms\n", baselineStableTime);
            } else {
                Serial.println("✗ Invalid value. Range: 0-5000 ms");
            }
        }
        else if (command == "status" || command == "s") {
            // 显示当前配置
            Serial.println("\n========== System Configuration ==========");
            Serial.printf("Baseline Delay (bd):      %lu ms\n", baselineDelay);
            Serial.printf("Scan Interval (si):       %lu ms\n", scanInterval);
            Serial.printf("Baseline Stable Time (st): %lu ms\n", baselineStableTime);
            Serial.printf("Current State:            %d (0=IDLE, 1=ACTIVE, 2=BASELINE_WAITING, 3=BASELINE_INIT_0, 4=BASELINE_INIT_1, 5=BASELINE_CALC, 6=BASELINE_ACTIVE)\n", currentState);
            Serial.printf("MQTT Connected:           %s\n", client.connected() ? "Yes" : "No");
            Serial.printf("Trigger Sent:             %s\n", triggerSent ? "Yes" : "No");
            Serial.println("==========================================\n");
        }
        else if (command == "baseline" || command == "b") {
            // 打印当前基线数据和掩码
            Serial.println("\n========== Baseline Information ==========");
            printDeviceData("CURRENT BASELINE", baseline);
            int activeBaseline = countActiveBits(baseline);
            Serial.printf("Active baseline bits: %d / %d\n", activeBaseline, NUM_DEVICES * NUM_INPUTS_PER_DEVICE);

            printDeviceData("BASELINE MASK", baselineMask);
            int activeMask = countActiveBits(baselineMask);
            Serial.printf("Monitored bits: %d / %d\n", activeMask, NUM_DEVICES * NUM_INPUTS_PER_DEVICE);
            Serial.println("==========================================\n");
        }
        else if (command == "scan" || command == "sc") {
            // 执行一次手动扫描并显示结果
            Serial.println("\n========== Manual Scan ==========");
            uint8_t manualScan[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
            for (int device = 1; device <= NUM_DEVICES; device++) {
                bool readSuccess = readInputStatus(device, manualScan[device - 1]);
                if (!readSuccess) {
                    Serial.printf("!!! Failed to read device %d\n", device);
                }
                if (device < NUM_DEVICES) {
                    delay(10);  // 优化为10ms
                }
            }
            printDeviceData("MANUAL SCAN RESULT", manualScan);
            int activeBits = countActiveBits(manualScan);
            Serial.printf("Active bits: %d / %d\n", activeBits, NUM_DEVICES * NUM_INPUTS_PER_DEVICE);
            Serial.println("=================================\n");
        }
        else if (command == "help" || command == "h") {
            // 显示帮助信息
            Serial.println("\n========== Serial Commands ==========");
            Serial.println("bd=<value>   Set baseline delay (0-5000 ms)");
            Serial.println("             Example: bd=500");
            Serial.println();
            Serial.println("si=<value>   Set scan interval (100-2000 ms)");
            Serial.println("             Example: si=300");
            Serial.println();
            Serial.println("st=<value>   Set baseline stable time (0-5000 ms)");
            Serial.println("             Example: st=1000");
            Serial.println();
            Serial.println("status, s    Show current configuration");
            Serial.println("baseline, b  Show current baseline and mask data");
            Serial.println("scan, sc     Perform manual scan and show results");
            Serial.println("help, h      Show this help message");
            Serial.println("=====================================\n");
        }
        else if (command.length() > 0) {
            Serial.printf("✗ Unknown command: '%s'. Type 'help' for available commands.\n", command.c_str());
        }
    }
}

void setup() {
    Serial.begin(115200);
    rs485Serial.begin(BAUD_RATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    if (!rs485Serial.setPins(-1, -1, -1, RS485_DE_RE_PIN)) {
        Serial.println("Failed to set RS485 DE/RE pin");
    }
    if (!rs485Serial.setMode(UART_MODE_RS485_HALF_DUPLEX)) {
        Serial.println("Failed to set RS485 mode");
    }

    setup_wifi();
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);
    client.setBufferSize(1024); // 增加 MQTT 缓冲区到 512 字节（默认 256）
    client.setKeepAlive(60); // 设置 keepalive 为60秒
    client.setSocketTimeout(15); // 设置 socket 超时为15秒

    // 启动Web服务器
    webServer.begin();

    // 同步基线延迟设置到WebServer
    webServer.setBaselineDelay(baselineDelay);

    // 初始化所有设备状态为0（确保WebUI有数据显示）
    uint8_t zeroStates[NUM_INPUTS_PER_DEVICE] = {0};
    for (int device = 1; device <= NUM_DEVICES; device++) {
        webServer.updateAllDeviceStates(device, zeroStates);
    }

    // 初始化计时变量
    lastTriggerTime = 0;
    lastReadFailTime = 0;
    lastScanTime = 0;
    currentDevice = 1;

    // 初始化系统状态为 ACTIVE（直接激活，无需等待 btn/resetAll）
    currentState = ACTIVE;
    Serial.println("\n╔════════════════════════════════════════════╗");
    Serial.println("║     LASER SENSOR SYSTEM INITIALIZED        ║");
    Serial.println("║   Status: ACTIVE - Ready for changeState   ║");
    Serial.println("╚════════════════════════════════════════════╝");
    Serial.println("Type 'help' for serial commands\n");
}

void loop() {
    // 非阻塞的 MQTT 重连
    if (!client.connected()) {
        reconnect();
    }
    // 只有连接成功才处理 MQTT 消息
    if (client.connected()) {
        client.loop();
    }

    // 处理串口命令
    handleSerialCommands();

    // 处理Web客户端请求（修复WebUI无法访问问题）
    webServer.handleClient();

    unsigned long currentTime = millis();

    // 定期广播状态到所有SSE客户端
    static unsigned long lastBroadcastTime = 0;
    if (currentTime - lastBroadcastTime >= 1000) {
        webServer.broadcastStates();
        lastBroadcastTime = currentTime;
    }

    // 定期同步WebServer的基线延迟设置
    static unsigned long lastSyncTime = 0;
    if (currentTime - lastSyncTime >= 1000) {
        baselineDelay = webServer.getBaselineDelay();
        lastSyncTime = currentTime;
    }

    // 状态机主逻辑
    switch (currentState) {
        case IDLE:
            // 等待 btn/resetAll MQTT 消息激活系统
            // 在此状态下不进行任何扫描操作
            break;

        case ACTIVE:
            // 激活状态：等待 changeState 设置基线
            // 在此状态下不进行扫描，等待 changeState 消息
            break;

        case BASELINE_WAITING:
            // 等待基线延迟时间到达
            if (currentTime >= baselineSetTime) {
                currentState = BASELINE_INIT_0; // 开始第一次基线扫描
                baselineSetTime = millis() + 50; // 50ms后进行第一次扫描
            }
            break;

        case BASELINE_INIT_0:
            // 等待第一次扫描时间到达
            if (currentTime >= baselineSetTime) {
                Serial.println("\n╔════════════════════════════════════════════╗");
                Serial.println("║       BASELINE SCAN #0 INITIATED           ║");
                Serial.println("╚════════════════════════════════════════════╝");
                scanBaseline(init_0); // 执行第一次扫描
                printDeviceData("RAW SCAN #0 RESULT", init_0);
                int count0 = countActiveBits(init_0);
                Serial.printf("✓ Scan #0: %d active bits captured\n", count0);
                Serial.println(">>> Waiting 50ms before scan #1...\n");
                currentState = BASELINE_INIT_1; // 转到第二次扫描
                baselineSetTime = millis() + 50; // 50ms后进行第二次扫描
            }
            break;

        case BASELINE_INIT_1:
            // 等待第二次扫描时间到达
            if (currentTime >= baselineSetTime) {
                Serial.println("\n╔════════════════════════════════════════════╗");
                Serial.println("║       BASELINE SCAN #1 INITIATED           ║");
                Serial.println("╚════════════════════════════════════════════╝");
                scanBaseline(init_1); // 执行第二次扫描
                printDeviceData("RAW SCAN #1 RESULT", init_1);
                int count1 = countActiveBits(init_1);
                Serial.printf("✓ Scan #1: %d active bits captured\n", count1);
                Serial.println(">>> Proceeding to baseline calculation...\n");
                currentState = BASELINE_CALC; // 转到基线计算阶段
            }
            break;

        case BASELINE_CALC:
            // 计算最终基线（两次扫描结果进行AND运算）
            calculateFinalBaseline(); // 计算并设置最终基线，自动进入BASELINE_ACTIVE状态
            break;

        case BASELINE_ACTIVE:
            // 基线监控模式：根据 scanInterval 检测与基线的差异
            if (currentTime - lastBaselineCheck >= scanInterval) {
                lastBaselineCheck = currentTime;
                if (checkForChanges()) {
                    handleTriggerDetected();
                }
            }
            // 继续停留在 BASELINE_ACTIVE 状态，等待下一个 changeState
            break;
    }
}
