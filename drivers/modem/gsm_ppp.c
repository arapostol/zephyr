/*
 * Copyright (c) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(modem_gsm, CONFIG_MODEM_LOG_LEVEL);

#include <kernel.h>
#include <device.h>
#include <sys/ring_buffer.h>
#include <sys/util.h>
#include <net/ppp.h>
#include <drivers/gsm_ppp.h>
#include <drivers/uart.h>
#include <drivers/console/uart_mux.h>
#include <stdlib.h>
#include <stdio.h>

#include "modem_context.h"
#include "modem_iface_uart.h"
#include "modem_cmd_handler.h"
#include "../console/gsm_mux.h"
#include "gsm_apn.h"

#define GSM_CMD_READ_BUF       128
#define GSM_CMD_AT_TIMEOUT     K_SECONDS(2)
#define GSM_CMD_SETUP_TIMEOUT  K_SECONDS(6)
#define GSM_RX_STACK_SIZE      CONFIG_MODEM_GSM_RX_STACK_SIZE
#define GSM_RECV_MAX_BUF       30
#define GSM_RECV_BUF_SIZE      128
#define GSM_BUF_ALLOC_TIMEOUT  K_SECONDS(1)

/* During the modem setup, we first create DLCI control channel and then
 * PPP and AT channels. Currently the modem does not create possible GNSS
 * channel.
 */
enum setup_state {
	STATE_INIT = 0,
	STATE_CONTROL_CHANNEL = 0,
	STATE_PPP_CHANNEL,
	STATE_AT_CHANNEL,
	STATE_DONE
};

static struct gsm_modem {
	struct modem_context context;

	struct modem_cmd_handler_data cmd_handler_data;
	uint8_t cmd_read_buf[GSM_CMD_READ_BUF];
	uint8_t cmd_match_buf[GSM_CMD_READ_BUF];
	struct k_sem sem_response;

	struct modem_iface_uart_data gsm_data;
	struct k_delayed_work gsm_configure_work;
	char gsm_isr_buf[PPP_MRU];
	char gsm_rx_rb_buf[PPP_MRU * 3];

	uint8_t *ppp_recv_buf;
	size_t ppp_recv_buf_len;

	enum setup_state state;
	const struct device *ppp_dev;
	const struct device *at_dev;
	const struct device *control_dev;

	struct net_if *iface;

	bool mux_enabled : 1;
	bool mux_setup_done : 1;
	bool setup_done : 1;
} gsm;

NET_BUF_POOL_DEFINE(gsm_recv_pool, GSM_RECV_MAX_BUF, GSM_RECV_BUF_SIZE,
		    0, NULL);
K_THREAD_STACK_DEFINE(gsm_rx_stack, GSM_RX_STACK_SIZE);

struct k_thread gsm_rx_thread;

static void gsm_rx(struct gsm_modem *gsm)
{
	LOG_DBG("starting");

	while (true) {
		k_sem_take(&gsm->gsm_data.rx_sem, K_FOREVER);

		/* The handler will listen AT channel */
		gsm->context.cmd_handler.process(&gsm->context.cmd_handler,
						 &gsm->context.iface);
	}
}

