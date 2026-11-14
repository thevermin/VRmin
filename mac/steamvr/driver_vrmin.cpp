// VRmin's driver for SteamVR (Windows; on the Mac it runs inside VRmin's Windows).
//
// SteamVR loads it in vrserver.exe and sees a headset and two Touch controllers. Step 1: the devices, their
// properties and a fixed pose (standing, looking ahead), a virtual display whose Present only counts frames, and the
// play area provided by the driver so SteamVR doesn't ask for room setup. Frames to VRmin Host, the Quest's poses
// and buttons and vibration come next.
//
// Log: C:\questvrbridge\driver_vrmin.log (and SteamVR's vrserver.txt)
#include <openvr_driver.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace vr;

static FILE *g_log;
static void logf(const char *fmt, ...) {
  char b[1024];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  if (!g_log) g_log = fopen("C:\\questvrbridge\\driver_vrmin.log", "w");
  if (g_log) { fprintf(g_log, "%s\n", b); fflush(g_log); }
  if (VRDriverLog()) { strncat(b, "\n", sizeof b - strlen(b) - 1); VRDriverLog()->Log(b); }
}

static double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- what the headset is like (Quest 3 defaults; VRmin Host will send the real ones) ----
static const uint32_t EYE_W = 1832, EYE_H = 1920;
static const float HZ = 72.f, IPD = 0.063f;
static const float FOV_L = 1.0f, FOV_R = 1.0f, FOV_U = 1.0f, FOV_D = 1.0f;   // tan of the half angles (90°)
static const uint64_t UNIVERSE = 0x56526d696e;                                 // "VRmin"

// Play area: 2 x 2 m, standing at the origin
static const char *CHAPERONE_JSON = R"({
  "jsonid" : "chaperone_info",
  "universes" : [ {
    "collision_bounds" : [ [ [ -1, 0, -1 ], [ -1, 2.4, -1 ], [ -1, 2.4, 1 ], [ -1, 0, 1 ] ],
                           [ [ -1, 0, 1 ], [ -1, 2.4, 1 ], [ 1, 2.4, 1 ], [ 1, 0, 1 ] ],
                           [ [ 1, 0, 1 ], [ 1, 2.4, 1 ], [ 1, 2.4, -1 ], [ 1, 0, -1 ] ],
                           [ [ 1, 0, -1 ], [ 1, 2.4, -1 ], [ -1, 2.4, -1 ], [ -1, 0, -1 ] ] ],
    "play_area" : [ 2, 2 ],
    "seated" : { "translation" : [ 0, -1.2, 0 ], "yaw" : 0 },
    "standing" : { "translation" : [ 0, 0, 0 ], "yaw" : 0 },
    "universeID" : "370750089582"
  } ],
  "version" : 5
})";

static DriverPose_t pose_at(double x, double y, double z) {
  DriverPose_t p = {};
  p.poseIsValid = true;
  p.result = TrackingResult_Running_OK;
  p.deviceIsConnected = true;
  p.qWorldFromDriverRotation.w = 1;
  p.qDriverFromHeadRotation.w = 1;
  p.qRotation.w = 1;
  p.vecPosition[0] = x;
  p.vecPosition[1] = y;
  p.vecPosition[2] = z;
  return p;
}

// ---- headset ----
class VRminDisplay : public IVRDisplayComponent {
public:
  void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override { *x = 0; *y = 0; *w = EYE_W * 2; *h = EYE_H; }
  bool IsDisplayOnDesktop() override { return false; }
  bool IsDisplayRealDisplay() override { return false; }
  void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override { *w = EYE_W; *h = EYE_H; }
  void GetEyeOutputViewport(EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override {
    *x = eye == Eye_Left ? 0 : EYE_W; *y = 0; *w = EYE_W; *h = EYE_H;
  }
  void GetProjectionRaw(EVREye, float *l, float *r, float *t, float *b) override { *l = -FOV_L; *r = FOV_R; *t = -FOV_U; *b = FOV_D; }
  DistortionCoordinates_t ComputeDistortion(EVREye, float u, float v) override {
    DistortionCoordinates_t c;
    c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
    c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
    return c;
  }
  // (no lens: the Quest corrects its own; SteamVR's compositor needs the inverse too, or it doesn't start)
  bool ComputeInverseDistortion(HmdVector2_t *r, EVREye, uint32_t, float u, float v) override {
    r->v[0] = u;
    r->v[1] = v;
    return true;
  }
};

class VRminVirtualDisplay : public IVRVirtualDisplay {
  std::atomic<uint64_t> frames{0};
  double t0 = now_s(), lastLog = 0;
  uint64_t lastFrames = 0;
public:
  void Present(const PresentInfo_t *info, uint32_t) override {
    uint64_t n = ++frames;
    double t = now_s();
    if (n == 1) logf("First frame from the compositor (texture %llu)", (unsigned long long)info->backbufferTextureHandle);
    if (t - lastLog > 5) {
      logf("Compositor: %.0f frames/s", (n - lastFrames) / (t - (lastLog ? lastLog : t0)));
      lastLog = t;
      lastFrames = n;
    }
  }
  void WaitForPresent() override {}
  bool GetTimeSinceLastVsync(float *since, uint64_t *counter) override {
    double t = now_s() - t0, period = 1.0 / HZ;
    *counter = (uint64_t)(t / period);
    *since = (float)(t - *counter * period);
    return true;
  }
};

class VRminHmd : public ITrackedDeviceServerDriver {
  VRminDisplay display;
  VRminVirtualDisplay virtualDisplay;
public:
  uint32_t id = k_unTrackedDeviceIndexInvalid;
  EVRInitError Activate(uint32_t objectId) override {
    id = objectId;
    PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
    VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "VRmin");
    VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "VRmin");
    VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "vrmin");
    VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, "generic_hmd");
    VRProperties()->SetFloatProperty(c, Prop_UserIpdMeters_Float, IPD);
    VRProperties()->SetFloatProperty(c, Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    VRProperties()->SetFloatProperty(c, Prop_DisplayFrequency_Float, HZ);
    VRProperties()->SetFloatProperty(c, Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
    VRProperties()->SetUint64Property(c, Prop_CurrentUniverseId_Uint64, UNIVERSE);
    VRProperties()->SetBoolProperty(c, Prop_IsOnDesktop_Bool, false);
    VRProperties()->SetBoolProperty(c, Prop_DisplayDebugMode_Bool, false);
    VRProperties()->SetBoolProperty(c, Prop_HasDisplayComponent_Bool, true);
    VRProperties()->SetBoolProperty(c, Prop_HasVirtualDisplayComponent_Bool, true);
    VRProperties()->SetStringProperty(c, Prop_DriverProvidedChaperoneJson_String, CHAPERONE_JSON);
    VRProperties()->SetBoolProperty(c, Prop_DriverProvidedChaperoneVisibility_Bool, false);
    VRProperties()->SetInt32Property(c, Prop_ExpectedControllerCount_Int32, 2);
    logf("Headset activated (device %u): %ux%u per eye, %.0f Hz", id, EYE_W, EYE_H, HZ);
    return VRInitError_None;
  }
  void Deactivate() override { id = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *name) override {
    if (!strcmp(name, IVRDisplayComponent_Version)) return &display;
    if (!strcmp(name, IVRVirtualDisplay_Version)) return &virtualDisplay;
    return nullptr;
  }
  void DebugRequest(const char *, char *buf, uint32_t size) override { if (size) buf[0] = 0; }
  DriverPose_t GetPose() override { return pose_at(0, 1.6, 0); }
};

