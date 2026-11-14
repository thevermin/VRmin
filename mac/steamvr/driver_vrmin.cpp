// VRmin's driver for SteamVR (Windows; on the Mac it runs inside VRmin's Windows).
//
// SteamVR loads it in vrserver.exe and sees a headset and two Touch controllers: their properties and a fixed pose
// (standing, looking ahead), the play area provided by the driver so SteamVR doesn't ask for room setup, and a
// virtual display that sends the compositor's frames to VRmin Host at the headset's rate. The Quest's poses and
// buttons and vibration come next.
//
// Log: C:\questvrbridge\driver_vrmin.log (and SteamVR's vrserver.txt)
#include <winsock2.h>
#include <openvr_driver.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
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
  virtual void GetProjectionRaw(EVREye, float *l, float *r, float *t, float *b) { *l = -FOV_L; *r = FOV_R; *t = -FOV_U; *b = FOV_D; }
  // (no lens: the Quest corrects its own)
  virtual DistortionCoordinates_t *ComputeDistortion(DistortionCoordinates_t *c, EVREye, float u, float v) {
    c->rfRed[0] = c->rfGreen[0] = c->rfBlue[0] = u;
    c->rfRed[1] = c->rfGreen[1] = c->rfBlue[1] = v;
    return c;
  }
  virtual bool ComputeInverseDistortion(HmdVector2_t *r, EVREye, uint32_t, float u, float v) {
    r->v[0] = u;
    r->v[1] = v;
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

// Control connection: tells VRmin Host a game is running (type 9, its name). What VRmin Host sends back (the Quest's
// poses, settings) is read and, for now, not used.
class HostControl {
  std::atomic<bool> running{false};
  std::atomic<SOCKET> sock{INVALID_SOCKET};
  std::thread th;
public:
  void start() {
    running = true;
    th = std::thread([this] {
      char buf[65536];
      while (running) {
        SOCKET s = host_connect();
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }
        sock = s;
        logf("Connected to VRmin Host");
        send_packet(s, 9, "SteamVR", 7);
        while (running && recv(s, buf, sizeof buf, 0) > 0) {}
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
  KMT_HANDLE hAdapter; UINT Flags; KMT_HANDLE hDevice; void *pCommandBuffer; UINT CommandBufferSize;
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

class FrameSender {
  std::mutex lock;
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  bool failed = false;   // (no D3D11 or no shared textures: frames aren't sent)
  // compositor's textures, opened once
  SharedTextureHandle_t srcHandle[4] = {};
  ID3D11Texture2D *src[4] = {};
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
  uint32_t texW = 0, texH = 0, gen = 0;
  DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
  KMT_HANDLE kdev = 0;
  kmt_fn kQuery = nullptr, kOpen = nullptr, kDestroy = nullptr;
public:
  std::atomic<uint64_t> sent{0}, dropped{0};

private:
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

  ID3D11Texture2D *open_source(SharedTextureHandle_t h) {
    for (int i = 0; i < 4; i++) if (src[i] && srcHandle[i] == h) return src[i];
    int i = 0;
    while (i < 4 && src[i]) i++;
    if (i == 4) { for (auto &t : src) { t->Release(); t = nullptr; } i = 0; }   // (the compositor made new ones)
    if (FAILED(dev->OpenSharedResource((HANDLE)(uintptr_t)h, __uuidof(ID3D11Texture2D), (void **)&src[i]))) {
      src[i] = nullptr;
      logf("Frames: couldn't open the compositor's texture %llu", (unsigned long long)h);
      return nullptr;
    }
    srcHandle[i] = h;
    D3D11_TEXTURE2D_DESC d;
    src[i]->GetDesc(&d);
    logf("Compositor's texture %llu: %ux%u, format %d", (unsigned long long)h, d.Width, d.Height, (int)d.Format);
    return src[i];
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
  bool offer_textures(const D3D11_TEXTURE2D_DESC &sd) {
    release_textures();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = typeless(sd.Format); d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
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
    texW = sd.Width; texH = sd.Height; texFmt = d.Format; gen++;
    shm->reserved[0] = 0; shm->reserved[1] = gen;
    MemoryBarrier();
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(gen >> (24 - 8 * i));
    p[4] = (uint8_t)(texW / 2 >> 8); p[5] = (uint8_t)(texW / 2); p[6] = (uint8_t)(texH >> 8); p[7] = (uint8_t)texH;
    p[8] = texFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ? 0 : 1;
    logf("Frames: shared textures offered to VRmin Host (%ux%u per eye)", texW / 2, texH);
    return send_packet(fs, 10, p, sizeof p);
  }

  // [11][slot][pose id u32][format][width of an eye u16][height u16]
  bool notify(int slot) {
    uint8_t p[10] = {(uint8_t)slot, 0, 0, 0, 0, (uint8_t)(texFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ? 0 : 1),
                     (uint8_t)(texW / 2 >> 8), (uint8_t)(texW / 2), (uint8_t)(texH >> 8), (uint8_t)texH};
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
      if (sd.Width != texW || sd.Height != texH || typeless(sd.Format) != texFmt) {
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
  // From Present: copies the frame into a free shared texture (the GPU does it while the compositor goes on)
  void copy(SharedTextureHandle_t h) {
    std::lock_guard<std::mutex> g(lock);
    if (failed) return;
    if (!dev && !init_device()) { failed = true; return; }
    ID3D11Texture2D *s = open_source(h);
    if (!s) return;
    D3D11_TEXTURE2D_DESC sd;
    s->GetDesc(&sd);
    if (!ready(sd)) return;
    if (pending >= 0) finish();
    int slot = -1;
    for (int i = 0; i < SLOTS && slot < 0; i++) if (shm->state[i] == 0) slot = i;
    if (slot < 0) { dropped++; return; }   // (VRmin Host is still busy with the 3)
    shm->state[slot] = 2;
    ctx->CopyResource(tex[slot], s);
    ctx->End(query[slot]);
    ctx->Flush();
    pending = slot;
  }

  // From WaitForPresent: when the GPU finished the copy, VRmin Host is told
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
    if (!notify(slot)) { shm->state[slot] = 0; close_connection("lost; trying again"); return; }
    sent++;
  }
  void finish_locked() {
    std::lock_guard<std::mutex> g(lock);
    finish();
  }
};

class VRminVirtualDisplay : public IVRVirtualDisplay {
  FrameSender frames;
  std::atomic<uint64_t> presented{0};
  double t0 = now_s(), lastLog = 0;
  uint64_t lastPresented = 0, lastSent = 0;
public:
  void Present(const PresentInfo_t *info, uint32_t) override {
    uint64_t n = ++presented;
    if (n == 1) logf("First frame from the compositor (texture %llu)", (unsigned long long)info->backbufferTextureHandle);
    frames.copy(info->backbufferTextureHandle);
    double t = now_s();
    if (t - lastLog > 5) {
      double dt = t - (lastLog ? lastLog : t0);
      logf("Compositor: %.0f frames/s, %.0f sent to VRmin Host (%llu dropped so far)", (n - lastPresented) / dt,
           (frames.sent - lastSent) / dt, (unsigned long long)frames.dropped.load());
      lastLog = t;
      lastPresented = n;
      lastSent = frames.sent;
    }
  }
  // The compositor waits here for the next vsync of the headset: VRmin Host is told about the frame, then it sleeps
  void WaitForPresent() override {
    frames.finish_locked();
    double period = 1.0 / HZ, t = now_s() - t0;
    double next = (std::floor(t / period) + 1) * period;
    while ((t = now_s() - t0) < next) Sleep((DWORD)((next - t) * 1000 > 1 ? (next - t) * 1000 - 1 : 0));
  }
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
    if (!strcmp(name, IVRDisplayComponent_Version)) return &display;   // (see VRminDisplay)
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