MODEM_CMD_DEFINE(gsm_cmd_ok)
{
	modem_cmd_handler_set_error(data, 0);
	LOG_DBG("ok");
	k_sem_give(&gsm.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(gsm_cmd_error)
{
	modem_cmd_handler_set_error(data, -EINVAL);
	LOG_DBG("error");
	k_sem_give(&gsm.sem_response);
	return 0;
}

static const struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", gsm_cmd_ok, 0U, ""),
	MODEM_CMD("ERROR", gsm_cmd_error, 0U, ""),
	MODEM_CMD("CONNECT", gsm_cmd_ok, 0U, ""),
};

#define MDM_MANUFACTURER_LENGTH  10
#define MDM_MODEL_LENGTH         16
#define MDM_REVISION_LENGTH      64
#define MDM_IMEI_LENGTH          16
#define MDM_APN_LENGTH			 100
#define MDM_MCCMNC_LENGTH		 7

struct modem_info {
	char mdm_manufacturer[MDM_MANUFACTURER_LENGTH];
	char mdm_model[MDM_MODEL_LENGTH];
	char mdm_revision[MDM_REVISION_LENGTH];
	char mdm_imei[MDM_IMEI_LENGTH];
	char mdm_apn[MDM_APN_LENGTH];
	char mdm_mccmnc[MDM_MCCMNC_LENGTH];
};

static struct modem_info minfo;

static char cgdcont_cmd[150];
static char clvl_cmd[15];

int apn_lookup(char **apn, const char *mccmcn)
{
	int mcc_mcn;

	mcc_mcn = atoi(mccmcn);

	for (int i = 0; i < ARRAY_SIZE(apn_list); i++) {
		if (mcc_mcn == apn_list[i].mcc_mcn) {
			LOG_INF("Found APN: %s", log_strdup(apn_list[i].apn));
			*apn = apn_list[i].apn;
			return 0;
		}
	}

	return -ENOENT;
}

int gsm_set_volume(uint8_t volume)
{
	if (volume > 5) {
		LOG_ERR("CLVL Volume error");
		return -EINVAL;
	}

	sprintf(clvl_cmd, "AT+CLVL=%d", volume);
	return 0;
}

int gsm_set_apn(const char *apn)
{
	static bool apn_set = false;
	int len;

	len = strnlen(apn, MDM_APN_LENGTH);

	if (len == MDM_APN_LENGTH) {
		LOG_ERR("APN length error");
		return -EINVAL;
	} else if (len == 0) {
		LOG_INF("Auto APN selected");
		return 0;
	} else if (apn_set) {
		/* If APN was manual set, don't override */
		LOG_ERR("APN already set");
		return -EEXIST;
	}
	
	apn_set = true;
	strcpy(minfo.mdm_apn, apn);
	sprintf(cgdcont_cmd, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
	return 0;
}

const char *gsm_imei()
{
	return minfo.mdm_imei;
}

/*
 * Provide modem info if modem shell is enabled. This can be shown with
 * "modem list" shell command.
 */

/* Handler: <manufacturer> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_networkinfo)
{
	char *substr;
	char temp[100];
	size_t out_len;

	out_len = net_buf_linearize(temp,
				    sizeof(temp) - 1,
				    data->rx_buf, 0, len);
	temp[out_len] = '\0';

	substr = strrchr(temp, ',');	   /* ,"493253" */
	substr += 2; 					   /* 493253" */
	substr[strlen(substr) - 1] = '\0'; /* 493253 */

	strncpy(minfo.mdm_mccmnc, substr, MDM_MCCMNC_LENGTH);

	LOG_INF("MCC-MNC: %s", log_strdup(minfo.mdm_mccmnc));

	if (apn_lookup(&substr, minfo.mdm_mccmnc) == 0) {
		gsm_set_apn(substr);
	}

	return 0;
}

/* Handler: <manufacturer> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_manufacturer)
{
	size_t out_len;

	out_len = net_buf_linearize(minfo.mdm_manufacturer,
				    sizeof(minfo.mdm_manufacturer) - 1,
				    data->rx_buf, 0, len);
	minfo.mdm_manufacturer[out_len] = '\0';
	LOG_INF("Manufacturer: %s", log_strdup(minfo.mdm_manufacturer));

	return 0;
}

/* Handler: <model> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_model)
{
	size_t out_len;

	out_len = net_buf_linearize(minfo.mdm_model,
				    sizeof(minfo.mdm_model) - 1,
				    data->rx_buf, 0, len);
	minfo.mdm_model[out_len] = '\0';
	LOG_INF("Model: %s", log_strdup(minfo.mdm_model));

	return 0;
}

/* Handler: <rev> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_revision)
{
	size_t out_len;

	out_len = net_buf_linearize(minfo.mdm_revision,
				    sizeof(minfo.mdm_revision) - 1,
				    data->rx_buf, 0, len);
	minfo.mdm_revision[out_len] = '\0';
	LOG_INF("Revision: %s", log_strdup(minfo.mdm_revision));

	return 0;
}

/* Handler: <IMEI> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_imei)
{
	size_t out_len;

	out_len = net_buf_linearize(minfo.mdm_imei, sizeof(minfo.mdm_imei) - 1,
				    data->rx_buf, 0, len);
	minfo.mdm_imei[out_len] = '\0';
	LOG_INF("IMEI: %s", log_strdup(minfo.mdm_imei));

	return 0;
}

static const struct setup_cmd setup_cmds[] = {
	/* no echo */
	SETUP_CMD_NOHANDLE("ATE0"),
	/* hang up */
	SETUP_CMD_NOHANDLE("ATH"),
	/* extender errors in numeric form */
	SETUP_CMD_NOHANDLE("AT+CMEE=1"),
	/* Enable outgoing call answer indication */
	SETUP_CMD_NOHANDLE("AT+COLP=1"),
	/* Enable incoming call identification */
	SETUP_CMD_NOHANDLE("AT+CLIP=1"),
	/* Max call audio level */
	SETUP_CMD_NOHANDLE(clvl_cmd),
	/* Set PCM driver to TLV320AIC3104 */
	// SETUP_CMD_NOHANDLE("AT+QDAI=1"),
	/* Enable DTMF detection */
	SETUP_CMD_NOHANDLE("AT+QTONEDET=1"),
	/* Configure URC Indication Option */
	SETUP_CMD_NOHANDLE("AT+QURCCFG=\"urcport\",\"uart1\""),

	/* query modem info */
	SETUP_CMD("AT+QSPN", "", on_cmd_atcmdinfo_networkinfo, 0U, ""),
	SETUP_CMD("AT+CGMI", "", on_cmd_atcmdinfo_manufacturer, 0U, ""),
	SETUP_CMD("AT+CGMM", "", on_cmd_atcmdinfo_model, 0U, ""),
	SETUP_CMD("AT+CGMR", "", on_cmd_atcmdinfo_revision, 0U, ""),
	SETUP_CMD("AT+CGSN", "", on_cmd_atcmdinfo_imei, 0U, ""),

	/* disable unsolicited network registration codes */
	SETUP_CMD_NOHANDLE("AT+CREG=0"),

	/* create PDP context */
	SETUP_CMD_NOHANDLE(cgdcont_cmd),
};

