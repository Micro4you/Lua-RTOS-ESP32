/*
 * Lua RTOS, Lora WAN driver
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.  
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LORA_DEVICE_TYPE_NODE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_attr.h"
#include "esp_deep_sleep.h"

#include "lora.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/syslog.h>
#include <sys/mutex.h>
#include <sys/resource.h>
#include <sys/driver.h>
#include <sys/status.h>

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
#include "lmic.h"
#endif

#include "common.h"

// Driver message errors
DRIVER_REGISTER_ERROR(LORA, lora, KeysNotConfigured, "keys are not configured", LORA_ERR_KEYS_NOT_CONFIGURED);
DRIVER_REGISTER_ERROR(LORA, lora, JoinDenied, "join denied", LORA_ERR_JOIN_DENIED);
DRIVER_REGISTER_ERROR(LORA, lora, UnexpectedResponse, "unexpected response", LORA_ERR_UNEXPECTED_RESPONSE);
DRIVER_REGISTER_ERROR(LORA, lora, NotJoined, "not joined", LORA_ERR_NOT_JOINED);
DRIVER_REGISTER_ERROR(LORA, lora, NotSetup, "lora is not setup, setup first", LORA_ERR_NOT_SETUP);
DRIVER_REGISTER_ERROR(LORA, lora, NotEnoughtMemory, "not enough memory", LORA_ERR_NO_MEM);
DRIVER_REGISTER_ERROR(LORA, lora, ABPExpected, "ABP expected", LORA_ERR_ABP_EXPECTED);
DRIVER_REGISTER_ERROR(LORA, lora, CannotSetup, "can't setup", LORA_ERR_CANT_SETUP);
DRIVER_REGISTER_ERROR(LORA, lora, TransmissionFail, "transmission fail, ack not received", LORA_ERR_TRANSMISSION_FAIL_ACK_NOT_RECEIVED);
DRIVER_REGISTER_ERROR(LORA, lora, InvalidArgument, "invalid argument", LORA_ERR_INVALID_ARGUMENT);
DRIVER_REGISTER_ERROR(LORA, lora, InvalidDataRate, "invalid data rate for your location", LORA_ERR_INVALID_DR);
DRIVER_REGISTER_ERROR(LORA, lora, InvalidBand, "invalid band for your location", LORA_ERR_INVALID_BAND);

#define evLORA_INITED 	       	 ( 1 << 0 )
#define evLORA_JOINED  	       	 ( 1 << 1 )
#define evLORA_JOIN_DENIED     	 ( 1 << 2 )
#define evLORA_TX_COMPLETE    	 ( 1 << 3 )
#define evLORA_ACK_NOT_RECEIVED  ( 1 << 4 )

extern uint8_t flash_unique_id[8];

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
// LMIC job for start LMIC stack
static osjob_t initjob;
#endif

// Mutext for lora 
static struct mtx lora_mtx;

// Event group handler for sync LoRa events with driver functions
static EventGroupHandle_t loraEvent;

// Data needed for OTAA
static u1_t APPEUI[8] = {0,0,0,0,0,0,0,0};
static u1_t DEVEUI[8] = {0,0,0,0,0,0,0,0};
static u1_t APPKEY[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

static u1_t joined = 0;

// Data needed for ABP
static u4_t DEVADDR = 0x00000000;
static u1_t NWKSKEY[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static u1_t APPSKEY[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

static u1_t session_init = 0;

// Current message id. We put this in RTC memory for survive a deep sleep.
// ABP needs to keep msgid in sequence between tranfers.
RTC_DATA_ATTR static u4_t msgid = 0;

// If = 1 driver is setup, if = 0 is not setup
static u1_t setup = 0;

// Callback function to call when data is received
static lora_rx *lora_rx_callback = NULL;

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
// Table for translate numeric datarates to LMIC definitions
#if CONFIG_LUA_RTOS_LORA_NODE_BAND_EU868
static const u1_t data_rates[] = {
	DR_SF12, DR_SF11, DR_SF10, DR_SF9, DR_SF8, DR_SF7, DR_SF7B, DR_FSK, DR_NONE,
	DR_NONE, DR_NONE, DR_NONE, DR_NONE, DR_NONE, DR_NONE, DR_NONE
};
#endif

#if CONFIG_LUA_RTOS_LORA_NODE_BAND_US915
static const u1_t data_rates[] = {
	DR_SF10, DR_SF9, DR_SF8, DR_SF7, DR_SF8C, DR_NONE, DR_NONE, DR_NONE,
	DR_SF12CR, DR_SF11CR, DR_SF10CR, DR_SF9CR, DR_SF8CR, DR_SF7CR, DR_NONE,
	DR_NONE
};
#endif
#endif

// Current datarate set by user
static u1_t current_dr = 0;

// ADR active?
static u1_t adr = 0;

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
// LMIC event handler
void onEvent (ev_t ev) {
    switch(ev) {
	    case EV_SCAN_TIMEOUT:
	      break;

	    case EV_BEACON_FOUND:
	      break;

	    case EV_BEACON_MISSED:
	      break;

	    case EV_BEACON_TRACKED:
	      break;

	    case EV_JOINING:
          joined = 0;
	      break;

	    case EV_JOINED:
          joined = 1;
		  xEventGroupSetBits(loraEvent, evLORA_JOINED);

		  /* TTN uses SF9 for its RX2 window. */
		  LMIC.dn2Dr = DR_SF9;
	      break;

	    case EV_RFU1:
	      break;

	    case EV_JOIN_FAILED:
          joined = 0;
		  xEventGroupSetBits(loraEvent, evLORA_JOIN_DENIED);
	      break;

	    case EV_REJOIN_FAILED:
          joined = 0;
	      break;

	    case EV_TXCOMPLETE:
		  if (LMIC.pendTxConf) {
			  if (LMIC.txrxFlags & TXRX_ACK) {
		  		  xEventGroupSetBits(loraEvent, evLORA_TX_COMPLETE);
			  }

			  if (LMIC.txrxFlags & TXRX_NACK) {
		  		  xEventGroupSetBits(loraEvent, evLORA_ACK_NOT_RECEIVED);
			  }
		  } else {
		      if (LMIC.dataLen && lora_rx_callback) {
				  // Make a copy of the payload and call callback function
				  u1_t *payload = (u1_t *)malloc(LMIC.dataLen * 2 + 1);
				  if (payload) {
					  // Coding payload into an hex string
					  val_to_hex_string((char *)payload, (char *)&LMIC.frame[LMIC.dataBeg], LMIC.dataLen, 0);
					  payload[LMIC.dataLen * 2] = 0x00;

					  lora_rx_callback(1, (char *)payload);
				  }
		      }

		      xEventGroupSetBits(loraEvent, evLORA_TX_COMPLETE);
		  }

	      break;

	    case EV_LOST_TSYNC:
	      break;

	    case EV_RESET:
	      break;

	    case EV_RXCOMPLETE:
	      break;

	    case EV_LINK_DEAD:
	      break;

	    case EV_LINK_ALIVE:
	      break;

	    default:
	      break;
  	}
}

