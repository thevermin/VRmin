// VRmin's driver for SteamVR (Windows; on the Mac it runs inside VRmin's Windows).
//
// SteamVR loads it in vrserver.exe and sees a headset and two Touch controllers that follow the Quest's (poses and
// buttons come from VRmin Host), the play area provided by the driver so SteamVR doesn't ask for room setup, and
// SteamVR's direct mode: the game's frames come to the driver (SteamVR's last drawing pass, the lens warp, doesn't run:
// inside VRmin's Windows it drew a single color) and go to VRmin Host at the headset's rate. Vibration comes next.
//
// Log: C:\questvrbridge\driver_vrmin.log (and SteamVR's vrserver.txt)
#include <winsock2.h>
#include <openvr_driver.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>

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

// ---- what the headset is like (Quest 3 defaults, until VRmin Host and the Quest's first pose say: wait_display) ----
static uint32_t EYE_W = 1832, EYE_H = 1920;
static float HZ = 72.f, IPD = 0.063f;
// Per eye: left, right, top, bottom, as GetProjectionRaw wants them: tangents where «top» is the lower edge (negative) and
// «bottom» the upper one (Valve's projection matrix uses y downwards: m[1][2] = (bottom + top) / (bottom - top))
static float FOV[2][4] = {{-1, 1, -1, 1}, {-1, 1, -1, 1}};
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

// A pose for SteamVR from a position and a rotation (x y z w); without one, looking ahead
static DriverPose_t pose_at(double x, double y, double z, const double *q = nullptr) {
  DriverPose_t p = {};
  p.poseIsValid = true;
  p.result = TrackingResult_Running_OK;
  p.deviceIsConnected = true;
  p.qWorldFromDriverRotation.w = 1;
  p.qDriverFromHeadRotation.w = 1;
  p.qRotation.w = 1;
  if (q) {
    double m = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (m > 1e-4) { p.qRotation.x = q[0] / m; p.qRotation.y = q[1] / m; p.qRotation.z = q[2] / m; p.qRotation.w = q[3] / m; }
  }
  p.vecPosition[0] = x;
  p.vecPosition[1] = y;
  p.vecPosition[2] = z;
  return p;
}

// ---- headset ----
// Methods that return a struct (ComputeDistortion; GetPose too, but SteamVR never calls it) can't be written the normal way: SteamVR is built with
// MSVC, which passes the address for the result after «this», and mingw's g++ passes it before (vrserver crashed
// in SteamVR's distortion call). So the display component is an MSVC-shaped vtable written by hand: the same methods
// in the same order as IVRDisplayComponent, with the result address as an explicit argument after «this».
struct VRminDisplay {
  virtual void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) { *x = 0; *y = 0; *w = EYE_W * 2; *h = EYE_H; }
  virtual bool IsDisplayOnDesktop() { return false; }
  virtual bool IsDisplayRealDisplay() { return false; }
  virtual void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) { *w = EYE_W; *h = EYE_H; }
  virtual void GetEyeOutputViewport(EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    *x = eye == Eye_Left ? 0 : EYE_W; *y = 0; *w = EYE_W; *h = EYE_H;
  }
  virtual void GetProjectionRaw(EVREye eye, float *l, float *r, float *t, float *b) {
    const float *f = FOV[eye == Eye_Left ? 0 : 1];
    *l = f[0]; *r = f[1]; *t = f[2]; *b = f[3];
  }
  // (no lens: the Quest corrects its own)
  virtual DistortionCoordinates_t *ComputeDistortion(DistortionCoordinates_t *c, EVREye eye, float u, float v) {
    c->rfRed[0] = c->rfGreen[0] = c->rfBlue[0] = u;
    c->rfRed[1] = c->rfGreen[1] = c->rfBlue[1] = v;
    static std::atomic<int> n{0};
    int i = ++n;
    if (i <= 4 || i % 1000 == 0) logf("ComputeDistortion #%d: eye %d, u %.3f, v %.3f", i, (int)eye, u, v);
    return c;
  }
  virtual bool ComputeInverseDistortion(HmdVector2_t *r, EVREye eye, uint32_t ch, float u, float v) {
    r->v[0] = u;
    r->v[1] = v;
    static std::atomic<int> n{0};
    int i = ++n;
    if (i <= 4 || i % 1000 == 0) logf("ComputeInverseDistortion #%d: eye %d, channel %u, u %.3f, v %.3f", i, (int)eye, ch, u, v);
    return true;
  }
};

// ---- connection to VRmin Host ----
// The same protocol as VRmin's OpenXR runtime (mac/openxr): a control connection and an image connection to
// VRmin Host on port 9002. Packets are [length u32 big-endian][type][payload].
static const uint16_t HOST_PORT = 9002;

static bool send_all(SOCKET s, const void *data, int n) {
  const char *p = (const char *)data;
  while (n > 0) {
    int r = send(s, p, n, 0);
    if (r <= 0) return false;
    p += r;
    n -= r;
  }
  return true;
}

static bool send_packet(SOCKET s, uint8_t type, const void *data, uint32_t n) {
  uint32_t len = 1 + n;
  uint8_t hdr[5] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type};
  return send_all(s, hdr, 5) && (!n || send_all(s, data, (int)n));
}

static SOCKET host_connect() {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(HOST_PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (sockaddr *)&a, sizeof a) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
  return s;
}

// ---- what the Quest sends ----
// A pose line from VRmin Host: "id":N … "xr":[77 numbers]: valid, then per eye its position, rotation (x y z w) and the
// tangents of its field of view (left, right, down, up), then per hand: valid, grip pose, aim pose and 12 inputs.
// Positions are in meters from the Quest's floor, with the same axes SteamVR uses.
enum { IN_TRIGGER, IN_SQUEEZE, IN_TX, IN_TY, IN_TCLICK, IN_A, IN_B, IN_TTOUCH, IN_THTOUCH, IN_ATOUCH, IN_BTOUCH, IN_MENU, IN_COUNT };
struct Track {
  uint32_t id = 0;
  bool valid = false, handValid[2] = {};
  double eye[2][7] = {}, fov[2][4] = {}, grip[2][7] = {}, aim[2][7] = {};
  float in[2][IN_COUNT] = {};
};
static std::mutex g_trackLock;
static Track g_track;
static std::atomic<uint32_t> g_poseId{0};   // (frames carry it: the Quest only shows frames whose pose it knows)
static std::atomic<bool> g_handsValid{false};   // (the Quest sees both controllers)

// The head poses given to SteamVR, by the number of the Quest's pose: a frame says which head pose it was drawn with,
// and VRmin Host needs the Quest's number for it
struct HeadSent { uint32_t id; double pos[3], rot[9]; };
static std::mutex g_sentLock;
static HeadSent g_sent[128];
static int g_sentNext;
static void remember_head(uint32_t id, const DriverPose_t &p) {
  const HmdQuaternion_t &q = p.qRotation;
  HeadSent h = {id, {p.vecPosition[0], p.vecPosition[1], p.vecPosition[2]},
                {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w),
                 2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w),
                 2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}};
  std::lock_guard<std::mutex> g(g_sentLock);
  g_sent[g_sentNext] = h;
  g_sentNext = (g_sentNext + 1) % 128;
}
// The Quest's pose closest to the head pose a frame was drawn with (0: none); *dist: how far it was
static uint32_t pose_for(const HmdMatrix34_t &m, double *dist) {
  std::lock_guard<std::mutex> g(g_sentLock);
  uint32_t best = 0;
  double bestD = 1e9;
  for (const HeadSent &h : g_sent) {
    if (!h.id) continue;
    double d = 0;
    for (int r = 0; r < 3; r++) {
      d += std::fabs(m.m[r][3] - h.pos[r]);
      for (int c = 0; c < 3; c++) d += std::fabs(m.m[r][c] - h.rot[r * 3 + c]);
    }
    if (d < bestD) { bestD = d; best = h.id; }
  }
  *dist = bestD;
  return best;
}
// VRmin Host's settings for the game ("config" line): size of an eye, frames per second, part of the field of view
static std::atomic<int> g_cfgEyeW{0}, g_cfgEyeH{0}, g_cfgFps{0};
static std::atomic<float> g_cfgFovK{1.f};

