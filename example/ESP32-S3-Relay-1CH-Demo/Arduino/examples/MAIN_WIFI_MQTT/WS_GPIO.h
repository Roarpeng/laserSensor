#pragma once

#include <HardwareSerial.h>     // Reference the ESP32 built-in serial port library

/*************************************************************  I/O  *************************************************************/
#define GPIO_PIN_CH1      47    // CH1 Control GPIO

#define TXD1              17    //The TXD of UART1 corresponds to GPIO   RS485
#define RXD1              18    //The RXD of UART1 corresponds to GPIO   RS485
#define TXD1EN            21



/*************************************************************  I/O  *************************************************************/
void GPIO_Init();
void digitalToggle(int pin);
