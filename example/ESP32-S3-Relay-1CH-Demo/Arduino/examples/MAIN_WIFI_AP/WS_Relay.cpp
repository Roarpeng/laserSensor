#include "WS_Relay.h"

bool Failure_Flag = 0;
/*************************************************************  Relay I/O  *************************************************************/
void Relay_Open(void)
{
  digitalWrite(GPIO_PIN_CH1, HIGH);
}
void Relay_Closs(void)
{
  digitalWrite(GPIO_PIN_CH1, LOW);
}
void Relay_Toggle()
{
  digitalToggle(GPIO_PIN_CH1);
}
void Relay_Set_State(bool State)
{
  if(State)
    Relay_Open();
  else
    Relay_Closs();
}

void Relay_Init(void)
{
  /*************************************************************************
  Relay GPIO
  *************************************************************************/
  pinMode(GPIO_PIN_CH1, OUTPUT);     // Initialize the control GPIO of relay CH1
}
/********************************************************  Data Analysis  ********************************************************/
bool Relay_Flag[Relay_Number_MAX] = {0};       // Relay current status flag
void Relay_Analysis(uint8_t *buf,uint8_t Mode_Flag)
{
  uint8_t ret = 0;
  if(Mode_Flag == Bluetooth_Mode)
    printf("Bluetooth Data :\r\n");
  else if(Mode_Flag == WIFI_Mode)
    printf("WIFI Data :\r\n");
  else if(Mode_Flag == MQTT_Mode)
    printf("MQTT Data :\r\n");
  else if(Mode_Flag == RS485_Mode)
    printf("RS485 Data :\r\n");  
  switch(buf[0])
  {
    case CH1: 
      Relay_Toggle();                                              //Toggle the level status of the GPIO_PIN_CH1 pin
      Relay_Flag[0] =! Relay_Flag[0];
      if(Relay_Flag[0])
        printf("|***  Relay CH1 on  ***|\r\n");
      else
        printf("|***  Relay CH1 off ***|\r\n");
      break;
    case CH1_ON:
      Relay_Open();                                               // Turn on all relay
      memset(Relay_Flag,1, sizeof(Relay_Flag));
      printf("|***  Relay CH1 on  ***|\r\n");
      break;
    case CH1_OFF:
      Relay_Closs();                                                // Turn off all relay
      memset(Relay_Flag,0, sizeof(Relay_Flag));
      printf("|***  Relay CH1 off ***|\r\n");
    break;
    default:
      printf("Note : Non-instruction data was received !  -  %c\r\n", buf[0]);
  }
}

void Relay_Immediate(uint8_t CHx, bool State, uint8_t Mode_Flag)
{
  if(Mode_Flag == RTC_Mode)
    printf("RTC Data :\r\n");
  Relay_Set_State(State);        
  Relay_Flag[0] = State;
  if(Relay_Flag[0])
    printf("|***  Relay CH1 on  ***|\r\n");
  else
    printf("|***  Relay CH1 off ***|\r\n");
}
void Relay_Immediate_CHxn(Status_adjustment * Relay_n, uint8_t Mode_Flag)
{
  uint8_t ret = 0;
  if(Mode_Flag == RTC_Mode)
    printf("RTC Data :\r\n");            
  if(Relay_n[0] == STATE_Open || Relay_n[0] == STATE_Closs){
    Relay_Flag[0] = (bool)Relay_n[0];
    Relay_Set_State(Relay_n[0]);  
    if(Relay_n[0] == STATE_Open)
      printf("|***  Relay CH1 on  ***|\r\n");
    else if(Relay_n[0] == STATE_Closs)
      printf("|***  Relay CH1 off ***|\r\n");
  }
  
}