static void parse_pose(const char *line) {
  const char *p = strstr(line, "\"xr\":");
  const char *idp = strstr(line, "\"id\":");
  if (!p || !idp || !(p = strchr(p, '['))) return;
  p++;
  double v[77];
  int n = 0;
  while (n < 77 && *p && *p != ']') {
    char *end;
    double d = strtod(p, &end);
    if (end == p) { p++; continue; }
    v[n++] = d;
    p = end;
  }
  if (n < 77) return;
  for (double d : v) if (!std::isfinite(d) || std::fabs(d) > 1000) return;
  Track t;
  int k = 0;
  t.id = (uint32_t)strtoul(idp + 5, nullptr, 10);
  t.valid = v[k++] > 0.5;
  for (int e = 0; e < 2; e++) {
    for (int i = 0; i < 7; i++) t.eye[e][i] = v[k++];
    for (int i = 0; i < 4; i++) t.fov[e][i] = v[k++];
  }
  for (int h = 0; h < 2; h++) {
    t.handValid[h] = v[k++] > 0.5;
    for (int i = 0; i < 7; i++) t.grip[h][i] = v[k++];
    for (int i = 0; i < 7; i++) t.aim[h][i] = v[k++];
    for (int i = 0; i < IN_COUNT; i++) t.in[h][i] = (float)v[k++];
  }
  std::lock_guard<std::mutex> g(g_trackLock);
  g_track = t;
  g_poseId = t.id;
  g_handsValid = t.valid && t.handValid[0] && t.handValid[1];
}

static void parse_config(const char *line) {
  const char *p;
  int eye = 0;
  if ((p = strstr(line, "\"eye\":"))) eye = atoi(p + 6);
  if (eye >= 256 && eye <= 4096) { g_cfgEyeW = eye; g_cfgEyeH = eye; }
  if ((p = strstr(line, "\"eyeH\":")) && atoi(p + 7) >= 256 && atoi(p + 7) <= 4096) g_cfgEyeH = atoi(p + 7);
  if ((p = strstr(line, "\"fovk\":")) && atof(p + 7) >= 0.6 && atof(p + 7) <= 1.0) g_cfgFovK = (float)atof(p + 7);
  if ((p = strstr(line, "\"fps\":")) && atoi(p + 6) >= 30 && atoi(p + 6) <= 144) g_cfgFps = atoi(p + 6);
}

// The display's size, rate and field of view can't change once SteamVR has the headset: before adding it, this waits
// (a little) for VRmin Host's settings and the Quest's first pose. Without them the defaults stay.
static void wait_display() {
  Track t;
  for (int i = 0; i < 150; i++) {
    { std::lock_guard<std::mutex> g(g_trackLock); t = g_track; }
    if (g_cfgFps && t.valid) break;
    Sleep(20);
  }
  if (g_cfgFps && g_cfgEyeW) { EYE_W = g_cfgEyeW; EYE_H = g_cfgEyeH; HZ = (float)g_cfgFps; }
  if (t.valid) {
    float k = g_cfgFovK;
    for (int e = 0; e < 2; e++) {
      FOV[e][0] = (float)t.fov[e][0] * k; FOV[e][1] = (float)t.fov[e][1] * k;
      FOV[e][2] = (float)t.fov[e][2] * k; FOV[e][3] = (float)t.fov[e][3] * k;   // (the Quest's down and up)
    }
    double d[3] = {t.eye[1][0] - t.eye[0][0], t.eye[1][1] - t.eye[0][1], t.eye[1][2] - t.eye[0][2]};
    IPD = (float)std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  }
  logf("Display: %ux%u per eye, %.0f Hz, eyes %.1f mm apart, field of view %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f (%s)", EYE_W, EYE_H, HZ,
       IPD * 1000, FOV[0][0], FOV[0][1], FOV[0][2], FOV[0][3], FOV[1][0], FOV[1][1], FOV[1][2], FOV[1][3],
       g_cfgFps && t.valid ? "from VRmin Host and the Quest" : g_cfgFps ? "no pose from the Quest: default field of view" : "VRmin Host didn't answer: defaults");
}

// Control connection: tells VRmin Host a game is running (type 9, its name) and receives its settings and the
// Quest's poses, one JSON line each.
class HostControl {
  std::atomic<bool> running{false};
  std::atomic<SOCKET> sock{INVALID_SOCKET};
  std::thread th;
public:
  void start() {
    running = true;
    th = std::thread([this] {
      static char buf[65536];
      while (running) {
        SOCKET s = host_connect();
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }
        sock = s;
        logf("Connected to VRmin Host");
        send_packet(s, 9, "SteamVR", 7);
        int used = 0, r;
        while (running && (r = recv(s, buf + used, (int)sizeof buf - 1 - used, 0)) > 0) {
          used += r;
          buf[used] = 0;
          char *start = buf, *nl;
          while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            if (strstr(start, "\"xr\":")) parse_pose(start);
            else if (strstr(start, "\"config\"")) parse_config(start);
            start = nl + 1;
          }
          used = (int)(buf + used - start);
          memmove(buf, start, used);
          if (used >= (int)sizeof buf - 1) used = 0;
        }
        sock = INVALID_SOCKET;
        closesocket(s);
        if (running) { logf("VRmin Host disconnected"); Sleep(1000); }
      }
    });
  }
  void stop() {
    running = false;
    SOCKET s = sock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    if (th.joinable()) th.join();
  }
};

// ---- frames to VRmin Host ----
// The compositor's frame is copied on the GPU into 3 textures shared with VRmin Host (DXMT shares them as Metal
// textures with a name VRmin Host opens), so it never leaves the GPU. A small memory-mapped file holds whose turn
// each texture is (0 free, 2 being written, 1 ready for VRmin Host, which sets it back to 0).
static const int SLOTS = 3;
static const uint32_t SHM_MAGIC = 0x31535256;                       // "VRS1"
static const uint32_t GPU_OK = 0x31555047, GPU_FAIL = 0x4c494146;   // "GPU1", "FAIL" (VRmin Host opened them or not)
static const uint32_t SHM_SLOT = 65536;                             // (only floating panels would go there)
struct ShmHdr {
  volatile uint32_t magic, nonce, ack, nslots, slotSize;
  volatile uint32_t reserved[3];   // [0] GPU_OK / GPU_FAIL, [1] texture generation it answers
  volatile LONG state[SLOTS];
};

