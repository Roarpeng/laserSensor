#include <HardwareSerial.h>     // Reference the ESP32 built-in serial port library
#include "WS_Bluetooth.h"
#include "WS_GPIO.h"
#include "WS_RTC.h"
#include "WS_GPIO.h"
#include "WS_RS485.h"

uint32_t Simulated_time=0;      // Analog time counting

/********************************************************  Initializing  ********************************************************/
void setup() { 
  I2C_Init();
  RTC_Init();   // RTC
  RS485_Init();
  WIFI_Init();
  Bluetooth_Init();// Bluetooth
  
  Relay_Init();
  printf("Connect to the WIFI network named \"ESP32-S3-Relay-1CH\" and access the Internet using the connected IP address!!!\r\n");
}

/**********************************************************  While  **********************************************************/
void loop() {

}