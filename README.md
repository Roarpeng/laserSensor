# 激光传感器监控系统

基于ESP32-S3的激光传感器实时监控系统，支持通过RS485 Modbus协议读取多个激光传感器设备状态，并提供Web界面实时显示和MQTT消息推送功能。

## 功能特性

- **多设备监控**: 支持最多4个Modbus设备，每个设备48个数字量输入点
- **实时Web界面**: 提供响应式Web界面，实时显示所有传感器状态
- **MQTT消息推送**: 检测到激光触发时自动发送MQTT消息
- **非阻塞设计**: 使用millis()计时，确保系统响应性
- **轻量级架构**: 仅使用ESP32内置功能，无额外依赖库

## 硬件要求

- ESP32-S3-DevKitM-1 开发板
- RS485转TTL模块
- 激光传感器设备（支持Modbus RTU协议）

## 接线说明

| ESP32-S3 | RS485模块 | 功能 |
|----------|-----------|------|
| GPIO 17  | TX        | RS485发送 |
| GPIO 18  | RX        | RS485接收 |
| GPIO 21  | DE/RE     | RS485发送/接收控制 |
| 3.3V     | VCC       | 电源 |
| GND      | GND       | 地线 |

## 软件架构

### 核心模块

1. **主程序 (main.cpp)**
   - WiFi连接管理
   - MQTT客户端通信
   - Modbus设备轮询
   - 非阻塞状态机

2. **Web服务器 (WebServer.h/cpp)**
   - HTTP服务器实现
   - 实时状态推送
   - 响应式Web界面

### 通信协议

- **Modbus RTU**: 读取激光传感器状态
  - 波特率: 9600
  - 功能码: 0x02 (读取输入状态)
  - 设备地址: 1-4

- **MQTT**: 事件消息推送
  - 主题: `receiver/triggered`
  - QoS: 0

- **HTTP/Web**: 实时监控界面
  - 端口: 80
  - 实时更新: Server-Sent Events

## 配置说明

### 网络配置

```cpp
// WiFi设置
const char* ssid = "LC_01";
const char* password = "12345678";

// MQTT设置
const char* mqtt_server = "192.168.10.80";
const char* mqtt_client_id = "receiver";
const char* mqtt_topic = "receiver/triggered";
```

### 设备配置

```cpp
#define NUM_DEVICES 4              // 设备数量
#define NUM_INPUTS_PER_DEVICE 48   // 每设备输入点数
#define BAUD_RATE 9600             // 通信波特率
```

## 编译和上传

### 环境要求

- PlatformIO IDE
- ESP32-S3开发板支持包

### 编译命令

```bash
# 编译项目
pio run

# 上传到设备
pio run --target upload

# 监视串口输出
pio device monitor

# 清理构建文件
pio run --target clean
```

## 使用方法

### 1. 硬件连接

按照接线说明连接ESP32-S3和RS485模块，确保激光传感器正确连接到RS485总线。

### 2. 配置网络

修改代码中的WiFi和MQTT配置，确保设备能连接到网络和MQTT代理。

### 3. 编译上传

使用PlatformIO编译并上传代码到ESP32-S3设备。

### 4. 访问Web界面

设备启动后，通过浏览器访问设备IP地址：
```
http://[设备IP地址]
```

### 5. 监控状态

- Web界面实时显示4个设备×48个输入点的状态
- 红色表示检测到激光触发
- 灰色表示正常状态
- 界面每秒自动更新

## 工作流程

1. **系统初始化**
   - 连接WiFi网络
   - 连接MQTT代理
   - 启动Web服务器
   - 初始化RS485通信

2. **主循环**
   - 处理MQTT消息
   - 处理Web客户端请求
   - 轮询Modbus设备状态
   - 更新Web界面状态
   - 检测触发并发送MQTT消息

3. **状态管理**
   - 非阻塞计时机制
   - 设备状态缓存
   - 实时Web推送

## 故障排除

### 常见问题

1. **WiFi连接失败**
   - 检查SSID和密码是否正确
   - 确认WiFi信号强度
   - 查看串口输出错误信息

2. **MQTT连接失败**
   - 验证MQTT代理地址和端口
   - 检查网络防火墙设置
   - 确认MQTT代理运行状态

3. **RS485通信失败**
   - 检查接线是否正确
   - 确认波特率设置
   - 验证设备地址配置
   - 检查RS485总线终端电阻

4. **Web界面无法访问**
   - 确认设备IP地址
   - 检查设备是否连接到WiFi
   - 尝试刷新浏览器页面

### 调试方法

1. **串口监视器**
   ```bash
   pio device monitor -b 115200
   ```

2. **网络测试**
   ```bash
   ping [设备IP地址]
   ```

3. **MQTT测试**
   - 使用MQTT客户端工具订阅主题
   - 验证消息是否正常发送

## 技术规格

- **处理器**: ESP32-S3 (双核 Xtensa LX7, 240MHz)
- **内存**: 512KB SRAM, 8MB PSRAM
- **无线**: WiFi 802.11 b/g/n, 蓝牙 5.0
- **接口**: RS485, GPIO, I2C, SPI
- **功耗**: 约 150mA @ 3.3V
- **工作温度**: -40°C to +85°C

## 许可证

本项目采用 MIT 许可证。详见 LICENSE 文件。

## 贡献

欢迎提交 Issue 和 Pull Request 来改进项目。

## 更新日志

### v1.0.0
- 初始版本发布
- 支持4个设备×48个输入点监控
- Web界面实时显示
- MQTT消息推送
- 非阻塞状态机设计