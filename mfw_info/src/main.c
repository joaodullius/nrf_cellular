#include <zephyr/kernel.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_info.h>
#include <stdio.h>

int main(void)
{
    int err = nrf_modem_lib_init();
    if (err) {
        printk("Modem lib init failed: %d\n", err);
        return -1;
    }

    err = modem_info_init();
    if (err) {
        printk("modem_info_init failed: %d\n", err);
        return -1;
    }

    char fw[64];
    err = modem_info_get_fw_version(fw, sizeof(fw));
    if (err == 0) {
        printk("Modem FW version: %s\n", fw);
    } else {
        printk("Failed to get modem FW version, err: %d\n", err);
    }
    return 0;
}