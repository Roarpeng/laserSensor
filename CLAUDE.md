# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an **ESP32-S3 based industrial IoT monitoring system** for laser sensors. The system bridges legacy Modbus/RS485 industrial sensors with modern web and MQTT technologies, providing real-time monitoring through a web interface and event-driven messaging.

**Key Architecture**: Event-driven, non-blocking state machine with modular design combining RS485/Modbus RTU, WiFi, MQTT, and HTTP/SSE protocols.

## Build Commands

```bash
# Compile the project
pio run

# Upload to ESP32-S3 device
pio run --target upload

# Monitor serial output (115200 baud)
pio device monitor

# Compile and upload in one command
pio run --target upload && pio device monitor

# Clean build files
pio run --target clean
```

**Note**: USB CDC is enabled (`ARDUINO_USB_CDC_ON_BOOT=1`), so Serial communication works over USB.

## Code Architecture

### Core Components

**1. Main Application ([src/main.cpp](src/main.cpp:1))**
- WiFi connection management (Station mode)
- MQTT client for event messaging (PubSubClient library)
- Modbus RTU communication over RS485 (custom implementation)
- Non-blocking state machine using `millis()` timing
- Baseline monitoring mode with configurable delay
- Hardware coordinate mapping system (`INDEX_MAP[144]`)

**2. Web Server ([src/WebServer.h](src/WebServer.h:1), [src/WebServer.cpp](src/WebServer.cpp:1))**
- HTTP server on port 80
- RESTful API endpoints for device states and configuration
- Server-Sent Events (SSE) for real-time updates
- Client connection management (max 4 concurrent clients)
- Responsive HTML/CSS/JavaScript UI (no external frameworks)

### Communication Protocols

**RS485/Modbus RTU**:
- Baud rate: 9600
- Function code: 0x02 (Read Discrete Inputs)
- 4 slave devices (addresses 1-4), 48 inputs per device
- Custom CRC16 checksum implementation
- Timeout handling: 1 second

**MQTT**:
- Broker: 192.168.10.80:1883
- Client ID: "receiver"
- Topics:
  - `receiver/triggered` - Published when laser detection occurs
  - `changeState` - Subscribed to trigger baseline recalibration
  - `tbg/triggered` - Subscribed to activate system
- QoS: 0 (fire-and-forget)

**HTTP/REST API**:
- `GET /` - Main dashboard HTML page
- `GET /api/states` - Device status JSON
- `GET /api/baselineDelay` - Get baseline delay configuration
- `POST /api/baselineDelay` - Update baseline delay (0-5000ms)
- `GET /events` - SSE stream for real-time updates (1-second interval)

### State Machine Logic

The system operates in multiple modes controlled by MQTT messages:

1. **Idle State**: Waiting for `tbg/triggered` MQTT message to activate
2. **Active State**: System monitoring enabled after receiving `tbg/triggered`
3. **Baseline Mode**: Triggered by `changeState` MQTT message:
   - Waits configurable delay (default 200ms, adjustable via WebUI)
   - Captures current sensor states as baseline
   - Continuously compares against baseline (100ms interval)
   - Publishes `receiver/triggered` on first deviation
   - Auto-deactivates system after first trigger
4. **Cooldown State**: 5-second wait after trigger to prevent message flooding

### Hardware Coordinate Mapping

The `INDEX_MAP[144]` array translates linear Modbus input indices to physical (row, col) grid coordinates:
- 12x12 grid layout
- Enables spatial understanding of laser beam detection
- Used for physical positioning and visualization

**Example**: Input index 39 maps to grid position (0, 0) - the physical top-left corner.

### Non-Blocking Design Patterns

All timing uses `millis()` to avoid blocking delays:
- **Trigger Cooldown**: 5-second wait after detection
- **Read Fail Recovery**: 200ms wait on Modbus timeout
- **Device Scan Interval**: 100ms between device polls
- **SSE Broadcast**: 1-second update interval
- **Baseline Check**: 100ms comparison interval

## Hardware Configuration

**Target Board**: ESP32-S3-DevKitM-1 (Dual-core Xtensa LX7 @ 240MHz)

**RS485 Pins**:
```cpp
#define RS485_TX_PIN 17       // Serial transmit
#define RS485_RX_PIN 18       // Serial receive
#define RS485_DE_RE_PIN 21    // Driver Enable / Receiver Enable
```

**Network Configuration** (hardcoded in [src/main.cpp](src/main.cpp:13-14)):
```cpp
const char* ssid = "LC_01";
const char* password = "12345678";
```

**MQTT Configuration** (hardcoded in [src/main.cpp](src/main.cpp:17-21)):
```cpp
const char* mqtt_server = "192.168.10.80";
const char* mqtt_client_id = "receiver";
```