// ---- controllers (Touch, so SteamVR uses Valve's Touch bindings for each game) ----
class VRminController : public ITrackedDeviceServerDriver {
  bool left;
public:
  uint32_t id = k_unTrackedDeviceIndexInvalid;
  explicit VRminController(bool l) : left(l) {}
  EVRInitError Activate(uint32_t objectId) override {
    id = objectId;
    PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
    VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, left ? "Oculus Quest3 (Left Controller)" : "Oculus Quest3 (Right Controller)");
    VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "Oculus");
    VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "oculus");
    VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right");
    VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "oculus_touch");
    VRProperties()->SetStringProperty(c, Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
    VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, left ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand);
    VRProperties()->SetInt32Property(c, Prop_DeviceClass_Int32, TrackedDeviceClass_Controller);
    VRProperties()->SetUint64Property(c, Prop_CurrentUniverseId_Uint64, UNIVERSE);
    logf("%s controller activated (device %u)", left ? "Left" : "Right", id);
    return VRInitError_None;
  }
  void Deactivate() override { id = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *) override { return nullptr; }
  void DebugRequest(const char *, char *buf, uint32_t size) override { if (size) buf[0] = 0; }
  DriverPose_t GetPose() override { return pose_at(left ? -0.2 : 0.2, 1.2, -0.3); }
};

// ---- the driver ----
class VRminProvider : public IServerTrackedDeviceProvider {
  VRminHmd hmd;
  VRminController leftHand{true}, rightHand{false};
  std::atomic<bool> running{false};
  std::thread poses;
public:
  EVRInitError Init(IVRDriverContext *ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    logf("VRmin driver for SteamVR starting");
    if (!VRServerDriverHost()->TrackedDeviceAdded("VRMIN-HMD", TrackedDeviceClass_HMD, &hmd)) logf("SteamVR didn't take the headset");
    VRServerDriverHost()->TrackedDeviceAdded("VRMIN-LEFT", TrackedDeviceClass_Controller, &leftHand);
    VRServerDriverHost()->TrackedDeviceAdded("VRMIN-RIGHT", TrackedDeviceClass_Controller, &rightHand);
    running = true;
    poses = std::thread([this] {
      while (running) {
        if (hmd.id != k_unTrackedDeviceIndexInvalid) {
          DriverPose_t p = hmd.GetPose();
          VRServerDriverHost()->TrackedDevicePoseUpdated(hmd.id, p, sizeof p);
        }
        for (VRminController *c : {&leftHand, &rightHand})
          if (c->id != k_unTrackedDeviceIndexInvalid) {
            DriverPose_t p = c->GetPose();
            VRServerDriverHost()->TrackedDevicePoseUpdated(c->id, p, sizeof p);
          }
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
      }
    });
    return VRInitError_None;
  }
  void Cleanup() override {
    running = false;
    if (poses.joinable()) poses.join();
    logf("VRmin driver stopping");
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
  void RunFrame() override {
    VREvent_t e;
    while (VRServerDriverHost()->PollNextEvent(&e, sizeof e)) {}
  }
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}
};

static VRminProvider g_provider;

extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *interfaceName, int *returnCode) {
  if (!strcmp(interfaceName, IServerTrackedDeviceProvider_Version)) return &g_provider;
  if (returnCode) *returnCode = VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
