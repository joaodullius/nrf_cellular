#include <zephyr/kernel.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <stdio.h>

int main(void)
{
    int err = nrf_modem_lib_init();
    if (err) {
        printk("Modem lib init failed: %d\n", err);
        return -1;
    }

    char response[64];
    err = nrf_modem_at_cmd(response, sizeof(response), "AT+CGMR");
    if (err == 0) {
        printk("Modem FW version:\n%s", response);
    } else {
        // Positive = modem returned ERROR/CME/CMS; Negative = library/errno
        printk("Failed to get modem FW version, err: %d\n", err);
    }
	return 0;
}