// D3DKMT, to learn the name DXMT gave a shared texture ("DXMT_shared_resource_…")
typedef UINT KMT_HANDLE;
struct KMT_OPENADAPTERFROMLUID { LUID luid; KMT_HANDLE hAdapter; };
struct KMT_CREATEDEVICE {
  union { KMT_HANDLE hAdapter; void *pAdapter; }; UINT Flags; KMT_HANDLE hDevice; void *pCommandBuffer; UINT CommandBufferSize;
  void *pAllocationList; UINT AllocationListSize; void *pPatchLocationList; UINT PatchLocationListSize;
};
struct KMT_QUERYRESOURCEINFO {
  KMT_HANDLE hDevice; KMT_HANDLE hGlobalShare; void *pPrivateRuntimeData; UINT PrivateRuntimeDataSize;
  UINT TotalPrivateDriverDataSize; UINT ResourcePrivateDriverDataSize; UINT NumAllocations;
};
struct KMT_OPENALLOCATIONINFO2 { KMT_HANDLE hAllocation; const void *pPrivateDriverData; UINT PrivateDriverDataSize; UINT64 GpuVirtualAddress; ULONG_PTR Reserved[6]; };
struct KMT_OPENRESOURCE {
  KMT_HANDLE hDevice; KMT_HANDLE hGlobalShare; UINT NumAllocations; KMT_OPENALLOCATIONINFO2 *pOpenAllocationInfo2;
  void *pPrivateRuntimeData; UINT PrivateRuntimeDataSize; void *pResourcePrivateDriverData; UINT ResourcePrivateDriverDataSize;
  void *pTotalPrivateDriverDataBuffer; UINT TotalPrivateDriverDataBufferSize; KMT_HANDLE hResource;
};
struct KMT_DESTROYALLOCATION { KMT_HANDLE hDevice; KMT_HANDLE hResource; const KMT_HANDLE *phAllocationList; UINT AllocationCount; };
typedef LONG(WINAPI *kmt_fn)(void *);

static DXGI_FORMAT typeless(DXGI_FORMAT f) {
  switch (f) {
  case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
  case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
  default: return f;
  }
}

// A layer drawn over the game's image (SteamVR's own objects and panels): each eye's texture and the part of it used
struct Overlay { ID3D11Texture2D *tex[2]; float rect[2][4]; };

// Draws a texture over part of another (alpha blending): what SteamVR's compositor does with the layers on Windows
static const char *BLEND_HLSL = R"(
cbuffer C : register(b0) { float4 r; };
Texture2D t : register(t0);
SamplerState s : register(s0);
void vs(uint id : SV_VertexID, out float4 p : SV_Position, out float2 uv : TEXCOORD0) {
  float2 q = float2((id << 1) & 2, id & 2);
  p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1);
  uv = r.xy + (r.zw - r.xy) * q;
}
float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }
)";

static DXGI_FORMAT srgb_of(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : f == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : f;
}
static DXGI_FORMAT unorm_of(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : f == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : f;
}
static bool is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