MODEM_CMD_DEFINE(on_cmd_atcmdinfo_attached)
{
	int error = -EAGAIN;

	/* Expected response is "+CGATT: 0|1" so simply look for '1' */
	if (argc && atoi(argv[0]) == 1) {
		error = 0;
		LOG_INF("Attached to packet service!");
	}

	modem_cmd_handler_set_error(data, error);
	k_sem_give(&gsm.sem_response);

	return 0;
}

static const struct modem_cmd check_attached_cmd =
	MODEM_CMD("+CGATT:", on_cmd_atcmdinfo_attached, 1U, ",");

static const struct setup_cmd connect_cmds[] = {
	/* connect to network */
	SETUP_CMD_NOHANDLE("ATD*99#"),
};

static int gsm_setup_mccmno(struct gsm_modem *gsm)
{
	int ret;

	if (CONFIG_MODEM_GSM_MANUAL_MCCMNO[0]) {
		/* use manual MCC/MNO entry */
		ret = modem_cmd_send(&gsm->context.iface,
				     &gsm->context.cmd_handler,
				     NULL, 0,
				     "AT+COPS=1,2,\""
				     CONFIG_MODEM_GSM_MANUAL_MCCMNO
				     "\"",
				     &gsm->sem_response,
				     GSM_CMD_AT_TIMEOUT);
	} else {
		/* register operator automatically */
		ret = modem_cmd_send(&gsm->context.iface,
				     &gsm->context.cmd_handler,
				     NULL, 0, "AT+COPS=0,0",
				     &gsm->sem_response,
				     GSM_CMD_AT_TIMEOUT);
	}

	if (ret < 0) {
		LOG_ERR("AT+COPS ret:%d", ret);
	}

	return ret;
}

static struct net_if *ppp_net_if(void)
{
	return net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
}

