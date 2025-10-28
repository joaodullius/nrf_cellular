/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <modem/modem_info.h>

#define UDP_IP_HEADER_SIZE 28

static struct sockaddr_storage host_addr;
static struct k_work_delayable socket_transmission_work;
static int data_upload_iterations = CONFIG_UDP_DATA_UPLOAD_ITERATIONS;

K_SEM_DEFINE(lte_connected_sem, 0, 1);
K_SEM_DEFINE(modem_shutdown_sem, 0, 1);

LOG_MODULE_REGISTER(udp_sample, LOG_LEVEL_INF);


/* Estados possíveis */
enum connection_state {
    STATE_DISCONNECTED,
    STATE_CONNECTED,
    STATE_SOCKET_CREATED
};

/* Variáveis de estado */
static enum connection_state current_state = STATE_DISCONNECTED;
static struct sockaddr_storage host_addr;
static int socket_fd = -1;

/* Setar estado de conectado */
void set_connected_state(void)
{
    current_state = STATE_CONNECTED;
    printk("Estado: CONNECTED\n");
}

/* Criar socket, conectar e setar estado */
int create_socket_and_set_state(void)
{
	int err;
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	/* Configurar endereço do servidor */
	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_UDP_SERVER_PORT);
	inet_pton(AF_INET, CONFIG_UDP_SERVER_ADDRESS_STATIC, &server4->sin_addr);

	/* Criar socket UDP */
	socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (socket_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);
		current_state = STATE_DISCONNECTED;
		return -errno;
	}

	current_state = STATE_SOCKET_CREATED;
	LOG_INF("Socket created (fd: %d)", socket_fd);

	/* Conectar ao servidor */
	err = connect(socket_fd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		close(socket_fd);
		socket_fd = -1;
		current_state = STATE_DISCONNECTED;
		return err;
	}

	current_state = STATE_CONNECTED;
	LOG_INF("Socket connected successfully");

	return 0;
}

/* Verificar estado atual */
bool is_connected(void)
{
    return (current_state == STATE_CONNECTED || 
            current_state == STATE_SOCKET_CREATED);
}

bool is_socket_created(void)
{
    return (current_state == STATE_SOCKET_CREATED);
}

/* Limpar e voltar ao estado desconectado */
void disconnect_and_cleanup(void)
{
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    current_state = STATE_DISCONNECTED;
    printk("Estado: DISCONNECTED\n");
}

static void lte_handler(const struct lte_lc_evt *const evt);

