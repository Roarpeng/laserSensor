#pragma once

#include "WS_PCF85063.h"
#include "WS_Relay.h"
#include "WS_GPIO.h"

#define Timing_events_Number_MAX    10              // Indicates the number of timers that can be set

typedef enum {
  Repetition_NONE = 0,        // aperiodicity
  Repetition_everyday = 1,    // The event is repeated at this time every day
  Repetition_Weekly = 2,      // This event is repeated every week at this time
  Repetition_monthly = 3,     // This event is repeated every month at this time
} Repetition_event;

typedef struct {
  bool Enable_Flag = false;                                   // The timer event enabled flag.    
  uint8_t Event_Number = 0;                                   // Current event sequence number   
  Status_adjustment Relay_CH1 = STATE_Retain;                       // The CH1 status is changed periodically
  Status_adjustment Relay_CH2 = STATE_Retain;                       // The CH2 status is changed periodically
  Status_adjustment Relay_CH3 = STATE_Retain;                       // The CH3 status is changed periodically
  Status_adjustment Relay_CH4 = STATE_Retain;                       // The CH4 status is changed periodically
  Status_adjustment Relay_CH5 = STATE_Retain;                       // The CH5 status is changed periodically
  Status_adjustment Relay_CH6 = STATE_Retain;                       // The CH6 status is changed periodically
  Status_adjustment Relay_CH7 = STATE_Retain;                       // The CH7 status is changed periodically
  Status_adjustment Relay_CH8 = STATE_Retain;                       // The CH8 status is changed periodically
  datetime_t Time;
  Repetition_event repetition_State = Repetition_NONE;         // Periodic execution
}Timing_RTC;

extern uint8_t Timing_events_Num;
extern Timing_RTC CHx_State[Timing_events_Number_MAX];
extern char Event_str[Timing_events_Number_MAX][1000];

void RTCTask(void *parameter);
void TimerEvent_handling(Timing_RTC event);
void TimerEvent_printf(Timing_RTC event);
void TimerEvent_Del(Timing_RTC event);

void RTC_Init(void);
void TimerEvent_CHx_Set(datetime_t time,uint8_t CHx, bool State, Repetition_event Repetition);
void TimerEvent_CHxs_Set(datetime_t time,uint8_t PinState, Repetition_event Repetition);
void TimerEvent_CHxn_Set(datetime_t time,Status_adjustment *Relay_n, Repetition_event Repetition);
void TimerEvent_printf_ALL(void);
void TimerEvent_Del_Number(uint8_t Event_Number);