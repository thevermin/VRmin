// Codificador de video por hardware con Media Foundation (NVIDIA, AMD o Intel), HEVC o H.264.
// Si la PC no tiene uno por hardware, usa el H.264 de Windows (en el procesador).
#include <initguid.h>
#include "host.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

#ifndef MF_E_TRANSFORM_STREAM_CHANGE
#define MF_E_TRANSFORM_STREAM_CHANGE ((HRESULT)0xC00D6D61L)
#endif

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

struct VideoEncoder::Impl {
    IMFTransform *mft = nullptr;
    IMFMediaEventGenerator *events = nullptr;
    ICodecAPI *codec = nullptr;
    bool async = false, hevc = false, hw = false, providesSamples = false;
    int w = 0, h = 0, fps = 0, bitrate = 0;
    DWORD outSize = 0;
    std::vector<uint8_t> seqHeader;     // SPS/PPS (y VPS): se agregan a los cuadros completos si no los traen
    int needInput = 0;
    std::deque<uint64_t> inFlight;      // tags de los cuadros que entraron y todavía no salieron
    int64_t frameNo = 0;
    std::string name;

    ~Impl() {
        if (mft) { mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }
        rel(codec); rel(events); rel(mft);
    }

    static void setU32(ICodecAPI *c, const GUID &g, UINT32 v) {
        VARIANT x; VariantInit(&x); x.vt = VT_UI4; x.ulVal = v;
        c->SetValue(&g, &x);
    }
    static void setBool(ICodecAPI *c, const GUID &g, bool v) {
        VARIANT x; VariantInit(&x); x.vt = VT_BOOL; x.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
        c->SetValue(&g, &x);
    }