class FrameSender {
  std::mutex lock;
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  bool failed = false;   // (no D3D11 or no shared textures: frames aren't sent)
  // image connection
  SOCKET fs = INVALID_SOCKET;
  double retryAt = 0, askedAt = 0;
  enum { OFFER, WAIT_SHM, WAIT_GPU, ON } stage = OFFER;
  HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
  ShmHdr *shm = nullptr;
  wchar_t path[64] = {};
  // shared textures
  ID3D11Texture2D *tex[SLOTS] = {};
  ID3D11Query *query[SLOTS] = {};
  int pending = -1;   // slot copied, waiting for the GPU
  uint32_t pendingPose = 0, pendingW = 0, pendingH = 0;   // (size of an eye of the frame in the pending slot)
  uint32_t texW = 0, texH = 0, gen = 0;
  DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
  KMT_HANDLE kdev = 0;
  kmt_fn kQuery = nullptr, kOpen = nullptr, kDestroy = nullptr;
public:
  std::atomic<uint64_t> sent{0}, dropped{0};

private:
  // A look at frame 300 and then every 1500: their average color in the log and a picture in
  // C:\questvrbridge\steamvr_cuadro_N.bmp (a quarter of the size), to see what the game draws
  void snapshot(ID3D11Texture2D *s, const D3D11_TEXTURE2D_DESC &sd, int k) {
    D3D11_TEXTURE2D_DESC d = sd;
    d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) { logf("Snapshot: no staging texture"); return; }
    ctx->CopyResource(st, s);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { logf("Snapshot: Map failed"); st->Release(); return; }
    uint32_t w = sd.Width / 4, h = sd.Height / 4;
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    double sum[4] = {};
    bool bgra = typeless(sd.Format) == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t *p = (const uint8_t *)m.pData + (size_t)(y * 4) * m.RowPitch + (size_t)(x * 4) * 4;
        uint8_t *o = px + ((size_t)(h - 1 - y) * w + x) * 4;   // (BMP: bottom row first, BGRA)
        o[0] = bgra ? p[0] : p[2]; o[1] = p[1]; o[2] = bgra ? p[2] : p[0]; o[3] = 255;
        for (int c = 0; c < 4; c++) sum[c] += p[c];
      }
    ctx->Unmap(st, 0);
    st->Release();
    double n = (double)w * h;
    logf("Snapshot: format %d, average of the 4 channels %.1f %.1f %.1f %.1f", (int)sd.Format, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
    char name[64];
    snprintf(name, sizeof name, "C:\\questvrbridge\\steamvr_cuadro_%d.bmp", k);
    FILE *f = fopen(name, "wb");
    if (f) {
      uint32_t size = 54 + w * h * 4;
      uint8_t hd[54] = {'B', 'M'};
      memcpy(hd + 2, &size, 4); hd[10] = 54; hd[14] = 40;
      memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 32;
      fwrite(hd, 1, 54, f);
      fwrite(px, 1, (size_t)w * h * 4, f);
      fclose(f);
    }
    free(px);
  }
  uint64_t copies = 0, lastShot = 0;

  // Layer blending (made the first time a frame has layers; if it can't be made, the layers aren't drawn)
  int blendState = 0;   // 0 not made · 1 ready · -1 not available
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11Buffer *cb = nullptr;
  ID3D11SamplerState *sampler = nullptr;
  ID3D11BlendState *blend = nullptr;
  ID3D11RasterizerState *raster = nullptr;
  bool blend_init() {
    typedef HRESULT(WINAPI * compile_fn)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    if (!m) m = LoadLibraryA("d3dcompiler_43.dll");
    compile_fn compile = m ? (compile_fn)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) { logf("Layers: no shader compiler (d3dcompiler_47)"); return false; }
    ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
    if (FAILED(compile(BLEND_HLSL, strlen(BLEND_HLSL), "blend", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bv, &err)) ||
        FAILED(compile(BLEND_HLSL, strlen(BLEND_HLSL), "blend", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bp, &err))) {
      logf("Layers: the shader didn't compile: %s", err ? (const char *)err->GetBufferPointer() : "?");
      return false;
    }
    bool ok = SUCCEEDED(dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &vs)) &&
              SUCCEEDED(dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &ps));
    bv->Release();
    bp->Release();
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bl = {};
    D3D11_RENDER_TARGET_BLEND_DESC &rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb)) && SUCCEEDED(dev->CreateSamplerState(&sd, &sampler)) &&
         SUCCEEDED(dev->CreateBlendState(&bl, &blend)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &raster));
    if (!ok) logf("Layers: couldn't make the drawing objects");
    else logf("Layers: ready to draw SteamVR's layers over the game's image");
    return ok;
  }
  // Draws each layer over the game's image in the slot (each eye in its half)
  void draw_overlays(ID3D11Texture2D *dst, uint32_t ew, uint32_t eh, const std::vector<Overlay> &over) {
    if (over.empty()) return;
    static int layerShots;
    if (copies > 600 && layerShots < 1) {   // (one look at the first layer over the game's, alone: steamvr_cuadro_3.bmp)
      layerShots++;
      D3D11_TEXTURE2D_DESC d;
      over[0].tex[0]->GetDesc(&d);
      logf("Layer over the game's image: %ux%u, format %d", d.Width, d.Height, (int)d.Format);
      snapshot(over[0].tex[0], d, 3);
    }
    if (!blendState) blendState = blend_init() ? 1 : -1;
    if (blendState < 0) return;
    for (const Overlay &o : over)
      for (int e = 0; e < 2; e++) {
        D3D11_TEXTURE2D_DESC sdsc;
        o.tex[e]->GetDesc(&sdsc);
        // (an sRGB layer is read as linear and written back as sRGB: blending in linear light, as the compositor does)
        D3D11_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = is_srgb(sdsc.Format) ? srgb_of(texFmt) : unorm_of(texFmt);
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = sdsc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || sdsc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? unorm_of(sdsc.Format) : sdsc.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        ID3D11RenderTargetView *rtv = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        if (SUCCEEDED(dev->CreateRenderTargetView(dst, &rv, &rtv)) && SUCCEEDED(dev->CreateShaderResourceView(o.tex[e], &sv, &srv))) {
          D3D11_VIEWPORT vp = {(float)(e * ew), 0, (float)ew, (float)eh, 0, 1};
          ctx->UpdateSubresource(cb, 0, nullptr, o.rect[e], 0, 0);
          ctx->OMSetRenderTargets(1, &rtv, nullptr);
          ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
          ctx->RSSetState(raster);
          ctx->RSSetViewports(1, &vp);
          ctx->IASetInputLayout(nullptr);
          ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          ctx->VSSetShader(vs, nullptr, 0);
          ctx->VSSetConstantBuffers(0, 1, &cb);
          ctx->PSSetShader(ps, nullptr, 0);
          ctx->PSSetShaderResources(0, 1, &srv);
          ctx->PSSetSamplers(0, 1, &sampler);
          ctx->Draw(3, 0);
          ID3D11ShaderResourceView *none = nullptr;
          ctx->PSSetShaderResources(0, 1, &none);
          ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        if (rtv) rtv->Release();
        if (srv) srv->Release();
      }
  }

  bool init_device() {
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { logf("Frames: no D3D11 device (%lx)", (unsigned long)hr); return false; }
    HMODULE gdi = LoadLibraryA("gdi32.dll");
    kmt_fn kOpenAd = (kmt_fn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"), kCreate = (kmt_fn)GetProcAddress(gdi, "D3DKMTCreateDevice");
    kQuery = (kmt_fn)GetProcAddress(gdi, "D3DKMTQueryResourceInfo");
    kOpen = (kmt_fn)GetProcAddress(gdi, "D3DKMTOpenResource2");
    kDestroy = (kmt_fn)GetProcAddress(gdi, "D3DKMTDestroyAllocation");
    if (!kOpenAd || !kCreate || !kQuery || !kOpen || !kDestroy) { logf("Frames: this Wine has no shared resources (D3DKMT)"); return false; }
    IDXGIDevice *dd = nullptr;
    IDXGIAdapter *ad = nullptr;
    DXGI_ADAPTER_DESC desc;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd))) return false;
    hr = dd->GetAdapter(&ad);
    dd->Release();
    if (FAILED(hr)) return false;
    hr = ad->GetDesc(&desc);
    ad->Release();
    if (FAILED(hr)) return false;
    KMT_OPENADAPTERFROMLUID oa = {};
    oa.luid = desc.AdapterLuid;
    if (kOpenAd(&oa)) { logf("Frames: D3DKMTOpenAdapterFromLuid failed"); return false; }
    KMT_CREATEDEVICE cd = {};
    cd.hAdapter = oa.hAdapter;
    if (kCreate(&cd) || !cd.hDevice) { logf("Frames: D3DKMTCreateDevice failed"); return false; }
    kdev = cd.hDevice;
    return true;
  }

  bool shared_name(ID3D11Texture2D *t, char out[56]) {
    IDXGIResource *r = nullptr;
    HANDLE h = nullptr;
    if (FAILED(t->QueryInterface(__uuidof(IDXGIResource), (void **)&r))) return false;
    HRESULT hr = r->GetSharedHandle(&h);
    r->Release();
    if (FAILED(hr) || !h) return false;
    uint8_t data[512] = {};
    KMT_QUERYRESOURCEINFO q = {};
    q.hDevice = kdev; q.hGlobalShare = (KMT_HANDLE)(uintptr_t)h; q.pPrivateRuntimeData = data; q.PrivateRuntimeDataSize = sizeof data;
    if (kQuery(&q) || q.PrivateRuntimeDataSize < 54 || q.PrivateRuntimeDataSize > sizeof data) return false;
    KMT_OPENALLOCATIONINFO2 al = {};
    KMT_OPENRESOURCE o = {};
    o.hDevice = kdev; o.hGlobalShare = q.hGlobalShare; o.NumAllocations = 1; o.pOpenAllocationInfo2 = &al;
    o.pPrivateRuntimeData = data; o.PrivateRuntimeDataSize = q.PrivateRuntimeDataSize;
    if (kOpen(&o)) return false;
    KMT_DESTROYALLOCATION da = {};
    da.hDevice = kdev; da.hResource = o.hResource;
    kDestroy(&da);
    memcpy(out, data, 54);
    out[54] = 0;
    return !strncmp(out, "DXMT_shared_resource_", 21);
  }

  void close_connection(const char *why) {
    if (fs != INVALID_SOCKET) closesocket(fs);
    fs = INVALID_SOCKET;
    stage = OFFER;
    pending = -1;
    retryAt = now_s() + 1;
    if (why) logf("Image connection: %s", why);
  }

  // [5][nonce u32][slot size u32][file's path on the Mac]
  bool offer_shm() {
    if (!shm) {
      uint32_t total = 64 + SHM_SLOT * SLOTS;
      CreateDirectoryW(L"C:\\questvrbridge", nullptr);
      swprintf(path, 64, L"C:\\questvrbridge\\cuadros_%lu.bin", GetCurrentProcessId());
      file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) { logf("Frames: couldn't create the shared file"); return false; }
      map = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, total, nullptr);
      shm = map ? (ShmHdr *)MapViewOfFile(map, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, total) : nullptr;
      if (!shm) { logf("Frames: couldn't map the shared file"); return false; }
    }
    typedef char *(CDECL * unix_name_fn)(LPCWSTR);
    unix_name_fn unixName = (unix_name_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    char *u = unixName ? unixName(path) : nullptr;
    if (!u) { logf("Frames: not running in Wine (VRmin Host on Windows isn't supported yet)"); return false; }
    shm->magic = SHM_MAGIC; shm->nslots = SLOTS; shm->slotSize = SHM_SLOT; shm->ack = 0;
    shm->reserved[0] = shm->reserved[1] = shm->reserved[2] = 0;
    for (auto &s : shm->state) s = 0;
    uint32_t nonce = (GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x5a5a1234u) | 1;
    MemoryBarrier();
    shm->nonce = nonce;
    uint8_t p[8 + 1024];
    uint32_t n = (uint32_t)strlen(u);
    if (n > 1024) n = 1024;
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(nonce >> (24 - 8 * i)); p[4 + i] = (uint8_t)(SHM_SLOT >> (24 - 8 * i)); }
    memcpy(p + 8, u, n);
    HeapFree(GetProcessHeap(), 0, u);
    return send_packet(fs, 5, p, 8 + n);
  }

  void release_textures() {
    for (int i = 0; i < SLOTS; i++) {
      if (tex[i]) tex[i]->Release();
      if (query[i]) query[i]->Release();
      tex[i] = nullptr;
      query[i] = nullptr;
    }
    pending = -1;
    texW = texH = 0;
  }

  // [10][generation u32][width of an eye u16][height u16][format: 0 BGRA, 1 RGBA][3 names of 56]
  // (the textures only grow, rounded up: games that change their size every few seconds, like The Lab, don't make
  //  VRmin Host open new ones each time)
  bool offer_textures(const D3D11_TEXTURE2D_DESC &sd) {
    uint32_t w = std::max(sd.Width / 2, texW / 2), h = std::max(sd.Height, texH);
    w = (w + 127) / 128 * 128; h = (h + 127) / 128 * 128;
    release_textures();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w * 2; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = typeless(sd.Format); d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    uint8_t p[9 + SLOTS * 56] = {};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    for (int i = 0; i < SLOTS; i++) {
      if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex[i])) || FAILED(dev->CreateQuery(&qd, &query[i])) ||
          !shared_name(tex[i], (char *)p + 9 + i * 56)) {
        logf("Frames: couldn't make the shared textures");
        release_textures();
        return false;
      }
    }
    texW = d.Width; texH = d.Height; texFmt = d.Format; gen++;
    shm->reserved[0] = 0; shm->reserved[1] = gen;
    MemoryBarrier();
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(gen >> (24 - 8 * i));
    p[4] = (uint8_t)(texW / 2 >> 8); p[5] = (uint8_t)(texW / 2); p[6] = (uint8_t)(texH >> 8); p[7] = (uint8_t)texH;
    p[8] = texFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ? 0 : 1;
    logf("Frames: shared textures offered to VRmin Host (%ux%u per eye)", texW / 2, texH);
    return send_packet(fs, 10, p, sizeof p);
  }

  // [11][slot][pose id u32][format][width of an eye u16][height u16]
  bool notify(int slot, uint32_t pose, uint32_t ew, uint32_t eh) {
    uint8_t p[10] = {(uint8_t)slot, (uint8_t)(pose >> 24), (uint8_t)(pose >> 16), (uint8_t)(pose >> 8), (uint8_t)pose, (uint8_t)(texFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ? 0 : 1),
                     (uint8_t)(ew >> 8), (uint8_t)ew, (uint8_t)(eh >> 8), (uint8_t)eh};
    return send_packet(fs, 11, p, sizeof p);
  }

  // Connects and offers the shared file and textures, one step per frame. True when frames can go.
  bool ready(const D3D11_TEXTURE2D_DESC &sd) {
    double t = now_s();
    if (fs == INVALID_SOCKET) {
      if (t < retryAt) return false;
      retryAt = t + 1;
      fs = host_connect();
      if (fs == INVALID_SOCKET) return false;
      int size = 16 << 20;
      setsockopt(fs, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof size);
      if (!send_packet(fs, 4, "F", 1)) { close_connection("lost"); return false; }   // "this connection is for images"
      stage = OFFER;
    }
    switch (stage) {
    case OFFER:
      if (!offer_shm()) { failed = true; close_connection(nullptr); return false; }
      stage = WAIT_SHM;
      askedAt = t;
      return false;
    case WAIT_SHM:
      if (shm->ack == shm->nonce) {
        if (!offer_textures(sd)) { failed = true; close_connection(nullptr); return false; }
        stage = WAIT_GPU;
        askedAt = t;
      } else if (t - askedAt > 3) close_connection("VRmin Host didn't open the shared file; trying again");
      return false;
    case WAIT_GPU:
      if (shm->reserved[1] == gen && shm->reserved[0] == GPU_OK) { stage = ON; logf("Frames: sending to VRmin Host"); return true; }
      if (shm->reserved[1] == gen && shm->reserved[0] == GPU_FAIL) close_connection("VRmin Host couldn't open the shared textures; trying again");
      else if (t - askedAt > 3) close_connection("VRmin Host didn't answer about the shared textures; trying again");
      return false;
    case ON:
      if (sd.Width > texW || sd.Height > texH || typeless(sd.Format) != texFmt) {
        if (!offer_textures(sd)) { failed = true; close_connection(nullptr); return false; }
        stage = WAIT_GPU;
        askedAt = t;
        return false;
      }
      return true;
    }
    return false;
  }

