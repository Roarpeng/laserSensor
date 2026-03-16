# Project Context for iFlow CLI

## Project Overview
这是一个基于ESP32-S3的激光传感器监测系统项目。该项目通过RS485通信接口使用Modbus协议读取多个激光传感器设备的状态，当检测到激光传感器触发时，通过MQTT协议发送消息到指定的MQTT代理服务器。系统提供WebUI界面用于实时监控和配置。

## Hardware Configuration
- **主控板**: ESP32-S3-DevKitM-1
- **通信接口**: RS485 (半双工模式)
- **波特率**: 115200
- **RS485引脚配置**:
  - TX_PIN: 17
  - RX_PIN: 18
  - DE_RE_PIN: 21

## Software Architecture
### 核心功能模块
1. **WiFi连接模块**: 连接到指定的WiFi网络，支持自动重连
2. **MQTT客户端**: 连接到MQTT代理服务器并发布/订阅消息
3. **Modbus通信模块**: 通过RS485接口读取激光传感器设备状态
4. **CRC校验模块**: 确保Modbus通信数据的完整性
5. **WebServer模块**: 提供WebUI界面和RESTful API
6. **Preferences模块**: 持久化存储配置到Flash

### 技术栈
- **开发框架**: Arduino Framework for ESP32
- **通信协议**: Modbus RTU over RS485, MQTT, HTTP/SSE
- **依赖库**: PubSubClient (MQTT客户端), ArduinoJson (JSON解析)

## Network Configuration
- **WiFi SSID**: LC_01
- **WiFi密码**: 12345678
- **MQTT代理服务器**: 192.168.10.80:1883
- **MQTT客户端ID**: receiver
- **MQTT主题**:
  - `receiver/triggered` - 触发事件发布
  - `changeState` - 状态变更订阅
  - `btn/resetAll` - 重置订阅
  - `debug/printBaseline` - 调试订阅

## Device Configuration
- **设备数量**: 4个Modbus设备
- **每设备输入通道**: 48个数字输入
- **设备地址**: 1-4
- **Modbus功能码**: 0x02 (读取输入状态)

### 设备独立配置参数
```cpp
// 容差：缺失光束阈值
const int DEVICE_TOLERANCE[NUM_DEVICES] = {1, 1, 1, 1};

// 去抖：连续确认次数
const int DEVICE_DEBOUNCE[NUM_DEVICES] = {2, 2, 2, 2};
```

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

### OTA更新
通过WebUI上传固件进行无线更新

## Project Structure
```
laserSensor/
├── src/
│   ├── main.cpp              # 主程序文件
│   ├── WebServer.cpp         # Web服务器实现
│   └── WebServer.h           # Web服务器头文件
├── example/                  # 示例代码和文档
│   ├── ESP32-S3-Relay-1CH-Demo/
│   └── 数字量输入系列使用手册(RS485-RS232-TTL通信接口)-V3.0.pdf
├── lib/                      # 本地库文件
├── include/                  # 头文件
├── test/                     # 测试文件
├── platformio.ini            # PlatformIO配置文件
└── .gitignore               # Git忽略文件
```

## Key Features

### 1. 多设备监控
- 同时监控最多4个Modbus设备
- 每个设备48个输入通道，共192个监测点
- 独立设备配置（容差、去抖）

### 2. 基线建立机制
- 三次扫描取交集（AND逻辑）建立基线
- 支持屏蔽点，屏蔽点不计入基线计算
- 基线变化时自动重新计算

### 3. 触发检测与防护
- **策略A**: 设备读取失败时跳过触发判断
- **策略C**: 连续失败3次才标记设备离线
- **触发过滤**: 大于阈值点数同时触发时过滤（误触发保护）

### 4. 屏蔽点配置
- 通过WebUI点击设置屏蔽点
- 屏蔽点保存���Flash，重启后自动加载
- 支持一键清空所有屏蔽点

### 5. WebUI功能
- 实时显示所有监测点状态
- SSE实时更新，无需刷新页面
- 配置面板：
  - Baseline Delay（基线延迟）
  - Trigger Filter（触发过滤阈值）
  - Shield Config（屏蔽点配置）
  - OTA Update（固件更新）

### 6. 可靠通信
- CRC校验确保数据完整性
- 超时处理机制
- WiFi/MQTT自动重连

## API Endpoints

| 端点 | 方法 | 说明 |
|------|------|------|
| `/` | GET | WebUI主页 |
| `/api/states` | GET | 获取所有设备状态 |
| `/api/shield` | GET | 获取屏蔽点配置 |
| `/api/shield` | POST | 设置单个屏蔽点 |
| `/api/clearShield` | POST | 清空所有屏蔽点 |
| `/api/baselineDelay` | GET/POST | 获取/设置基线延迟 |
| `/api/triggerFilter` | GET/POST | 获取/设置触发过滤阈值 |
| `/events` | GET | SSE事件流 |
| `/update` | POST | OTA固件更新 |

## Operation Flow

### 系统启动流程
1. 初始化串口和RS485通信
2. 连接WiFi网络
3. 连接MQTT代理服务器
4. 启动Web服务器
5. 从Flash加载屏蔽配置和触发过滤阈值
6. 进入ACTIVE状态等待指令

### 基线建立流程
1. 接收MQTT `changeState` 消息
2. 进入BASELINE_WAITING状态，等待延迟时间
3. 执行三次扫描（BASELINE_INIT_0/1/2）
4. 计算最终基线（AND逻辑）
5. 进入BASELINE_ACTIVE状态开始监控

### 触发检测流程
1. 扫描所有设备状态
2. 读取失败的设备跳过判断
3. 逐位比较基线与当前状态
4. 计算缺失点数，累计到总缺失点
5. 判断是否满足触发条件：
   - 缺失点 >= 容差
   - 连续达到去抖次数
   - 总缺失点 < 过滤阈值
6. 发送MQTT触发消息

## Configuration Storage (Flash)

| 键名 | 命名空间 | 说明 |
|------|----------|------|
| `mask` | shielding | 屏蔽点配置 (192字节) |
| `filterThreshold` | trigger | 触发过滤阈值 |

## Troubleshooting

### RS485通信问题
- 检查引脚连接（TX:17, RX:18, DE/RE:21）
- 确认波特率设置（115200）
- 检查设备地址是否正确

### 触发误报
- 检查设备读取是否正常（查看串口日志）
- 调整 `DEVICE_TOLERANCE` 和 `DEVICE_DEBOUNCE`
- 设置合适的触发过滤阈值

### WebUI无法访问
- 确认设备与电脑在同一网络
- 检查WiFi连接状态
- 查看串口输出的IP地址

### 屏蔽点不生效
- 确认已点击LED设置屏蔽
- 检查是否在Shield Config模式下操作
- 重启后确认是否自动加载

## Dependencies
- knolleary/PubSubClient@^2.8 - MQTT客户端库
- ArduinoJson - JSON解析库（内置）
