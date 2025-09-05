/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/location.h>
#include <modem/nrf_modem_lib.h>
#include <date_time.h>

static K_SEM_DEFINE(location_event, 0, 1);

static K_SEM_DEFINE(lte_connected, 0, 1);

static K_SEM_DEFINE(time_update_finished, 0, 1);

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_update_finished);
}

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
            (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
            printk("Connected to LTE\n");
            k_sem_give(&lte_connected);
        }
        break;
    case LTE_LC_EVT_NEIGHBOR_CELL_MEAS: {
        const struct lte_lc_cells_info *cells = &evt->cells_info;
        printk("Neighbor cell measurement results received:\n");
        printk("Current cell: id=%d tac=%d mcc=%d mnc=%d earfcn=%d rsrp=%d rsrq=%d\n",
            cells->current_cell.id, cells->current_cell.tac, cells->current_cell.mcc,
            cells->current_cell.mnc, cells->current_cell.earfcn, cells->current_cell.rsrp,
            cells->current_cell.rsrq);
        for (size_t i = 0; i < cells->ncells_count; i++) {
            const struct lte_lc_ncell *ncell = &cells->neighbor_cells[i];
            printk("Neighbor cell %d: phys_cell_id=%d earfcn=%d rsrp=%d rsrq=%d\n",
                (int)i, ncell->phys_cell_id, ncell->earfcn, ncell->rsrp, ncell->rsrq);
        }
        break;
    }
    default:
        break;
    }
}


int main(void)
{
	int err;

	printk("Location sample started\n\n");

	err = nrf_modem_lib_init();
	if (err) {
		printk("Modem library initialization failed, error: %d\n", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		date_time_register_handler(date_time_evt_handler);
	}

	printk("Connecting to LTE...\n");

	lte_lc_register_handler(lte_event_handler);
	lte_lc_psm_req(true);
	lte_lc_connect();
	k_sem_take(&lte_connected, K_FOREVER);
	printk("LTE connected\n");

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		printk("Waiting for current time\n");
		k_sem_take(&time_update_finished, K_MINUTES(10));
		if (!date_time_is_valid()) {
			printk("Failed to get current time. Continuing anyway.\n");
		}
		printk("Current time is set.\n");
	}
	

	/* Trigger neighbor cell measurement after LTE is connected */

	printk("Requesting neighbor cell measurement with default parameters (NULL)...\n");
	err = lte_lc_neighbor_cell_measurement(NULL);
	if (err) {
		printk("Failed to request neighbor cell measurement, error: %d\n", err);
	}
	k_sleep(K_SECONDS(10));

	printk("Requesting neighbor cell measurement with extended parameters...\n");
	struct lte_lc_ncellmeas_params ncell_params = {
		.search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_EXTENDED_COMPLETE,
		.gci_count = 15
	};
	err = lte_lc_neighbor_cell_measurement(&ncell_params);
	if (err) {
		printk("Failed to request neighbor cell measurement, error: %d\n", err);
	}

	
}