    bool open(bool wantHevc, bool hardware) {
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, wantHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
        IMFActivate **acts = nullptr;
        UINT32 n = 0;
        UINT32 flags = hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER) : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER);
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &n)) || n == 0) { if (acts) CoTaskMemFree(acts); return false; }
        bool ok = false;
        for (UINT32 i = 0; i < n && !ok; i++) {
            WCHAR *fn = nullptr;
            UINT32 len = 0;
            acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len);
            std::string friendly = fn ? utf8(fn) : "?";
            if (fn) CoTaskMemFree(fn);
            if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft)))) {
                if (configure(wantHevc)) {
                    ok = true;
                    hevc = wantHevc; hw = hardware;
                    logmsg("[%s] Encoder: %s %s (%s) · %dx%d · %d Mbps", name.c_str(), wantHevc ? "HEVC" : "H.264",
                           hardware ? "hardware" : "software (CPU)", friendly.c_str(), w, h, bitrate / 1000000);
                } else {
                    logmsg("[%s] %s didn't accept %dx%d in %s", name.c_str(), friendly.c_str(), w, h, wantHevc ? "HEVC" : "H.264");
                    rel(codec); rel(events);
                    acts[i]->ShutdownObject();
                    rel(mft);
                }
            }
        }
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
        return ok;
    }

    bool configure(bool wantHevc) {
        IMFAttributes *a = nullptr;
        UINT32 isAsync = 0;
        if (SUCCEEDED(mft->GetAttributes(&a)) && a) {
            a->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            if (isAsync) a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            a->SetUINT32(MF_LOW_LATENCY, TRUE);
            a->Release();
        }
        async = isAsync != 0;
        if (async && FAILED(mft->QueryInterface(IID_PPV_ARGS(&events)))) return false;
        static const GUID IID_CodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};
        mft->QueryInterface(IID_CodecAPI, (void **)&codec);
        if (codec) {
            setU32(codec, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            setU32(codec, CODECAPI_AVEncCommonMeanBitRate, (UINT32)bitrate);
            setBool(codec, CODECAPI_AVLowLatencyMode, true);
            setU32(codec, CODECAPI_AVEncMPVGOPSize, (UINT32)(fps * 2));
            setU32(codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            setU32(codec, CODECAPI_AVEncCommonQualityVsSpeed, 33);   // (más velocidad que calidad: la calidad la da la tasa de bits)
        }
        IMFMediaType *ot = nullptr, *it = nullptr;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, wantHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
        MFSetAttributeSize(ot, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(ot, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(ot, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, wantHevc ? (UINT32)eAVEncH265VProfile_Main_420_8 : (UINT32)eAVEncH264VProfile_High);
        ot->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        ot->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        HRESULT hr = mft->SetOutputType(0, ot, 0);
        ot->Release();
        if (FAILED(hr)) return false;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(it, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(it, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(it, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        it->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        it->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        hr = mft->SetInputType(0, it, 0);
        it->Release();
        if (FAILED(hr)) return false;
        readSeqHeader();
        MFT_OUTPUT_STREAM_INFO si{};
        mft->GetOutputStreamInfo(0, &si);
        providesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outSize = std::max<DWORD>(si.cbSize, (DWORD)(w * h));
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void readSeqHeader() {
        IMFMediaType *t = nullptr;
        if (FAILED(mft->GetOutputCurrentType(0, &t)) || !t) return;
        UINT32 sz = 0;
        if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 0) {
            seqHeader.resize(sz);
            t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader.data(), sz, nullptr);
        }
        t->Release();
    }

    bool hasParamSets(const uint8_t *d, size_t n) const {
        for (size_t i = 0; i + 4 < n; i++) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                int t = hevc ? (d[i + 3] >> 1) & 0x3F : d[i + 3] & 0x1F;
                if (hevc ? (t == 32 || t == 33) : t == 7) return true;
                if (hevc ? (t < 32) : (t == 1 || t == 5)) return false;   // (ya empezó la imagen)
            }
        }
        return false;
    }

    // Saca un cuadro codificado (si hay). Devuelve false si no había nada.
    bool pull(const VideoEncoder::Out &out) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample *sample = nullptr;
        IMFMediaBuffer *mb = nullptr;
        if (!providesSamples) {
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(outSize, &mb);
            sample->AddBuffer(mb);
            ob.pSample = sample;
        }
        DWORD status = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType *t = nullptr;
            if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t)) && t) { mft->SetOutputType(0, t, 0); t->Release(); }
            readSeqHeader();
            rel(mb); rel(sample);
            return true;
        }
        if (FAILED(hr)) { rel(mb); rel(sample); return false; }
        IMFSample *got = ob.pSample;
        IMFMediaBuffer *buf = nullptr;
        if (got && SUCCEEDED(got->ConvertToContiguousBuffer(&buf))) {
            BYTE *p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                UINT32 clean = 0;
                got->GetUINT32(MFSampleExtension_CleanPoint, &clean);
                uint64_t tag = 0;
                if (!inFlight.empty()) { tag = inFlight.front(); inFlight.pop_front(); }
                if (clean && !seqHeader.empty() && !hasParamSets(p, len)) {
                    std::vector<uint8_t> v(seqHeader);
                    v.insert(v.end(), p, p + len);
                    out(v.data(), v.size(), true, tag);
                } else out(p, len, clean != 0, tag);
                buf->Unlock();
            }
            buf->Release();
        }
        if (providesSamples && got) got->Release();
        rel(mb); rel(sample);
        return true;
    }

    bool feed(const uint8_t *nv12, bool key, uint64_t tag) {
        size_t size = (size_t)w * h * 3 / 2;
        IMFMediaBuffer *mb = nullptr;
        IMFSample *s = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)size, &mb))) return false;
        BYTE *p = nullptr;
        mb->Lock(&p, nullptr, nullptr);
        memcpy(p, nv12, size);
        mb->Unlock();
        mb->SetCurrentLength((DWORD)size);
        MFCreateSample(&s);
        s->AddBuffer(mb);
        LONGLONG dur = 10000000LL / fps;
        s->SetSampleTime(frameNo++ * dur);
        s->SetSampleDuration(dur);
        if (key && codec) setU32(codec, CODECAPI_AVEncVideoForceKeyFrame, 1);
        HRESULT hr = mft->ProcessInput(0, s, 0);
        s->Release();
        mb->Release();
        if (FAILED(hr)) return false;
        inFlight.push_back(tag);
        while (inFlight.size() > 8) inFlight.pop_front();
        return true;
    }
};

// ---------------- El hilo de cada codificador ----------------
struct EncWorker {
    static void run(VideoEncoder *self);
};

namespace {
struct Job { std::vector<uint8_t> nv; int w, h, fps, bitrate; bool hevc, key; uint64_t tag; };
struct State {
    std::mutex m;
    std::condition_variable cv;
    bool has = false, quit = false, reset = false;
    Job job;
    std::thread th;
    int newBitrate = 0;
};
std::mutex statesLock;
std::map<VideoEncoder *, State *> states;
State *stateOf(VideoEncoder *e) {
    std::lock_guard<std::mutex> g(statesLock);
    auto it = states.find(e);
    if (it != states.end()) return it->second;
    State *s = new State();
    states[e] = s;
    s->th = std::thread(EncWorker::run, e);
    return s;
}
}

