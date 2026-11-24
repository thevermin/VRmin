/* libvulkan_kk.dylib: KosmicKrisp como lo espera el Wine de VRmin. Wine abre una biblioteca de Vulkan y le pide con
 * dlsym vkGetInstanceProcAddr y vkGetDeviceProcAddr (y winemac.drv se fija si tiene vkCreateMetalSurfaceEXT), como
 * con MoltenVK; KosmicKrisp, como todo driver de Mesa, solo da vk_icdGetInstanceProcAddr (para el loader de Vulkan).
 * Esto se los da a partir de esa. Wine la usa con VRMIN_VULKAN_LIB=<ruta de este archivo> (mac/wine/vulkan-metal.patch).
 * Va en la misma carpeta que libvulkan_kosmickrisp.dylib. */
#include <dlfcn.h>
#include <stddef.h>

typedef void (*vk_fn)(void);
typedef vk_fn (*vk_gipa)(void *instance, const char *name);
typedef vk_fn (*vk_gdpa)(void *device, const char *name);

static vk_gipa kk_gipa;
static vk_gdpa kk_gdpa;

__attribute__((constructor)) static void kk_load(void) {
  Dl_info info;
  if (!dladdr((void *)kk_load, &info) || !info.dli_fname) return;
  char path[1024];
  size_t n = 0;
  for (const char *p = info.dli_fname; *p && n < sizeof path - 64; p++) path[n++] = *p;
  while (n && path[n - 1] != '/') n--;
  const char *name = "libvulkan_kosmickrisp.dylib";
  for (const char *p = name; *p; p++) path[n++] = *p;
  path[n] = 0;
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (h) kk_gipa = (vk_gipa)dlsym(h, "vk_icdGetInstanceProcAddr");
}

vk_fn vkGetInstanceProcAddr(void *instance, const char *name) {
  if (!kk_gipa) return NULL;
  vk_fn f = kk_gipa(instance, name);
  // (la de los dispositivos sale de una instancia: es la misma para todas)
  if (instance && !kk_gdpa) kk_gdpa = (vk_gdpa)kk_gipa(instance, "vkGetDeviceProcAddr");
  return f;
}

vk_fn vkGetDeviceProcAddr(void *device, const char *name) {
  return kk_gdpa ? kk_gdpa(device, name) : NULL;
}

// (winemac.drv solo se fija si existe: la de verdad la pide con vkGetInstanceProcAddr de la instancia)
int vkCreateMetalSurfaceEXT(void *instance, const void *info, const void *alloc, void *surface) {
  typedef int (*fn)(void *, const void *, const void *, void *);
  fn f = kk_gipa ? (fn)kk_gipa(instance, "vkCreateMetalSurfaceEXT") : NULL;
  return f ? f(instance, info, alloc, surface) : -7;   /* VK_ERROR_EXTENSION_NOT_PRESENT */
}
