/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/watchdog.h>

K_SEM_DEFINE(lte_connected_sem, 0, 1);

#define WDT_MAX_WINDOW 5000  // Timeout in milliseconds

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		printk("Network registration status: %s\n",
		       evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
		       "Connected - home" : "Connected - roaming");
		k_sem_give(&lte_connected_sem);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		printk("PSM parameter update: TAU: %d s, Active time: %d s\n",
		       evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE:
		printk("eDRX parameter update: eDRX: %.2f s, PTW: %.2f s\n",
		       (double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		printk("RRC mode: %s\n",
		       evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle\n");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		printk("LTE cell changed: Cell ID: %d, Tracking area: %d\n",
		       evt->cell.id, evt->cell.tac);
		break;
	case LTE_LC_EVT_RAI_UPDATE:
		/* RAI notification is supported by modem firmware releases >= 2.0.2 */
		printk("RAI configuration update: "
		       "Cell ID: %d, MCC: %d, MNC: %d, AS-RAI: %d, CP-RAI: %d\n",
		       evt->rai_cfg.cell_id,
		       evt->rai_cfg.mcc,
		       evt->rai_cfg.mnc,
		       evt->rai_cfg.as_rai,
		       evt->rai_cfg.cp_rai);
		break;
	default:
		break;
	}
}

const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

    struct wdt_timeout_cfg wdt_config = {
        .window.min = 0,
        .window.max = WDT_MAX_WINDOW,
        .callback = NULL, // Set to a function if you want a callback before reset
        .flags = WDT_FLAG_RESET_SOC,
    };

#define STACK_SIZE 512
#define THREAD_PRIORITY 7

K_THREAD_STACK_DEFINE(watchdog_thread_stack, STACK_SIZE);
struct k_thread watchdog_thread_data;
int wdt_channel_id;


void watchdog_thread_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        wdt_feed(wdt, wdt_channel_id);
		printk("Watchdog fed\n");
        k_sleep(K_MSEC(WDT_MAX_WINDOW/2));
    }
}

int main(void)
{
	int err;

	printk("UDP sample has started\n");

	err = nrf_modem_lib_init();
	if (err) {
		printk("Failed to initialize modem library, error: %d\n", err);
		return -1;
	}

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		printk("Failed to connect to LTE network, error: %d\n", err);
		return -1;
	}

	k_sem_take(&lte_connected_sem, K_FOREVER);


	printk("Starting Watchdog\n");


    wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
    if (wdt_channel_id < 0) {
        // Handle error
    }

    if (wdt_setup(wdt, 0) < 0) {
        // Handle error
    }

	
	k_tid_t watchdog_thread_id = k_thread_create(&watchdog_thread_data, watchdog_thread_stack, STACK_SIZE,
												watchdog_thread_fn, NULL, NULL, NULL,
												THREAD_PRIORITY, 0, K_NO_WAIT);

	printk("Watchdog thread started with ID: %p\n", watchdog_thread_id);

	printk("LTE Power Off request\n");
	err = lte_lc_power_off();
	printk("Power off returned: %d\n", err);
	if (err) {
		printk("Failed to power off LTE, error: %d\n", err);
		return -1;
	}
	printk("Init Busy wait 100ms\n");
	k_busy_wait(100000);
	printk("Busy wait done\n");

	printk("Sleep 1000ms\n");
	k_sleep(K_MSEC(1000));
	printk("Sleep done\n");

	// printk("Rebooting System...\n");
	// sys_reboot(SYS_REBOOT_COLD);

	return 0;
}
