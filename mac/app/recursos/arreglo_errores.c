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
// --- SPIR-V sin gl_CullDistance
// Metal no tiene «cull distance» y MoltenVK 1.4.1 (SPIRV-Cross) arma mal los shaders que la escriben: el shader de
// Metal no compila y el juego no dibuja nada (DOOM VFR: todo negro, en la Mac y en el visor). La GPU no la ofrece
// (shaderCullDistance), pero hay juegos que la usan igual. Acá esas escrituras van a una variable propia del shader
// que nadie lee: el resultado es el mismo que en Metal (donde no hace nada) y el shader compila.
// Devuelve una copia nueva (malloc) o NULL si el shader no la usa o no se pudo.
static uint32_t* sin_cull(const uint32_t* c, size_t n, size_t* n_nuevo) {
  if (n < 5 || c[0] != 0x07230203) return NULL;
  uint32_t bound = c[3], version = c[1];
  uint8_t* cull_m = calloc(bound, 1);      // struct con miembro cull: número de miembro + 1
  uint8_t* cull_v = calloc(bound, 1);      // variable suelta marcada CullDistance
  uint32_t* puntero = calloc(bound, sizeof(uint32_t)); uint8_t* clase = calloc(bound, 1);   // OpTypePointer
  uint32_t* var_tipo = calloc(bound, sizeof(uint32_t));                                      // OpVariable de Output
  uint32_t* cte = calloc(bound, sizeof(uint32_t)); uint8_t* es_cte = calloc(bound, 1);
  uint32_t* miembro_tipo = calloc(bound, sizeof(uint32_t));   // tipo del miembro cull de cada struct
  uint32_t* nuevo = NULL; uint32_t* priv = NULL; uint32_t* bloque_var = NULL;
  size_t primera_funcion = 0, hay = 0;
  if (!cull_m || !cull_v || !puntero || !clase || !var_tipo || !cte || !es_cte || !miembro_tipo) goto fin;
  for (size_t i = 5; i < n;) {
    uint32_t wc = c[i] >> 16, op = c[i] & 0xffff;
    if (!wc || i + wc > n) goto fin;
    const uint32_t* w = c + i;
    if (op == 72 && wc >= 5 && w[3] == 11 && w[4] == 4 && w[1] < bound && w[2] < 255) cull_m[w[1]] = w[2] + 1;
    if (op == 71 && wc >= 4 && w[2] == 11 && w[3] == 4 && w[1] < bound) { cull_v[w[1]] = 1; hay = 1; }
    if (op == 32 && wc == 4 && w[1] < bound) { puntero[w[1]] = w[3]; clase[w[1]] = w[2]; }
    if (op == 59 && wc >= 4 && w[2] < bound && w[3] == 3) var_tipo[w[2]] = w[1];
    if (op == 43 && wc == 4 && w[2] < bound) { cte[w[2]] = w[3]; es_cte[w[2]] = 1; }
    if (op == 30 && w[1] < bound && cull_m[w[1]] && cull_m[w[1]] + 1 < wc) miembro_tipo[w[1]] = w[1 + cull_m[w[1]]];
    if (op == 54 && !primera_funcion) primera_funcion = i;
    i += wc;
  }
  // (glslang pone el miembro en todo gl_PerVertex: solo cuenta si el shader lo escribe o lo lee)
  for (size_t i = primera_funcion; i < n && !hay;) {
    uint32_t wc = c[i] >> 16, op = c[i] & 0xffff; const uint32_t* w = c + i;
    if ((op == 65 || op == 66) && wc >= 5 && w[3] < bound && var_tipo[w[3]] < bound && w[4] < bound && es_cte[w[4]]) {
      uint32_t s = puntero[var_tipo[w[3]]];
      if (s < bound && cull_m[s] && cte[w[4]] + 1 == cull_m[s]) hay = 1;
    }
    i += wc;
  }
  if (!hay || !primera_funcion) goto fin;
  // Ids nuevos: un puntero Private para cada tipo apuntado que haga falta, y una variable Private por cada bloque
  priv = calloc(bound, sizeof(uint32_t)); bloque_var = calloc(bound, sizeof(uint32_t));
  nuevo = malloc((n * 2 + 64) * sizeof(uint32_t));
  if (!priv || !bloque_var || !nuevo) goto fin;
  for (uint32_t t = 0; t < bound; t++)        // los Private que ya hay
    if (puntero[t] && clase[t] == 6 && puntero[t] < bound && !priv[puntero[t]]) priv[puntero[t]] = t;
  uint32_t sig = bound;
  size_t m = 5, extra_ini, extra_fin;
  memcpy(nuevo, c, 5 * sizeof(uint32_t));
  // 1) todo hasta la primera función, sin las marcas CullDistance de variables sueltas y con esas variables Private
  uint32_t interfaz_sacar[64]; int n_sacar = 0;
  for (size_t i = 5; i < primera_funcion;) {
    uint32_t wc = c[i] >> 16, op = c[i] & 0xffff; const uint32_t* w = c + i;
    if (op == 71 && wc >= 4 && w[2] == 11 && w[3] == 4) { i += wc; continue; }
    if (op == 59 && wc >= 4 && w[2] < bound && cull_v[w[2]]) {
      uint32_t t = w[1]; if (t >= bound || !puntero[t]) goto fin;
      uint32_t ap = puntero[t];
      if (!priv[ap]) { priv[ap] = sig++; nuevo[m++] = (4 << 16) | 32; nuevo[m++] = priv[ap]; nuevo[m++] = 6; nuevo[m++] = ap; }
      nuevo[m++] = (4 << 16) | 59; nuevo[m++] = priv[ap]; nuevo[m++] = w[2]; nuevo[m++] = 6;
      if (n_sacar < 64 && version < 0x10400) interfaz_sacar[n_sacar++] = w[2];
      i += wc; continue;
    }
    memcpy(nuevo + m, w, wc * sizeof(uint32_t)); m += wc; i += wc;
  }
  // (las variables Output que eran CullDistance salen de la interfaz de los OpEntryPoint)
  for (size_t i = 5; i < m;) {
    uint32_t wc = nuevo[i] >> 16, op = nuevo[i] & 0xffff;
    if (op == 15 && n_sacar) {
      size_t k = i + 3;   // después del nombre (termina en la palabra que tiene un byte 0)
      while (k < i + wc) {
        uint32_t x = nuevo[k++];
        if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break;
      }
      for (size_t j = k; j < i + wc;) {
        int sale = 0; for (int s = 0; s < n_sacar; s++) if (nuevo[j] == interfaz_sacar[s]) sale = 1;
        if (sale) { memmove(nuevo + j, nuevo + j + 1, (m - j - 1) * sizeof(uint32_t)); m--; wc--; nuevo[i] = (wc << 16) | op; }
        else j++;
      }
    }
    i += wc;
  }
  // 2) tipos y variables nuevos para los bloques (gl_PerVertex), antes de la primera función
  extra_ini = m;
  for (uint32_t v = 0; v < bound; v++) {
    uint32_t t = var_tipo[v]; if (!t || !puntero[t] || puntero[t] >= bound) continue;
    uint32_t s = puntero[t]; if (!cull_m[s] || !miembro_tipo[s]) continue;
    uint32_t mt = miembro_tipo[s];
    if (!priv[mt]) { priv[mt] = sig++; nuevo[m++] = (4 << 16) | 32; nuevo[m++] = priv[mt]; nuevo[m++] = 6; nuevo[m++] = mt; }
    bloque_var[v] = sig++;
    nuevo[m++] = (4 << 16) | 59; nuevo[m++] = priv[mt]; nuevo[m++] = bloque_var[v]; nuevo[m++] = 6;
  }
  extra_fin = m;
  // 3) las funciones: los accesos al miembro cull (o a la variable suelta) pasan a la variable Private
  for (size_t i = primera_funcion; i < n;) {
    uint32_t wc = c[i] >> 16, op = c[i] & 0xffff; const uint32_t* w = c + i;
    if ((op == 65 || op == 66) && wc >= 4 && w[3] < bound && w[1] < bound) {
      uint32_t base = w[3], tr = w[1];
      if (bloque_var[base] && wc >= 5 && w[4] < bound && es_cte[w[4]] &&
          cte[w[4]] + 1 == cull_m[puntero[var_tipo[base]]]) {
        uint32_t ap = puntero[tr];
        if (!ap) goto fin;
        if (!priv[ap]) {   // hace falta un puntero Private más: va con los nuevos
          priv[ap] = sig++;
          memmove(nuevo + extra_fin + 4, nuevo + extra_fin, (m - extra_fin) * sizeof(uint32_t));
          nuevo[extra_fin] = (4 << 16) | 32; nuevo[extra_fin + 1] = priv[ap]; nuevo[extra_fin + 2] = 6; nuevo[extra_fin + 3] = ap;
          extra_fin += 4; m += 4;
        }
        if (wc == 5) { nuevo[m++] = (4 << 16) | 83; nuevo[m++] = priv[ap]; nuevo[m++] = w[2]; nuevo[m++] = bloque_var[base]; }
        else {
          nuevo[m++] = ((wc - 1) << 16) | op; nuevo[m++] = priv[ap]; nuevo[m++] = w[2]; nuevo[m++] = bloque_var[base];
          memcpy(nuevo + m, w + 5, (wc - 5) * sizeof(uint32_t)); m += wc - 5;
        }
        i += wc; continue;
      }
      if (cull_v[base]) {
        uint32_t ap = puntero[tr];
        if (!ap) goto fin;
        if (!priv[ap]) {
          priv[ap] = sig++;
          memmove(nuevo + extra_fin + 4, nuevo + extra_fin, (m - extra_fin) * sizeof(uint32_t));
          nuevo[extra_fin] = (4 << 16) | 32; nuevo[extra_fin + 1] = priv[ap]; nuevo[extra_fin + 2] = 6; nuevo[extra_fin + 3] = ap;
          extra_fin += 4; m += 4;
        }
        memcpy(nuevo + m, w, wc * sizeof(uint32_t)); nuevo[m + 1] = priv[ap]; m += wc;
        i += wc; continue;
      }
    }
    memcpy(nuevo + m, w, wc * sizeof(uint32_t)); m += wc; i += wc;
    if (m + 64 > n * 2) goto fin;
  }
  (void) extra_ini;
  // (desde SPIR-V 1.4 la interfaz lleva también las variables Private)
  if (version >= 0x10400) {
    for (size_t i = 5; i < m;) {
      uint32_t wc = nuevo[i] >> 16, op = nuevo[i] & 0xffff;
      if (op == 15) {
        uint32_t agregar[64]; int na = 0;
        for (uint32_t v = 0; v < bound && na < 64; v++) if (bloque_var[v]) agregar[na++] = bloque_var[v];
        memmove(nuevo + i + wc + na, nuevo + i + wc, (m - i - wc) * sizeof(uint32_t));
        memcpy(nuevo + i + wc, agregar, na * sizeof(uint32_t)); m += na; wc += na; nuevo[i] = (wc << 16) | op;
      }
      i += wc;
    }
  }
  nuevo[3] = sig;
  *n_nuevo = m;
  free(cull_m); free(cull_v); free(puntero); free(clase); free(var_tipo); free(cte); free(es_cte); free(miembro_tipo);
  free(priv); free(bloque_var);
  return nuevo;
fin:
  free(cull_m); free(cull_v); free(puntero); free(clase); free(var_tipo); free(cte); free(es_cte); free(miembro_tipo);
  free(priv); free(bloque_var); free(nuevo);
  return NULL;
}
// --- fin SPIR-V
static int32_t (*mvk_create_shader)(void*, const void*, const void*, void*);
typedef struct { int32_t sType; const void* pNext; uint32_t flags; size_t codeSize; const uint32_t* pCode; } vk_shader_info;
static int32_t vrmin_create_shader(void* d, const void* info, const void* alloc, void* sm) {
  const vk_shader_info* ci = info;
  size_t n; uint32_t* sin = ci && ci->pCode ? sin_cull(ci->pCode, ci->codeSize / 4, &n) : NULL;
  if (!sin) return mvk_create_shader(d, info, alloc, sm);
  static int dicho;
  if (!dicho++) fprintf(stderr, "VRmin: the game's shaders write gl_CullDistance, which Metal doesn't have: left out\n");
  vk_shader_info copia = *ci; copia.codeSize = n * 4; copia.pCode = sin;
  int32_t r = mvk_create_shader(d, &copia, alloc, sm);
  free(sin);
  return r;
}
// Profundidad VK_FORMAT_X8_D24_UNORM_PACK32: las GPU de Apple no la tienen y MoltenVK no la reemplaza (sí reemplaza
// D24_UNORM_S8_UINT por D32_SFLOAT_S8_UINT), así que las imágenes y los render pass que la usan no se crean (DOOM
// VFR). Se usa VK_FORMAT_D32_SFLOAT, que ocupa lo mismo y es igual de preciso o más.
#define VK_X8_D24 125
#define VK_D32 126
static int32_t (*mvk_create_image)(void*, const void*, const void*, void*);
static int32_t (*mvk_create_view)(void*, const void*, const void*, void*);
static int32_t (*mvk_create_pass)(void*, const void*, const void*, void*);
static int32_t (*mvk_create_pass2)(void*, const void*, const void*, void*);
static int32_t (*mvk_create_pass2khr)(void*, const void*, const void*, void*);
// Copia la estructura de «tam» bytes con el formato en «donde» cambiado; NULL si no hace falta
static const void* con_d32(const void* info, size_t tam, size_t donde, unsigned char* copia) {
  if (!info || *(const int32_t*) ((const char*) info + donde) != VK_X8_D24) return info;
  memcpy(copia, info, tam); *(int32_t*) (copia + donde) = VK_D32;
  return copia;
}
static int32_t vrmin_create_image(void* d, const void* info, const void* alloc, void* img) {
  unsigned char copia[88]; return mvk_create_image(d, con_d32(info, 88, 24, copia), alloc, img);
}
static int32_t vrmin_create_view(void* d, const void* info, const void* alloc, void* v) {
  unsigned char copia[80]; return mvk_create_view(d, con_d32(info, 80, 36, copia), alloc, v);
}
// Render pass (1 y 2): attachmentCount en 20, pAttachments en 24; cada descripción mide «tam» con el formato en «donde»
static int32_t crear_pass(int32_t (*f)(void*, const void*, const void*, void*), size_t info_tam, size_t tam,
                          size_t donde, void* d, const void* info, const void* alloc, void* rp) {
  if (!info) return f(d, info, alloc, rp);
  uint32_t cant = *(const uint32_t*) ((const char*) info + 20);
  const unsigned char* att = *(const unsigned char* const*) ((const char*) info + 24);
  int hay = 0;
  for (uint32_t i = 0; att && i < cant; i++) if (*(const int32_t*) (att + i * tam + donde) == VK_X8_D24) hay = 1;
  if (!hay) return f(d, info, alloc, rp);
  unsigned char copia[80]; unsigned char* nuevos = malloc(cant * tam);
  if (!nuevos) return f(d, info, alloc, rp);
  memcpy(nuevos, att, cant * tam);
  for (uint32_t i = 0; i < cant; i++)
    if (*(int32_t*) (nuevos + i * tam + donde) == VK_X8_D24) *(int32_t*) (nuevos + i * tam + donde) = VK_D32;
  memcpy(copia, info, info_tam); *(const void**) (copia + 24) = nuevos;
  int32_t r = f(d, copia, alloc, rp);
  free(nuevos);
  return r;
}
static int32_t vrmin_create_pass(void* d, const void* info, const void* alloc, void* rp) {
  return crear_pass(mvk_create_pass, 64, 36, 4, d, info, alloc, rp);
}
static int32_t vrmin_create_pass2(void* d, const void* info, const void* alloc, void* rp) {
  return crear_pass(mvk_create_pass2, 80, 56, 20, d, info, alloc, rp);
}
static int32_t vrmin_create_pass2khr(void* d, const void* info, const void* alloc, void* rp) {
  return crear_pass(mvk_create_pass2khr, 80, 56, 20, d, info, alloc, rp);
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
  if (!strcmp(name, "vkCreateShaderModule") && mvk_create_shader) return (vk_fn) vrmin_create_shader;
  if (!strcmp(name, "vkCreateImage") && mvk_create_image) return (vk_fn) vrmin_create_image;
  if (!strcmp(name, "vkCreateImageView") && mvk_create_view) return (vk_fn) vrmin_create_view;
  if (!strcmp(name, "vkCreateRenderPass") && mvk_create_pass) return (vk_fn) vrmin_create_pass;
  if (!strcmp(name, "vkCreateRenderPass2") && mvk_create_pass2) return (vk_fn) vrmin_create_pass2;
  if (!strcmp(name, "vkCreateRenderPass2KHR") && mvk_create_pass2khr) return (vk_fn) vrmin_create_pass2khr;
  return NULL;
}
// Funciones de Vulkan 1.1 o más pedidas por su nombre sin «KHR» en un juego que creó Vulkan 1.0 (DOOM VFR): MoltenVK
// no las da, Windows sí. SteamVR las pide así (vkGetPhysicalDeviceImageFormatProperties2) y sin ellas rechaza las
// imágenes del juego («TextureUsesUnsupportedFormat»). Si falta, se da la de la extensión, que es la misma función.
static vk_fn con_khr(vk_gipa gpa, void* obj, const char* name) {
  char khr[128];
  size_t n = name ? strlen(name) : 0;
  if (!n || n + 4 > sizeof khr || strncmp(name, "vk", 2) || (n > 3 && !strcmp(name + n - 3, "KHR"))) return NULL;
  memcpy(khr, name, n); memcpy(khr + n, "KHR", 4);
  return gpa(obj, khr);
}
static vk_fn vrmin_gdpa(void* device, const char* name) {
  vk_fn f = propia(name);
  if (!f) f = mvk_gdpa(device, name);
  return f ? f : con_khr(mvk_gdpa, device, name);
}
static vk_fn vrmin_gipa(void* instance, const char* name) {
  vk_fn f = propia(name);
  if (!f) f = mvk_gipa(instance, name);
  return f ? f : con_khr(mvk_gipa, instance, name);
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
      mvk_create_shader = (int32_t (*)(void*, const void*, const void*, void*)) dlsym(handle, "vkCreateShaderModule");
      mvk_create_image = (int32_t (*)(void*, const void*, const void*, void*)) dlsym(handle, "vkCreateImage");
      mvk_create_view = (int32_t (*)(void*, const void*, const void*, void*)) dlsym(handle, "vkCreateImageView");
      mvk_create_pass = (int32_t (*)(void*, const void*, const void*, void*)) dlsym(handle, "vkCreateRenderPass");
      mvk_create_pass2 = (int32_t (*)(void*, const void*, const void*, void*)) dlsym(handle, "vkCreateRenderPass2");
      mvk_create_pass2khr = (int32_t (*)(void*, const void*, const void*, void*)) dlsym(handle, "vkCreateRenderPass2KHR");
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