static void socket_transmission_work_fn(struct k_work *work)
{
	int err;
	char buffer[CONFIG_UDP_DATA_UPLOAD_SIZE_BYTES] = {"\0"};

	if (current_state == STATE_DISCONNECTED) {
		LOG_INF("Connecting to LTE network...");
	 	/* Connect to LTE network */
		err = lte_lc_connect_async(lte_handler);
		if (err) {
			LOG_ERR("Failed to connect to LTE network, error: %d", err);
			return;
		}

		k_sem_take(&lte_connected_sem, K_FOREVER);
		set_connected_state();

		LOG_WRN("LTE link is up");
		int rsrp, snr;
		modem_info_get_rsrp(&rsrp);
		modem_info_get_snr(&snr);
		LOG_INF("Current rsrp: %ddBm", rsrp);
		LOG_INF("Current snr: %ddB", snr);
	}

	//Checa se Socket já está criado
	if (!is_socket_created()) {
		LOG_WRN("Socket not created. Creating socket...");
 	 	/* Criar socket */
		if (create_socket_and_set_state() == 0) {
			/* Socket criado com sucesso */
			LOG_INF("Socket created successfully.");
		} else {
			LOG_ERR("Failed to create socket.");
			return;
		}
	}


	LOG_INF("Transmitting UDP/IP payload of %d bytes to the IP address %s, port number %d",
		CONFIG_UDP_DATA_UPLOAD_SIZE_BYTES + UDP_IP_HEADER_SIZE,
		CONFIG_UDP_SERVER_ADDRESS_STATIC,
		CONFIG_UDP_SERVER_PORT);

#if defined(CONFIG_UDP_RAI_LAST)
	/* Let the modem know that this is the last packet for now and we do not
	 * wait for a response.
	 */
	int rai = RAI_LAST;

	err = setsockopt(socket_fd, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
		if (err) {
			LOG_ERR("Failed to set socket option, error: %d", errno);
		}
#endif

#if defined(CONFIG_UDP_RAI_ONGOING)
	/* Let the modem know that we expect to keep the network up longer.
	 */
	int rai = RAI_ONGOING;

	err = setsockopt(socket_fd, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
		if (err) {
			LOG_ERR("Failed to set socket option, error: %d", errno);
		}
#endif

	err = send(socket_fd, buffer, sizeof(buffer), 0);
	if (err < 0) {
		LOG_ERR("Failed to transmit UDP packet, error: %d", errno);
	}

#if defined(CONFIG_UDP_RAI_NO_DATA)
	/* Let the modem know that there will be no upcoming data transmission anymore.
	 */
	int rai = RAI_NO_DATA;

	err = setsockopt(socket_fd, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
		if (err) {
			LOG_ERR("Failed to set socket option, error: %d", errno);
		}
#endif

	/* Transmit a limited number of times and then shutdown. */
	if (data_upload_iterations > 0) {
		data_upload_iterations--;
	} else if (data_upload_iterations == 0) {
		k_sem_give(&modem_shutdown_sem);
		/* No need to schedule work if we're shutting down. */
		return;
	}

	/* Schedule work if we're either transmitting indefinitely or
	 * there are more iterations left.
	 */
	k_work_schedule(&socket_transmission_work,
			K_SECONDS(CONFIG_UDP_DATA_UPLOAD_FREQUENCY_SECONDS));
}

static void work_init(void)
{
	k_work_init_delayable(&socket_transmission_work, socket_transmission_work_fn);
}


//Define Modem Shutdown Workqueue function
static void modem_shutdown_handler(struct k_work *work)
{
    // Your work processing code here
	LOG_WRN("Modem shutdown workqueue handler invoked.");

	disconnect_and_cleanup();
	k_sleep(K_MSEC(100));
	lte_lc_power_off();
	//nrf_modem_lib_shutdown();
}

// Define the work item using the macro
static K_WORK_DEFINE(modem_shutdown_wk, modem_shutdown_handler);

static void lte_handler(const struct lte_lc_evt *const evt)
{
	
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

	 LOG_INF("Network registration status: %s",
		 evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
		 "Connected - home" : "Connected - roaming");
		k_sem_give(&lte_connected_sem);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
	 LOG_INF("PSM parameter update: TAU: %d s, Active time: %d s",
		 evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE:
	 LOG_INF("eDRX parameter update: eDRX: %.2f s, PTW: %.2f s",
		 (double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
		break;

	case LTE_LC_EVT_RRC_UPDATE:
	    if (current_state == STATE_DISCONNECTED) {
        // Ignore events after disconnect
        break;
    }
	LOG_INF("RRC mode: %s",
            evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle");
        if (evt->rrc_mode == LTE_LC_RRC_MODE_IDLE) {
            // RRC released, now disconnect and power off modem
           LOG_WRN("RRC connection released, shutting down modem");
		   k_work_submit(&modem_shutdown_wk);
            // Optionally, signal your application to stop further processing
        }
        break;

	case LTE_LC_EVT_CELL_UPDATE:
	 LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d",
		 evt->cell.id, evt->cell.tac);
		break;
	case LTE_LC_EVT_RAI_UPDATE:
		/* RAI notification is supported by modem firmware releases >= 2.0.2 */
	 LOG_INF("RAI configuration update: Cell ID: %d, MCC: %d, MNC: %d, AS-RAI: %d, CP-RAI: %d",
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


int main(void)
{
	int err;

	LOG_INF("UDP sample has started");

	work_init();

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize modem library, error: %d", err);
		return -1;
	}

	modem_info_init();

#if defined(CONFIG_UDP_RAI_ENABLE) && defined(CONFIG_SOC_NRF9160)
	/* Enable Access Stratum RAI support for nRF9160.
	 * Note: The 1.3.x modem firmware release is certified to be compliant with 3GPP Release 13.
	 * %REL14FEAT enables selected optional features from 3GPP Release 14. The 3GPP Release 14
	 * features are not GCF or PTCRB conformance certified by Nordic and must be certified
	 * by MNO before being used in commercial products.
	 * nRF9161 is certified to be compliant with 3GPP Release 14.
	 */
	err = nrf_modem_at_printf("AT%%REL14FEAT=0,1,0,0,0");
	if (err) {
		LOG_ERR("Failed to enable Access Stratum RAI support, error: %d", err);
		return -1;
	}
#endif

	LOG_INF("Waiting 10 seconds before connecting to LTE network...");
	k_sleep(K_SECONDS(10));

	if (current_state == STATE_DISCONNECTED) {
		LOG_INF("Connecting to LTE network...");
	 	/* Connect to LTE network */
		err = lte_lc_connect_async(lte_handler);
		if (err) {
			LOG_ERR("Failed to connect to LTE network, error: %d", err);
			return -1;
		}

		k_sem_take(&lte_connected_sem, K_FOREVER);
		set_connected_state();

		LOG_WRN("LTE link is up");
		int rsrp, snr;
		modem_info_get_rsrp(&rsrp);
		modem_info_get_snr(&snr);
		LOG_INF("Current rsrp: %ddBm", rsrp);
		LOG_INF("Current snr: %ddB", snr);
	}


	k_work_schedule(&socket_transmission_work, K_NO_WAIT);

	k_sem_take(&modem_shutdown_sem, K_FOREVER);

	err = nrf_modem_lib_shutdown();
	if (err) {
		return -1;
	}

	return 0;
}