public:
  std::mutex &mutex() { return lock; }
  // (with mutex() held) D3D11 device of the driver, made the first time
  ID3D11Device *device() {
    if (!dev && !failed && !init_device()) failed = true;
    return failed ? nullptr : dev;
  }

  // (with mutex() held) Copies each eye's part of the game's frame next to each other into a free shared texture
  // (the GPU does it while SteamVR goes on), drawn with the Quest's pose «pose»
  void copy(ID3D11Texture2D *src[2], const D3D11_BOX box[2], DXGI_FORMAT fmt, uint32_t pose, const std::vector<Overlay> &over) {
    if (!device()) return;
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = (box[0].right - box[0].left) * 2; sd.Height = box[0].bottom - box[0].top; sd.Format = fmt;
    if (!ready(sd)) return;
    if (pending >= 0) finish();
    int slot = -1;
    for (int i = 0; i < SLOTS && slot < 0; i++) if (shm->state[i] == 0) slot = i;
    if (slot < 0) { dropped++; return; }   // (VRmin Host is still busy with the 3)
    shm->state[slot] = 2;
    for (int e = 0; e < 2; e++) {
      D3D11_BOX b = box[e];
      b.right = b.left + sd.Width / 2; b.bottom = b.top + sd.Height;   // (the two eyes the same size)
      ctx->CopySubresourceRegion(tex[slot], 0, e * sd.Width / 2, 0, 0, src[e], 0, &b);
    }
    draw_overlays(tex[slot], sd.Width / 2, sd.Height, over);
    ctx->End(query[slot]);
    ctx->Flush();
    pending = slot;
    pendingPose = pose;
    pendingW = sd.Width / 2;
    pendingH = sd.Height;
    ++copies;
    if (copies == 300 || (copies > 1500 && copies - lastShot >= 1500 && g_handsValid)) {   // (1: at the start; 2: every ~30 s, with the controllers seen)
      if (copies != 300) lastShot = copies;
      D3D11_TEXTURE2D_DESC d;
      tex[slot]->GetDesc(&d);
      snapshot(tex[slot], d, copies == 300 ? 1 : 2);
    }
  }

  // (with mutex() held) When the GPU finished the copy, VRmin Host is told
  void finish() {
    if (pending < 0 || fs == INVALID_SOCKET) return;
    BOOL done = FALSE;
    for (double until = now_s() + 0.02; ctx->GetData(query[pending], &done, sizeof done, 0) != S_OK;) {
      if (now_s() > until) return;   // (checked again with the next frame)
      Sleep(0);
    }
    int slot = pending;
    pending = -1;
    MemoryBarrier();
    shm->state[slot] = 1;
    if (!notify(slot, pendingPose, pendingW, pendingH)) { shm->state[slot] = 0; close_connection("lost; trying again"); return; }
    sent++;
  }
};

