#include "WS_Bluetooth.h"

BLEServer* pServer;                                                             // Used to represent a BLE server
BLECharacteristic* pTxCharacteristic;
BLECharacteristic* pRxCharacteristic;

/**********************************************************  Bluetooth   *********************************************************/

class MyServerCallbacks : public BLEServerCallbacks {                           //By overriding the onConnect() and onDisconnect() functions
    void onConnect(BLEServer* pServer) {                                        // When the Device is connected, "Device connected" is printed.
    Serial.println("Device connected"); 
  }

  void onDisconnect(BLEServer* pServer) {                                       // "Device disconnected" will be printed when the device is disconnected
    Serial.println("Device disconnected");

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();                 // Re-broadcast so that the device can query
    pAdvertising->addServiceUUID(SERVICE_UUID);                                 // Re-broadcast so that the device can query
    pAdvertising->setScanResponse(true);                                        // Re-broadcast so that the device can query
    pAdvertising->setMinPreferred(0x06);                                        // Re-broadcast so that the device can query 
    pAdvertising->setMinPreferred(0x12);                                        // Re-broadcast so that the device can query 
    BLEDevice::startAdvertising();                                              // Re-broadcast so that the device can query 
    pRxCharacteristic->notify();                                                // Re-broadcast so that the device can query  
    pAdvertising->start();                                                      // Re-broadcast so that the device can query
  }
};
class MyRXCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {                            // The onWrite function is called when the remote device sends data to your feature
    String rxValue = pCharacteristic->getValue();
    if (!rxValue.isEmpty()) {
      // The received data rxValue is processed here
      if(rxValue.length() == 1)
      {
        printf("%s\n", rxValue.c_str());                                        // Print output through the serial port
        uint8_t* valueBytes = reinterpret_cast<uint8_t*>(const_cast<char*>(rxValue.c_str())); // Convert value to uint8 t*
        Relay_Analysis(valueBytes,Bluetooth_Mode);                              // pilot relay
      }
      else if(rxValue.length() == 2)
      {
        if(Extension_Enable)
        {
          printf("%s\n", rxValue.c_str());                                      // Print output through the serial port       
          uint8_t* valueBytes = reinterpret_cast<uint8_t*>(const_cast<char*>(rxValue.c_str())); // Convert value to uint8 t*
          if(valueBytes[0] == 0x06)                                             // Instruction check correct
            RS485_Analysis(valueBytes);                                         // Control external relay
          else
            printf("Note : Non-instruction data was received - Bluetooth !\r\n");
        }
        else
          printf("Note : Non-instruction data was received or external relays are not enabled - Bluetooth !\r\n");
      }
      
      else if(rxValue.length() == 14)
      {
        if(RTC_Event_Enable)
        {   
          // printf("%s\n", rxValue.c_str());                                      // Print output through the serial port  
          uint8_t* valueBytes = reinterpret_cast<uint8_t*>(const_cast<char*>(rxValue.c_str())); 
          BLE_Set_RTC_Event(valueBytes);
        }
        else
          printf("Note : Non-instruction data was received or RTC events were not enabled - Bluetooth !\r\n");
      }
      else
      {
        printf("Note : Non-instruction data was received - Bluetooth !\r\n");
      }
      pRxCharacteristic->setValue("");                                           // After data is read, set it to blank for next read
    }
  }
};

void BLE_Set_RTC_Event(uint8_t* valueBytes){
  if(valueBytes[0] == 0xA1 && valueBytes[6] == 0xAA  && valueBytes[13] == 0xFF ){
    datetime_t Event_Time={0};
    Event_Time.year = (valueBytes[1]/16*10 + valueBytes[1] % 16) *100 + valueBytes[2]/16*10 + valueBytes[2] % 16;
    Event_Time.month = valueBytes[3]/16*10 + valueBytes[3] % 16;
    Event_Time.day = valueBytes[4]/16*10 + valueBytes[4] % 16;
    Event_Time.dotw = valueBytes[5]/16*10 + valueBytes[5] % 16;
    // valueBytes[6] == 0xAA; // check
    Event_Time.hour = valueBytes[7]/16*10 + valueBytes[7] % 16;
    Event_Time.minute = valueBytes[8]/16*10 + valueBytes[8] % 16;
    Event_Time.second = valueBytes[9]/16*10 + valueBytes[9] % 16;
    Repetition_event Repetition = (Repetition_event)valueBytes[12];       // cyclical indicators
    if(valueBytes[11]){                                                   // Whether to control all relays   1:Control all relays    0：Control a relay
      uint8_t CHxs = valueBytes[10];                                      // relay control
      TimerEvent_CHxs_Set(Event_Time, CHxs, Repetition);
    }
    else{
      uint8_t CHx = valueBytes[10]/16;
      bool State =  (valueBytes[10] % 16);
      TimerEvent_CHx_Set(Event_Time,CHx, State, Repetition);
    } 
  }
}
void Bluetooth_SendData(char* Data) {  // Send data using Bluetooth
  if (Data != nullptr && strlen(Data) > 0) {
    if (pServer->getConnectedCount() > 0) {
      String SendValue = String(Data);  // Convert char* to String
      pTxCharacteristic->setValue(SendValue);  // Set SendValue to the eigenvalue (String type)
      pTxCharacteristic->notify();  // Sends a notification to all connected devices
    }
  }
}
void Bluetooth_Init()
{
  /*************************************************************************
  Bluetooth
  *************************************************************************/
  BLEDevice::init("ESP32-S3-Relay-1CH");                                        // Initialize Bluetooth and start broadcasting                           
  pServer = BLEDevice::createServer();                                          
  pServer->setCallbacks(new MyServerCallbacks());                               
  BLEService* pService = pServer->createService(SERVICE_UUID);                  
  pTxCharacteristic = pService->createCharacteristic(
                                    TX_CHARACTERISTIC_UUID,
                                    BLECharacteristic:: PROPERTY_READ);         // The eigenvalues are readable and can be read by remote devices
  pRxCharacteristic = pService->createCharacteristic(
                                    RX_CHARACTERISTIC_UUID,
                                    BLECharacteristic::PROPERTY_WRITE);         // The eigenvalues are writable and can be written to by remote devices
  pRxCharacteristic->setCallbacks(new MyRXCallback());

  pRxCharacteristic->setValue("Successfully Connect To ESP32-S3-Relay-1CH");      
  pService->start();   

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();                   
  pAdvertising->addServiceUUID(SERVICE_UUID);                                   
  pAdvertising->setScanResponse(true);                                          
  pAdvertising->setMinPreferred(0x06);                                          
  pAdvertising->setMinPreferred(0x12);                                          
  BLEDevice::startAdvertising();                                                
  pRxCharacteristic->notify();                                                    
  pAdvertising->start();
  printf("Now you can read it in your phone!\r\n");
  xTaskCreatePinnedToCore(
    BLETask,    
    "BLETask",   
    4096,                
    NULL,                 
    2,                   
    NULL,                 
    0                   
  );
}

void BLETask(void *parameter) {
  while(1){
    Bluetooth_SendData(ipStr);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelete(NULL);
}