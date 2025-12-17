#include "WS_RS485.h"
#include <algorithm>

HardwareSerial lidarSerial(1);  // Using serial port 1
uint8_t data[][8] = {                                       // ESP32-S3-Relay-1CH Control Command (RS485 receiving data)
  { 0x06, 0x05, 0x00, 0x01, 0x55, 0x00, 0xA2, 0xED },       // ESP32-S3-Relay-1CH CH1 Toggle
  { 0x06, 0x05, 0x00, 0xFF, 0xFF, 0x00, 0xBD, 0xBD },       // ESP32-S3-Relay-1CH ALL ON
  { 0x06, 0x05, 0x00, 0xFF, 0x00, 0x00, 0xFC, 0x4D },       // ESP32-S3-Relay-1CH ALL OFF
};
uint8_t Send_Data[][8] = {                                  // Modbus RTU Relay Control Command (RS485 send data)
  { 0x01, 0x05, 0x00, 0x00, 0x55, 0x00, 0xF2, 0x9A },       // Modbus RTU Relay CH1 Toggle
  { 0x01, 0x05, 0x00, 0x01, 0x55, 0x00, 0xA3, 0x5A },       // Modbus RTU Relay CH2 Toggle
  { 0x01, 0x05, 0x00, 0x02, 0x55, 0x00, 0x53, 0x5A },       // Modbus RTU Relay CH3 Toggle
  { 0x01, 0x05, 0x00, 0x03, 0x55, 0x00, 0x02, 0x9A },       // Modbus RTU Relay CH4 Toggle
  { 0x01, 0x05, 0x00, 0x04, 0x55, 0x00, 0xB3, 0x5B },       // Modbus RTU Relay CH5 Toggle
  { 0x01, 0x05, 0x00, 0x05, 0x55, 0x00, 0xE2, 0x9B },       // Modbus RTU Relay CH6 Toggle
  { 0x01, 0x05, 0x00, 0x06, 0x55, 0x00, 0x12, 0x9B },       // Modbus RTU Relay CH7 Toggle
  { 0x01, 0x05, 0x00, 0x07, 0x55, 0x00, 0x43, 0x5B },       // Modbus RTU Relay CH8 Toggle
  { 0x01, 0x05, 0x00, 0xFF, 0xFF, 0xFF, 0xFC, 0x4A },       // Modbus RTU Relay ALL ON
  { 0x01, 0x05, 0x00, 0xFF, 0x00, 0x00, 0xFD, 0xFA },       // Modbus RTU Relay ALL OFF
};
uint8_t buf[20] = {0};          // Data storage area
int numRows = sizeof(data) / sizeof(data[0]);

void SetData(uint8_t* data, size_t length) {
  lidarSerial.write(data, length);                          // Send data from the RS485
}
void ReadData(uint8_t* buf, uint8_t length) {
  uint8_t Receive_Flag = 0;       
  Receive_Flag = lidarSerial.available();
  if (Receive_Flag >= length) {
    lidarSerial.readBytes(buf, length); 
    char printBuf[length * 3 + 1];
    sprintf(printBuf, "Received data: ");
    for (int i = 0; i < length; i++) {
      sprintf(printBuf + strlen(printBuf), "%02X ", buf[i]); 
    }
    printf(printBuf); 
    /*************************
    Add a receiving data handler
    *************************/
    Receive_Flag = 0;
    memset(buf, 0, sizeof(buf));   
  }
}
void RS485_Analysis(uint8_t *buf)
{
  switch(buf[1])
  {
    case Extension_CH1:
      SetData(Send_Data[0],sizeof(Send_Data[0])); 
      printf("|***  Toggle expansion channel 1 ***|\r\n");
      break;
    case Extension_ALL_ON:
      SetData(Send_Data[8],sizeof(Send_Data[8])); 
      printf("|***  Enable all extension channels ***|\r\n");
      break;
    case Extension_ALL_OFF:
      SetData(Send_Data[9],sizeof(Send_Data[9])); 
      printf("|***  Close all expansion channels ***|\r\n");
      break;
    default:
      printf("Note : Non-control external device instructions !\r\n");
  }
}
uint32_t Baudrate = 0;
double  transmission_time = 0;
double RS485_cmd_Time = 0;
void RS485_Init()                                             // Initializing serial port
{    
  Baudrate = 9600;                                            // Set the baud rate of the serial port                                              
  lidarSerial.begin(Baudrate, SERIAL_8N1, RXD1, TXD1);        // Initializing serial port
  if (!lidarSerial.setPins(-1, -1, -1, TXD1EN)) {
    printf("Failed to set TXDEN pins\r\n");
  }
  if (!lidarSerial.setMode(UART_MODE_RS485_HALF_DUPLEX)) {
    printf("Failed to set RS485 mode\r\n");
  }
  transmission_time = 10.0 / Baudrate * 1000 ;
  RS485_cmd_Time = transmission_time*8;                       // 8:data length
  xTaskCreatePinnedToCore(
    RS485Task,    
    "RS485Task",   
    4096,                
    NULL,                 
    3,                   
    NULL,                 
    0                   
  );
}

void RS485Task(void *parameter) {
  while(1){
    RS485_Loop();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelete(NULL);
}

void RS485_Loop()
{
  uint8_t Receive_Flag = 0;       // Receiving mark
  Receive_Flag = lidarSerial.available();    

  if (Receive_Flag > 0) {
    if(RS485_cmd_Time > 1)              // Time greater than 1 millisecond
      delay((uint16_t)RS485_cmd_Time);
    else                      // Time is less than 1 millisecond 
      delay(1);
    Receive_Flag = lidarSerial.available();
    lidarSerial.readBytes(buf, Receive_Flag);              // The Receive_Flag length is read
    if(Receive_Flag == 8){
      uint8_t i=0;
      for(i=0;i<numRows;i++){
        bool result = std::equal(std::begin(buf), std::begin(buf) + 8, std::begin(data[i]));    // Compare two arrays
        if(result){
          if(i < numRows-1)
            buf[0] = i+1+48;
          else if(i == numRows-1)
            buf[0] = 48;
          Relay_Analysis(buf,RS485_Mode);
          break;
        }
      }   
      if(i > numRows-1)
        printf("Note : Non-instruction data was received - RS485 !\r\n");
    }
    else{
      printf("Note : Non-instruction data was received .Number of bytes: %d - RS485 !\r\n",Receive_Flag);
    }
    Receive_Flag=0;
    memset(buf,0, sizeof(buf));
  }
}