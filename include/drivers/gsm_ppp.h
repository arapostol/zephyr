#ifndef GSM_PPP_H_
#define GSM_PPP_H_

#define GSM_MODEM_DEVICE_NAME "modem_gsm"

/** @cond INTERNAL_HIDDEN */
struct device;
extern struct device *hola_gsm;
void gsm_ppp_resume(const struct device *device);
void gsm_ppp_start(const struct device *device);
void gsm_ppp_stop(const struct device *device);
/** @endcond */

#endif /* GSM_PPP_H_ */