// LMIC first job
static void lora_init(osjob_t *j) {
    // Reset MAC state
    LMIC_reset();

	#if CONFIG_LUA_RTOS_LORA_NODE_BAND_EU868
    	LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);
	    LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
	    LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);	
	#endif

	#if CONFIG_LUA_RTOS_LORA_NODE_BAND_US915
	    LMIC_selectSubBand(1);
	#endif

	/* Disable link check validation */
    LMIC_setLinkCheckMode(0);

    /* adr disabled */
    adr = 0;
    LMIC_setAdrMode(0);

	/* TTN uses SF9 for its RX2 window. */
	LMIC.dn2Dr = DR_SF9;

	/* Set data rate and transmit power for uplink (note: txpow seems to be ignored by the library) */
	current_dr = DR_SF7;
	LMIC_setDrTxpow(current_dr, 14);

    xEventGroupSetBits(loraEvent, evLORA_INITED);
}
#endif

#define lora_must_join() \
    ( \
		(DEVADDR == 0) && \
		(memcmp(APPSKEY, (u1_t[]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 16) == 0) && \
		(memcmp(APPSKEY, (u1_t[]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 16) == 0) \
	)

#define lora_can_participate_otaa() \
	( \
		(memcmp(APPEUI,  (u1_t[]){0,0,0,0,0,0,0,0}, 8) != 0) && \
		(memcmp(DEVEUI, (u1_t[]){0,0,0,0,0,0,0,0}, 8) != 0) && \
		(memcmp(APPKEY, (u1_t[]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 16) != 0) \
	)

#define lora_can_participate_abp() \
	( \
		(DEVADDR != 0) && \
		(memcmp(APPSKEY, (u1_t[]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 16) != 0) && \
		(memcmp(APPSKEY, (u1_t[]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 16) != 0) \
	)

// Setup driver
driver_error_t *lora_setup(int band) {
	driver_error_t *error;

	#if CONFIG_LUA_RTOS_LORA_NODE_BAND_EU868
	if (band != 868) {
		return driver_operation_error(LORA_DRIVER, LORA_ERR_INVALID_BAND, NULL);
	}
	#endif

	#if CONFIG_LUA_RTOS_LORA_NODE_BAND_US915
	if (band != 915) {
		return driver_operation_error(LORA_DRIVER, LORA_ERR_INVALID_BAND, NULL);
	}
	#endif

    mtx_lock(&lora_mtx);

    if (!setup) {
        syslog(LOG_DEBUG, "lora: setup, band %d", band);
		
        // Create event group for sync driver with LoRa events
        if (!loraEvent) {
        	loraEvent = xEventGroupCreate();
        }

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
		// LMIC init
		if (!(error = os_init())) {
			// Set first callback, for init lora stack
			os_setCallback(&initjob, lora_init);
		} else {
			setup = 0;
			mtx_unlock(&lora_mtx);

			return driver_operation_error(LORA_DRIVER, LORA_ERR_CANT_SETUP, NULL);
		}

		// Wait for stack initialization
	    xEventGroupWaitBits(loraEvent, evLORA_INITED, pdTRUE, pdFALSE, portMAX_DELAY);
#endif

#if CONFIG_LUA_RTOS_LORA_NODE_SEMTECH_STACK
		if ((error = _lora_setup())) {
			return error;
		}
#endif
    }

	setup = 1;

    mtx_unlock(&lora_mtx);
    
    return NULL;
}

driver_error_t *lora_mac_set(const char command, const char *value) {
	driver_error_t *error;

    mtx_lock(&lora_mtx);

    if (!setup) {
        mtx_unlock(&lora_mtx);
		return driver_operation_error(LORA_DRIVER, LORA_ERR_NOT_SETUP, NULL);
    }

	if ((error = _lora_mac_set(command, value))) {
	    mtx_unlock(&lora_mtx);
		return error;
	}

    mtx_unlock(&lora_mtx);

	return NULL;
}

driver_error_t *lora_mac_get(const char command, char **value) {
	driver_error_t *error;

	mtx_lock(&lora_mtx);

	if ((error = _lora_mac_get(command, value))) {
	    mtx_unlock(&lora_mtx);
		return error;
	}

    mtx_unlock(&lora_mtx);

	return NULL;
}

driver_error_t *lora_join() {
    mtx_lock(&lora_mtx);

    // Sanity checks
    if (!setup) {
    	mtx_unlock(&lora_mtx);
		return driver_operation_error(LORA_DRIVER, LORA_ERR_NOT_SETUP, NULL);
    }

    if (!lora_must_join()) {
        mtx_unlock(&lora_mtx);
		return driver_operation_error(LORA_DRIVER, LORA_ERR_ABP_EXPECTED, NULL);
    }

    if (!lora_can_participate_otaa()) {
        mtx_unlock(&lora_mtx);
		return driver_operation_error(LORA_DRIVER, LORA_ERR_KEYS_NOT_CONFIGURED, NULL);
    }

    // Join, if needed
    if (joined) {
        mtx_unlock(&lora_mtx);
    	return NULL;
    }

    // If we use join, set msgid to 0
    msgid = 0;

    // Set DR
    if (!adr) {
#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
        LMIC_setDrTxpow(current_dr, 14);
#endif
    }

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
	hal_lmic_join();
#endif

	// Wait for one of the expected events
    EventBits_t uxBits = xEventGroupWaitBits(loraEvent, evLORA_JOINED | evLORA_JOIN_DENIED, pdTRUE, pdFALSE, portMAX_DELAY);
    if (uxBits & (evLORA_JOINED)) {
	    mtx_unlock(&lora_mtx);   
		return NULL;
    }

    if (uxBits & (evLORA_JOIN_DENIED)) {
	    mtx_unlock(&lora_mtx);   
		return driver_operation_error(LORA_DRIVER, LORA_ERR_NOT_JOINED, NULL);
    }
	
	mtx_unlock(&lora_mtx);

	return driver_operation_error(LORA_DRIVER, LORA_ERR_UNEXPECTED_RESPONSE, NULL);
}

driver_error_t *lora_tx(int cnf, int port, const char *data) {
	driver_error_t *error;
	uint8_t *payload;
	uint8_t payload_len;
	
    mtx_lock(&lora_mtx);

    if (!setup) {
        mtx_unlock(&lora_mtx);
        return driver_operation_error(LORA_DRIVER, LORA_ERR_NOT_SETUP, NULL);
    }

#if 0
    if (lora_must_join()) {
    	if (lora_can_participate_otaa()) {
            if (!joined) {
                mtx_unlock(&lora_mtx);
                return driver_operation_error(LORA_DRIVER, LORA_ERR_NOT_JOINED, NULL);
            }
    	} else {
            mtx_unlock(&lora_mtx);
            return driver_operation_error(LORA_DRIVER, LORA_ERR_KEYS_NOT_CONFIGURED, NULL);
    	}
    } else {
    	if (!lora_can_participate_abp()) {
            mtx_unlock(&lora_mtx);
            return driver_operation_error(LORA_DRIVER, LORA_ERR_KEYS_NOT_CONFIGURED, NULL);
    	} else {
    		if (!session_init) {
#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
    			LMIC_setSession (0x1, DEVADDR, NWKSKEY, APPSKEY);

    		    /* TTN uses SF9 for its RX2 window. */
    		    LMIC.dn2Dr = DR_SF9;
#endif
    		    session_init = 1;

    		}
    	}
    }
#endif

	payload_len = strlen(data) / 2;

	// Allocate buffer por payload	
	payload = (uint8_t *)malloc(payload_len + 1);
	if (!payload) {
		mtx_unlock(&lora_mtx);
		return driver_operation_error(LORA_DRIVER, LORA_ERR_NO_MEM, NULL);
	}
	
	// Convert input payload (coded in hex string) into a byte buffer
	hex_string_to_val((char *)data, (char *)payload, payload_len, 0);

	if ((error = _lora_tx(cnf, port, payload, payload_len))) {
		mtx_unlock(&lora_mtx);
		return error;
	}

	// Put message id
	msgid++;

	// Wait for one of the expected events
    EventBits_t uxBits = xEventGroupWaitBits(loraEvent, evLORA_TX_COMPLETE | evLORA_ACK_NOT_RECEIVED, pdTRUE, pdFALSE, portMAX_DELAY);
    if (uxBits & (evLORA_TX_COMPLETE)) {
	    mtx_unlock(&lora_mtx);   
		return NULL;
    }

    if (uxBits & (evLORA_ACK_NOT_RECEIVED)) {
        mtx_unlock(&lora_mtx);
        return driver_operation_error(LORA_DRIVER, LORA_ERR_TRANSMISSION_FAIL_ACK_NOT_RECEIVED, NULL);
    }
	
	mtx_unlock(&lora_mtx);

    return driver_operation_error(LORA_DRIVER, LORA_ERR_UNEXPECTED_RESPONSE, NULL);
}

void lora_set_rx_callback(lora_rx *callback) {
    mtx_lock(&lora_mtx);
	
    lora_rx_callback = callback;
    
	mtx_unlock(&lora_mtx);
}

#if CONFIG_LUA_RTOS_LORA_NODE_LMIC_STACK
// This functions are needed for the LMIC stack for pass
// connection data
void os_getArtEui (u1_t* buf) { 
	memcpy(buf, APPEUI, 8);
}

void os_getDevEui (u1_t* buf) {
	memcpy(buf, DEVEUI, 8);
}

void os_getDevKey (u1_t* buf) { 
	memcpy(buf, APPKEY, 16);
}
#endif

void _lora_init() {
    // Create lora mutex
    mtx_init(&lora_mtx, NULL, NULL, 0);

    status_set(STATUS_NEED_RTC_SLOW_MEM);

    // Get device EUI from flash id
	#if CONFIG_LUA_RTOS_READ_FLASH_UNIQUE_ID
    	int i = 0;

    	for(i=0;i<8;i++) {
    		DEVEUI[i] = flash_unique_id[7-i];
    	}
	#endif
}

DRIVER_REGISTER(LORA,lora, NULL,_lora_init,NULL);

#endif