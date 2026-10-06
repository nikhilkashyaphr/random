/******************************************************************************
 * pcie_access.c - Host PCIe register access layer
 ******************************************************************************/

#include "pcie_access.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define PCI_DEVICES_DIR "/sys/bus/pci/devices"

static int read_hex_file(const char *path, unsigned *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -errno;
    unsigned v = 0;
    int rc = (fscanf(f, "%x", &v) == 1) ? 0 : -EINVAL;
    fclose(f);
    if (rc == 0) *out = v;
    return rc;
}

int pcie_find_devices(int vendor_id, int device_id,
                      char out[][32], int max_out)
{
    DIR *dir = opendir(PCI_DEVICES_DIR);
    if (!dir) return -errno;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < max_out) {
        if (e->d_name[0] == '.') continue;

        char p[512];
        unsigned ven = 0, dev = 0;

        snprintf(p, sizeof(p), PCI_DEVICES_DIR "/%s/vendor", e->d_name);
        if (read_hex_file(p, &ven) != 0) continue;
        snprintf(p, sizeof(p), PCI_DEVICES_DIR "/%s/device", e->d_name);
        if (read_hex_file(p, &dev) != 0) continue;

        if (vendor_id >= 0 && (unsigned)vendor_id != ven) continue;
        if (device_id >= 0 && (unsigned)device_id != dev) continue;

        snprintf(out[n], 32, "%.31s", e->d_name);
        n++;
    }
    closedir(dir);
    return n;
}

int pcie_open_sysfs(PcieDev *d, const char *bdf, int bar, uint64_t reg_base)
{
    memset(d, 0, sizeof(*d));
    d->backend  = PCIE_BACKEND_SYSFS;
    d->fd       = -1;
    d->reg_base = reg_base;

    snprintf(d->path, sizeof(d->path),
             PCI_DEVICES_DIR "/%s/resource%d", bdf, bar);

    struct stat st;
    if (stat(d->path, &st) != 0) {
        fprintf(stderr, "pcie: cannot stat %s: %s\n", d->path, strerror(errno));
        fprintf(stderr, "      Is the BDF correct? Try:  lspci -D\n");
        return -errno;
    }

    d->fd = open(d->path, O_RDWR | O_SYNC);
    if (d->fd < 0) {
        fprintf(stderr, "pcie: cannot open %s: %s\n", d->path, strerror(errno));
        if (errno == EACCES)
            fprintf(stderr, "      Run as root, or grant CAP_SYS_RAWIO.\n");
        if (errno == EBUSY)
            fprintf(stderr, "      A driver holds this BAR exclusively. Unbind it,\n"
                            "      or use the chardev backend instead.\n");
        return -errno;
    }

    d->map_len = (size_t)st.st_size;
    d->map = mmap(NULL, d->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->map == MAP_FAILED) {
        fprintf(stderr, "pcie: mmap(%zu) failed: %s\n", d->map_len, strerror(errno));
        close(d->fd);
        d->fd = -1;
        d->map = NULL;
        return -errno;
    }

    if (d->verbose)
        fprintf(stderr, "pcie: mapped %s (%zu bytes), reg_base=0x%llx\n",
                d->path, d->map_len, (unsigned long long)d->reg_base);
    return 0;
}

int pcie_open_chardev(PcieDev *d, const char *path, uint64_t reg_base)
{
    memset(d, 0, sizeof(*d));
    d->backend  = PCIE_BACKEND_CHARDEV;
    d->reg_base = reg_base;
    snprintf(d->path, sizeof(d->path), "%s", path);

    d->fd = open(path, O_RDWR | O_SYNC);
    if (d->fd < 0) {
        fprintf(stderr, "pcie: cannot open %s: %s\n", path, strerror(errno));
        return -errno;
    }
    return 0;
}

void pcie_close_device(PcieDev *d)
{
    if (!d) return;
    if (d->map && d->map != MAP_FAILED) {
        munmap(d->map, d->map_len);
        d->map = NULL;
    }
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
}

/*---------------------------------------------------------------------------
 * Register access.
 *
 * The sysfs backend uses a volatile pointer so the compiler cannot merge,
 * reorder or elide MMIO accesses - essential for a polled handshake.
 *---------------------------------------------------------------------------*/
int pcie_read_register(PcieDev *d, uint32_t offset, uint32_t *value)
{
    if (!d || !value) return -EINVAL;
    uint64_t off = d->reg_base + offset;

    if (d->backend == PCIE_BACKEND_SYSFS) {
        if (!d->map) return -EBADF;
        if (off + 4 > d->map_len) {
            fprintf(stderr, "pcie: read 0x%llx beyond BAR (%zu bytes)\n",
                    (unsigned long long)off, d->map_len);
            return -ERANGE;
        }
        *value = *(volatile uint32_t *)((volatile uint8_t *)d->map + off);
        return 0;
    }

    if (d->fd < 0) return -EBADF;
    ssize_t r = pread(d->fd, value, 4, (off_t)off);
    if (r != 4) {
        fprintf(stderr, "pcie: pread @0x%llx failed: %s\n",
                (unsigned long long)off, strerror(errno));
        return (r < 0) ? -errno : -EIO;
    }
    return 0;
}

int pcie_write_register(PcieDev *d, uint32_t offset, uint32_t value)
{
    if (!d) return -EINVAL;
    uint64_t off = d->reg_base + offset;

    if (d->backend == PCIE_BACKEND_SYSFS) {
        if (!d->map) return -EBADF;
        if (off + 4 > d->map_len) {
            fprintf(stderr, "pcie: write 0x%llx beyond BAR (%zu bytes)\n",
                    (unsigned long long)off, d->map_len);
            return -ERANGE;
        }
        *(volatile uint32_t *)((volatile uint8_t *)d->map + off) = value;
        /* Read back to push the posted write out before we return. Without
         * this the ordering between the payload write and the NEW_CMD write
         * is not guaranteed at the endpoint. */
        (void)*(volatile uint32_t *)((volatile uint8_t *)d->map + off);
        return 0;
    }

    if (d->fd < 0) return -EBADF;
    ssize_t w = pwrite(d->fd, &value, 4, (off_t)off);
    if (w != 4) {
        fprintf(stderr, "pcie: pwrite @0x%llx failed: %s\n",
                (unsigned long long)off, strerror(errno));
        return (w < 0) ? -errno : -EIO;
    }
    return 0;
}
