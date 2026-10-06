/******************************************************************************
 *
 * pcie_access.h  -  Host PCIe register access layer
 *
 * Deliberately separated from the application logic so a GUI can reuse it.
 *
 * BACKENDS
 * --------
 * The brief said not to assume a mechanism, and the correct one depends on how
 * the endpoint is exposed on your host. Two are provided; pick at runtime:
 *
 *   PCIE_BACKEND_SYSFS   mmap() of /sys/bus/pci/devices/<BDF>/resource<N>
 *                        Works for any endpoint whose BAR the kernel maps and
 *                        that has no exclusive driver bound. No driver needed.
 *
 *   PCIE_BACKEND_CHARDEV pread/pwrite on a character device, for stacks that
 *                        expose the BAR as a file (e.g. an XDMA/QDMA control
 *                        node such as /dev/qdma<...>_bypass or /dev/xdma0_user).
 *
 * Determine which applies with:  lspci -vv -s <BDF>   and   ls /dev
 * See docs/PCIE_MIGRATION.md for the decision procedure.
 *
 ******************************************************************************/

#ifndef PCIE_ACCESS_H_
#define PCIE_ACCESS_H_

#include <stdint.h>
#include <stddef.h>

typedef enum {
    PCIE_BACKEND_SYSFS = 0,
    PCIE_BACKEND_CHARDEV
} PcieBackend;

typedef struct {
    PcieBackend backend;
    int         fd;
    void       *map;        /* sysfs backend only */
    size_t      map_len;    /* sysfs backend only */
    uint64_t    reg_base;   /* offset added to every access (see note)      */
    char        path[512];
    int         verbose;
} PcieDev;

/*---------------------------------------------------------------------------
 * Lifecycle
 *---------------------------------------------------------------------------*/

/* Open by sysfs BDF, e.g. "0000:01:00.0", BAR index (usually 0).
 * reg_base is added to every register offset. When the BAR window starts at
 * the PL aperture, pass 0. If the BAR maps a larger space and the control
 * block sits at an offset inside it, pass that offset. */
int pcie_open_sysfs(PcieDev *d, const char *bdf, int bar, uint64_t reg_base);

/* Open a character device that supports pread/pwrite at BAR offsets. */
int pcie_open_chardev(PcieDev *d, const char *path, uint64_t reg_base);

void pcie_close_device(PcieDev *d);

/*---------------------------------------------------------------------------
 * Register access. Both return 0 on success, negative errno on failure.
 *---------------------------------------------------------------------------*/
int pcie_read_register(PcieDev *d, uint32_t offset, uint32_t *value);
int pcie_write_register(PcieDev *d, uint32_t offset, uint32_t value);

/*---------------------------------------------------------------------------
 * Discovery helper: list candidate endpoints by vendor/device id.
 * Pass -1 for either to wildcard. Returns the number found; fills out[] with
 * up to max_out BDF strings.
 *---------------------------------------------------------------------------*/
int pcie_find_devices(int vendor_id, int device_id,
                      char out[][32], int max_out);

#endif /* PCIE_ACCESS_H_ */
