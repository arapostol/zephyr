/*
 * Copyright (c) 2020 Endian Technologies AB
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSM_PPP_H_
#define GSM_PPP_H_

#define GSM_MODEM_DEVICE_NAME "modem_gsm"

#include <stdint.h>
#include <stdbool.h>

/** @cond INTERNAL_HIDDEN */
struct device;
void gsm_ppp_start(const struct device *device);
int gsm_ppp_resume(const struct device *device);
void gsm_ppp_restart(const struct device *device);
int gsm_ppp_stop(const struct device *device);

void gsm_setup(const struct device *device);
bool gsm_is_ready(const struct device *device);
bool gsm_setup_finished(const struct device *device);

int gsm_set_apn(const char *apn);
int gsm_set_volume(uint8_t volume);
const char *gsm_imei();
/** @endcond */

#endif /* GSM_PPP_H_ */
