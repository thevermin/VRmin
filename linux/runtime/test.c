/*
 * Prueba del runtime de OpenXR de VRmin para Linux, por el loader de OpenXR de verdad: crea la instancia (con
 * XR_KHR_vulkan_enable, como wineopenxr y OpenComposite, o con XR_KHR_vulkan_enable2 si se le pasa «2»), el sistema, la
 * sesión de Vulkan, una acción de cada tipo, dos swapchains (una por ojo) y un panel flotante, y entrega cuadros de
 * colores. Con VRmin Host abierto, en su registro tiene que aparecer «First frame received».
 *   gcc -O2 -o vrmin-xrtest test.c -I<OpenXR-SDK>/include -lopenxr_loader -lvulkan
 *   XR_RUNTIME_JSON=<carpeta>/runtime/vrmin_openxr.json ./vrmin-xrtest [1|2] [cuadros]
 * (sin GPU, con Vulkan por el procesador: «sudo apt install mesa-vulkan-drivers», y VK_ICD_FILENAMES con lvp_icd.json)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define XR(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define FN(inst, name) PFN_##name name; XR(xrGetInstanceProcAddr(inst, #name, (PFN_xrVoidFunction *)&name))

static XrInstance inst;
static XrSession session;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;

/* Pinta toda la imagen de un color (queda en COLOR_ATTACHMENT_OPTIMAL, como la deja el runtime) */
static void paint(VkImage img, float r, float g, float b) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    VK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier br = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, rg };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &br);
    VkClearColorValue c = { { r, g, b, 1.0f } };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rg);
    br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; br.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; br.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &br);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

typedef struct { XrSwapchain sc; uint32_t n, w, h; XrSwapchainImageVulkanKHR img[8]; } Chain;
static void make_chain(Chain *c, int64_t fmt, uint32_t w, uint32_t h) {
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XR(xrCreateSwapchain(session, &ci, &c->sc));
    XR(xrEnumerateSwapchainImages(c->sc, 0, &c->n, NULL));
    if (c->n > 8) { fprintf(stderr, "FAIL too many images\n"); exit(1); }
    for (uint32_t i = 0; i < c->n; i++) c->img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    XR(xrEnumerateSwapchainImages(c->sc, c->n, &c->n, (XrSwapchainImageBaseHeader *)c->img));
    c->w = w; c->h = h;
}
static void draw_chain(Chain *c, float r, float g, float b) {
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XR(xrAcquireSwapchainImage(c->sc, &ai, &idx));
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    XR(xrWaitSwapchainImage(c->sc, &wi));
    paint(c->img[idx].image, r, g, b);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    XR(xrReleaseSwapchainImage(c->sc, &ri));
}

static XrSessionState poll_events(void) {
    static XrSessionState st = XR_SESSION_STATE_UNKNOWN;
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) st = ((XrEventDataSessionStateChanged *)&ev)->state;
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    return st;
}

