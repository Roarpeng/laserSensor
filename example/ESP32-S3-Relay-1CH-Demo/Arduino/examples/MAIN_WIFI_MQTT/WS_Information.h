#pragma once

#define Extension_Enable      1                   // Whether to extend the connection to external devices     1:Expansion device Modbus RTU Relay     0:No extend
#define RTC_Event_Enable      1                   // Whether to enable RTC events  (Bluetooth)                1:Enable                                0:Disable



// Name and password of the WiFi access point
#define STASSID       "JSBPI"
#define STAPSK        "waveshare0755"

// Details about devices on the Waveshare cloud
#define MQTT_Server   "mqtt.waveshare.cloud"
#define MQTT_Port     1883
#define MQTT_ID       "1e2daecd"
#define MQTT_Pub      "Pub/59/498/1e2daecd"
#define MQTT_Sub      "Sub/59/498/1e2daecd"
