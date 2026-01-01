#include "WebServer.h"
#include <Arduino.h>
#include <Update.h>

LaserWebServer::LaserWebServer() : server(80) {
  lastUpdateTime = 0;
  isWebServerRunning = false;
  clientCount = 0;
  baselineDelay = 200; // 默认200ms延迟
  shieldingChangeCallback = nullptr;

  // 初始化所有设备状态为0
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 48; j++) {
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
  DynamicJsonDocument doc(8192);

  for (int device = 1; device <= 4; device++) {
    String deviceKey = "device" + String(device);
    JsonArray inputs = doc.createNestedArray(deviceKey);

    for (int input = 1; input <= 48; input++) {
      JsonObject inputObj = inputs.createNestedObject();
      inputObj["id"] = input;
      inputObj["state"] = deviceStates[device - 1][input - 1];
    }
  }

  String output;
  serializeJson(doc, output);
  return output;
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
  size_t contentLength = 0;
  // 读取请求行和头部
  while (client.connected()) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      request += line;

      // 解析 Content-Length (不区分大小写)
      String lowerLine = line;
      lowerLine.toLowerCase();
      if (lowerLine.startsWith("content-length: ")) {
        contentLength = lowerLine.substring(16).toInt();
      }

      if (line == "\r" || line == "")
        break;
    }
  }

  // 解析HTTP请求
  if (request.indexOf("GET / ") >= 0 ||
      request.indexOf("GET /index.html") >= 0) {
    String html = getHTMLPage();
    client.print(getHTTPResponse("text/html", html));
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /api/states") >= 0) {
    String json = getDeviceStatesJSON();
    client.print(getHTTPResponse("application/json", json));
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /api/shield") >= 0) {
    String json = getShieldMaskJSON();
    client.print(getHTTPResponse("application/json", json));
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("POST /api/shield") >= 0) {
    // 简化的 Body 读取
    String body = "";
    while (client.available())
      body += (char)client.read();

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      if (doc.containsKey("device") && doc.containsKey("id") &&
          doc.containsKey("state")) {
        setShieldState(doc["device"], doc["id"], doc["state"]);
        client.print(
            getHTTPResponse("application/json", "{\"status\":\"ok\"}"));
      }
    }
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /api/baselineDelay") >= 0) {
    String json = getBaselineDelayJSON();
    client.print(getHTTPResponse("application/json", json));
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("POST /api/baselineDelay") >= 0) {
    String body = "";
    while (client.available())
      body += (char)client.read();
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      setBaselineDelay(doc["delay"]);
      client.print(getHTTPResponse("application/json", getBaselineDelayJSON()));
    }
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("POST /update") >= 0) {
    // OTA Update handler
    if (contentLength > 0) {
      Serial.printf("Starting OTA Update... Size: %u bytes\n", contentLength);
      if (Update.begin(contentLength)) {
        size_t written = Update.writeStream(client);
        if (written == contentLength) {
          Serial.printf("Written : %u successfully\n", written);
        } else {
          Serial.printf("Written only : %u/%u. Fail?\n", written,
                        contentLength);
        }

        if (Update.end()) {
          Serial.println("OTA Update Success!");
          if (Update.isFinished()) {
            Serial.println("Update successfully completed. Rebooting...");
            client.print(getHTTPResponse("text/plain", "OK"));
            client.stop();
            delay(100);
            ESP.restart();
          } else {
            Serial.println("Update not finished? Something went wrong!");
            client.print(
                getHTTPResponse("text/plain", "Update Finished Error"));
          }
        } else {
          Serial.printf("Update Error Occurred. Error #: %u\n",
                        Update.getError());
          client.print(getHTTPResponse(
              "text/plain", "Update Error: " + String(Update.getError())));
        }
      } else {
        Serial.println("Not enough space to begin OTA");
        client.print(getHTTPResponse("text/plain", "Not enough space"));
      }
    } else {
      client.print(getHTTPResponse("text/plain", "No Content?"));
    }
    client.stop();
    isSSEClient[slotIndex] = false;
  } else if (request.indexOf("GET /events") >= 0) {
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/event-stream\r\n";
    response += "Cache-Control: no-cache\r\n";
    response += "Connection: keep-alive\r\n";
    response += "Access-Control-Allow-Origin: *\r\n\r\n";
    client.print(response);
    client.flush();
    isSSEClient[slotIndex] = true;
  } else {
    client.print(getHTTPResponse("text/html", "404 Not Found"));
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
  html += "    <meta name=\"viewport\" content=\"width=device-width, "
          "initial-scale=1.0\">\n";
  html += "    <title>Laser Sensor Monitoring</title>\n";
  html += "    <style>\n";
  html += "        body { font-family: sans-serif; margin: 0; padding: 20px; "
          "background: #f0f2f5; }\n";
  html += "        .container { max-width: 1200px; margin: 0 auto; background: "
          "white; padding: 20px; border-radius: 12px; box-shadow: 0 4px 6px "
          "rgba(0,0,0,0.1); }\n";
  html += "        .header { display: flex; justify-content: space-between; "
          "align-items: center; margin-bottom: 20px; }\n";
  html +=
      "        .status-bar { background: #e3f2fd; padding: 10px 20px; "
      "border-radius: 8px; margin-bottom: 20px; display: flex; gap: 20px; }\n";
  html += "        .control-panel { background: #f8f9fa; padding: 15px; "
          "border-radius: 8px; margin-bottom: 20px; display: flex; flex-wrap: "
          "wrap; gap: 15px; align-items: center; }\n";
  html += "        .device-grid { display: grid; grid-template-columns: "
          "repeat(auto-fit, minmax(500px, 1fr)); gap: 20px; }\n";
  html += "        .device-card { border: 1px solid #dee2e6; border-radius: "
          "10px; padding: 15px; background: #fff; }\n";
  html += "        .device-title { font-size: 1.25rem; font-weight: bold; "
          "margin-bottom: 15px; color: #1a73e8; border-bottom: 2px solid "
          "#e8f0fe; padding-bottom: 5px; }\n";
  html += "        .input-grid { display: grid; grid-template-columns: "
          "repeat(12, 1fr); gap: 5px; }\n";
  html += "        .input-node { display: flex; flex-direction: column; "
          "align-items: center; gap: 2px; }\n";
  html += "        .id-label { font-size: 10px; color: #666; }\n";
  html += "        .led { width: 18px; height: 18px; border-radius: 4px; "
          "background: #e0e0e0; border: 1px solid #bdbdbd; cursor: pointer; "
          "transition: all 0.2s; }\n";
  html += "        .led:hover { transform: scale(1.2); }\n";
  html += "        .led.active { background: #ff5252; border-color: #d32f2f; "
          "box-shadow: 0 0 8px rgba(255,82,82,0.5); }\n";
  html += "        .led.shielded { background: #fb8c00; border-color: #ef6c00; "
          "position: relative; }\n";
  html += "        .led.shielded::after { content: '×'; position: absolute; "
          "color: white; font-size: 14px; top: 50%; left: 50%; transform: "
          "translate(-50%, -50%); }\n";
  html +=
      "        .led.shielded.active { background: #ffa726; opacity: 0.7; }\n";
  html += "        button { padding: 8px 16px; border: none; border-radius: "
          "4px; background: #1a73e8; color: white; cursor: pointer; "
          "transition: background 0.2s; }\n";
  html += "        button:hover { background: #1557b0; }\n";
  html += "        button.secondary { background: #6c757d; }\n";
  html += "        button.danger { background: #dc3545; }\n";
  html += "        input[type='number'] { padding: 6px; border: 1px solid "
          "#ced4da; border-radius: 4px; width: 80px; }\n";
  html += "        #config-banner { display: none; background: #fff3e0; color: "
          "#e65100; padding: 10px; border-radius: 4px; text-align: center; "
          "margin-bottom: 15px; font-weight: bold; }\n";
  html += "    </style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "    <div class=\"container\">\n";
  html += "        <div class=\"header\">\n";
  html += "            <h1>Laser Sensor System</h1>\n";
  html += "            <div id=\"conn-status\" style=\"color: red; "
          "font-weight: bold;\">Disconnected</div>\n";
  html += "        </div>\n";
  html += "\n";
  html += "        <div id=\"config-banner\">SHIELD CONFIGURATION MODE ACTIVE "
          "- Click points to toggle mask</div>\n";
  html += "\n";
  html += "        <div class=\"status-bar\">\n";
  html += "            <div>Last Update: <span "
          "id=\"last-time\">--:--:--</span></div>\n";
  html += "            <div>Active Clients: <span "
          "id=\"client-count\">0</span></div>\n";
  html += "        </div>\n";
  html += "\n";
  html += "        <div class=\"control-panel\">\n";
  html += "            <label>Baseline Delay:</label>\n";
  html +=
      "            <input type=\"number\" id=\"delay-input\" value=\"200\">\n";
  html += "            <button onclick=\"updateDelay()\">Set Delay</button>\n";
  html += "            <button class=\"secondary\" onclick=\"toggleConfig()\" "
          "id=\"config-btn\">Enter Shield Config</button>\n";
  html += "            <div style=\"margin-left: auto;\">\n";
  html += "                <input type=\"file\" id=\"ota-file\" "
          "style=\"display:none\">\n";
  html += "                <button class=\"danger\" "
          "onclick=\"document.getElementById('ota-file').click()\">Select "
          "Update</button>\n";
  html += "                <button onclick=\"doOTA()\">Flash</button>\n";
  html += "            </div>\n";
  html += "        </div>\n";
  html += "\n";
  html += "        <div class=\"device-grid\" id=\"grid\"></div>\n";
  html += "    </div>\n";
  html += "\n";
  html += "    <script>\n";
  html += "        let configMode = false;\n";
  html += "        let shieldMask = {};\n";
  html += "        let eventSource = null;\n";
  html += "\n";
  html += "        function init() {\n";
  html += "            fetch('/api/shield').then(r => r.json()).then(d => "
          "shieldMask = d);\n";
  html += "            fetch('/api/baselineDelay').then(r => r.json()).then(d "
          "=> document.getElementById('delay-input').value = d.delay);\n";
  html += "            setupSSE();\n";
  html += "            renderEmpty();\n";
  html += "        }\n";
  html += "\n";
  html += "        function setupSSE() {\n";
  html += "            if(eventSource) eventSource.close();\n";
  html += "            eventSource = new EventSource('/events');\n";
  html += "            eventSource.onopen = () => {\n";
  html += "                document.getElementById('conn-status').textContent "
          "= 'Connected';\n";
  html += "                document.getElementById('conn-status').style.color "
          "= 'green';\n";
  html += "            };\n";
  html += "            eventSource.onmessage = e => "
          "updateDisplay(JSON.parse(e.data));\n";
  html += "            eventSource.onerror = () => {\n";
  html += "                document.getElementById('conn-status').textContent "
          "= 'Disconnected';\n";
  html += "                document.getElementById('conn-status').style.color "
          "= 'red';\n";
  html += "            };\n";
  html += "        }\n";
  html += "\n";
  html += "        function renderEmpty() {\n";
  html += "            const grid = document.getElementById('grid');\n";
  html += "            grid.innerHTML = '';\n";
  html += "            for(let d=1; d<=4; d++) {\n";
  html += "                const card = document.createElement('div');\n";
  html += "                card.className = 'device-card';\n";
  html += "                card.innerHTML = `<div class='device-title'>Device "
          "${d}</div><div class='input-grid' id='d-${d}'></div>`;\n";
  html += "                grid.appendChild(card);\n";
  html +=
      "                const devGrid = card.querySelector('.input-grid');\n";
  html += "                for(let i=1; i<=48; i++) {\n";
  html += "                    devGrid.innerHTML += `<div "
          "class='input-node'><div class='led' id='l-${d}-${i}' "
          "onclick='handleLedClick(${d},${i})'></div><div "
          "class='id-label'>${i}</div></div>`;\n";
  html += "                }\n";
  html += "            }\n";
  html += "        }\n";
  html += "\n";
  html += "        function updateDisplay(data) {\n";
  html += "            document.getElementById('last-time').textContent = new "
          "Date().toLocaleTimeString();\n";
  html += "            for(let d=1; d<=4; d++) {\n";
  html += "                const inputs = data['device' + d];\n";
  html += "                if(!inputs) continue;\n";
  html += "                inputs.forEach(input => {\n";
  html += "                    const led = "
          "document.getElementById(`l-${d}-${input.id}`);\n";
  html += "                    if(!led) return;\n";
  html += "                    const isShielded = shieldMask['device'+d] && "
          "shieldMask['device'+d].includes(input.id);\n";
  html += "                    led.className = 'led' + (input.state ? ' "
          "active' : '') + (isShielded ? ' shielded' : '');\n";
  html += "                });\n";
  html += "            }\n";
  html += "        }\n";
  html += "\n";
  html += "        function handleLedClick(d, i) {\n";
  html += "            if(!configMode) return;\n";
  html += "            const key = 'device' + d;\n";
  html += "            if(!shieldMask[key]) shieldMask[key] = [];\n";
  html += "            const index = shieldMask[key].indexOf(i);\n";
  html += "            const newState = index === -1;\n";
  html += "            \n";
  html += "            fetch('/api/shield', { method: 'POST', body: "
          "JSON.stringify({ device: d, id: i, state: newState }) })\n";
  html += "            .then(r => r.json()).then(res => {\n";
  html += "                if(newState) shieldMask[key].push(i);\n";
  html += "                else shieldMask[key].splice(index, 1);\n";
  html +=
      "                const led = document.getElementById(`l-${d}-${i}`);\n";
  html += "                led.classList.toggle('shielded', newState);\n";
  html += "            });\n";
  html += "        }\n";
  html += "\n";
  html += "        function toggleConfig() {\n";
  html += "            configMode = !configMode;\n";
  html += "            document.getElementById('config-btn').textContent = "
          "configMode ? 'Exit Shield Config' : 'Enter Shield Config';\n";
  html += "            "
          "document.getElementById('config-btn').classList.toggle('secondary', "
          "!configMode);\n";
  html += "            "
          "document.getElementById('config-btn').classList.toggle('danger', "
          "configMode);\n";
  html += "            document.getElementById('config-banner').style.display "
          "= configMode ? 'block' : 'none';\n";
  html += "        }\n";
  html += "\n";
  html += "        function updateDelay() {\n";
  html +=
      "            const val = document.getElementById('delay-input').value;\n";
  html += "            fetch('/api/baselineDelay', { method: 'POST', body: "
          "JSON.stringify({ delay: parseInt(val) }) });\n";
  html += "        }\n";
  html += "\n";
  html += "        function doOTA() {\n";
  html += "            const file = "
          "document.getElementById('ota-file').files[0];\n";
  html += "            if(!file) return alert('Select file');\n";
  html += "            const formData = new FormData();\n";
  html += "            formData.append('update', file);\n";
  html += "            fetch('/update', { method: 'POST', body: file }).then(r "
          "=> {\n";
  html += "                if(r.ok) alert('Update sent, rebooting...');\n";
  html += "                else alert('Update failed');\n";
  html += "            });\n";
  html += "        }\n";
  html += "\n";
  html += "        init();\n";
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

void LaserWebServer::setShieldState(uint8_t deviceAddr, uint8_t inputNum,
                                    bool state) {
  if (deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
    uint8_t oldState = shieldMask[deviceAddr - 1][inputNum - 1];
    shieldMask[deviceAddr - 1][inputNum - 1] = state ? 1 : 0;

    // Trigger callback only if state actually changed
    if (oldState != (state ? 1 : 0) && shieldingChangeCallback != nullptr) {
      shieldingChangeCallback(deviceAddr, inputNum, state);
    }
  }
}

bool LaserWebServer::getShieldState(uint8_t deviceAddr, uint8_t inputNum) {
  if (deviceAddr >= 1 && deviceAddr <= 4 && inputNum >= 1 && inputNum <= 48) {
    return shieldMask[deviceAddr - 1][inputNum - 1] == 1;
  }
  return false;
}

String LaserWebServer::getShieldMaskJSON() {
  DynamicJsonDocument doc(4096);

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

void LaserWebServer::loadShielding(uint8_t shielding[4][48]) {
  memcpy(shieldMask, shielding, sizeof(shieldMask));
}

void LaserWebServer::setShieldingChangeCallback(
    ShieldingChangeCallback callback) {
  shieldingChangeCallback = callback;
  Serial.println("Shielding change callback registered");
}