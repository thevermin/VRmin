package org.questvrbridge.client;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;

/** Decodifica un video H.264 de la Mac (escritorio o una ventana) directo a una superficie de la pantalla. */
final class StreamDecoder {
    interface Host { void requestKey(); void onSize(int w, int h); }

    private final Object lock = new Object();
    private MediaCodec codec;
    private int cw, ch;
    private Surface surface;
    private Host host;
    private volatile boolean needKey = true;
    private long pts = 0, lastAsk = 0;

    void attach(Surface s, Host h) { synchronized (lock) { close(); surface = s; host = h; } }
    void detach() { synchronized (lock) { close(); surface = null; host = null; } }
    void reset() { synchronized (lock) { close(); } }

    private void close() {
        if (codec != null) { try { codec.stop(); } catch (Exception ignored) { } codec.release(); codec = null; }
        cw = ch = 0;
        needKey = true;
    }

    private void askKey() {
        needKey = true;
        long t = System.currentTimeMillis();
        if (t - lastAsk < 300 || host == null) return;
        lastAsk = t;
        host.requestKey();
    }

    void feed(byte[] p, int off, int len, boolean key, int w, int h) {
        if (w < 64 || h < 64) return;
        synchronized (lock) {
            if (surface == null) return;
            if (codec == null || w != cw || h != ch) {
                close();
                try {
                    MediaFormat f = MediaFormat.createVideoFormat("video/avc", w, h);
                    f.setInteger("low-latency", 1);
                    f.setInteger("vendor.qti-ext-dec-low-latency.enable", 1);
                    f.setInteger(MediaFormat.KEY_OPERATING_RATE, 120);
                    f.setInteger(MediaFormat.KEY_PRIORITY, 0);
                    codec = MediaCodec.createDecoderByType("video/avc");
                    codec.configure(f, surface, null, 0);
                    codec.start();
                    cw = w; ch = h;
                    if (host != null) host.onSize(w, h);
                    final MediaCodec c = codec;
                    new Thread(() -> drain(c), "qvb-video").start();
                } catch (Exception e) {
                    Log.e("QVB2D", "Decodificador: " + e);
                    codec = null;
                    return;
                }
                askKey();
            }
            if (needKey && !key) return;
            try {
                int idx = codec.dequeueInputBuffer(5000);
                if (idx < 0) { askKey(); return; }
                ByteBuffer in = codec.getInputBuffer(idx);
                if (in == null || in.capacity() < len) { codec.queueInputBuffer(idx, 0, 0, 0, 0); askKey(); return; }
                in.clear();
                in.put(p, off, len);
                codec.queueInputBuffer(idx, 0, len, ++pts * 16666, 0);
                if (key) needKey = false;
            } catch (Exception e) {
                askKey();
            }
        }
    }

    private void drain(MediaCodec c) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (true) {
            synchronized (lock) { if (codec != c) return; }
            try {
                int idx = c.dequeueOutputBuffer(info, 10000);
                if (idx >= 0) c.releaseOutputBuffer(idx, true);
            } catch (Exception e) {
                return;
            }
        }
    }
}
