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
    BASELINE_ACTIVE    // 基线监控中：检测与基线的差异
};
SystemState currentState = IDLE;

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
unsigned long baselineDelay = 200; // 基线设置延迟时间(毫秒)，可通过串口/WebUI调整
unsigned long scanInterval = 200; // 扫描周期(毫秒)，可通过串口调整
unsigned long baselineStableTime = 500; // 基线稳定时间(毫秒)，建立基线后等待此时间再开始检测
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
    Serial.printf("MQTT recv: topic=[%s]\n", topic);
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");

    // 打印 payload（限制在前100个字符）
    int printLen = length > 100 ? 100 : length;
    for (int i = 0; i < printLen; i++) {
        Serial.print((char)payload[i]);
    }
    if (length > 100) {
        Serial.print("...");
    }
    Serial.println();

    // 调试：打印主题和payload详情
    Serial.printf("Topic debug: len=%d, topic='%s'\n", strlen(topic), topic);
    Serial.printf("Payload length: %d bytes\n", length);
    Serial.printf("Comparing with changeState_topic='%s' (len=%d)\n",
                  changeState_topic, strlen(changeState_topic));
    Serial.printf("Current system state: %d (0=IDLE, 1=ACTIVE, 2=BASELINE_WAITING, 3=BASELINE_ACTIVE)\n",
                  currentState);

    // 处理 btn/resetAll 主题 - 激活系统
    if (strcmp(topic, btn_resetAll_topic) == 0) {
        Serial.printf("btn/resetAll received, activating system to ACTIVE state\n");
        currentState = ACTIVE;
        return;
    }

    // 处理 changeState 主题 - 设置基线
    if (strcmp(topic, changeState_topic) == 0) {
        if (currentState == IDLE) {
            Serial.println("System is IDLE, ignoring changeState message");
            return;
        }

        Serial.printf("changeState received with %d bytes payload\n", length);

        // TODO: 解析 JSON payload 并使用其中的状态数据
        // 当前实现：忽略 payload，延迟后读取传感器状态作为基线
        Serial.printf("Will set baseline after %lu ms delay by reading sensors...\n", baselineDelay);
        currentState = BASELINE_WAITING;
        baselineSetTime = millis() + baselineDelay;
        triggerSent = false; // 重置触发标志，允许下一次触发
        return;
    }

    // 如果没有匹配任何主题，打印警告
    Serial.println("WARNING: Topic not handled!");
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

            // 检查订阅结果并打印详细信息（使用 QoS 1）
            bool sub1 = client.subscribe(mqtt_topic, 1);
            bool sub2 = client.subscribe(changeState_topic, 1);
            bool sub3 = client.subscribe(btn_resetAll_topic, 1);
            // 临时：订阅所有主题用于调试
            bool sub4 = client.subscribe("#", 1);

            Serial.printf("Subscriptions: receiver/triggered=%d, changeState=%d, btn/resetAll=%d, wildcard=#=%d\n",
                          sub1, sub2, sub3, sub4);
            Serial.printf("Listening for topic: '%s' (with QoS 1)\n", changeState_topic);
            Serial.printf("MQTT buffer size: %d bytes\n", client.getBufferSize());
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" will retry in 5 seconds");
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

    for (int device = 1; device <= NUM_DEVICES; device++) {
        uint8_t currentStatus[NUM_INPUTS_PER_DEVICE];
        bool readSuccess = readInputStatus(device, currentStatus);

        if (readSuccess) {
            // 更新Web服务器的设备状态
            webServer.updateAllDeviceStates(device, currentStatus);

            // 检查是否有变化
            for (int i = 0; i < NUM_INPUTS_PER_DEVICE; i++) {
                if (currentStatus[i] != baseline[device - 1][i]) {
                    Serial.printf("Change detected on Device %d, Input %d\n", device, i + 1);
                    return true;
                }
            }
        }

        // 设备间延迟，避免 RS485 总线冲突
        if (device < NUM_DEVICES) {
            delay(20);  // 20ms 延迟让设备有时间处理
        }
    }
    return false;
}

// 统一的触发处理函数
void handleTriggerDetected() {
    if (!triggerSent) {
        Serial.println("Trigger detected, publishing MQTT message (first time)");
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
            Serial.printf("Current State:            %d (0=IDLE, 1=ACTIVE, 2=BASELINE_WAITING, 3=BASELINE_ACTIVE)\n", currentState);
            Serial.printf("MQTT Connected:           %s\n", client.connected() ? "Yes" : "No");
            Serial.printf("Trigger Sent:             %s\n", triggerSent ? "Yes" : "No");
            Serial.println("==========================================\n");
        }
        else if (command == "help" || command == "h") {
            // 显示帮助信息
            Serial.println("\n========== Serial Commands ==========");
            Serial.println("bd=<value>  Set baseline delay (0-5000 ms)");
            Serial.println("            Example: bd=500");
            Serial.println();
            Serial.println("si=<value>  Set scan interval (100-2000 ms)");
            Serial.println("            Example: si=300");
            Serial.println();
            Serial.println("st=<value>  Set baseline stable time (0-5000 ms)");
            Serial.println("            Example: st=1000");
            Serial.println();
            Serial.println("status, s   Show current configuration");
            Serial.println("help, h     Show this help message");
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
    Serial.println("Initialized all device states to 0");

    // 初始化计时变量
    lastTriggerTime = 0;
    lastReadFailTime = 0;
    lastScanTime = 0;
    currentDevice = 1;

    // 初始化系统状态为 IDLE（等待 btn/resetAll 激活）
    currentState = IDLE;
    Serial.println("System initialized in IDLE state, waiting for btn/resetAll...");
    Serial.println("Type 'help' for serial commands");
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

    // 定期打印系统状态（每10秒）
    static unsigned long lastStatusReport = 0;
    if (currentTime - lastStatusReport >= 10000) {
        Serial.printf("Status Report - MQTT: %s, State: %d, Time: %lu\n",
                      client.connected() ? "Connected" : "Disconnected",
                      currentState,
                      currentTime);
        lastStatusReport = currentTime;
    }

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
                setBaseline();
                // setBaseline() 会自动转换到 BASELINE_ACTIVE 状态
            }
            break;

        case BASELINE_ACTIVE:
            // 基线监控模式：根据 scanInterval 检测与基线的差异
            if (currentTime - lastBaselineCheck >= scanInterval) {
                lastBaselineCheck = currentTime;
                if (checkForChanges()) {
                    Serial.println("Baseline deviation detected");
                    handleTriggerDetected();
                }
            }
            // 继续停留在 BASELINE_ACTIVE 状态，等待下一个 changeState
            break;
    }
}
