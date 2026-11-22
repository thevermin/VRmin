// VRmin: arreglo para juegos Unity (Mono) bajo Wine en la Mac.
// El Mono de Unity lee el "último error" de Windows directamente de %gs:0x68. En la Mac,
// %gs no apunta al TEB de Wine sino a los datos del hilo de macOS, y en hilos nuevos ese
// lugar vale 0 ("sin error"). Así, File.Exists de un archivo que no existe da "true".
// Este arreglo hace que cada hilo nuevo tenga ese lugar ocupado (con el búfer propio que
// macOS reserva ahí), de modo que Mono nunca vea un falso "sin error".
#ifdef __x86_64__   // solo Wine (Intel por Rosetta); en programas nativos de la Mac no hace nada
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#define DYLD_INTERPOSE(_r, _o) \
  __attribute__((used)) static struct { const void* r; const void* o; } _interpose_##_o \
  __attribute__((section("__DATA,__interpose"))) = { (const void*)(unsigned long)&_r, (const void*)(unsigned long)&_o };
static unsigned long gs68(void) { unsigned long v; __asm__ volatile("movq %%gs:0x68,%0" : "=r"(v)); return v; }
static void llenar(void) {
  if (gs68()) return;
  time_t t = 0; struct tm* a = gmtime(&t); (void)a;
  if (gs68()) return;
  a = localtime(&t); (void)a;
}
struct arranque { void* (*f)(void*); void* a; };
static void* inicio(void* p) {
  struct arranque w = *(struct arranque*)p; free(p);
  llenar();
  return w.f(w.a);
}
static int vrmin_pthread_create(pthread_t* t, const pthread_attr_t* at, void* (*f)(void*), void* a) {
  struct arranque* p = malloc(sizeof *p);
  if (!p) return pthread_create(t, at, f, a);
  p->f = f; p->a = a;
  int r = pthread_create(t, at, inicio, p);
  if (r) free(p);
  return r;
}
DYLD_INTERPOSE(vrmin_pthread_create, pthread_create)

// Juegos Vulkan (MoltenVK): si un juego pide una función de la GPU que Metal no tiene (por ejemplo depthBounds o
// shaderCullDistance), MoltenVK no crea el dispositivo y el juego se cierra (DOOM VFR). Wine abre MoltenVK y le pide
// vkGetInstanceProcAddr con dlsym: acá se le da una que, para vkCreateDevice, saca de lo pedido lo que esta GPU no
// tiene (y lo dice), y para todo lo demás es la de MoltenVK. Si la GPU tiene todo lo pedido, no cambia nada.
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
typedef void (*vk_fn)(void);
typedef vk_fn (*vk_gipa)(void*, const char*);
typedef struct { uint32_t f[55]; } vk_features;            // VkPhysicalDeviceFeatures: 55 VkBool32
typedef struct { int32_t sType; void* pNext; vk_features features; } vk_features2;   // VkPhysicalDeviceFeatures2
typedef struct { int32_t sType; const void* pNext; } vk_base;
typedef struct {
  int32_t sType; const void* pNext; uint32_t flags; uint32_t queueCount; const void* pQueues;
  uint32_t layerCount; const char* const* layers; uint32_t extCount; const char* const* exts;
  const vk_features* pEnabledFeatures;
} vk_device_info;                                          // VkDeviceCreateInfo
static const char* const vk_nombres[55] = {"robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray",
  "independentBlend", "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp",
  "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds",
  "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
  "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
  "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
  "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
  "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
  "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
  "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
  "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
  "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D",
  "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples",
  "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries"};
