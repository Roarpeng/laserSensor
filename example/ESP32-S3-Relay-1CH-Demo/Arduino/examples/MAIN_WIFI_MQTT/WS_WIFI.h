#pragma once

#include "stdio.h"
#include <stdint.h>
#include <WiFi.h>
#include <WebServer.h> 
#include <WiFiClient.h>
#include <WiFiAP.h>
#include "WS_GPIO.h"
#include "WS_Information.h"
#include "WS_Relay.h"
#include "WS_RTC.h"

extern char ipStr[16];
extern bool WIFI_Connection;     

void WIFI_Init();
void WifiStaTask(void *parameter);
