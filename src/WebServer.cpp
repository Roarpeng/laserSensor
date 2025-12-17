#include "WebServer.h"

LaserWebServer::LaserWebServer() : server(80) {
    lastUpdateTime = 0;
    isWebServerRunning = false;
    clientCount = 0;
    
    // 初始化客户端数组
    for(int i = 0; i < 4; i++) {
        clients[i] = nullptr;
    }
    
    // 初始化所有设备状态为0
    for(int i = 0; i < 4; i++) {
        for(int j = 0; j < 48; j++) {
            deviceStates[i][j] = 0;
        }
    }
}

void LaserWebServer::begin() {
    server.begin();
    isWebServerRunning = true;
    Serial.println("Web服务器已启动");
    Serial.print("访问地址: http://");
    Serial.println(WiFi.localIP());
}

void LaserWebServer::handleClient() {
    // 检查新客户端连接
    WiFiClient newClient = server.available();
    if (newClient) {
        Serial.println("新客户端连接");
        
        // 找到空槽位
        for(int i = 0; i < 4; i++) {
            if(clients[i] == nullptr || !clients[i]->connected()) {
                if(clients[i] != nullptr) {
                    delete clients[i];
                }
                clients[i] = new WiFiClient(newClient);
                clientCount++;
                break;
            }
        }
    }
    
    // 处理现有客户端
    for(int i = 0; i < 4; i++) {
        if(clients[i] != nullptr && clients[i]->connected()) {
            if(clients[i]->available()) {
                handleHTTPRequest(*clients[i]);
            }
        } else if(clients[i] != nullptr) {
            delete clients[i];
            clients[i] = nullptr;
            clientCount--;
        }
    }
    
    // 定期广播状态更新
    if(millis() - lastUpdateTime > 1000) { // 每秒更新一次
        broadcastStates();
        lastUpdateTime = millis();
    }
}

void LaserWebServer::updateDeviceState(uint8_t deviceAddr, uint8_t inputNum, bool state) {
    if(deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
        deviceStates[deviceAddr-1][inputNum-1] = state ? 1 : 0;
    }
}

void LaserWebServer::updateAllDeviceStates(uint8_t deviceAddr, uint8_t* states) {
    if(deviceAddr >= 1 && deviceAddr <= 4) {
        for(int i = 0; i < 48; i++) {
            deviceStates[deviceAddr-1][i] = states[i];
        }
    }
}

void LaserWebServer::broadcastStates() {
    String json = getDeviceStatesJSON();
    for(int i = 0; i < 4; i++) {
        if(clients[i] != nullptr && clients[i]->connected()) {
            sendWebSocketUpdate(*clients[i], json);
        }
    }
}

