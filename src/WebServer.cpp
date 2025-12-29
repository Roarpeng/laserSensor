#include "WebServer.h"
#include <Arduino.h>

LaserWebServer::LaserWebServer() : server(80) {
  lastUpdateTime = 0;
  isWebServerRunning = false;
  clientCount = 0;
  baselineDelay = 200; // 默认200ms延迟

    // 初始化所有设备状态为0
    for(int i = 0; i < 4; i++) {
        for(int j = 0; j < 48; j++) {
            deviceStates[i][j] = 0;
        }
        isSSEClient[i] = false; // 初始化 SSE 标记
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
  // 清理断开的客户端
  for (int i = 0; i < 4; i++) {
    if (clients[i]) {
      if (!clients[i].connected()) {
        Serial.printf("Client %d disconnected\n", i);
        clients[i].stop();
        isSSEClient[i] = false;
        if (clientCount > 0)
          clientCount--;
      } else if (clients[i].available() && !isSSEClient[i]) {
        // 只处理非 SSE 客户端的新请求
        handleHTTPRequest(clients[i], i);
      }
    }
  }

  // 检查新客户端连接
  WiFiClient newClient = server.available();
  if (newClient) {
    // 找到空槽位
    int freeSlot = -1;
    for (int i = 0; i < 4; i++) {
      if (!clients[i] || !clients[i].connected()) {
        freeSlot = i;
        break;
      }
    }

    if (freeSlot >= 0) {
      // 打印客户端 IP 地址用于调试
      IPAddress clientIP = newClient.remoteIP();
      Serial.printf("New client connected from %s, stored in slot %d\n",
                    clientIP.toString().c_str(), freeSlot);
      clients[freeSlot] = newClient;
      isSSEClient[freeSlot] = false;
      clientCount++;
    } else {
      // 限流：只每5秒打印一次"No free slots"警告
      static unsigned long lastNoSlotWarning = 0;
      unsigned long currentTime = millis();
      if (currentTime - lastNoSlotWarning > 5000) {
        Serial.println("No free slots available:");
        for (int i = 0; i < 4; i++) {
          if (clients[i].connected()) {
            Serial.printf("  Slot %d: connected=%d, SSE=%d, IP=%s\n", i,
                          clients[i].connected(), isSSEClient[i],
                          clients[i].remoteIP().toString().c_str());
          }
        }
        lastNoSlotWarning = currentTime;
      }
      newClient.stop();
    }
  }
}

void LaserWebServer::updateDeviceState(uint8_t deviceAddr, uint8_t inputNum,
                                       bool state) {
  if (deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
    deviceStates[deviceAddr - 1][inputNum - 1] = state ? 1 : 0;
  }
}

void LaserWebServer::updateAllDeviceStates(uint8_t deviceAddr,
                                           uint8_t *states) {
  if (deviceAddr >= 1 && deviceAddr <= 4) {
    for (int i = 0; i < 48; i++) {
      deviceStates[deviceAddr - 1][i] = states[i];
    }
  }
}

void LaserWebServer::broadcastStates() {
  String json = getDeviceStatesJSON();
  for (int i = 0; i < 4; i++) {
    if (clients[i].connected() && isSSEClient[i]) {
      // 尝试发送数据
      size_t written = clients[i].print("data: ");
      if (written == 0) {
        // 写入失败，连接可能已断开
        Serial.printf(
            "Failed to write to SSE client on slot %d, closing connection\n",
            i);
        clients[i].stop();
        isSSEClient[i] = false;
        if (clientCount > 0)
          clientCount--;
        continue;
      }
      clients[i].print(json);
      clients[i].print("\n\n");
      clients[i].flush();
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

void LaserWebServer::setShieldState(uint8_t deviceAddr, uint8_t inputNum,
                                    bool state) {
  if (deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
    shieldMask[deviceAddr - 1][inputNum - 1] = state ? 1 : 0;
  }
}

bool LaserWebServer::getShieldState(uint8_t deviceAddr, uint8_t inputNum) {
  if (deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
    return shieldMask[deviceAddr - 1][inputNum - 1] == 1;
  }
  return false;
}

String LaserWebServer::getShieldMaskJSON() {
  DynamicJsonDocument doc(2048);

  for (int device = 1; device <= 4; device++) {
    String deviceKey = "device" + String(device);
    JsonArray inputs = doc.createNestedArray(deviceKey);

    for (int input = 1; input <= 48; input++) {
      if (shieldMask[device - 1][input - 1] == 1) {
        inputs.add(input);
      }
    }
  }

  String output;
  serializeJson(doc, output);
  return output;
}

void LaserWebServer::loadShieldMask() {
  // TODO: Implement Flash storage
}

void LaserWebServer::saveShieldMask() {
  // TODO: Implement Flash storage
}

String LaserWebServer::getHTTPResponse(const String &contentType,
                                       const String &content) {
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

void LaserWebServer::sendWebSocketUpdate(WiFiClient &client,
                                         const String &data) {
  // SSE 格式: "data: " + JSON + "\n\n"
  if (client.connected()) {
    client.print("data: ");
    client.print(data);
    client.print("\n\n");
    client.flush(); // 确保数据立即发送
  }
}

void LaserWebServer::handleHTTPRequest(WiFiClient &client, int slotIndex) {
  String request = "";
  while (client.available()) {
    request += client.readStringUntil('\r');
  }

  // 解析HTTP请求
  if (request.indexOf("GET / ") >= 0 ||
      request.indexOf("GET /index.html") >= 0) {
    // 主页面请求
    String html = getHTMLPage();
    client.print(getHTTPResponse("text/html", html));
    client.stop(); // 立即关闭连接
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /api/states") >= 0) {
    // API状态请求
    String json = getDeviceStatesJSON();
    client.print(getHTTPResponse("application/json", json));
    client.stop();
    isSSEClient[slotIndex] = false;
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /api/shield") >= 0) {
    // 获取所有屏蔽位
    String json = getShieldMaskJSON();
    client.print(getHTTPResponse("application/json", json));
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("POST /api/shield") >= 0) {
    // 设置屏蔽位
    String body = "";
    bool bodyStarted = false;
    while (client.available()) {
      String line = client.readStringUntil('\r');
      if (bodyStarted) {
        body += line;
      }
      if (line.length() == 0) {
        bodyStarted = true;
      }
    }

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      if (doc.containsKey("device") && doc.containsKey("id") &&
          doc.containsKey("state")) {
        int device = doc["device"];
        int id = doc["id"];
        bool state = doc["state"];

        setShieldState(device, id, state);
        client.print(
            getHTTPResponse("application/json", "{\"status\":\"ok\"}"));

        // 也可以选择保存到Flash
        // saveShieldMask();
      } else {
        client.print(getHTTPResponse("application/json",
                                     "{\"error\":\"Missing parameters\"}"));
      }
    } else {
      client.print(
          getHTTPResponse("application/json", "{\"error\":\"Invalid JSON\"}"));
    }
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /api/baselineDelay") >= 0) {
    // 获取基线延迟设置
    String json = getBaselineDelayJSON();
    client.print(getHTTPResponse("application/json", json));
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("POST /api/baselineDelay") >= 0) {
    // 设置基线延迟
    String body = "";
    bool bodyStarted = false;
    while (client.available()) {
      String line = client.readStringUntil('\r');
      if (bodyStarted) {
        body += line;
      }
      if (line.length() == 0) {
        bodyStarted = true;
      }
    }

    // 解析JSON获取延迟值
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      unsigned long delayValue = doc["delay"];
      setBaselineDelay(delayValue);

            String responseJson = getBaselineDelayJSON();
            client.print(getHTTPResponse("application/json", responseJson));
        } else {
            String errorJson = "{\"error\":\"Invalid JSON\"}";
            client.print(getHTTPResponse("application/json", errorJson));
        }
        client.stop();
        isSSEClient[slotIndex] = false;
    }
    else if(request.indexOf("GET /events") >= 0) {
        // Server-Sent Events for real-time updates
        IPAddress clientIP = client.remoteIP();

    // 检查是否已经有来自同一 IP 的 SSE 连接
    for (int i = 0; i < 4; i++) {
      if (i != slotIndex && isSSEClient[i] && clients[i].connected()) {
        if (clients[i].remoteIP() == clientIP) {
          Serial.printf("Closing duplicate SSE connection from %s on slot %d\n",
                        clientIP.toString().c_str(), i);
          clients[i].stop();
          isSSEClient[i] = false;
          if (clientCount > 0)
            clientCount--;
        }
      }
    }

    Serial.printf("SSE client on slot %d from %s\n", slotIndex,
                  clientIP.toString().c_str());
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/event-stream\r\n";
    response += "Cache-Control: no-cache\r\n";
    response += "Connection: keep-alive\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "\r\n";
    client.print(response);
    client.flush(); // 确保 HTTP 头部发送

    // 发送初始数据
    String json = getDeviceStatesJSON();
    sendWebSocketUpdate(client, json);

    // 标记为 SSE 长连接，不关闭
    isSSEClient[slotIndex] = true;
  } else {
    // 404错误
    String notFound = "<html><body><h1>404 Not Found</h1></body></html>";
    client.print(getHTTPResponse("text/html", notFound));
    client.stop();
    isSSEClient[slotIndex] = false;
  }
}

String LaserWebServer::getHTMLPage() {
    String html = "";
    html += "<!DOCTYPE html>\n";
    html += "<html lang=\"en\">\n";
    html += "<head>\n";
    html += "    <meta charset=\"UTF-8\">\n";
    html += "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    html += "    <title>Laser Sensor Real-time Monitoring System</title>\n";
    html += "    <style>\n";
    html += "        body {\n";
    html += "            font-family: Arial, sans-serif;\n";
    html += "            margin: 0;\n";
    html += "            padding: 20px;\n";
    html += "            background-color: #f5f5f5;\n";
    html += "        }\n";
    html += "        .container {\n";
    html += "            max-width: 1400px;\n";
    html += "            margin: 0 auto;\n";
    html += "            background-color: white;\n";
    html += "            padding: 20px;\n";
    html += "            border-radius: 8px;\n";
    html += "            box-shadow: 0 2px 10px rgba(0,0,0,0.1);\n";
    html += "        }\n";
    html += "        h1 {\n";
    html += "            text-align: center;\n";
    html += "            color: #333;\n";
    html += "            margin-bottom: 30px;\n";
    html += "        }\n";
    html += "        .control-group {\n";
    html += "            margin-bottom: 20px;\n";
    html += "            padding: 15px;\n";
    html += "            background-color: #e8f4fd;\n";
    html += "            border-radius: 4px;\n";
    html += "        }\n";
    html += "        .control-group label {\n";
    html += "            display: inline-block;\n";
    html += "            width: 150px;\n";
    html += "            font-weight: bold;\n";
    html += "        }\n";
    html += "        .control-group input {\n";
    html += "            margin-right: 10px;\n";
    html += "            padding: 5px;\n";
    html += "            border: 1px solid #ccc;\n";
    html += "            border-radius: 4px;\n";
    html += "        }\n";
    html += "        .control-group button {\n";
    html += "            padding: 5px 15px;\n";
    html += "            background-color: #007bff;\n";
    html += "            color: white;\n";
    html += "            border: none;\n";
    html += "            border-radius: 4px;\n";
    html += "            cursor: pointer;\n";
    html += "            margin-right: 10px;\n";
    html += "        }\n";
    html += "        .control-group button:hover {\n";
    html += "            background-color: #0056b3;\n";
    html += "        }\n";
    html += "        .status-message {\n";
    html += "            padding: 10px;\n";
    html += "            margin: 10px 0;\n";
    html += "            border-radius: 4px;\n";
    html += "            display: none;\n";
    html += "        }\n";
    html += "        .status-message.success {\n";
    html += "            background-color: #d4edda;\n";
    html += "            color: #155724;\n";
    html += "        }\n";
    html += "        .status-message.error {\n";
    html += "            background-color: #f8d7da;\n";
    html += "            color: #721c24;\n";
    html += "        }\n";
    html += "        .device-grid {\n";
    html += "            display: grid;\n";
    html += "            grid-template-columns: repeat(2, 1fr);\n";
    html += "            gap: 20px;\n";
    html += "            margin-bottom: 20px;\n";
    html += "        }\n";
    html += "        @media (max-width: 1024px) {\n";
    html += "            .device-grid {\n";
    html += "                grid-template-columns: 1fr;\n";
    html += "            }\n";
    html += "        }\n";
    html += "        .device-card {\n";
    html += "            border: 2px solid #ddd;\n";
    html += "            border-radius: 8px;\n";
    html += "            padding: 15px;\n";
    html += "            background-color: #fafafa;\n";
    html += "            min-height: 200px;\n";
    html += "        }\n";
    html += "        .device-title {\n";
    html += "            font-size: 18px;\n";
    html += "            font-weight: bold;\n";
    html += "            margin-bottom: 15px;\n";
    html += "            color: #007bff;\n";
    html += "            text-align: center;\n";
    html += "            padding: 10px;\n";
    html += "            background-color: #e8f4fd;\n";
    html += "            border-radius: 4px;\n";
    html += "        }\n";
    html += "        .input-grid {\n";
    html += "            display: grid;\n";
    html += "            grid-template-columns: repeat(8, 1fr);\n";
    html += "            gap: 3px;\n";
    html += "        }\n";
    html += "        .input-item {\n";
    html += "            display: flex;\n";
    html += "            flex-direction: column;\n";
    html += "            align-items: center;\n";
    html += "            padding: 3px;\n";
    html += "            border-radius: 4px;\n";
    html += "            font-size: 10px;\n";
    html += "        }\n";
    html += "        .input-id {\n";
    html += "            font-weight: bold;\n";
    html += "            margin-bottom: 2px;\n";
    html += "        }\n";
    html += "        .input-state {\n";
    html += "            width: 20px;\n";
    html += "            height: 20px;\n";
    html += "            border-radius: 50%;\n";
    html += "            border: 1px solid #ccc;\n";
    html += "        }\n";
    html += "        .state-active {\n";
    html += "            background-color: #ff4444;\n";
    html += "            border-color: #cc0000;\n";
    html += "        }\n";
    html += "        .state-inactive {\n";
    html += "            background-color: #cccccc;\n";
    html += "        }\n";
    html += "        .status-bar {\n";
    html += "            text-align: center;\n";
    html += "            padding: 10px;\n";
    html += "            background-color: #e8f4fd;\n";
    html += "            border-radius: 4px;\n";
    html += "            margin-bottom: 20px;\n";
    html += "        }\n";
    html += "        .connection-status {\n";
    html += "            display: inline-block;\n";
    html += "            padding: 5px 10px;\n";
    html += "            border-radius: 4px;\n";
    html += "            font-weight: bold;\n";
    html += "        }\n";
    html += "        .connected {\n";
    html += "            background-color: #d4edda;\n";
    html += "            color: #155724;\n";
    html += "        }\n";
    html += "        .disconnected {\n";
    html += "            background-color: #f8d7da;\n";
    html += "            color: #721c24;\n";
    html += "        }\n";
    html += "    </style>\n";
    html += "</head>\n";
    html += "<body>\n";
    html += "    <div class=\"container\">\n";
    html += "        <h1>Laser Sensor Real-time Monitoring System</h1>\n";
    html += "        \n";
    html += "        <div class=\"control-group\">\n";
    html += "            <h3>Baseline Settings Control</h3>\n";
    html += "            <label>Baseline Delay (ms):</label>\n";
    html += "            <input type=\"number\" id=\"baselineDelay\" min=\"0\" max=\"5000\" step=\"50\" value=\"200\">\n";
    html += "            <button onclick=\"setBaselineDelay()\">Set Delay</button>\n";
    html += "            <button onclick=\"getCurrentBaselineDelay()\">Get Current</button>\n";
    html += "        </div>\n";
    html += "        \n";
    html += "        <div id=\"statusMessage\" class=\"status-message\"></div>\n";
    html += "\n";
    html += "        <div class=\"status-bar\">\n";
    html += "            <span>Connection Status: </span>\n";
    html += "            <span id=\"connectionStatus\" class=\"connection-status disconnected\">Disconnected</span>\n";
    html += "            <span style=\"margin-left: 20px;\">Last Update: </span>\n";
    html += "            <span id=\"lastUpdate\">--:--:--</span>\n";
    html += "        </div>\n";
    html += "\n";
    html += "        <div class=\"device-grid\" id=\"deviceContainer\">\n";
    html += "            <!-- Device cards will be dynamically generated by JavaScript -->\n";
    html += "        </div>\n";
    html += "    </div>\n";
    html += "\n";
    html += "    <script>\n";
    html += "        var eventSource;\n";
    html += "        var deviceData = {};\n";
    html += "\n";
    html += "        function setBaselineDelay() {\n";
    html += "            var delay = document.getElementById('baselineDelay').value;\n";
    html += "            \n";
    html += "            fetch('/api/baselineDelay', {\n";
    html += "                method: 'POST',\n";
    html += "                headers: {\n";
    html += "                    'Content-Type': 'application/json',\n";
    html += "                },\n";
    html += "                body: JSON.stringify({ delay: parseInt(delay) })\n";
    html += "            })\n";
    html += "            .then(response => response.json())\n";
    html += "            .then(data => {\n";
    html += "                showStatusMessage('Baseline delay set to ' + delay + 'ms', 'success');\n";
    html += "            })\n";
    html += "            .catch(error => {\n";
    html += "                console.error('Failed to set baseline delay:', error);\n";
    html += "                showStatusMessage('Failed to set baseline delay', 'error');\n";
    html += "            });\n";
    html += "        }\n";
    html += "\n";
    html += "        function getCurrentBaselineDelay() {\n";
    html += "            fetch('/api/baselineDelay')\n";
    html += "            .then(response => response.json())\n";
    html += "            .then(data => {\n";
    html += "                document.getElementById('baselineDelay').value = data.delay;\n";
    html += "                showStatusMessage('Current baseline delay: ' + data.delay + 'ms', 'success');\n";
    html += "            })\n";
    html += "            .catch(error => {\n";
    html += "                console.error('Failed to get baseline delay:', error);\n";
    html += "                showStatusMessage('Failed to get baseline delay', 'error');\n";
    html += "            });\n";
    html += "        }\n";
    html += "\n";
    html += "        function showStatusMessage(message, type) {\n";
    html += "            var statusElement = document.getElementById('statusMessage');\n";
    html += "            statusElement.textContent = message;\n";
    html += "            statusElement.className = 'status-message ' + type;\n";
    html += "            statusElement.style.display = 'block';\n";
    html += "            \n";
    html += "            // Auto hide message after 3 seconds\n";
    html += "            setTimeout(function() {\n";
    html += "                statusElement.style.display = 'none';\n";
    html += "            }, 3000);\n";
    html += "        }\n";
    html += "\n";
    html += "        function initEventSource() {\n";
    html += "            // 关闭已存在的连接\n";
    html += "            if (eventSource) {\n";
    html += "                console.log('Closing existing EventSource');\n";
    html += "                eventSource.close();\n";
    html += "            }\n";
    html += "            \n";
    html += "            eventSource = new EventSource('/events');\n";
    html += "            \n";
    html += "            eventSource.onopen = function() {\n";
    html += "                console.log('EventSource connection established');\n";
    html += "                updateConnectionStatus(true);\n";
    html += "                // Get current baseline delay when page loads\n";
    html += "                getCurrentBaselineDelay();\n";
    html += "            };\n";
    html += "            \n";
    html += "            eventSource.onmessage = function(event) {\n";
    html += "                try {\n";
    html += "                    var data = JSON.parse(event.data);\n";
    html += "                    updateDeviceDisplay(data);\n";
    html += "                    updateLastUpdateTime();\n";
    html += "                } catch (e) {\n";
    html += "                    console.error('Failed to parse EventSource message:', e);\n";
    html += "                }\n";
    html += "            };\n";
    html += "            \n";
    html += "            eventSource.onerror = function(error) {\n";
    html += "                console.error('EventSource error:', error);\n";
    html += "                updateConnectionStatus(false);\n";
    html += "                eventSource.close(); // 明确关闭失败的连接\n";
    html += "                // 3秒后重新连接\n";
    html += "                setTimeout(initEventSource, 3000);\n";
    html += "            };\n";
    html += "        }\n";
    html += "\n";
    html += "        function updateConnectionStatus(connected) {\n";
    html += "            var statusElement = document.getElementById('connectionStatus');\n";
    html += "            if (connected) {\n";
    html += "                statusElement.textContent = 'Connected';\n";
    html += "                statusElement.className = 'connection-status connected';\n";
    html += "            } else {\n";
    html += "                statusElement.textContent = 'Disconnected';\n";
    html += "                statusElement.className = 'connection-status disconnected';\n";
    html += "            }\n";
    html += "        }\n";
    html += "\n";
    html += "        function updateLastUpdateTime() {\n";
    html += "            var now = new Date();\n";
    html += "            var timeString = now.toLocaleTimeString();\n";
    html += "            document.getElementById('lastUpdate').textContent = timeString;\n";
    html += "        }\n";
    html += "\n";
    html += "        function createDeviceCard(deviceId) {\n";
    html += "            var card = document.createElement('div');\n";
    html += "            card.className = 'device-card';\n";
    html += "            card.id = 'device-' + deviceId;\n";
    html += "            \n";
    html += "            card.innerHTML = \n";
    html += "                '<div class=\"device-title\">Device ' + deviceId + '</div>' +\n";
    html += "                '<div class=\"input-grid\" id=\"input-grid-' + deviceId + '\">' +\n";
    html += "                '<!-- Input status will be dynamically generated by JavaScript -->' +\n";
    html += "                '</div>';\n";
    html += "            \n";
    html += "            return card;\n";
    html += "        }\n";
    html += "\n";
    html += "        function updateDeviceDisplay(data) {\n";
    html += "            var container = document.getElementById('deviceContainer');\n";
    html += "            console.log('Received data:', data);\n";
    html += "            \n";
    html += "            // If first load, create all device cards\n";
    html += "            if (container.children.length === 0) {\n";
    html += "                console.log('Creating device cards...');\n";
    html += "                for (var deviceId = 1; deviceId <= 4; deviceId++) {\n";
    html += "                    container.appendChild(createDeviceCard(deviceId));\n";
    html += "                }\n";
    html += "                console.log('Created ' + container.children.length + ' device cards');\n";
    html += "            }\n";
    html += "            \n";
    html += "            // Update status for each device\n";
    html += "            for (var deviceId = 1; deviceId <= 4; deviceId++) {\n";
    html += "                var deviceKey = 'device' + deviceId;\n";
    html += "                var inputs = data[deviceKey];\n";
    html += "                \n";
    html += "                if (inputs) {\n";
    html += "                    var inputGrid = document.getElementById('input-grid-' + deviceId);\n";
    html += "                    \n";
    html += "                    // If first load, create all input items\n";
    html += "                    if (inputGrid.children.length === 0) {\n";
    html += "                        for (var inputId = 1; inputId <= 48; inputId++) {\n";
    html += "                            var inputItem = document.createElement('div');\n";
    html += "                            inputItem.className = 'input-item';\n";
    html += "                            inputItem.innerHTML = \n";
    html += "                                '<div class=\"input-id\">' + inputId + '</div>' +\n";
    html += "                                '<div class=\"input-state\" id=\"device-' + deviceId + '-input-' + inputId + '\"></div>';\n";
    html += "                            inputGrid.appendChild(inputItem);\n";
    html += "                        }\n";
    html += "                    }\n";
    html += "                    \n";
    html += "                    // Update input status\n";
    html += "                    for (var i = 0; i < inputs.length; i++) {\n";
    html += "                        var input = inputs[i];\n";
    html += "                        var stateElement = document.getElementById('device-' + deviceId + '-input-' + input.id);\n";
    html += "                        if (stateElement) {\n";
    html += "                            if (input.state === 1) {\n";
    html += "                                stateElement.className = 'input-state state-active';\n";
    html += "                            } else {\n";
    html += "                                stateElement.className = 'input-state state-inactive';\n";
    html += "                            }\n";
    html += "                        }\n";
    html += "                    }\n";
    html += "                }\n";
    html += "            }\n";
    html += "        }\n";
    html += "\n";
    html += "        // Initialize EventSource after page loads\n";
    html += "        window.onload = function() {\n";
    html += "            initEventSource();\n";
    html += "        };\n";
    html += "    </script>\n";
    html += "</body>\n";
    html += "</html>\n";
    return html;
}

void LaserWebServer::setBaselineDelay(unsigned long delay) {
  baselineDelay = delay;
}

unsigned long LaserWebServer::getBaselineDelay() { return baselineDelay; }

String LaserWebServer::getBaselineDelayJSON() {
  DynamicJsonDocument doc(256);
  doc["delay"] = baselineDelay;

  String output;
  serializeJson(doc, output);
  return output;
}

void LaserWebServer::setShielding(uint8_t deviceAddr, uint8_t inputNum, bool shielded) {
    if(deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
        shieldedPoints[deviceAddr-1][inputNum-1] = shielded ? 1 : 0;
        
        // 关键：这里需要通知 main.cpp 保存到 Flash
        // 由于本类目前不直接持有 Preferences，我们假设 main 会轮询或通过回调
        // 实际上我们可以通过外部传入的指针直接修改 main 的 shielding 数组
    }
}

bool LaserWebServer::isShielded(uint8_t deviceAddr, uint8_t inputNum) {
    if(deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
        return shieldedPoints[deviceAddr-1][inputNum-1] == 1;
    }
    return false;
}

String LaserWebServer::getShieldingJSON() {
    DynamicJsonDocument doc(2048);
    for(int d = 0; d < 4; d++) {
        JsonArray deviceArr = doc.createNestedArray("device" + String(d+1));
        for(int i = 0; i < 48; i++) {
            deviceArr.add(shieldedPoints[d][i]);
        }
    }
    String output;
    serializeJson(doc, output);
    return output;
}

void LaserWebServer::loadShielding(uint8_t shielding[4][48]) {
    memcpy(shieldedPoints, shielding, sizeof(shieldedPoints));
}