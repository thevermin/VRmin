// Codificador de video con FFmpeg (libavcodec): por hardware con NVIDIA (NVENC) o AMD/Intel (VA-API), HEVC o H.264.
// Si la PC no tiene uno por hardware, H.264 en el procesador (x264).
#include "host.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

struct EncImpl {
    AVCodecContext *ctx = nullptr;
    AVBufferRef *dev = nullptr;          // (VA-API: la placa de video)
    AVFrame *frame = nullptr, *hwFrame = nullptr;
    AVPacket *pkt = nullptr;
    bool hevc = false, hw = false, vaapi = false;
    int w = 0, h = 0, fps = 0, bitrate = 0;
    int64_t frameNo = 0;
    std::map<int64_t, uint64_t> tags;    // número de cuadro -> tag (sin cuadros B: salen en orden)
    std::string name;

    ~EncImpl() {
        av_packet_free(&pkt);
        av_frame_free(&hwFrame);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        av_buffer_unref(&dev);
    }

    bool tryCodec(const char *codecName) {
        const AVCodec *c = avcodec_find_encoder_by_name(codecName);
        if (!c) return false;
        vaapi = strstr(codecName, "vaapi") != nullptr;
        ctx = avcodec_alloc_context3(c);
        ctx->width = w;
        ctx->height = h;
        ctx->time_base = AVRational{1, fps};
        ctx->framerate = AVRational{fps, 1};
        ctx->gop_size = fps * 2;
        ctx->max_b_frames = 0;
        ctx->bit_rate = bitrate;
        ctx->rc_max_rate = bitrate;
        ctx->rc_buffer_size = bitrate / fps * 2;   // (unos dos cuadros: poca demora)
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->color_range = AVCOL_RANGE_MPEG;
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        if (strstr(codecName, "nvenc")) {
            av_opt_set(ctx->priv_data, "preset", "p1", 0);
            av_opt_set(ctx->priv_data, "tune", "ull", 0);
            av_opt_set(ctx->priv_data, "rc", "cbr", 0);
            av_opt_set_int(ctx->priv_data, "zerolatency", 1, 0);
            av_opt_set_int(ctx->priv_data, "delay", 0, 0);
            av_opt_set_int(ctx->priv_data, "forced-idr", 1, 0);
        } else if (vaapi) {
            if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) return false;
            AVBufferRef *fr = av_hwframe_ctx_alloc(dev);
            auto *fc = (AVHWFramesContext *)fr->data;
            fc->format = AV_PIX_FMT_VAAPI;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = w;
            fc->height = h;
            fc->initial_pool_size = 4;
            if (av_hwframe_ctx_init(fr) < 0) { av_buffer_unref(&fr); return false; }
            ctx->hw_frames_ctx = fr;
            ctx->pix_fmt = AV_PIX_FMT_VAAPI;
            av_opt_set(ctx->priv_data, "rc_mode", "CBR", 0);
            av_opt_set_int(ctx->priv_data, "async_depth", 1, 0);
        } else {
            av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
            av_opt_set_int(ctx->priv_data, "forced-idr", 1, 0);
            ctx->thread_count = (int)std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        }
        if (avcodec_open2(ctx, c, nullptr) < 0) return false;
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_NV12;
        frame->width = w;
        frame->height = h;
        if (av_frame_get_buffer(frame, 0) < 0) return false;
        if (vaapi) hwFrame = av_frame_alloc();
        pkt = av_packet_alloc();
        return true;
    }

    bool open(bool wantHevc, bool hardware) {
        static const char *const hwHevc[] = {"hevc_nvenc", "hevc_vaapi"}, *const hwH264[] = {"h264_nvenc", "h264_vaapi"};
        av_log_set_level(AV_LOG_FATAL);   // (probar codificadores que la PC no tiene no es un error)
        std::vector<const char *> names;
        if (hardware) for (const char *n : wantHevc ? hwHevc : hwH264) names.push_back(n);
        else if (!wantHevc) names.push_back("libx264");
        for (const char *n : names) {
            if (tryCodec(n)) {
                hevc = wantHevc; hw = hardware;
                logmsg("[%s] Encoder: %s %s (%s) · %dx%d · %d Mbps", name.c_str(), wantHevc ? "HEVC" : "H.264",
                       hardware ? "hardware" : "software (CPU)", n, w, h, bitrate / 1000000);
                return true;
            }
            av_frame_free(&hwFrame); av_frame_free(&frame); avcodec_free_context(&ctx); av_buffer_unref(&dev);
        }
        return false;
    }

    /// Codifica un cuadro y entrega lo que salga
    bool feed(const uint8_t *nv12, bool key, uint64_t tag, const VideoEncoder::Out &out) {
        if (av_frame_make_writable(frame) < 0) return false;
        for (int y = 0; y < h; y++) memcpy(frame->data[0] + (size_t)y * frame->linesize[0], nv12 + (size_t)y * w, w);
        const uint8_t *uv = nv12 + (size_t)w * h;
        for (int y = 0; y < h / 2; y++) memcpy(frame->data[1] + (size_t)y * frame->linesize[1], uv + (size_t)y * w, w);
        AVFrame *in = frame;
        if (vaapi) {
            av_frame_unref(hwFrame);
            if (av_hwframe_get_buffer(ctx->hw_frames_ctx, hwFrame, 0) < 0 || av_hwframe_transfer_data(hwFrame, frame, 0) < 0) return false;
            in = hwFrame;
        }
        in->pts = frameNo++;
        in->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        tags[in->pts] = tag;
        while (tags.size() > 16) tags.erase(tags.begin());
        if (avcodec_send_frame(ctx, in) < 0) return false;
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            uint64_t t = 0;
            auto it = tags.find(pkt->pts);
            if (it != tags.end()) { t = it->second; tags.erase(it); }
            out(pkt->data, (size_t)pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) != 0, t);
            av_packet_unref(pkt);
        }
        return true;
    }
};

