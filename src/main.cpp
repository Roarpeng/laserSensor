#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
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

// ============== Modbus Device Settings ==============
#define BAUD_RATE 9600
#define NUM_DEVICES 4
#define NUM_INPUTS_PER_DEVICE 48

// ============== Global Objects ==============
HardwareSerial rs485Serial(1);
WiFiClient espClient;
PubSubClient client(espClient);
LaserWebServer webServer;

// ============== Timing Variables ==============
unsigned long lastTriggerTime = 0;
unsigned long lastReadFailTime = 0;
unsigned long lastScanTime = 0;
bool triggerWaitActive = false;
bool readFailWaitActive = false;
int currentDevice = 1;

// ============== Baseline State Variables ==============
bool baselineMode = false;
unsigned long baselineSetTime = 0;
unsigned long lastBaselineCheck = 0;
uint8_t baseline[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
bool baselineInitialized = false;
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
    
    // 检查是否是changeState主题
    if (strcmp(topic, changeState_topic) == 0) {
        Serial.printf("ChangeState message received, will set baseline after %d ms delay...\n", baselineDelay);
        baselineMode = true;
        baselineInitialized = false;
        baselineSetTime = millis() + baselineDelay; // 设置延迟时间
    }
}

void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect(mqtt_client_id)) {
            Serial.println("connected");
            client.subscribe(mqtt_topic);
            client.subscribe(changeState_topic);
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
            baseline[device - 1][0] = 0xFF; // 设置为无效值
        } else {
            Serial.printf("Baseline set for device %d\n", device);
        }
    }
    baselineInitialized = true;
    Serial.println("Baseline setup completed");
}

bool checkForChanges() {
    if (!baselineInitialized) return false;
    
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
    triggerWaitActive = false;
    readFailWaitActive = false;
    currentDevice = 1;
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    // 处理Web服务器客户端
    webServer.handleClient();

    unsigned long currentTime = millis();
    
    // 定期同步WebServer的基线延迟设置
    static unsigned long lastSyncTime = 0;
    if (currentTime - lastSyncTime >= 1000) { // 每秒同步一次
        baselineDelay = webServer.getBaselineDelay();
        lastSyncTime = currentTime;
    }
    
    // 基线模式逻辑
    if (baselineMode) {
        // 检查是否到了设置基线的时间
        if (!baselineInitialized && currentTime >= baselineSetTime) {
            setBaseline();
            lastBaselineCheck = currentTime;
        }
        
        // 如果基线已设置，每100ms检查一次变化
        if (baselineInitialized && currentTime - lastBaselineCheck >= 100) {
            lastBaselineCheck = currentTime;
            if (checkForChanges()) {
                Serial.println("State change detected, sending MQTT trigger");
                client.publish(mqtt_topic, "");
                // 可以选择是否退出基线模式或继续监控
                // baselineMode = false; // 如果需要退出基线模式，取消注释
            }
        }
        return; // 基线模式下不执行正常扫描
    }
    
    // 处理触发后的等待状态
    if (triggerWaitActive) {
        if (currentTime - lastTriggerTime >= 5000) {
            triggerWaitActive = false;
            currentDevice = 1; // 重新开始扫描
        } else {
            return; // 在等待期间，不进行其他操作
        }
    }
    
    // 处理读取失败后的等待状态
    if (readFailWaitActive) {
        if (currentTime - lastReadFailTime >= 200) {
            readFailWaitActive = false;
        } else {
            return; // 在等待期间，不进行其他操作
        }
    }
    
    // 控制扫描频率
    if (currentTime - lastScanTime < 100) {
        return; // 还没到扫描时间
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
                Serial.printf("Trigger detected on Device %d, Input %d. Sending MQTT message.\n", currentDevice, j + 1);
                client.publish(mqtt_topic, "");
                // 启动触发等待
                triggerWaitActive = true;
                lastTriggerTime = currentTime;
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
        // 读取失败，启动失败等待
        readFailWaitActive = true;
        lastReadFailTime = currentTime;
        Serial.printf("Read failed for device %d, waiting...\n", currentDevice);
    }
}