String LaserWebServer::getDeviceStatesJSON() {
    DynamicJsonDocument doc(2048);
    
    for(int device = 1; device <= 4; device++) {
        String deviceKey = "device" + String(device);
        JsonArray inputs = doc.createNestedArray(deviceKey);
        
        for(int input = 1; input <= 48; input++) {
            JsonObject inputObj = inputs.createNestedObject();
            inputObj["id"] = input;
            inputObj["state"] = deviceStates[device-1][input-1];
        }
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

String LaserWebServer::getHTTPResponse(const String& contentType, const String& content) {
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response += "Access-Control-Allow-Headers: Content-Type\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: " + String(content.length()) + "\r\n";
    response += "\r\n";
    response += content;
    return response;
}

void LaserWebServer::sendWebSocketUpdate(WiFiClient& client, const String& data) {
    // 简单的WebSocket-like更新（通过HTTP长轮询模拟）
    if(client.connected()) {
        client.print("data: " + data + "\n\n");
    }
}

void LaserWebServer::handleHTTPRequest(WiFiClient& client) {
    String request = "";
    while(client.available()) {
        request += client.readStringUntil('\r');
    }
    
    // 解析HTTP请求
    if(request.indexOf("GET / ") >= 0 || request.indexOf("GET /index.html") >= 0) {
        // 主页面请求
        String html = getHTMLPage();
        client.print(getHTTPResponse("text/html", html));
    }
    else if(request.indexOf("GET /api/states") >= 0) {
        // API状态请求
        String json = getDeviceStatesJSON();
        client.print(getHTTPResponse("application/json", json));
    }
    else if(request.indexOf("GET /events") >= 0) {
        // Server-Sent Events for real-time updates
        String response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/event-stream\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "Connection: keep-alive\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "\r\n";
        client.print(response);
        
        // 发送初始数据
        String json = getDeviceStatesJSON();
        sendWebSocketUpdate(client, json);
    }
    else {
        // 404错误
        String notFound = "<html><body><h1>404 Not Found</h1></body></html>";
        client.print(getHTTPResponse("text/html", notFound));
    }
    
    // 延迟关闭连接以允许数据传输
    delay(10);
    client.stop();
}

String LaserWebServer::getHTMLPage() {
    return R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>激光传感器监控系统</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            max-width: 1400px;
            margin: 0 auto;
            background-color: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        h1 {
            text-align: center;
            color: #333;
            margin-bottom: 30px;
        }
        .device-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }
        .device-card {
            border: 2px solid #ddd;
            border-radius: 8px;
            padding: 15px;
            background-color: #fafafa;
        }
        .device-title {
            font-size: 18px;
            font-weight: bold;
            margin-bottom: 15px;
            color: #555;
            text-align: center;
        }
        .input-grid {
            display: grid;
            grid-template-columns: repeat(8, 1fr);
            gap: 3px;
        }
        .input-item {
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 3px;
            border-radius: 4px;
            font-size: 10px;
        }
        .input-id {
            font-weight: bold;
            margin-bottom: 2px;
        }
        .input-state {
            width: 20px;
            height: 20px;
            border-radius: 50%;
            border: 1px solid #ccc;
        }
        .state-active {
            background-color: #ff4444;
            border-color: #cc0000;
        }
        .state-inactive {
            background-color: #cccccc;
        }
        .status-bar {
            text-align: center;
            padding: 10px;
            background-color: #e8f4fd;
            border-radius: 4px;
            margin-bottom: 20px;
        }
        .connection-status {
            display: inline-block;
            padding: 5px 10px;
            border-radius: 4px;
            font-weight: bold;
        }
        .connected {
            background-color: #d4edda;
            color: #155724;
        }
        .disconnected {
            background-color: #f8d7da;
            color: #721c24;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>激光传感器实时监控系统</h1>
        
        <div class="status-bar">
            <span>连接状态: </span>
            <span id="connectionStatus" class="connection-status disconnected">未连接</span>
            <span style="margin-left: 20px;">最后更新: </span>
            <span id="lastUpdate">--:--:--</span>
        </div>

        <div class="device-grid" id="deviceContainer">
            <!-- 设备卡片将通过JavaScript动态生成 -->
        </div>
    </div>

    <script>
        let eventSource;
        let deviceData = {};

        function initEventSource() {
            eventSource = new EventSource('/events');
            
            eventSource.onopen = function() {
                console.log('EventSource连接已建立');
                updateConnectionStatus(true);
            };
            
            eventSource.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    updateDeviceDisplay(data);
                    updateLastUpdateTime();
                } catch (e) {
                    console.error('解析EventSource消息失败:', e);
                }
            };
            
            eventSource.onerror = function(error) {
                console.error('EventSource错误:', error);
                updateConnectionStatus(false);
                // 尝试重新连接
                setTimeout(initEventSource, 3000);
            };
        }

        function updateConnectionStatus(connected) {
            const statusElement = document.getElementById('connectionStatus');
            if (connected) {
                statusElement.textContent = '已连接';
                statusElement.className = 'connection-status connected';
            } else {
                statusElement.textContent = '未连接';
                statusElement.className = 'connection-status disconnected';
            }
        }

        function updateLastUpdateTime() {
            const now = new Date();
            const timeString = now.toLocaleTimeString('zh-CN');
            document.getElementById('lastUpdate').textContent = timeString;
        }

        function createDeviceCard(deviceId) {
            const card = document.createElement('div');
            card.className = 'device-card';
            card.id = `device-${deviceId}`;
            
            card.innerHTML = `
                <div class="device-title">设备 ${deviceId}</div>
                <div class="input-grid" id="input-grid-${deviceId}">
                    <!-- 输入状态将通过JavaScript动态生成 -->
                </div>
            `;
            
            return card;
        }

        function updateDeviceDisplay(data) {
            const container = document.getElementById('deviceContainer');
            
            // 如果是第一次加载，创建所有设备卡片
            if (container.children.length === 0) {
                for (let deviceId = 1; deviceId <= 4; deviceId++) {
                    container.appendChild(createDeviceCard(deviceId));
                }
            }
            
            // 更新每个设备的状态
            for (let deviceId = 1; deviceId <= 4; deviceId++) {
                const deviceKey = `device${deviceId}`;
                const inputs = data[deviceKey];
                
                if (inputs) {
                    const inputGrid = document.getElementById(`input-grid-${deviceId}`);
                    
                    // 如果是第一次加载，创建所有输入项
                    if (inputGrid.children.length === 0) {
                        for (let inputId = 1; inputId <= 48; inputId++) {
                            const inputItem = document.createElement('div');
                            inputItem.className = 'input-item';
                            inputItem.innerHTML = `
                                <div class="input-id">${inputId}</div>
                                <div class="input-state" id="device-${deviceId}-input-${inputId}"></div>
                            `;
                            inputGrid.appendChild(inputItem);
                        }
                    }
                    
                    // 更新输入状态
                    inputs.forEach(input => {
                        const stateElement = document.getElementById(`device-${deviceId}-input-${input.id}`);
                        if (stateElement) {
                            if (input.state === 1) {
                                stateElement.className = 'input-state state-active';
                            } else {
                                stateElement.className = 'input-state state-inactive';
                            }
                        }
                    });
                }
            }
        }

        // 页面加载完成后初始化EventSource
        window.onload = function() {
            initEventSource();
        };
    </script>
</body>
</html>
)";
}