// ---------------- El hilo de cada codificador ----------------
namespace {
struct Job { std::vector<uint8_t> nv; int w, h, fps, bitrate; bool hevc, key; uint64_t tag; };
struct State {
    std::mutex m;
    std::condition_variable cv;
    bool has = false, reset = false;
    Job job;
};
std::mutex statesLock;
std::map<VideoEncoder *, State *> states;
}

struct EncWorker {
    static void run(VideoEncoder *self, State *st);
    static State *stateOf(VideoEncoder *e) {
        std::lock_guard<std::mutex> g(statesLock);
        auto it = states.find(e);
        if (it != states.end()) return it->second;
        State *s = new State();
        states[e] = s;
        std::thread(run, e, s).detach();
        return s;
    }
};

void EncWorker::run(VideoEncoder *self, State *st) {
    EncImpl *im = nullptr;
    int failW = 0, failH = 0, noneW = 0, noneH = 0;   // (fail: ese tamaño no se puede en HEVC; none: no se puede con ninguno)
    bool failHevc = false;
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> g(st->m);
            st->cv.wait(g, [&] { return st->has || st->reset; });
            if (st->reset) { st->reset = false; delete im; im = nullptr; failW = noneW = 0; }
            if (!st->has) continue;
            job = std::move(st->job);
            st->has = false;
        }
        if (!im || im->w != job.w || im->h != job.h || im->fps != job.fps || (job.hevc != im->hevc && !(failHevc && failW == job.w && failH == job.h))) {
            delete im;
            im = nullptr;
            if (!(noneW == job.w && noneH == job.h)) {
                // HEVC primero (si se eligió); si la PC no puede, H.264; por hardware y, si no hay, en el procesador
                const bool order[3][2] = {{true, true}, {false, true}, {false, false}};
                for (auto &o : order) {
                    if (o[0] && !job.hevc) continue;
                    im = new EncImpl();
                    im->name = self->name_; im->w = job.w; im->h = job.h; im->fps = job.fps; im->bitrate = job.bitrate;
                    if (im->open(o[0], o[1])) break;
                    delete im;
                    im = nullptr;
                }
                if (!im) { noneW = job.w; noneH = job.h; logmsg("[%s] ERROR: no video encoder for %dx%d", self->name_.c_str(), job.w, job.h); }
                else {
                    if (job.hevc && !im->hevc) { failHevc = true; failW = job.w; failH = job.h; }
                    job.key = true;
                }
            }
        }
        if (im && !im->feed(job.nv.data(), job.key, job.tag, self->out_) && job.key) self->key_ = true;
        self->pending_--;
    }
}

bool VideoEncoder::encode(const uint8_t *nv12, int w, int h, int fps, int bitrate, bool hevc, bool forceKey, uint64_t tag) {
    State *st = EncWorker::stateOf(this);
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
void VideoEncoder::reset() {
    State *st = EncWorker::stateOf(this);
    std::lock_guard<std::mutex> g(st->m);
    st->reset = true;
    if (st->has) { st->has = false; pending_--; }
    key_ = true;
    st->cv.notify_one();
}

/// ¿Esta PC puede codificar HEVC por hardware a este tamaño? (si no, se usa H.264 y así se le avisa al Quest)
bool encoderCanHevc(int w, int h, int fps, int bitrate) {
    static std::mutex m;
    static std::map<std::string, bool> cache;
    char key[64];
    snprintf(key, sizeof key, "%dx%d@%d", w, h, fps);
    std::lock_guard<std::mutex> g(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    bool ok;
    {
        EncImpl im;
        im.name = "test"; im.w = w; im.h = h; im.fps = fps; im.bitrate = bitrate;
        ok = im.open(true, true);
    }
    cache[key] = ok;
    if (!ok) logmsg("[video] This PC can't encode HEVC at %dx%d in hardware: using H.264", w, h);
    return ok;
}
