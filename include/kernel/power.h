/*
 * kernel/power.h - System power management
 *
 * Provides shutdown, restart, and sleep functionality.
 * Uses ACPI (for QEMU/VirtualBox) and keyboard controller reset fallback.
 */
#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

/* Shut down the system (power off) */
void power_shutdown(void);

/* Restart / reboot the system */
void power_restart(void);

/* Put system into sleep/halt state (wakes on interrupt) */
void power_sleep(void);

#endif /* KERNEL_POWER_H */