void EncWorker::run(VideoEncoder *self) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    State *st = stateOf(self);
    VideoEncoder::Impl *im = nullptr;
    int failW = 0, failH = 0, noneW = 0, noneH = 0;   // (fail: ese tamaño no se puede en HEVC; none: no se puede con ninguno)
    bool failHevc = false;
    while (true) {
        Job job;
        bool haveJob = false;
        // (los de hardware son asincrónicos: avisan cuándo quieren un cuadro; mientras tanto el cuadro nuevo espera acá)
        bool wantJob = !im || !im->async || im->needInput > 0;
        {
            std::unique_lock<std::mutex> g(st->m);
            st->cv.wait_for(g, std::chrono::milliseconds(im && im->async ? 1 : 50), [&] { return (st->has && wantJob) || st->quit || st->reset; });
            if (st->quit) break;
            if (st->reset) { st->reset = false; delete im; im = nullptr; failW = noneW = 0; wantJob = true; }
            if (st->newBitrate && im && im->codec) { VideoEncoder::Impl::setU32(im->codec, CODECAPI_AVEncCommonMeanBitRate, (UINT32)st->newBitrate); im->bitrate = st->newBitrate; }
            st->newBitrate = 0;
            if (st->has && wantJob) { job = std::move(st->job); st->has = false; haveJob = true; }
        }
        if (haveJob && (!im || im->w != job.w || im->h != job.h || im->fps != job.fps || (job.hevc != im->hevc && !(failHevc && failW == job.w && failH == job.h)))) {
            delete im;
            im = nullptr;
            if (!(noneW == job.w && noneH == job.h)) {
                // HEVC primero (si se eligió); si la PC no puede, H.264; por hardware y, si no hay, en el procesador
                const bool order[3][2] = {{true, true}, {false, true}, {false, false}};
                for (auto &o : order) {
                    if (o[0] && !job.hevc) continue;
                    im = new VideoEncoder::Impl();
                    im->name = self->name_; im->w = job.w; im->h = job.h; im->fps = job.fps; im->bitrate = job.bitrate;
                    if (im->open(o[0], o[1])) break;
                    delete im;
                    im = nullptr;
                }
                if (!im) { noneW = job.w; noneH = job.h; logmsg("[%s] ERROR: no video encoder for %dx%d", self->name_.c_str(), job.w, job.h); }
                else {
                    self->hevcNow_ = im->hevc;
                    if (job.hevc && !im->hevc) { failHevc = true; failW = job.w; failH = job.h; }
                    job.key = true;
                }
            }
        }
        if (haveJob) {
            if (im) {
                bool fed = im->feed(job.nv.data(), job.key, job.tag);
                if (!fed && job.key) self->key_ = true;
                if (im->async) { if (fed && im->needInput > 0) im->needInput--; }
                else if (fed) while (im->pull(self->out_)) {}
            }
            self->pending_--;
        }
        if (im && im->async) {
            while (true) {
                IMFMediaEvent *ev = nullptr;
                if (im->events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev) != S_OK || !ev) break;
                MediaEventType t = 0;
                ev->GetType(&t);
                ev->Release();
                if (t == METransformNeedInput) im->needInput++;
                else if (t == METransformHaveOutput) im->pull(self->out_);
            }
        }
    }
    delete im;
    MFShutdown();
    CoUninitialize();
}

bool VideoEncoder::encode(const uint8_t *nv12, int w, int h, int fps, int bitrate, bool hevc, bool forceKey, uint64_t tag) {
    State *st = stateOf(this);
    std::lock_guard<std::mutex> g(st->m);
    bool replace = st->has;   // (todavía no tomó el anterior: va este, que es más nuevo)
    Job &j = st->job;
    j.nv.assign(nv12, nv12 + (size_t)w * h * 3 / 2);
    j.w = w; j.h = h; j.fps = fps; j.bitrate = bitrate; j.hevc = hevc; j.tag = tag;
    j.key = forceKey || key_.exchange(false) || (replace && j.key);
    st->has = true;
    if (!replace) pending_++;
    st->cv.notify_one();
    return true;
}
void VideoEncoder::setBitrate(int b) { State *st = stateOf(this); std::lock_guard<std::mutex> g(st->m); st->newBitrate = b; st->cv.notify_one(); }
void VideoEncoder::reset() {
    State *st = stateOf(this);
    std::lock_guard<std::mutex> g(st->m);
    st->reset = true;
    if (st->has) { st->has = false; pending_--; }
    key_ = true;
    st->cv.notify_one();
}
VideoEncoder::~VideoEncoder() {}

/// ¿Esta PC puede codificar HEVC por hardware a este tamaño? (si no, se usa H.264 y así se le avisa al Quest)
bool encoderCanHevc(int w, int h, int fps, int bitrate) {
    static std::mutex m;
    static std::map<std::string, bool> cache;
    char key[64];
    snprintf(key, sizeof key, "%dx%d@%d", w, h, fps);
    std::lock_guard<std::mutex> g(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    bool ok;
    {
        VideoEncoder::Impl im;
        im.name = "test"; im.w = w; im.h = h; im.fps = fps; im.bitrate = bitrate;
        ok = im.open(true, true);
    }
    MFShutdown();
    if (SUCCEEDED(ci)) CoUninitialize();
    cache[key] = ok;
    if (!ok) logmsg("[video] This PC can't encode HEVC at %dx%d in hardware: using H.264", w, h);
    return ok;
}