## Code Modification Guidelines

### Adding New Modbus Devices

To change the number of devices or inputs per device:

1. Update constants in [src/main.cpp](src/main.cpp:24-26):
   ```cpp
   #define NUM_DEVICES 4
   #define NUM_INPUTS_PER_DEVICE 48
   ```

2. Update state arrays in [src/WebServer.h](src/WebServer.h:15):
   ```cpp
   uint8_t deviceStates[4][48];
   ```

3. Update baseline arrays in [src/main.cpp](src/main.cpp:80):
   ```cpp
   uint8_t baseline[NUM_DEVICES][NUM_INPUTS_PER_DEVICE];
   ```

4. Regenerate `INDEX_MAP` if physical layout changes

### Modifying Network Settings

Network credentials are hardcoded. To make them configurable:
- Consider adding WiFi Manager library
- Or implement EEPROM/NVS storage for credentials
- Or use web portal for first-time setup

### Changing MQTT Topics

All MQTT topics are defined in [src/main.cpp](src/main.cpp:17-21). Update these constants and corresponding callback logic in `mqtt_callback()` function.

### Customizing Web UI

The HTML page is generated in [src/WebServer.cpp](src/WebServer.cpp:1) in the `getHTMLPage()` method. The UI uses vanilla JavaScript with EventSource API for SSE. Modify this function to change layout, styling, or functionality.

### Adjusting Timing Parameters

Key timing constants:
- **Trigger cooldown**: `5000` (ms) - in main loop after trigger detection
- **Read fail wait**: `200` (ms) - after Modbus timeout
- **Scan interval**: `100` (ms) - between device polls
- **SSE broadcast**: `1000` (ms) - in `broadcastStates()` method
- **Baseline check**: `100` (ms) - in baseline comparison logic

## Dependencies

From [platformio.ini](platformio.ini:19-21):
- `knolleary/PubSubClient@^2.8` - MQTT client library
- `bblanchon/ArduinoJson@^6.21.3` - JSON serialization/deserialization

Both are automatically installed by PlatformIO.

## Debugging and Troubleshooting

### Serial Output
All debug information is printed to Serial at 115200 baud. Use `pio device monitor` to view:
- WiFi connection status
- MQTT connection status
- Modbus communication details
- Trigger events
- State changes

### Common Issues

**RS485 Communication Failures**:
- Check wiring: TX→17, RX→18, DE/RE→21
- Verify baud rate (9600) matches sensors
- Confirm device addresses (1-4)
- Check CRC calculation in `calculateCRC16()` function

**MQTT Connection Issues**:
- Verify broker address: 192.168.10.80:1883
- Check network connectivity: `ping 192.168.10.80`
- Monitor MQTT broker logs
- Confirm firewall settings

**Web Interface Not Accessible**:
- Confirm WiFi connection successful (check Serial output for IP address)
- Verify device IP: `ping [device_ip]`
- Clear browser cache
- Check browser console for SSE connection errors

**Baseline Mode Not Working**:
- Ensure system is activated (received `tbg/triggered` message)
- Verify `changeState` message received before baseline capture
- Check baseline delay setting via `/api/baselineDelay` endpoint
- Monitor Serial output for baseline state changes

## Testing MQTT Integration

Use MQTT client tools to test messaging:

```bash
# Subscribe to trigger events
mosquitto_sub -h 192.168.10.80 -t "receiver/triggered"

# Publish system activation
mosquitto_pub -h 192.168.10.80 -t "tbg/triggered" -m ""

# Publish baseline recalibration
mosquitto_pub -h 192.168.10.80 -t "changeState" -m ""
```

## System Operation Flow

1. **Startup**: Initialize Serial, RS485, WiFi, MQTT, Web Server
2. **Wait for Activation**: System idle until `tbg/triggered` received
3. **Active Monitoring**: Poll Modbus devices sequentially (100ms cycle)
4. **Baseline Capture**: On `changeState` message, wait configured delay, capture current states
5. **Trigger Detection**: Compare current states vs baseline, publish `receiver/triggered` on first difference
6. **Auto-Deactivation**: System returns to idle after first trigger
7. **Web Updates**: Broadcast SSE updates every 1 second regardless of system state

## Chinese Documentation

This project includes comprehensive Chinese documentation:
- [README.md](README.md) - Full project documentation in Chinese
- [IFLOW.md](IFLOW.md) - Project context for iFlow CLI
- [WebUI使用说明.md](WebUI使用说明.md) - Web interface user guide
- `数字量输入系列使用手册.pdf` - Hardware device manual

When interacting with users, consider that documentation is primarily in Chinese and technical terms may be referenced in Chinese.
