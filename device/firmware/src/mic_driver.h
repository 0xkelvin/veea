#ifndef MIC_DRIVER_H
#define MIC_DRIVER_H

#include <zephyr/types.h>
#include <stdbool.h>

int mic_driver_init(void);
void mic_driver_start(void);
void mic_driver_stop(void);
bool mic_driver_is_running(void);

#endif /* MIC_DRIVER_H */