static void* mvk;                                          // MoltenVK (el que abrió Wine)
static vk_gipa mvk_gipa;
static int32_t (*mvk_create_device)(void*, const vk_device_info*, const void*, void**);
static void (*mvk_get_features)(void*, vk_features*);
// Saca de lo pedido lo que la GPU no tiene; devuelve cuántas sacó
static int recortar(vk_features* pedido, const vk_features* hay) {
  int n = 0;
  for (int i = 0; i < 55; i++)
    if (pedido->f[i] && !hay->f[i]) {
      pedido->f[i] = 0; n++;
      fprintf(stderr, "VRmin: the game asked for %s, which this Mac's GPU doesn't have: left out\n", vk_nombres[i]);
    }
  return n;
}
static int32_t vrmin_create_device(void* gpu, const vk_device_info* ci, const void* alloc, void** dev) {
  if (!ci || !mvk_get_features) return mvk_create_device(gpu, ci, alloc, dev);
  vk_features hay; memset(&hay, 0, sizeof hay);
  mvk_get_features(gpu, &hay);
  vk_device_info copia = *ci;
  vk_features pedido;
  if (ci->pEnabledFeatures) { pedido = *ci->pEnabledFeatures; recortar(&pedido, &hay); copia.pEnabledFeatures = &pedido; }
  // (o en VkPhysicalDeviceFeatures2, en la cadena pNext: se recorta ahí mismo y se deja como estaba después)
  vk_features2* f2 = NULL; vk_features antes;
  for (const vk_base* b = ci->pNext; b; b = b->pNext)
    if (b->sType == 1000059000) { f2 = (vk_features2*) b; break; }
  if (f2) { antes = f2->features; recortar(&f2->features, &hay); }
  int32_t r = mvk_create_device(gpu, &copia, alloc, dev);
  if (f2) f2->features = antes;
  return r;
}
// VK_SUBOPTIMAL_KHR (la imagen de la ventana no tiene justo su tamaño: en la Mac pasa seguido) es un resultado de
// éxito, pero algunos juegos lo toman como error y se cierran (DOOM VFR: «vkAcquireNextImageKHR failed with error
// (VK_SUBOPTIMAL_KHR)»). Se les devuelve VK_SUCCESS: la imagen sirve igual.
#define VK_SUBOPTIMAL 1000001003
static int32_t (*mvk_acquire)(void*, void*, uint64_t, void*, void*, uint32_t*);
static int32_t (*mvk_acquire2)(void*, const void*, uint32_t*);
static int32_t (*mvk_present)(void*, const void*);
static int32_t vrmin_acquire(void* d, void* sc, uint64_t t, void* sem, void* fence, uint32_t* i) {
  int32_t r = mvk_acquire(d, sc, t, sem, fence, i); return r == VK_SUBOPTIMAL ? 0 : r;
}
static int32_t vrmin_acquire2(void* d, const void* info, uint32_t* i) {
  int32_t r = mvk_acquire2(d, info, i); return r == VK_SUBOPTIMAL ? 0 : r;
}
static int32_t vrmin_present(void* q, const void* info) {
  int32_t r = mvk_present(q, info); return r == VK_SUBOPTIMAL ? 0 : r;
}
static vk_fn vrmin_gdpa(void* device, const char* name);
static vk_gipa mvk_gdpa;
// Las funciones que VRmin cambia, por nombre (NULL: la de MoltenVK)
static vk_fn propia(const char* name) {
  if (!name) return NULL;
  if (!strcmp(name, "vkCreateDevice") && mvk_create_device) return (vk_fn) vrmin_create_device;
  if (!strcmp(name, "vkAcquireNextImageKHR") && mvk_acquire) return (vk_fn) vrmin_acquire;
  if (!strcmp(name, "vkAcquireNextImage2KHR") && mvk_acquire2) return (vk_fn) vrmin_acquire2;
  if (!strcmp(name, "vkQueuePresentKHR") && mvk_present) return (vk_fn) vrmin_present;
  if (!strcmp(name, "vkGetDeviceProcAddr") && mvk_gdpa) return (vk_fn) vrmin_gdpa;
  return NULL;
}
static vk_fn vrmin_gdpa(void* device, const char* name) {
  vk_fn f = propia(name);
  return f ? f : mvk_gdpa(device, name);
}
static vk_fn vrmin_gipa(void* instance, const char* name) {
  vk_fn f = propia(name);
  return f ? f : mvk_gipa(instance, name);
}
static void* vrmin_dlsym(void* handle, const char* name) {
  void* p = dlsym(handle, name);
  // (Wine pide primero vkGetDeviceProcAddr y después vkGetInstanceProcAddr: MoltenVK se reconoce con cualquiera)
  if (p && name && (!strcmp(name, "vkGetInstanceProcAddr") || !strcmp(name, "vkGetDeviceProcAddr")) && !mvk) {
    Dl_info info;
    if (dladdr(p, &info) && info.dli_fname && strstr(info.dli_fname, "MoltenVK")) {
      mvk = handle; mvk_gipa = (vk_gipa) dlsym(handle, "vkGetInstanceProcAddr");
      mvk_create_device = (int32_t (*)(void*, const vk_device_info*, const void*, void**)) dlsym(handle, "vkCreateDevice");
      mvk_get_features = (void (*)(void*, vk_features*)) dlsym(handle, "vkGetPhysicalDeviceFeatures");
      mvk_acquire = (int32_t (*)(void*, void*, uint64_t, void*, void*, uint32_t*)) dlsym(handle, "vkAcquireNextImageKHR");
      mvk_acquire2 = (int32_t (*)(void*, const void*, uint32_t*)) dlsym(handle, "vkAcquireNextImage2KHR");
      mvk_present = (int32_t (*)(void*, const void*)) dlsym(handle, "vkQueuePresentKHR");
      mvk_gdpa = (vk_gipa) dlsym(handle, "vkGetDeviceProcAddr");
    }
  }
  if (p && mvk && handle == mvk && name && !strcmp(name, "vkGetInstanceProcAddr")) return (void*) vrmin_gipa;
  if (p && mvk && handle == mvk && name && !strcmp(name, "vkGetDeviceProcAddr") && mvk_gdpa) return (void*) vrmin_gdpa;
  return p;
}
DYLD_INTERPOSE(vrmin_dlsym, dlsym)

__attribute__((constructor)) static void al_cargar(void) {
  llenar();
  if (getenv("VRMIN_DIAG")) fprintf(stderr, "[arreglo cargado]\n");
}
#endif
