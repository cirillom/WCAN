#ifndef ADC_READER_H
#define ADC_READER_H

#include "esp_err.h"
#include "driver/adc.h"
#include "freertos/queue.h"

#include "wcan_communication.h" 


esp_err_t adc_reader_start_task(
    adc_channel_t channel,
    uint32_t can_id,
    uint32_t frequency_hz
);

#endif // ADC_READER_H