static void set_ppp_carrier_on(struct gsm_modem *gsm)
{
	static const struct ppp_api *api;
	const struct device *ppp_dev =
		device_get_binding(CONFIG_NET_PPP_DRV_NAME);
	struct net_if *iface = gsm->iface;
	int ret;

	if (!ppp_dev) {
		LOG_ERR("Cannot find PPP %s!", "device");
		return;
	}

	if (!api) {
		api = (const struct ppp_api *)ppp_dev->api;

		/* For the first call, we want to call ppp_start()... */
		ret = api->start(ppp_dev);
		if (ret) {
			LOG_ERR("ppp start returned %d", ret);
		}
	} else {
		/* ...but subsequent calls should be to ppp_enable() */
		ret = net_if_l2(iface)->enable(iface, true);
		if (ret) {
			LOG_ERR("ppp l2 enable returned %d", ret);
		}
	}
}

static void gsm_finalize_connection(struct gsm_modem *gsm)
{
	int ret;

	if (IS_ENABLED(CONFIG_GSM_MUX) && gsm->mux_enabled) {
		ret = modem_cmd_send(&gsm->context.iface,
				     &gsm->context.cmd_handler,
				     &response_cmds[0],
				     ARRAY_SIZE(response_cmds),
				     "AT", &gsm->sem_response,
				     GSM_CMD_AT_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("modem setup returned %d, %s",
				ret, "retrying...");
			(void)k_delayed_work_submit(&gsm->gsm_configure_work,
						    K_SECONDS(1));
			return;
		}
	}

	(void)gsm_setup_mccmno(gsm);

	ret = modem_cmd_handler_setup_cmds(&gsm->context.iface,
					   &gsm->context.cmd_handler,
					   setup_cmds,
					   ARRAY_SIZE(setup_cmds),
					   &gsm->sem_response,
					   GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_DBG("modem setup returned %d, %s",
			ret, "retrying...");
		(void)k_delayed_work_submit(&gsm->gsm_configure_work,
					    K_SECONDS(1));
		return;
	}

	/* Don't initialize PPP until we're attached to packet service */
	ret = modem_cmd_send_nolock(&gsm->context.iface,
				    &gsm->context.cmd_handler,
				    &check_attached_cmd, 1, "AT+CGATT?",
				    &gsm->sem_response, GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_DBG("Not attached, %s", "retrying...");
		(void)k_delayed_work_submit(&gsm->gsm_configure_work,
					    K_SECONDS(1));
		return;
	}

	LOG_DBG("modem setup returned %d, %s", ret, "enable PPP");
	
	ret = modem_cmd_handler_setup_cmds(&gsm->context.iface,
					   &gsm->context.cmd_handler,
					   connect_cmds,
					   ARRAY_SIZE(connect_cmds),
					   &gsm->sem_response,
					   GSM_CMD_SETUP_TIMEOUT);
	if (ret < 0) {
		LOG_DBG("modem setup returned %d, %s",
			ret, "retrying...");
		(void)k_delayed_work_submit(&gsm->gsm_configure_work,
					    K_SECONDS(1));
		return;
	}

	gsm->setup_done = true;

	

	/* If we are not muxing, the modem interface and gsm_rx() thread is not
	 * needed as PPP will handle the incoming traffic internally.
	 */
	// if (!IS_ENABLED(CONFIG_GSM_MUX)) {
	// 	k_thread_abort(&gsm_rx_thread);
	// }

	set_ppp_carrier_on(gsm);

	if (IS_ENABLED(CONFIG_GSM_MUX) && gsm->mux_enabled) {
		/* Re-use the original iface for AT channel */
		ret = modem_iface_uart_init_dev(&gsm->context.iface,
						gsm->at_dev->name);
		if (ret < 0) {
			LOG_DBG("iface %suart error %d", "AT ", ret);
		} else {
			/* Do a test and try to send AT command to modem */
			ret = modem_cmd_send(&gsm->context.iface,
					     &gsm->context.cmd_handler,
					     &response_cmds[0],
					     ARRAY_SIZE(response_cmds),
					     "AT", &gsm->sem_response,
					     GSM_CMD_AT_TIMEOUT);
			if (ret < 0) {
				LOG_DBG("modem setup returned %d, %s",
					ret, "AT cmds failed");
			} else {
				LOG_INF("AT channel %d connected to %s",
					DLCI_AT, gsm->at_dev->name);
			}
		}
	}
}

