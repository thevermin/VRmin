package org.questvrbridge.client;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;

/** Superficie donde se dibuja el video de la Mac. En la sala VR se usa TextureView (va dentro de una pantalla virtual). */
final class VideoSurface {
    interface Callback { void created(Surface s); void destroyed(); }

    static View create(Context c, boolean texture, Callback cb) {
        if (texture) {
            TextureView t = new TextureView(c);
            t.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
                Surface s;
                @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) { s = new Surface(st); cb.created(s); }
                @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) { }
                @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) { cb.destroyed(); if (s != null) s.release(); return true; }
                @Override public void onSurfaceTextureUpdated(SurfaceTexture st) { }
            });
            return t;
        }
        SurfaceView v = new SurfaceView(c);
        v.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { cb.created(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { }
            @Override public void surfaceDestroyed(SurfaceHolder h) { cb.destroyed(); }
        });
        return v;
    }
}