// SteamVR's direct mode: SteamVR asks the driver for the textures the game's frames go into (3 per set, shared with
// SteamVR's processes), and each frame says which ones have the game's eyes (SubmitLayer) and when it's complete
// (Present). Layers after the first (SteamVR's own panels) aren't shown yet.
class VRminDirectMode : public IVRDriverDirectModeComponent {
  FrameSender frames;
  struct Set { uint32_t pid; SharedTextureHandle_t handle[3]; ID3D11Texture2D *tex[3]; bool compositor; };
  std::vector<Set> sets;
  // The layers of the frame being submitted and of the previous one: the game's image (the first layer from a game's
  // textures) and the ones after it
  struct Layer { SubmitLayerPerEye_t eye[2]; };
  std::vector<Layer> layers, prevLayers;
  uint64_t presented = 0, lastPresented = 0, lastSent = 0, gameFrames = 0;
  double t0 = now_s(), lastLog = 0;

  Set *find(SharedTextureHandle_t h, int *i) {
    for (Set &st : sets)
      for (*i = 0; *i < 3; (*i)++) if (st.handle[*i] == h) return &st;
    return nullptr;
  }
  // SteamVR's own frames (vrcompositor.exe, while a game loads or misses a frame) come out a single color inside VRmin's
  // Windows (its drawing doesn't work there): they aren't sent, and the Quest keeps showing the game's last one
  static bool is_compositor(uint32_t pid) {
    char path[MAX_PATH] = "";
    DWORD n = MAX_PATH;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) { QueryFullProcessImageNameA(p, 0, path, &n); CloseHandle(p); }
    return strstr(path, "vrcompositor") != nullptr;
  }

public:
  void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out) override {
    std::lock_guard<std::mutex> g(frames.mutex());
    memset(out, 0, sizeof *out);
    ID3D11Device *dev = frames.device();
    if (!dev) return;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = desc->nWidth; d.Height = desc->nHeight; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = (DXGI_FORMAT)desc->nFormat; d.SampleDesc.Count = desc->nSampleCount ? desc->nSampleCount : 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    Set st = {pid, {}, {}, is_compositor(pid)};
    for (int i = 0; i < 3; i++) {
      IDXGIResource *r = nullptr;
      HANDLE h = nullptr;
      if (FAILED(dev->CreateTexture2D(&d, nullptr, &st.tex[i])) || FAILED(st.tex[i]->QueryInterface(__uuidof(IDXGIResource), (void **)&r)) ||
          FAILED(r->GetSharedHandle(&h))) {
        logf("Direct mode: couldn't make a texture for SteamVR (%ux%u, format %u)", d.Width, d.Height, desc->nFormat);
        if (r) r->Release();
        for (int j = 0; j <= i; j++) if (st.tex[j]) st.tex[j]->Release();
        return;
      }
      r->Release();
      st.handle[i] = out->rSharedTextureHandles[i] = (SharedTextureHandle_t)(uintptr_t)h;
    }
    sets.push_back(st);
    logf("Direct mode: textures for process %u%s: %ux%u, format %u, %u samples", pid, st.compositor ? " (SteamVR's)" : "", d.Width, d.Height,
         desc->nFormat, d.SampleDesc.Count);
  }
  void DestroySwapTextureSet(SharedTextureHandle_t h) override {
    std::lock_guard<std::mutex> g(frames.mutex());
    prevLayers.clear();
    for (size_t i = 0; i < sets.size(); i++)
      for (int j = 0; j < 3; j++)
        if (sets[i].handle[j] == h) {
          for (ID3D11Texture2D *t : sets[i].tex) t->Release();
          sets.erase(sets.begin() + i);
          return;
        }
  }
  void DestroyAllSwapTextureSets(uint32_t pid) override {
    std::lock_guard<std::mutex> g(frames.mutex());
    prevLayers.clear();
    for (size_t i = 0; i < sets.size();)
      if (sets[i].pid == pid) {
        for (ID3D11Texture2D *t : sets[i].tex) t->Release();
        sets.erase(sets.begin() + i);
      } else i++;
  }
  void GetNextSwapTextureSetIndex(SharedTextureHandle_t h[2], uint32_t (*indices)[2]) override {
    std::lock_guard<std::mutex> g(frames.mutex());
    for (int e = 0; e < 2; e++) {
      (*indices)[e] = 0;
      for (Set &st : sets)
        for (int i = 0; i < 3; i++)
          if (st.handle[i] == h[e]) { (*indices)[e] = (i + 1) % 3; }
    }
  }
  void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override {
    std::lock_guard<std::mutex> g(frames.mutex());
    if (layers.size() < 16) layers.push_back({{perEye[0], perEye[1]}});
  }
  void Present(SharedTextureHandle_t) override {
    std::lock_guard<std::mutex> g(frames.mutex());
    uint64_t n = ++presented;
    // The game's frame is copied one frame later: nothing tells this process when the game's GPU finished drawing it
    // (in Windows SteamVR's sync texture does; in VRmin's Windows it doesn't), and copied right away it sometimes
    // still had the image from 3 frames before
    std::vector<Layer> now;
    now.swap(prevLayers);
    prevLayers.swap(layers);
    // The game's image: the first layer from a game's textures. SteamVR's own image under it (vrcompositor.exe's) comes
    // out a single color inside VRmin's Windows and isn't used; the layers after it are drawn over it
    int base = -1;
    for (size_t k = 0; k < now.size() && base < 0; k++) {
      int i;
      Set *st = find(now[k].eye[0].hTexture, &i);
      if (st && !st->compositor) base = (int)k;
    }
    bool go = base >= 0;
    SubmitLayerPerEye_t use[2];
    std::vector<Overlay> over;
    if (go) {
      use[0] = now[base].eye[0];
      use[1] = now[base].eye[1];
      for (size_t k = base + 1; k < now.size(); k++) {
        Overlay o;
        bool ok = true;
        for (int e = 0; e < 2; e++) {
          int i;
          Set *st = find(now[k].eye[e].hTexture, &i);
          if (!st) { ok = false; break; }
          o.tex[e] = st->tex[i];
          const VRTextureBounds_t &tb = now[k].eye[e].bounds;
          o.rect[e][0] = tb.uMin; o.rect[e][1] = tb.vMin; o.rect[e][2] = tb.uMax; o.rect[e][3] = tb.vMax;
        }
        if (ok) over.push_back(o);
      }
    }
    if (go) {
      int i0, i1;
      Set *s0 = find(use[0].hTexture, &i0), *s1 = find(use[1].hTexture, &i1);
      ID3D11Texture2D *src[2] = {s0 ? s0->tex[i0] : nullptr, s1 ? s1->tex[i1] : nullptr};
      if (src[0] && src[1]) {
        D3D11_BOX box[2];
        D3D11_TEXTURE2D_DESC d;
        for (int e = 0; e < 2; e++) {
          src[e]->GetDesc(&d);
          const VRTextureBounds_t &b = use[e].bounds;
          float v0 = std::min(b.vMin, b.vMax), v1 = std::max(b.vMin, b.vMax);
          box[e] = {(UINT)(b.uMin * d.Width + 0.5f), (UINT)(v0 * d.Height + 0.5f), 0, (UINT)(b.uMax * d.Width + 0.5f), (UINT)(v1 * d.Height + 0.5f), 1};
        }
        double dist;
        uint32_t pose = pose_for(use[0].mHmdPose, &dist);
        if (++gameFrames == 1 || gameFrames % 500 == 0) {
          // The projection the game drew each eye with, as tangents (left, right, top, bottom: GetProjectionRaw's)
          for (int e = 0; e < 2; e++) {
            const HmdMatrix44_t &P = use[e].mProjection;
            float l = (P.m[0][2] - 1) / P.m[0][0], r = (P.m[0][2] + 1) / P.m[0][0];
            float tp = (P.m[1][2] - 1) / P.m[1][1], bt = (P.m[1][2] + 1) / P.m[1][1];
            logf("  eye %d drawn with %.3f %.3f %.3f %.3f (projection %.3f %.3f %.3f %.3f)", e, l, r, tp, bt, P.m[0][0], P.m[0][2], P.m[1][1], P.m[1][2]);
          }
          const HmdMatrix34_t &m = use[0].mHmdPose;
          logf("  head looking towards %.2f %.2f %.2f", -m.m[0][2], -m.m[1][2], -m.m[2][2]);
          logf("Direct mode: game frame %llu, eyes %ux%u (bounds %.2f-%.2f, %.2f-%.2f), format %d; drawn at %.3f %.3f %.3f: Quest pose %u "
               "(the latest is %u), difference %.4f", (unsigned long long)gameFrames, box[0].right - box[0].left, box[0].bottom - box[0].top,
               use[0].bounds.uMin, use[0].bounds.uMax, use[0].bounds.vMin, use[0].bounds.vMax, (int)d.Format, m.m[0][3], m.m[1][3], m.m[2][3],
               pose, g_poseId.load(), dist);
        }
        if (gameFrames == 1 || gameFrames % 500 == 0) {
          std::string who;
          for (size_t k = 0; k < now.size(); k++) {
            int i;
            Set *st = find(now[k].eye[0].hTexture, &i);
            who += !st ? " ?" : st->compositor ? " SteamVR" : " game";
          }
          logf("  layers:%s (drawing %zu over the game's)", who.c_str(), over.size());
        }
        frames.copy(src, box, typeless(d.Format), pose, over);
      }
    }
    double t = now_s();
    if (t - lastLog > 5) {
      double dt = t - (lastLog ? lastLog : t0);
      logf("SteamVR: %.0f frames/s, %.0f sent to VRmin Host (%llu dropped so far)", (n - lastPresented) / dt,
           (frames.sent - lastSent) / dt, (unsigned long long)frames.dropped.load());
      lastLog = t;
      lastPresented = n;
      lastSent = frames.sent;
    }
  }
  // After Present: VRmin Host is told about the frame, then this waits for the headset's next vsync
  void PostPresent(const Throttling_t *) override {
    { std::lock_guard<std::mutex> g(frames.mutex()); frames.finish(); }
    double period = 1.0 / HZ, t = now_s() - t0;
    double next = (std::floor(t / period) + 1) * period;
    while ((t = now_s() - t0) < next) Sleep((DWORD)((next - t) * 1000 > 1 ? (next - t) * 1000 - 1 : 0));
  }
};