static int mux_enable(struct gsm_modem *gsm)
{
	int ret;

	/* Turn on muxing */
	if (IS_ENABLED(CONFIG_MODEM_GSM_SIMCOM)) {
		ret = modem_cmd_send(
			&gsm->context.iface,
			&gsm->context.cmd_handler,
			&response_cmds[0],
			ARRAY_SIZE(response_cmds),
#if defined(SIMCOM_LTE)
			/* FIXME */
			/* Some SIMCOM modems can set the channels */
			/* Control channel always at DLCI 0 */
			"AT+CMUXSRVPORT=0,0;"
			/* PPP should be at DLCI 1 */
			"+CMUXSRVPORT=" STRINGIFY(DLCI_PPP) ",1;"
			/* AT should be at DLCI 2 */
			"+CMUXSRVPORT=" STRINGIFY(DLCI_AT) ",1;"
#else
			"AT"
#endif
			"+CMUX=0,0,5,"
			STRINGIFY(CONFIG_GSM_MUX_MRU_DEFAULT_LEN),
			&gsm->sem_response,
			GSM_CMD_AT_TIMEOUT);
	} else {
		/* Generic GSM modem */
		ret = modem_cmd_send(&gsm->context.iface,
				     &gsm->context.cmd_handler,
				     &response_cmds[0],
				     ARRAY_SIZE(response_cmds),
				     "AT+CMUX=0", &gsm->sem_response,
				     GSM_CMD_AT_TIMEOUT);
	}

	if (ret < 0) {
		LOG_ERR("AT+CMUX ret:%d", ret);
	}

	return ret;
}

static void mux_setup_next(struct gsm_modem *gsm)
{
	(void)k_delayed_work_submit(&gsm->gsm_configure_work, K_MSEC(1));
}

static void mux_attach_cb(const struct device *mux, int dlci_address,
			  bool connected, void *user_data)
{
	LOG_DBG("DLCI %d to %s %s", dlci_address, mux->name,
		connected ? "connected" : "disconnected");

	if (connected) {
		uart_irq_rx_enable(mux);
		uart_irq_tx_enable(mux);
	}

	mux_setup_next(user_data);
}

static int mux_attach(const struct device *mux, const struct device *uart,
		      int dlci_address, void *user_data)
{
	int ret = uart_mux_attach(mux, uart, dlci_address, mux_attach_cb,
				  user_data);
	if (ret < 0) {
		LOG_ERR("Cannot attach DLCI %d (%s) to %s (%d)", dlci_address,
			mux->name, uart->name, ret);
		return ret;
	}

	return 0;
}

static void mux_setup(struct k_work *work)
{
	struct gsm_modem *gsm = CONTAINER_OF(work, struct gsm_modem,
					     gsm_configure_work);
	const struct device *uart = device_get_binding(CONFIG_MODEM_GSM_UART_NAME);
	int ret;

	switch (gsm->state) {
	case STATE_CONTROL_CHANNEL:
		/* Get UART device. There is one dev / DLCI */
		gsm->control_dev = uart_mux_alloc();
		if (gsm->control_dev == NULL) {
			LOG_DBG("Cannot get UART mux for %s channel",
				"control");
			goto fail;
		}

		gsm->state = STATE_PPP_CHANNEL;

		ret = mux_attach(gsm->control_dev, uart, DLCI_CONTROL, gsm);
		if (ret < 0) {
			goto fail;
		}

		break;

	case STATE_PPP_CHANNEL:
		gsm->ppp_dev = uart_mux_alloc();
		if (gsm->ppp_dev == NULL) {
			LOG_DBG("Cannot get UART mux for %s channel", "PPP");
			goto fail;
		}

		gsm->state = STATE_AT_CHANNEL;

		ret = mux_attach(gsm->ppp_dev, uart, DLCI_PPP, gsm);
		if (ret < 0) {
			goto fail;
		}

		break;

	case STATE_AT_CHANNEL:
		gsm->at_dev = uart_mux_alloc();
		if (gsm->at_dev == NULL) {
			LOG_DBG("Cannot get UART mux for %s channel", "AT");
			goto fail;
		}

		gsm->state = STATE_DONE;

		ret = mux_attach(gsm->at_dev, uart, DLCI_AT, gsm);
		if (ret < 0) {
			goto fail;
		}

		break;

	case STATE_DONE:
		/* At least the SIMCOM modem expects that the Internet
		 * connection is created in PPP channel. We will need
		 * to attach the AT channel to context iface after the
		 * PPP connection is established in order to give AT commands
		 * to the modem.
		 */
		ret = modem_iface_uart_init_dev(&gsm->context.iface,
						gsm->ppp_dev->name);
		if (ret < 0) {
			LOG_DBG("iface %suart error %d", "PPP ", ret);
			gsm->mux_enabled = false;
			goto fail;
		}

		LOG_INF("PPP channel %d connected to %s",
			DLCI_PPP, gsm->ppp_dev->name);

		gsm_finalize_connection(gsm);
		break;
	}

	return;

fail:
	gsm->state = STATE_INIT;
	gsm->mux_enabled = false;
}

