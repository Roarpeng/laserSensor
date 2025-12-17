# Project Context for iFlow CLI

## Project Overview
这是一个基于ESP32-S3的激光传感器监测系统项目。该项目通过RS485通信接口使用Modbus协议读取多个激光传感器设备的状态，当检测到激光传感器触发时，通过MQTT协议发送消息到指定的MQTT代理服务器。

## Hardware Configuration
- **主控板**: ESP32-S3-DevKitM-1
- **通信接口**: RS485 (半双工模式)
- **波特率**: 9600
- **RS485引脚配置**:
  - TX_PIN: 17
  - RX_PIN: 18
  - DE_RE_PIN: 21

## Software Architecture
### 核心功能模块
1. **WiFi连接模块**: 连接到指定的WiFi网络
2. **MQTT客户端**: 连接到MQTT代理服务器并发布/订阅消息
3. **Modbus通信模块**: 通过RS485接口读取激光传感器设备状态
4. **CRC校验模块**: 确保Modbus通信数据的完整性

### 技术栈
- **开发框架**: Arduino Framework for ESP32
- **通信协议**: Modbus RTU over RS485, MQTT
- **依赖库**: PubSubClient (MQTT客户端)

## Network Configuration
- **WiFi SSID**: LC_01
- **WiFi密码**: 12345678
- **MQTT代理服务器**: 192.168.10.80:1883
- **MQTT客户端ID**: receiver
- **MQTT主题**: receiver/triggered

## Device Configuration
- **设备数量**: 4个Modbus设备
- **每设备输入通道**: 36个数字输入
- **设备地址**: 1-4
- **Modbus功能码**: 0x02 (读取输入状态)

## Building and Running
### 构建命令
```bash
pio run
```

### 上传命令
```bash
pio run --target upload
```

### 串口监视
```bash
pio device monitor
```

### 清理构建
```bash
pio run --target clean
```

## Development Conventions
1. **代码风格**: 遵循Arduino C++编码规范
2. **错误处理**: 所有通信操作都包含超时和错误检查
3. **调试输出**: 使用Serial输出调试信息和状态
4. **模块化设计**: 功能按模块分离，便于维护和扩展

## Project Structure
```
laserSensor/
├── src/
│   └── main.cpp              # 主程序文件
├── example/                  # 示例代码和文档
│   └── ESP32-S3-Relay-1CH-Demo/
├── lib/                      # 本地库文件
├── include/                  # 头文件
├── test/                     # 测试文件
├── platformio.ini            # PlatformIO配置文件
└── .gitignore               # Git忽略文件
```

## Key Features
1. **多设备支持**: 可同时监控最多4个Modbus设备
2. **实时监控**: 持续扫描所有输入通道，检测激光触发
3. **可靠通信**: 包含CRC校验和超时处理机制
4. **网络通知**: 通过MQTT实时发送触发事件
5. **自动重连**: WiFi和MQTT连接断开时自动重连

## Operation Flow
1. 系统初始化并连接WiFi网络
2. 连接到MQTT代理服务器
3. 循环读取所有Modbus设备的输入状态
4. 检测到任何输入触发时立即发送MQTT消息
5. 发送消息后等待5秒再继续扫描，避免消息 flooding

## Troubleshooting
- **RS485通信问题**: 检查引脚连接和波特率设置
- **WiFi连接问题**: 确认SSID和密码正确
- **MQTT连接问题**: 验证代理服务器地址和端口
- **Modbus超时**: 检查设备地址和通信线路

## Dependencies
- knolleary/PubSubClient@^2.8 - MQTT客户端库