class VRminHmd : public ITrackedDeviceServerDriver {
  VRminDisplay display;
  VRminDirectMode directMode;
  VRInputComponentHandle_t proximity = 0;
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
    VRProperties()->SetBoolProperty(c, Prop_HasDriverDirectModeComponent_Bool, true);
    VRProperties()->SetStringProperty(c, Prop_DriverProvidedChaperoneJson_String, CHAPERONE_JSON);
    VRProperties()->SetBoolProperty(c, Prop_DriverProvidedChaperoneVisibility_Bool, false);
    VRProperties()->SetInt32Property(c, Prop_ExpectedControllerCount_Int32, 2);
    // The headset is on (the Quest only sends poses while it's worn): SteamVR looks for /proximity
    VRProperties()->SetBoolProperty(c, Prop_ContainsProximitySensor_Bool, true);
    VRDriverInput()->CreateBooleanComponent(c, "/proximity", &proximity);
    VRDriverInput()->UpdateBooleanComponent(proximity, true, 0);
    logf("Headset activated (device %u): %ux%u per eye, %.0f Hz", id, EYE_W, EYE_H, HZ);
    return VRInitError_None;
  }
  void Deactivate() override { id = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *name) override {
    if (!strcmp(name, IVRDisplayComponent_Version)) return &display;   // (see VRminDisplay)
    if (!strcmp(name, IVRDriverDirectModeComponent_Version)) return &directMode;
    return nullptr;
  }
  void DebugRequest(const char *, char *buf, uint32_t size) override { if (size) buf[0] = 0; }
  DriverPose_t GetPose() override { return pose_at(0, 1.6, 0); }
  // The head: between the two eyes, turned like them
  void update(const Track &t) {
    if (id == k_unTrackedDeviceIndexInvalid) return;
    DriverPose_t p = t.valid ? pose_at((t.eye[0][0] + t.eye[1][0]) / 2, (t.eye[0][1] + t.eye[1][1]) / 2, (t.eye[0][2] + t.eye[1][2]) / 2, t.eye[0] + 3)
                             : GetPose();
    if (t.valid) remember_head(t.id, p);
    VRServerDriverHost()->TrackedDevicePoseUpdated(id, p, sizeof p);
  }
};

