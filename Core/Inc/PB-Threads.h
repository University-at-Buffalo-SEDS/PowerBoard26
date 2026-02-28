#pragma once
#include "tx_api.h"
#include "ltc2990.h"

/* ------ Telemetry Thread ------ */
extern TX_THREAD telemetry_thread;

void telemetry_thread_entry(ULONG initial_input);
UINT create_telemetry_thread(TX_BYTE_POOL *byte_pool);
/* ------ Telemetry Thread ------ */

/* ------ Sensor Thread ------ */
extern TX_THREAD sensor_thread;

void sensor_thread_entry(ULONG entry_input);
UINT create_sensor_thread(TX_BYTE_POOL *byte_pool, LTC2990_Handle_t *ltc2990_voltage_handle_ptr, LTC2990_Handle_t *ltc2990_current_handle_ptr);

/* ------ Sensor Thread ------ */