int main(int argc, char **argv) {
    int v2 = argc > 1 && atoi(argv[1]) == 2;
    int frames = argc > 2 ? atoi(argv[2]) : 120;

    /* Extensiones */
    uint32_t n = 0;
    XR(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL));
    XrExtensionProperties props[32];
    for (uint32_t i = 0; i < 32; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES, props[i].next = NULL;
    XR(xrEnumerateInstanceExtensionProperties(NULL, n < 32 ? n : 32, &n, props));
    int need = 0;
    for (uint32_t i = 0; i < n; i++) {
        printf("extension: %s\n", props[i].extensionName);
        if (!strcmp(props[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) || !strcmp(props[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)
            || !strcmp(props[i].extensionName, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) need++;
    }
    if (need != 3) { fprintf(stderr, "FAIL missing extensions\n"); return 1; }

    const char *exts[] = { v2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "vrmin-xrtest");
    strcpy(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 2; ici.enabledExtensionNames = exts;
    XR(xrCreateInstance(&ici, &inst));
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    XR(xrGetInstanceProperties(inst, &ip));
    printf("runtime: %s\n", ip.runtimeName);
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    XR(xrGetSystem(inst, &sgi, &sys));

    /* Vulkan */
    XrGraphicsRequirementsVulkanKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    VkInstance vki; VkPhysicalDevice pd;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vrmin-xrtest"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    vci.pApplicationInfo = &app;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    if (!v2) {
        FN(inst, xrGetVulkanGraphicsRequirementsKHR); FN(inst, xrGetVulkanInstanceExtensionsKHR);
        FN(inst, xrGetVulkanGraphicsDeviceKHR); FN(inst, xrGetVulkanDeviceExtensionsKHR);
        XR(xrGetVulkanGraphicsRequirementsKHR(inst, sys, &req));
        char buf[1024]; uint32_t len;
        XR(xrGetVulkanInstanceExtensionsKHR(inst, sys, sizeof buf, &len, buf));
        printf("vulkan instance extensions: '%s'\n", buf);
        XR(xrGetVulkanDeviceExtensionsKHR(inst, sys, sizeof buf, &len, buf));
        printf("vulkan device extensions: '%s'\n", buf);
        VK(vkCreateInstance(&vci, NULL, &vki));
        XR(xrGetVulkanGraphicsDeviceKHR(inst, sys, vki, &pd));
    } else {
        FN(inst, xrGetVulkanGraphicsRequirements2KHR); FN(inst, xrCreateVulkanInstanceKHR);
        FN(inst, xrGetVulkanGraphicsDevice2KHR); FN(inst, xrCreateVulkanDeviceKHR);
        XR(xrGetVulkanGraphicsRequirements2KHR(inst, sys, &req));
        XrVulkanInstanceCreateInfoKHR xi = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
        xi.systemId = sys; xi.pfnGetInstanceProcAddr = vkGetInstanceProcAddr; xi.vulkanCreateInfo = &vci;
        VkResult vr;
        XR(xrCreateVulkanInstanceKHR(inst, &xi, &vki, &vr)); VK(vr);
        XrVulkanGraphicsDeviceGetInfoKHR gi = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        gi.systemId = sys; gi.vulkanInstance = vki;
        XR(xrGetVulkanGraphicsDevice2KHR(inst, &gi, &pd));
    }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    printf("gpu: %s\n", pp.deviceName);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t fam = 0;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
    qci.queueFamilyIndex = fam;
    if (!v2) VK(vkCreateDevice(pd, &dci, NULL, &dev));
    else {
        FN(inst, xrCreateVulkanDeviceKHR);
        XrVulkanDeviceCreateInfoKHR xd = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        xd.systemId = sys; xd.pfnGetInstanceProcAddr = vkGetInstanceProcAddr; xd.vulkanPhysicalDevice = pd; xd.vulkanCreateInfo = &dci;
        VkResult vr;
        XR(xrCreateVulkanDeviceKHR(inst, &xd, &dev, &vr)); VK(vr);
    }
    vkGetDeviceQueue(dev, fam, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = fam; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    /* Sesión */
    XrGraphicsBindingVulkanKHR gb = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    gb.instance = vki; gb.physicalDevice = pd; gb.device = dev; gb.queueFamilyIndex = fam; gb.queueIndex = 0;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb; sci.systemId = sys;
    XR(xrCreateSession(inst, &sci, &session));
    if (poll_events() != XR_SESSION_STATE_READY) { fprintf(stderr, "FAIL session not READY\n"); return 1; }
    XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR(xrBeginSession(session, &sbi));
    if (poll_events() != XR_SESSION_STATE_FOCUSED) { fprintf(stderr, "FAIL session not FOCUSED\n"); return 1; }

    /* Acciones: una pose y un botón de los controles de Quest */
    XrActionSetCreateInfo asi = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asi.actionSetName, "test"); strcpy(asi.localizedActionSetName, "test");
    XrActionSet aset;
    XR(xrCreateActionSet(inst, &asi, &aset));
    XrPath hands[2];
    XR(xrStringToPath(inst, "/user/hand/left", &hands[0]));
    XR(xrStringToPath(inst, "/user/hand/right", &hands[1]));
    XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT; strcpy(aci.actionName, "grip"); strcpy(aci.localizedActionName, "grip");
    aci.countSubactionPaths = 2; aci.subactionPaths = hands;
    XrAction grip, trig;
    XR(xrCreateAction(aset, &aci, &grip));
    aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT; strcpy(aci.actionName, "trigger"); strcpy(aci.localizedActionName, "trigger");
    XR(xrCreateAction(aset, &aci, &trig));
    XrPath prof, b[4];
    XR(xrStringToPath(inst, "/interaction_profiles/oculus/touch_controller", &prof));
    XR(xrStringToPath(inst, "/user/hand/left/input/grip/pose", &b[0]));
    XR(xrStringToPath(inst, "/user/hand/right/input/grip/pose", &b[1]));
    XR(xrStringToPath(inst, "/user/hand/left/input/trigger/value", &b[2]));
    XR(xrStringToPath(inst, "/user/hand/right/input/trigger/value", &b[3]));
    XrActionSuggestedBinding sb[4] = { { grip, b[0] }, { grip, b[1] }, { trig, b[2] }, { trig, b[3] } };
    XrInteractionProfileSuggestedBinding ipsb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    ipsb.interactionProfile = prof; ipsb.countSuggestedBindings = 4; ipsb.suggestedBindings = sb;
    XR(xrSuggestInteractionProfileBindings(inst, &ipsb));
    XrSessionActionSetsAttachInfo aai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    aai.countActionSets = 1; aai.actionSets = &aset;
    XR(xrAttachSessionActionSets(session, &aai));
    XrActionSpaceCreateInfo asci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
    asci.action = grip; asci.subactionPath = hands[1]; asci.poseInActionSpace.orientation.w = 1;
    XrSpace gripSpace;
    XR(xrCreateActionSpace(session, &asci, &gripSpace));

    /* Espacio, vistas y swapchains */
    XrReferenceSpaceCreateInfo rsi = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE; rsi.poseInReferenceSpace.orientation.w = 1;
    XrSpace stage;
    XR(xrCreateReferenceSpace(session, &rsi, &stage));
    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    XR(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv));
    uint32_t w = vcv[0].recommendedImageRectWidth, h = vcv[0].recommendedImageRectHeight;
    printf("eye: %ux%u\n", w, h);
    int64_t fmts[16];
    XR(xrEnumerateSwapchainFormats(session, 16, &n, fmts));
    int64_t fmt = fmts[0];
    Chain eye[2], quad;
    make_chain(&eye[0], fmt, w, h);
    make_chain(&eye[1], fmt, w, h);
    make_chain(&quad, fmt, 256, 128);

    /* Cuadros */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) {
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        XR(xrWaitFrame(session, &fwi, &fs));
        XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
        XR(xrBeginFrame(session, &fbi));
        XrActiveActionSet act = { aset, XR_NULL_PATH };
        XrActionsSyncInfo asy = { XR_TYPE_ACTIONS_SYNC_INFO };
        asy.countActiveActionSets = 1; asy.activeActionSets = &act;
        XR(xrSyncActions(session, &asy));
        XrActionStateGetInfo agi = { XR_TYPE_ACTION_STATE_GET_INFO };
        agi.action = trig; agi.subactionPath = hands[1];
        XrActionStateBoolean bs = { XR_TYPE_ACTION_STATE_BOOLEAN };
        XR(xrGetActionStateBoolean(session, &agi, &bs));
        XrSpaceLocation gl = { XR_TYPE_SPACE_LOCATION };
        XR(xrLocateSpace(gripSpace, stage, fs.predictedDisplayTime, &gl));
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = fs.predictedDisplayTime; vli.space = stage;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        XR(xrLocateViews(session, &vli, &vs, 2, &n, views));
        float k = (float)(f % 60) / 60.0f;
        draw_chain(&eye[0], k, 0.2f, 1.0f - k);
        draw_chain(&eye[1], 1.0f - k, 0.2f, k);
        draw_chain(&quad, 1.0f, 1.0f, 1.0f);
        XrCompositionLayerProjectionView pv[2];
        for (int e = 0; e < 2; e++) {
            memset(&pv[e], 0, sizeof pv[e]);
            pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[e].pose = views[e].pose; pv[e].fov = views[e].fov;
            pv[e].subImage.swapchain = eye[e].sc;
            pv[e].subImage.imageRect.extent.width = (int32_t)w; pv[e].subImage.imageRect.extent.height = (int32_t)h;
        }
        XrCompositionLayerProjection pl = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        pl.space = stage; pl.viewCount = 2; pl.views = pv;
        XrCompositionLayerQuad ql = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        ql.space = stage; ql.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        ql.subImage.swapchain = quad.sc; ql.subImage.imageRect.extent.width = 256; ql.subImage.imageRect.extent.height = 128;
        ql.pose.orientation.w = 1; ql.pose.position.y = 1.6f; ql.pose.position.z = -1.5f;
        ql.size.width = 0.5f; ql.size.height = 0.25f;
        const XrCompositionLayerBaseHeader *layers[2] = { (const XrCompositionLayerBaseHeader *)&pl, (const XrCompositionLayerBaseHeader *)&ql };
        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 2; fei.layers = layers;
        XR(xrEndFrame(session, &fei));
    }
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("frames: %d in %.2f s (%.1f/s)\n", frames, secs, frames / secs);

    XR(xrRequestExitSession(session));
    poll_events();
    XR(xrEndSession(session));
    for (int e = 0; e < 2; e++) XR(xrDestroySwapchain(eye[e].sc));
    XR(xrDestroySwapchain(quad.sc));
    XR(xrDestroySpace(stage));
    XR(xrDestroySpace(gripSpace));
    XR(xrDestroySession(session));
    XR(xrDestroyInstance(inst));
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(vki, NULL);
    printf("OK\n");
    return 0;
}