// ---- controllers (Touch, so SteamVR uses Valve's Touch bindings for each game) ----
class VRminController : public ITrackedDeviceServerDriver {
  bool left;
  VRInputComponentHandle_t joyX = 0, joyY = 0, joyClick = 0, joyTouch = 0, trigger = 0, triggerClick = 0, triggerTouch = 0,
                           grip = 0, gripTouch = 0, lower = 0, lowerTouch = 0, upper = 0, upperTouch = 0, system = 0;
public:
  uint32_t id = k_unTrackedDeviceIndexInvalid;
  explicit VRminController(bool l) : left(l) {}
  EVRInitError Activate(uint32_t objectId) override {
    PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(objectId);
    // (like SteamVR's own Touch for Quest 2: {oculus}/input/touch_profile.json's simulation_settings, which older games
    //  that use SteamVR's legacy input read: buttons, axes, model)
    VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, left ? "Oculus Quest2 (Left Controller)" : "Oculus Quest2 (Right Controller)");
    VRProperties()->SetStringProperty(c, Prop_SerialNumber_String, left ? "VRMIN-LEFT" : "VRMIN-RIGHT");
    VRProperties()->SetStringProperty(c, Prop_RegisteredDeviceType_String, left ? "oculus/VRMIN_Controller_Left" : "oculus/VRMIN_Controller_Right");
    VRProperties()->SetStringProperty(c, Prop_AttachedDeviceId_String, left ? "VRMIN-LEFT" : "VRMIN-RIGHT");
    VRProperties()->SetUint64Property(c, Prop_SupportedButtons_Uint64, (1ull << k_EButton_System) | (1ull << k_EButton_ApplicationMenu) | (1ull << k_EButton_Grip) |
                                                                         (1ull << k_EButton_A) | (1ull << k_EButton_SteamVR_Touchpad) | (1ull << k_EButton_SteamVR_Trigger) |
                                                                         (1ull << k_EButton_Axis2));
    VRProperties()->SetInt32Property(c, Prop_Axis0Type_Int32, k_eControllerAxis_Joystick);
    VRProperties()->SetInt32Property(c, Prop_Axis1Type_Int32, k_eControllerAxis_Trigger);
    VRProperties()->SetInt32Property(c, Prop_Axis2Type_Int32, k_eControllerAxis_Trigger);
    VRProperties()->SetBoolProperty(c, Prop_DeviceIsWireless_Bool, true);
    VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);
    VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "Oculus");
    VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "oculus");
    VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right");
    VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "oculus_touch");
    VRProperties()->SetStringProperty(c, Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
    VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32, left ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand);
    VRProperties()->SetInt32Property(c, Prop_DeviceClass_Int32, TrackedDeviceClass_Controller);
    VRProperties()->SetUint64Property(c, Prop_CurrentUniverseId_Uint64, UNIVERSE);
    // The inputs of {oculus}/input/touch_profile.json (A and B on the right, X and Y on the left)
    IVRDriverInput *in = VRDriverInput();
    in->CreateScalarComponent(c, "/input/joystick/x", &joyX, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
    in->CreateScalarComponent(c, "/input/joystick/y", &joyY, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
    in->CreateBooleanComponent(c, "/input/joystick/click", &joyClick);
    in->CreateBooleanComponent(c, "/input/joystick/touch", &joyTouch);
    in->CreateScalarComponent(c, "/input/trigger/value", &trigger, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
    in->CreateBooleanComponent(c, "/input/trigger/click", &triggerClick);
    in->CreateBooleanComponent(c, "/input/trigger/touch", &triggerTouch);
    in->CreateScalarComponent(c, "/input/grip/value", &grip, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
    in->CreateBooleanComponent(c, "/input/grip/touch", &gripTouch);
    in->CreateBooleanComponent(c, left ? "/input/x/click" : "/input/a/click", &lower);
    in->CreateBooleanComponent(c, left ? "/input/x/touch" : "/input/a/touch", &lowerTouch);
    in->CreateBooleanComponent(c, left ? "/input/y/click" : "/input/b/click", &upper);
    in->CreateBooleanComponent(c, left ? "/input/y/touch" : "/input/b/touch", &upperTouch);
    if (left) in->CreateBooleanComponent(c, "/input/system/click", &system);   // (the Quest's menu button)
    id = objectId;   // (after its inputs: from here on the pose thread updates it)
    logf("%s controller activated (device %u)", left ? "Left" : "Right", id);
    return VRInitError_None;
  }
  void Deactivate() override { id = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *) override { return nullptr; }
  void DebugRequest(const char *, char *buf, uint32_t size) override { if (size) buf[0] = 0; }
  DriverPose_t GetPose() override { return pose_at(left ? -0.2 : 0.2, 1.2, -0.3); }
  // The controller where the Quest's is (its aim pose: it points where the Quest's ray does) and its buttons
  void update(const Track &t) {
    if (id == k_unTrackedDeviceIndexInvalid) return;
    int h = left ? 0 : 1;
    DriverPose_t p;
    if (!t.valid) p = GetPose();
    else if (t.handValid[h]) p = pose_at(t.aim[h][0], t.aim[h][1], t.aim[h][2], t.aim[h] + 3);
    else { p = GetPose(); p.poseIsValid = false; p.result = TrackingResult_Running_OutOfRange; }   // (put down or out of sight)
    VRServerDriverHost()->TrackedDevicePoseUpdated(id, p, sizeof p);
    if (!t.valid) return;
    const float *v = t.in[h];
    IVRDriverInput *in = VRDriverInput();
    in->UpdateScalarComponent(joyX, v[IN_TX], 0);
    in->UpdateScalarComponent(joyY, v[IN_TY], 0);
    in->UpdateBooleanComponent(joyClick, v[IN_TCLICK] > 0.5f, 0);
    in->UpdateBooleanComponent(joyTouch, v[IN_THTOUCH] > 0.5f, 0);
    in->UpdateScalarComponent(trigger, v[IN_TRIGGER], 0);
    in->UpdateBooleanComponent(triggerClick, v[IN_TRIGGER] > 0.9f, 0);
    in->UpdateBooleanComponent(triggerTouch, v[IN_TTOUCH] > 0.5f || v[IN_TRIGGER] > 0.05f, 0);
    in->UpdateScalarComponent(grip, v[IN_SQUEEZE], 0);
    in->UpdateBooleanComponent(gripTouch, v[IN_SQUEEZE] > 0.05f, 0);
    in->UpdateBooleanComponent(lower, v[IN_A] > 0.5f, 0);
    in->UpdateBooleanComponent(lowerTouch, v[IN_ATOUCH] > 0.5f || v[IN_A] > 0.5f, 0);
    in->UpdateBooleanComponent(upper, v[IN_B] > 0.5f, 0);
    in->UpdateBooleanComponent(upperTouch, v[IN_BTOUCH] > 0.5f || v[IN_B] > 0.5f, 0);
    if (left) in->UpdateBooleanComponent(system, v[IN_MENU] > 0.5f, 0);
  }
};

// ---- the driver ----
class VRminProvider : public IServerTrackedDeviceProvider {
  VRminHmd hmd;
  VRminController leftHand{true}, rightHand{false};
  HostControl host;
  std::atomic<bool> running{false};
  std::thread poses;
public:
  EVRInitError Init(IVRDriverContext *ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    logf("VRmin driver for SteamVR starting");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    host.start();
    wait_display();
    if (!VRServerDriverHost()->TrackedDeviceAdded("VRMIN-HMD", TrackedDeviceClass_HMD, &hmd)) logf("SteamVR didn't take the headset");
    VRServerDriverHost()->TrackedDeviceAdded("VRMIN-LEFT", TrackedDeviceClass_Controller, &leftHand);
    VRServerDriverHost()->TrackedDeviceAdded("VRMIN-RIGHT", TrackedDeviceClass_Controller, &rightHand);
    running = true;
    // Each new pose from the Quest goes to SteamVR (without the Quest: every 11 ms, the fixed ones)
    poses = std::thread([this] {
      uint32_t last = 0;
      double sentAt = 0, newAt = 0;
      while (running) {
        Track t;
        { std::lock_guard<std::mutex> g(g_trackLock); t = g_track; }
        double now = now_s();
        if (t.id != last || now - sentAt > 0.011) {
          if (t.id != last) {
            if (!last) logf("First pose from the Quest (%u)", t.id);
            last = t.id;
            newAt = now;
          } else if (now - newAt > 0.5) t.valid = false;   // (no poses: the Quest isn't there)
          sentAt = now;
          static double logAt;
          if (t.valid && now - logAt > 5) {
            logAt = now;
            logf("Quest pose %u: head %.3f %.3f %.3f, left hand %s %.3f %.3f %.3f, right hand %s %.3f %.3f %.3f", t.id, (t.eye[0][0] + t.eye[1][0]) / 2,
                 (t.eye[0][1] + t.eye[1][1]) / 2, (t.eye[0][2] + t.eye[1][2]) / 2, t.handValid[0] ? "yes" : "no", t.aim[0][0], t.aim[0][1], t.aim[0][2],
                 t.handValid[1] ? "yes" : "no", t.aim[1][0], t.aim[1][1], t.aim[1][2]);
            // What SteamVR has for the three devices
            TrackedDevicePose_t sv[3] = {};
            VRServerDriverHost()->GetRawTrackedDevicePoses(0, sv, 3);
            for (int i = 0; i < 3; i++)
              logf("  SteamVR device %d: valid %d, connected %d, tracking %d, at %.3f %.3f %.3f", i, sv[i].bPoseIsValid, sv[i].bDeviceIsConnected,
                   (int)sv[i].eTrackingResult, sv[i].mDeviceToAbsoluteTracking.m[0][3], sv[i].mDeviceToAbsoluteTracking.m[1][3],
                   sv[i].mDeviceToAbsoluteTracking.m[2][3]);
          }
          hmd.update(t);
          leftHand.update(t);
          rightHand.update(t);
        }
        Sleep(1);
      }
    });
    return VRInitError_None;
  }
  void Cleanup() override {
    running = false;
    if (poses.joinable()) poses.join();
    host.stop();
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
