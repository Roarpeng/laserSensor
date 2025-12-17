#pragma once

#include <HardwareSerial.h>     // Reference the ESP32 built-in serial port library
#include "WS_GPIO.h"


/*************************************************************  I/O  *************************************************************/
#define Relay_Number_MAX  1 


#define CH1 '1'                 // CH1 Enabled Instruction              Hex : 0x31
#define CH1_ON  '9'             // Start all channel instructions       Hex : 0x39
#define CH1_OFF '0'             // Disable all channel instructions     Hex : 0x30

#define RS485_Mode        1     // Used to distinguish data sources
#define Bluetooth_Mode    2
#define WIFI_Mode         3
#define MQTT_Mode         4
#define RTC_Mode          5

typedef enum {
  STATE_Closs = 0,    // Closs Relay
  STATE_Open = 1,     // Open Relay
  STATE_Retain = 2,   // Stay in place
} Status_adjustment;

extern bool Relay_Flag[Relay_Number_MAX];  // Relay current status flag

void Relay_Init(void);
void Relay_Closs(void);
void Relay_Open(void);
void Relay_Toggle(void);
void Relay_Set_State(bool State);

void Relay_Analysis(uint8_t *buf,uint8_t Mode_Flag);
void Relay_Immediate(uint8_t CHx, bool State, uint8_t Mode_Flag);
void Relay_Immediate_CHxn(Status_adjustment * Relay_n, uint8_t Mode_Flag);
