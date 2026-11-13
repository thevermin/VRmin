// Burrow: the GPU as libdrm sees it. Linux finds GPUs by reading /sys; Burrow has no /sys for its GPU, so this
// library (loaded first with LD_PRELOAD) answers libdrm's device list itself: one virtio-gpu (what Mesa's Venus
// driver looks for) at /dev/dri/renderD128, which Burrow serves.
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

static drmDevicePtr one_device(void) {
  // (one block, like libdrm's own, so free() releases it)
  size_t sz = sizeof(drmDevice) + DRM_NODE_MAX * sizeof(char *) + sizeof(drmPciBusInfo) + sizeof(drmPciDeviceInfo) + 32;
  char *p = calloc(1, sz);
  if (!p) return NULL;
  drmDevicePtr d = (drmDevicePtr)p;
  p += sizeof(drmDevice);
  d->nodes = (char **)p; p += DRM_NODE_MAX * sizeof(char *);
  d->businfo.pci = (drmPciBusInfoPtr)p; p += sizeof(drmPciBusInfo);
  d->deviceinfo.pci = (drmPciDeviceInfoPtr)p; p += sizeof(drmPciDeviceInfo);
  strcpy(p, "/dev/dri/renderD128");
  d->nodes[DRM_NODE_RENDER] = p;
  d->available_nodes = 1 << DRM_NODE_RENDER;
  d->bustype = DRM_BUS_PCI;
  d->businfo.pci->dev = 2;
  d->deviceinfo.pci->vendor_id = 0x1af4;   // virtio
  d->deviceinfo.pci->device_id = 0x1050;   // virtio-gpu
  d->deviceinfo.pci->subvendor_id = 0x1af4;
  d->deviceinfo.pci->subdevice_id = 0x1100;
  return d;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max) {
  (void)flags;
  if (!devices || max < 1) return 1;
  devices[0] = one_device();
  return devices[0] ? 1 : -12;
}
int drmGetDevices(drmDevicePtr devices[], int max) { return drmGetDevices2(0, devices, max); }
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) { (void)fd; (void)flags; *device = one_device(); return *device ? 0 : -12; }
int drmGetDevice(int fd, drmDevicePtr *device) { return drmGetDevice2(fd, 0, device); }
void drmFreeDevice(drmDevicePtr *device) { if (device) { free(*device); *device = NULL; } }
void drmFreeDevices(drmDevicePtr devices[], int count) { for (int i = 0; devices && i < count; i++) { free(devices[i]); devices[i] = NULL; } }