static void gsm_configure(struct k_work *work)
{
	struct gsm_modem *gsm = CONTAINER_OF(work, struct gsm_modem,
					     gsm_configure_work);
	int ret = -1;

	LOG_DBG("Starting modem %p configuration", gsm);

	ret = modem_cmd_send(&gsm->context.iface,
			     &gsm->context.cmd_handler,
			     &response_cmds[0],
			     ARRAY_SIZE(response_cmds),
			     "AT", &gsm->sem_response,
			     GSM_CMD_AT_TIMEOUT);
	if (ret < 0) {
		LOG_DBG("modem not ready %d", ret);

		(void)k_delayed_work_submit(&gsm->gsm_configure_work,
					    K_NO_WAIT);

		return;
	}

	if (IS_ENABLED(CONFIG_GSM_MUX) && ret == 0 &&
	    gsm->mux_enabled == false) {
		gsm->mux_setup_done = false;

		ret = mux_enable(gsm);
		if (ret == 0) {
			gsm->mux_enabled = true;
		} else {
			gsm->mux_enabled = false;
			(void)k_delayed_work_submit(&gsm->gsm_configure_work,
						    K_NO_WAIT);
			return;
		}

		LOG_DBG("GSM muxing %s", gsm->mux_enabled ? "enabled" :
							    "disabled");

		if (gsm->mux_enabled) {
			gsm->state = STATE_INIT;

			k_delayed_work_init(&gsm->gsm_configure_work,
					    mux_setup);

			(void)k_delayed_work_submit(&gsm->gsm_configure_work,
						    K_NO_WAIT);
			return;
		}
	}

	gsm_finalize_connection(gsm);
}

void gsm_ppp_start(const struct device *device)
{
	struct gsm_modem *gsm = device->data;

	/* Re-init underlying UART comms */
	int r = modem_iface_uart_init_dev(&gsm->context.iface,
					  CONFIG_MODEM_GSM_UART_NAME);
	if (r) {
		LOG_ERR("modem_iface_uart_init returned %d", r);
		return;
	}

	k_delayed_work_init(&gsm->gsm_configure_work, gsm_configure);
	(void)k_delayed_work_submit(&gsm->gsm_configure_work, K_NO_WAIT);
}


int gsm_ppp_resume(const struct device *device)
{
	struct gsm_modem *gsm = device->data;
	int rc;

	rc = modem_cmd_send(&gsm->context.iface, &gsm->context.cmd_handler,
		       &response_cmds[2], 1, "ATO", &gsm->sem_response,
		       K_SECONDS(2));

	if (rc < 0) {
		rc = modem_cmd_send(&gsm->context.iface, &gsm->context.cmd_handler,
				&response_cmds[2], 1, "ATD*99#", &gsm->sem_response,
				K_SECONDS(2));
		if (rc < 0) { 
			return rc;
		}
	}

	set_ppp_carrier_on(gsm);
	return 0;
}

int gsm_ppp_stop(const struct device *device)
{
	struct gsm_modem *gsm = device->data;
	struct net_if *iface = gsm->iface;
	int rc;

	net_if_l2(iface)->enable(iface, false);

	/* Re-init underlying UART comms */
	rc = modem_iface_uart_init_dev(&gsm->context.iface,
					  CONFIG_MODEM_GSM_UART_NAME);
	if (rc < 0) {
		LOG_ERR("modem_iface_uart_init returned %d", rc);
		return rc;
	}
	
	k_msleep(1200);
	gsm->cmd_handler_data.eol = "";
	gsm->cmd_handler_data.eol_len = 0;
	rc = modem_cmd_send(&gsm->context.iface, &gsm->context.cmd_handler,
			     &response_cmds[2], 1, "+++", &gsm->sem_response,
			     K_SECONDS(2));
	gsm->cmd_handler_data.eol = "\r";
	gsm->cmd_handler_data.eol_len = 1;

	return rc;
}

