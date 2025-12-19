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
const char* tbg_trigger_topic = "tbg/triggered";

// ============== Modbus Device Settings ==============
#define BAUD_RATE 9600
#define NUM_DEVICES 4
#define NUM_INPUTS_PER_DEVICE 48

// ============== System State Machine ==============
enum SystemState {
    IDLE,              // 初始状态：等待 tbg/triggered 激活
    SCANNING,          // 正常扫描模式：检测任意输入触发
    BASELINE_WAITING,  // 等待基线延迟
    BASELINE_ACTIVE,   // 基线监控中：检测与基线的差异
    TRIGGERED          // 已触发：等待 tbg/triggered 重新激活
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
    // --- Index 36 - 47 (这里包含你要找的 0,0) ---
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
unsigned long baselineDelay = 200; // 基线设置延迟时间(毫秒)，可通过WebUI调整

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
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    for (int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
    }
    Serial.println();

    // 处理 tbg/triggered 主题 - 激活系统
    if (strcmp(topic, tbg_trigger_topic) == 0) {
        Serial.printf("TBG trigger received (from %s state), activating system to SCANNING mode\n",
                      currentState == IDLE ? "IDLE" :
                      currentState == TRIGGERED ? "TRIGGERED" : "ACTIVE");
        currentState = SCANNING;
        currentDevice = 1;
        lastScanTime = 0;
        return;
    }

    // 处理 changeState 主题 - 启动基线模式
    if (strcmp(topic, changeState_topic) == 0) {
        if (currentState == IDLE || currentState == TRIGGERED) {
            Serial.printf("System is in %s state, ignoring changeState message\n",
                         currentState == IDLE ? "IDLE" : "TRIGGERED");
            return;
        }
        Serial.printf("ChangeState message received, will set baseline after %lu ms delay...\n", baselineDelay);
        currentState = BASELINE_WAITING;
        baselineSetTime = millis() + baselineDelay;
        return;
    }
}

void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect(mqtt_client_id)) {
            Serial.println("connected");
            client.subscribe(mqtt_topic);
            client.subscribe(changeState_topic);
            client.subscribe(tbg_trigger_topic);
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            delay(5000);
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
    }
    Serial.println("Baseline setup completed, entering BASELINE_ACTIVE state");
    currentState = BASELINE_ACTIVE;
    lastBaselineCheck = millis();
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
    }
    return false;
}

// 统一的触发处理函数
void handleTriggerDetected() {
    Serial.println("Trigger detected, publishing MQTT message and entering TRIGGERED state");
    client.publish(mqtt_topic, "");
    currentState = TRIGGERED;
    lastTriggerTime = millis();
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

    // 启动Web服务器
    webServer.begin();

    // 同步基线延迟设置到WebServer
    webServer.setBaselineDelay(baselineDelay);

    // 初始化计时变量
    lastTriggerTime = 0;
    lastReadFailTime = 0;
    lastScanTime = 0;
    currentDevice = 1;

    // 初始化系统状态为 IDLE（等待 tbg/triggered 激活）
    currentState = IDLE;
    Serial.println("System initialized in IDLE state, waiting for tbg/triggered...");
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

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
            // 等待 tbg/triggered MQTT 消息激活系统
            // 在此状态下不进行任何扫描操作
            break;

        case SCANNING: {
            // 正常扫描模式：检测任意输入触发

            // 控制扫描频率
            if (currentTime - lastScanTime < 100) {
                break;
            }

            // 执行设备扫描
            uint8_t inputs[NUM_INPUTS_PER_DEVICE];
            bool readSuccess = readInputStatus(currentDevice, inputs);

            if (readSuccess) {
                // 更新Web服务器的设备状态
                webServer.updateAllDeviceStates(currentDevice, inputs);

                // 检查是否有触发
                for (int j = 0; j < NUM_INPUTS_PER_DEVICE; j++) {
                    if (inputs[j] == 1) {
                        Serial.printf("Trigger detected on Device %d, Input %d\n", currentDevice, j + 1);
                        handleTriggerDetected();
                        return;
                    }
                }

                // 移动到下一个设备
                currentDevice++;
                if (currentDevice > NUM_DEVICES) {
                    currentDevice = 1;
                }
                lastScanTime = currentTime;
            } else {
                // 读取失败，记录时间并等待
                lastReadFailTime = currentTime;
                Serial.printf("Read failed for device %d, waiting 200ms...\n", currentDevice);
            }
            break;
        }

        case BASELINE_WAITING:
            // 等待基线延迟时间到达
            if (currentTime >= baselineSetTime) {
                setBaseline();
                // setBaseline() 会自动转换到 BASELINE_ACTIVE 状态
            }
            break;

        case BASELINE_ACTIVE:
            // 基线监控模式：检测与基线的差异
            if (currentTime - lastBaselineCheck >= 100) {
                lastBaselineCheck = currentTime;
                if (checkForChanges()) {
                    Serial.println("Baseline deviation detected");
                    handleTriggerDetected();
                }
            }
            break;

        case TRIGGERED:
            // 触发后冷却期，等待 tbg/triggered 重新激活
            // 冷却期内不处理任何扫描，仅响应 MQTT 消息
            // 系统将保持在此状态直到收到新的 tbg/triggered
            break;
    }
}
