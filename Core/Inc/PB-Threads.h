#pragma once
#include "tx_api.h"
#include "ltc2990.h"

/* ------ Telemetry Thread ------ */
extern TX_THREAD telemetry_thread;
extern ULONG telemetry_thread_stack[];

void telemetry_thread_entry(ULONG initial_input);
void create_telemetry_thread(void);
/* ------ Telemetry Thread ------ */

/* ------ Sensor Thread ------ */
extern TX_THREAD sensor_thread;
extern ULONG sensor_thread_stack[]; 

void sensor_thread_entry(ULONG entry_input);
void create_sensor_thread(LTC2990_Handle_t *ltc2990_handle);

/* ------ Sensor Thread ------ */