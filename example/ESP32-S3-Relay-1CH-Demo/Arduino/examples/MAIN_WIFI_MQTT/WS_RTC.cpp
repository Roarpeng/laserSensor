#include "WS_RTC.h"

Timing_RTC CHx_State[Timing_events_Number_MAX];       // Set a maximum of Timing_events_Number_MAX timers
char Event_str[Timing_events_Number_MAX][1000];
static Timing_RTC CHx_State_Default;            // Event initial state
const unsigned char Event_cycle[4][13] = {"Aperiodicity","everyday","Weekly","monthly"};

void RTC_Init(void){
  PCF85063_Init();
  xTaskCreatePinnedToCore(
    RTCTask,    
    "RTCTask",   
    4096,                
    NULL,                 
    3,                   
    NULL,                 
    0                   
  );
}
uint8_t Timing_events_Num = 0;
void RTCTask(void *parameter)
{ 
  static uint8_t Time_Old = 0;
  while(1){
    if(Timing_events_Num){
      for (int i = 0; i < Timing_events_Number_MAX; i++){
        if(CHx_State[i].Enable_Flag){
          if(CHx_State[i].Time.hour == datetime.hour && CHx_State[i].Time.minute == datetime.minute && CHx_State[i].Time.second == datetime.second && datetime.second != Time_Old){       // The event time is consistent with the current time
            switch(CHx_State[i].repetition_State){
              case Repetition_NONE:
                if(CHx_State[i].Time.year == datetime.year && CHx_State[i].Time.month == datetime.month && CHx_State[i].Time.day == datetime.day){ // Executes at the defined date and time
                  TimerEvent_handling(CHx_State[i]);
                  TimerEvent_Del(CHx_State[i]);
                }
                break;
              case Repetition_everyday:
                TimerEvent_handling(CHx_State[i]);
                break;
              case Repetition_Weekly:
                if(CHx_State[i].Time.dotw == datetime.dotw){
                  TimerEvent_handling(CHx_State[i]);
                }
                break;
              case Repetition_monthly:
                if(CHx_State[i].Time.day == datetime.day){
                  TimerEvent_handling(CHx_State[i]);
                }
                break;
              default:
                printf("Event error!!!!\n");
                break;
            }
          }
        }
      }
    }
    Time_Old = datetime.second;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelete(NULL);
}

void TimerEvent_handling(Timing_RTC event){
  uint8_t Retain_channels = 0;  
  printf("Event %d : \r\n", event.Event_Number);
  char datetime_str[50];
  datetime_to_str(datetime_str,event.Time);
  for (int i = 0; i < Relay_Number_MAX; i++) {
    if(*(&(event.Relay_CH1)+i) == STATE_Retain)                     // Find the modified channel
      Retain_channels ++;                                     // Number of unmodified channels  
  }
  if(Retain_channels < Relay_Number_MAX - 1){  
    printf("%s\r\n", datetime_str);
    printf("CHx Open   : ");  
    int j = 0;       
    for (j = 0; j < Relay_Number_MAX; j++) {
      if(*(&(event.Relay_CH1)+j) == STATE_Open)                          
        printf("CH%d    ", j+1); 
    } 
    printf("\r\nCHx Closs  : ");         
    for (j = 0; j < Relay_Number_MAX; j++) {
      if(*(&(event.Relay_CH1)+j) == STATE_Closs)                          
        printf("CH%d    ", j+1); 
    }
    if(Retain_channels){
      printf("\r\nCHx Retain : ");         
      for (j = 0; j < Relay_Number_MAX; j++) {
        if(*(&(event.Relay_CH1)+j) == STATE_Retain)                          
          printf("CH%d    ", j+1); 
      }
    }
    printf("\r\n");   
    Relay_Immediate_CHxn(&(event.Relay_CH1), RTC_Mode);
    printf("\r\n");
  }
  else if(Retain_channels == Relay_Number_MAX - 1){           // Modified a channel                (use TimerEvent_CHx_Set())
    printf("%s\r\n", datetime_str);
    for (int x = 0; x < Relay_Number_MAX; x++) {
      if(*(&(event.Relay_CH1)+x) != STATE_Retain){                   // Find the modified channel
        if(*(&(event.Relay_CH1)+x)){
          printf("CH%d Open\r\n", x); 
          Relay_Immediate(x, true, RTC_Mode);
          printf("\r\n");
        }
        else{
          printf("CH%d Closs\r\n", x); 
          Relay_Immediate(x, false, RTC_Mode);
          printf("\r\n");
        }
        break;
      }
    }
  }
  else{
    printf("Event error or no relay control!!!\r\n");
  }
}

void TimerEvent_CHx_Set(datetime_t time,uint8_t CHx, bool State, Repetition_event Repetition)
{
  char datetime_str[50];
  datetime_to_str(datetime_str,datetime);
  printf("Now Time: %s!!!!\r\n", datetime_str);
  if(CHx > Relay_Number_MAX){
    printf("Timing_CHx_Set(function): Error passing parameter CHx!!!!\r\n");
    return;
  }
  if(Timing_events_Num + 1 >= Timing_events_Number_MAX)
  {
    printf("Note : The number of scheduled events is full.\r\n");
  }
  else{
    CHx_State[Timing_events_Num].Enable_Flag = true;
    CHx_State[Timing_events_Num].Event_Number = Timing_events_Num + 1;
    *(&(CHx_State[Timing_events_Num].Relay_CH1)+CHx) = (Status_adjustment)State;
    CHx_State[Timing_events_Num].Time = time;
    CHx_State[Timing_events_Num].repetition_State = Repetition;
    Timing_events_Num ++;
    datetime_to_str(datetime_str,time);
    if(State){
      printf("New timing event%d :\r\n        %s set CH%d Open  ----- %s\r\n\r\n", Timing_events_Num, datetime_str, CHx, Event_cycle[Repetition]);
      sprintf(Event_str[Timing_events_Num-1], "Event %d : %s set CH%d Open  ----- %s\\n\\n", Timing_events_Num, datetime_str, CHx, Event_cycle[Repetition]);
    }
    else{
      printf("New timing event%d :\r\n        %s set CH%d Closs ----- %s\r\n\r\n", Timing_events_Num, datetime_str, CHx, Event_cycle[Repetition]);
      sprintf(Event_str[Timing_events_Num-1], "Event %d : %s set CH%d Closs  ----- %s\\n\\n", Timing_events_Num, datetime_str, CHx, Event_cycle[Repetition]);
    }
  }
}

void TimerEvent_CHxs_Set(datetime_t time,uint8_t PinState, Repetition_event Repetition)
{

  char datetime_str[50];
  datetime_to_str(datetime_str,datetime);
  printf("Now Time: %s!!!!\r\n", datetime_str);
  if(Timing_events_Num + 1 >= Timing_events_Number_MAX)
  {
    printf("Note : The number of scheduled events is full.\r\n");
  }
  else{
    CHx_State[Timing_events_Num].Enable_Flag = true;
    CHx_State[Timing_events_Num].Event_Number = Timing_events_Num + 1;
    for (int i = 0; i < Relay_Number_MAX; i++) {
      *(&(CHx_State[Timing_events_Num].Relay_CH1)+i) = (Status_adjustment)((PinState >> i) & 0x01); 
    }
    CHx_State[Timing_events_Num].Time = time;
    CHx_State[Timing_events_Num].repetition_State = Repetition;
    Timing_events_Num ++;
    datetime_to_str(datetime_str,time);
    printf("New timing event%d :\r\n       %s \r\n",Timing_events_Num, datetime_str);
    printf("        CHx   :");
    for (int i = 0; i < Relay_Number_MAX; i++) 
      printf("CH%d      ", i+1);
    printf("\r\n        State :");
    for (int i = 0; i < Relay_Number_MAX; i++) {
      if((PinState >> i) & 0x01)
        printf("Open     ");
      else
        printf("Closs    ");
    }
    printf("\r\n");
    printf("        ----- %s\r\n\r\n", Event_cycle[Repetition]);
    printf("\r\n");  

    int len = 0;  
    char Event_content[1000];  
    len += snprintf(Event_content + len, sizeof(Event_content) - len, "&nbsp;&nbsp;&nbsp;&nbsp;CHx&nbsp;&nbsp;:");
    for (int i = 0; i < Relay_Number_MAX; i++) {
      len += snprintf(Event_content + len, sizeof(Event_content) - len, "CH%d&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;", i + 1);
    }
    len += snprintf(Event_content + len, sizeof(Event_content) - len, "\\n &nbsp;&nbsp;&nbsp;&nbsp;State :");
    for (int i = 0; i < Relay_Number_MAX; i++) {
      if ((PinState >> i) & 0x01)  
        len += snprintf(Event_content + len, sizeof(Event_content) - len, "Open&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;");
      else
        len += snprintf(Event_content + len, sizeof(Event_content) - len, "Closs&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;");
    }
    len += snprintf(Event_content + len, sizeof(Event_content) - len, "\\n&nbsp;&nbsp;&nbsp;&nbsp;----- %s\\n\\n", Event_cycle[Repetition]);
    // printf("%s\r\n", Event_content);
    sprintf(Event_str[Timing_events_Num-1], "Event %d : %s \\n%s", Timing_events_Num, datetime_str,Event_content);
  }
}
void TimerEvent_CHxn_Set(datetime_t time,Status_adjustment *Relay_n, Repetition_event Repetition)
{
  char datetime_str[50];
  datetime_to_str(datetime_str,datetime);
  printf("Now Time: %s!!!!\r\n", datetime_str);
  if(Timing_events_Num + 1 >= Timing_events_Number_MAX)
  {
    printf("Note : The number of scheduled events is full.\r\n");
  }
  else{ 
    CHx_State[Timing_events_Num].Enable_Flag = true;
    CHx_State[Timing_events_Num].Event_Number = Timing_events_Num + 1;
    for (int i = 0; i < Relay_Number_MAX; i++) {
      *(&(CHx_State[Timing_events_Num].Relay_CH1)+i) = Relay_n[i]; 
    }
    CHx_State[Timing_events_Num].Time = time;
    CHx_State[Timing_events_Num].repetition_State = Repetition;
    Timing_events_Num ++;
    datetime_to_str(datetime_str,time);
    printf("New timing event%d :\r\n       %s \r\n",Timing_events_Num, datetime_str);
    printf("        CHx   :");
    for (int i = 0; i < Relay_Number_MAX; i++) 
      printf("CH%d      ", i+1);
    printf("\r\n        State :");
    for (int i = 0; i < Relay_Number_MAX; i++) {
      if(Relay_n[i] == STATE_Open)
        printf("Open     ");
      else if(Relay_n[i] == STATE_Closs)
        printf("Closs    ");
      else if(Relay_n[i] == STATE_Retain)
        printf("Retain   ");
    }
    printf("\r\n");
    printf("        ----- %s\r\n\r\n", Event_cycle[Repetition]);
    printf("\r\n");
    
    int len = 0;  
    char Event_content[1000];  
    len += snprintf(Event_content + len, sizeof(Event_content) - len, "&nbsp;&nbsp;&nbsp;&nbsp;CHx&nbsp;&nbsp;:");
    for (int i = 0; i < Relay_Number_MAX; i++) {
      len += snprintf(Event_content + len, sizeof(Event_content) - len, "CH%d&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;", i + 1);
    }
    len += snprintf(Event_content + len, sizeof(Event_content) - len, "\\n&nbsp;&nbsp;&nbsp;&nbsp;State&nbsp;:");
    for (int i = 0; i < Relay_Number_MAX; i++) {
      if (Relay_n[i] == STATE_Open)
        len += snprintf(Event_content + len, sizeof(Event_content) - len, "Open&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;");
      else if(Relay_n[i] == STATE_Closs)
        len += snprintf(Event_content + len, sizeof(Event_content) - len, "Closs&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;");
      else if(Relay_n[i] == STATE_Retain)
        len += snprintf(Event_content + len, sizeof(Event_content) - len, "Retain&nbsp;&nbsp;&nbsp;");
    }
    len += snprintf(Event_content + len, sizeof(Event_content) - len, "\\n&nbsp;&nbsp;&nbsp;&nbsp;----- %s\\n\\n", Event_cycle[Repetition]);
    // printf("%s\r\n", Event_content);  
    sprintf(Event_str[Timing_events_Num-1], "Event %d : %s \\n%s", Timing_events_Num, datetime_str,Event_content);
  }
}

void TimerEvent_printf(Timing_RTC event){
  uint8_t Retain_channels = 0;
  uint8_t open[8]={0};      
  printf("Event %d : \r\n", event.Event_Number);
  char datetime_str[50];
  datetime_to_str(datetime_str,event.Time);
  for (int i = 0; i < Relay_Number_MAX; i++) {
    if(*(&(event.Relay_CH1)+i) == STATE_Retain)                     // Find the modified channel
      Retain_channels ++;                                     // Number of unmodified channels  
    else
      open[i] = *(&(event.Relay_CH1)+i);
  }
  if(Retain_channels == 0){                                   // All channels have been modified   (use TimerEvent_CHxs_Set())   
    printf("%s\r\n", datetime_str);
    printf(" CHx Open  : ");         
    for (int j = 0; j < Relay_Number_MAX; j++) {
      if(open[j])                          
        printf("CH%d    ", j); 
    } 
    printf("\r\n CHx Closs : ");         
    for (int k = 0; k < Relay_Number_MAX; k++) {
      if(!open[k])                          
        printf("CH%d    ", k); 
    }
    printf("\r\n");    
  }
  else if(Retain_channels == Relay_Number_MAX - 1){           // Modified a channel                (use TimerEvent_CHx_Set())
    printf("%s    ,", datetime_str);
    for (int x = 0; x < Relay_Number_MAX; x++) {
      if(*(&(event.Relay_CH1)+x) != STATE_Retain){                   // Find the modified channel
        if(*(&(event.Relay_CH1)+x))
          printf("CH%d Open\r\n", x); 
        else
          printf("CH%d Closs\r\n", x); 
        break;
      }
    }
  }
  else{
    printf("%s\r\n", datetime_str);
    printf("CHx Open   : ");  
    int j = 0;       
    for (j = 0; j < Relay_Number_MAX; j++) {
      if(open[j] == STATE_Open)                          
        printf("CH%d    ", j+1); 
    } 
    printf("\r\nCHx Closs  : ");         
    for (j = 0; j < Relay_Number_MAX; j++) {
      if(open[j] == STATE_Closs)                          
        printf("CH%d    ", j+1); 
    }
    printf("\r\nCHx Retain : ");         
    for (j = 0; j < Relay_Number_MAX; j++) {
      if(open[j] == STATE_Retain)                          
        printf("CH%d    ", j+1); 
    }
    printf("\r\n");   
  }
}

void TimerEvent_printf_ALL(void)
{
  printf("/******************* Current RTC event *******************/ \r\n");
  for (int i = 0; i < Timing_events_Number_MAX; i++) {
    if(CHx_State[i].Enable_Flag)
      TimerEvent_printf(CHx_State[i]);
  }
  printf("/******************* Current RTC event *******************/\r\n\r\n ");
}
void TimerEvent_Del(Timing_RTC event){
  printf("Example Delete an RTC event%d\r\n\r\n",event.Event_Number);
  for (int i = event.Event_Number; i < Timing_events_Number_MAX; i++) {
    CHx_State[i].Event_Number = CHx_State[i].Event_Number -1;
    CHx_State[i-1] = CHx_State[i];  
  }
  CHx_State[Timing_events_Number_MAX - 1] = CHx_State_Default;
  memset(Event_str[Timing_events_Number_MAX - 1], 0, sizeof(Event_str[Timing_events_Number_MAX - 1]));
  Timing_events_Num --;
}
void TimerEvent_Del_Number(uint8_t Event_Number){
  TimerEvent_Del(CHx_State[Event_Number - 1]);
}