void gsm_ppp_restart(const struct device *device)
{
	const struct ppp_api *api;
	const struct device *ppp_dev =
		device_get_binding(CONFIG_NET_PPP_DRV_NAME);
	int rc;

	gsm_ppp_stop(device);

	if (!ppp_dev) {
		LOG_ERR("Cannot find PPP %s!", "device");
		return;
	}

	api = (const struct ppp_api *)ppp_dev->api;
	if (!api) {
		LOG_ERR("Cannot find ppp_api!");
		return;
	}

	rc = api->stop(ppp_dev);
	if (rc) {
		LOG_ERR("ppp stop returned %d", rc);
	}

	gsm_ppp_start(device);
}

static int gsm_init(const struct device *device)
{
	struct gsm_modem *gsm = device->data;
	int r;

	LOG_DBG("Generic GSM modem (%p)", gsm);

	gsm->cmd_handler_data.cmds[CMD_RESP] = response_cmds;
	gsm->cmd_handler_data.cmds_len[CMD_RESP] = ARRAY_SIZE(response_cmds);
	gsm->cmd_handler_data.read_buf = &gsm->cmd_read_buf[0];
	gsm->cmd_handler_data.read_buf_len = sizeof(gsm->cmd_read_buf);
	gsm->cmd_handler_data.match_buf = &gsm->cmd_match_buf[0];
	gsm->cmd_handler_data.match_buf_len = sizeof(gsm->cmd_match_buf);
	gsm->cmd_handler_data.buf_pool = &gsm_recv_pool;
	gsm->cmd_handler_data.alloc_timeout = GSM_BUF_ALLOC_TIMEOUT;
	gsm->cmd_handler_data.eol = "\r";

	k_sem_init(&gsm->sem_response, 0, 1);

	r = modem_cmd_handler_init(&gsm->context.cmd_handler,
				   &gsm->cmd_handler_data);
	if (r < 0) {
		LOG_DBG("cmd handler error %d", r);
		return r;
	}

	/* modem information storage */
	gsm->context.data_manufacturer = minfo.mdm_manufacturer;
	gsm->context.data_model = minfo.mdm_model;
	gsm->context.data_revision = minfo.mdm_revision;
	gsm->context.data_imei = minfo.mdm_imei;

	gsm->gsm_data.isr_buf = &gsm->gsm_isr_buf[0];
	gsm->gsm_data.isr_buf_len = sizeof(gsm->gsm_isr_buf);
	gsm->gsm_data.rx_rb_buf = &gsm->gsm_rx_rb_buf[0];
	gsm->gsm_data.rx_rb_buf_len = sizeof(gsm->gsm_rx_rb_buf);

	r = modem_iface_uart_init(&gsm->context.iface, &gsm->gsm_data,
				  CONFIG_MODEM_GSM_UART_NAME);
	if (r < 0) {
		LOG_DBG("iface uart error %d", r);
		return r;
	}

	r = modem_context_register(&gsm->context);
	if (r < 0) {
		LOG_DBG("context error %d", r);
		return r;
	}

	LOG_DBG("iface->read %p iface->write %p",
		gsm->context.iface.read, gsm->context.iface.write);

	k_thread_create(&gsm_rx_thread, gsm_rx_stack,
			K_THREAD_STACK_SIZEOF(gsm_rx_stack),
			(k_thread_entry_t) gsm_rx,
			gsm, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&gsm_rx_thread, "gsm_rx");

	gsm->iface = ppp_net_if();
	if (!gsm->iface) {
		LOG_ERR("Couldn't find ppp net_if!");
		return -ENODEV;
	}

	return 0;
}

DEVICE_INIT(gsm_ppp, GSM_MODEM_DEVICE_NAME, gsm_init, &gsm, NULL, POST_KERNEL,
	    CONFIG_MODEM_GSM_INIT_PRIORITY);
