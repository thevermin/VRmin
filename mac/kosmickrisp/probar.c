/* Prueba de KosmicKrisp en esta Mac: lo carga directo (sin el loader de Vulkan), crea la instancia y el dispositivo y
 * dice qué GPU ve y si tiene lo que necesitan los juegos de VRmin. Compilado para x86_64, como el Wine de VRmin: así
 * se ve si Metal 4 anda con Rosetta. Uso: ./probar [carpeta/libvulkan_kosmickrisp.dylib] */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

int main(int argc, char **argv) {
  const char *lib = argc > 1 ? argv[1] : "./lib/libvulkan_kosmickrisp.dylib";
  void *h = dlopen(lib, RTLD_NOW);
  if (!h) { printf("No se pudo cargar %s: %s\n", lib, dlerror()); return 1; }
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
  if (!gipa) { printf("Sin vk_icdGetInstanceProcAddr\n"); return 1; }
  PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "VRmin", 1, "VRmin", 1, VK_API_VERSION_1_3};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
  VkInstance inst;
  VkResult r = ci(&ici, NULL, &inst);
  if (r) { printf("vkCreateInstance falló: %d\n", r); return 1; }
  PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
  PFN_vkGetPhysicalDeviceProperties gpdp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
  PFN_vkGetPhysicalDeviceFeatures gpdf = (PFN_vkGetPhysicalDeviceFeatures)gipa(inst, "vkGetPhysicalDeviceFeatures");
  PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
  uint32_t n = 1;
  VkPhysicalDevice pd;
  r = epd(inst, &n, &pd);
  if (r < 0 || !n) { printf("Sin GPU (%d)\n", r); return 1; }
  VkPhysicalDeviceProperties p;
  gpdp(pd, &p);
  VkPhysicalDeviceFeatures f;
  gpdf(pd, &f);
  printf("GPU: %s · Vulkan %u.%u.%u\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
  printf("geometryShader %u · tessellationShader %u · shaderCullDistance %u · fillModeNonSolid %u · multiDrawIndirect %u\n",
         f.geometryShader, f.tessellationShader, f.shaderCullDistance, f.fillModeNonSolid, f.multiDrawIndirect);
  float prio = 1;
  VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q};
  VkDevice dev;
  r = cd(pd, &dci, NULL, &dev);
  printf("vkCreateDevice: %s (%d)\n", r ? "falló" : "bien", r);
  return r ? 1 : 0;
}
