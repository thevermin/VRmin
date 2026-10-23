package org.questvrbridge.client;

import android.view.MotionEvent;
import android.view.View;

import java.util.Locale;

/** Convierte el puntero del control (o el toque) sobre una imagen de la Mac en eventos de mouse para la Mac. */
final class Pointer {
    interface Sender { void send(float u, float v, String down, float scroll); }
    interface Gate { boolean blocked(); void tapWhileBlocked(); }

    static final Gate OPEN = new Gate() {
        @Override public boolean blocked() { return false; }
        @Override public void tapWhileBlocked() { }
    };

    static String json(String type, int id, float u, float v, String down, float scroll) {
        return String.format(Locale.US, "{\"type\":\"%s\"%s,\"u\":%.4f,\"v\":%.4f%s,\"scroll\":%.2f}",
                type, id >= 0 ? ",\"id\":" + id : "", u, v, down != null ? ",\"down\":" + down : "", scroll);
    }

    static void attach(View view, Gate gate, Sender s) {
        final long[] lastMove = {0};
        view.setOnTouchListener((v, e) -> {
            if (gate.blocked()) { if (e.getActionMasked() == MotionEvent.ACTION_DOWN) gate.tapWhileBlocked(); return true; }
            float u = e.getX() / Math.max(1, v.getWidth()), vv = e.getY() / Math.max(1, v.getHeight());
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: s.send(u, vv, "true", 0); break;
                case MotionEvent.ACTION_MOVE: move(s, lastMove, u, vv); break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL: s.send(u, vv, "false", 0); break;
            }
            return true;
        });
        view.setOnGenericMotionListener((v, e) -> {
            if (gate.blocked()) return false;
            float u = e.getX() / Math.max(1, v.getWidth()), vv = e.getY() / Math.max(1, v.getHeight());
            if (e.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE) { move(s, lastMove, u, vv); return true; }
            if (e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
                float sc = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
                if (sc != 0) s.send(u, vv, null, Math.max(-3f, Math.min(3f, sc * 2f)));
                return true;
            }
            return false;
        });
    }

    private static void move(Sender s, long[] last, float u, float v) {
        long t = System.currentTimeMillis();
        if (t - last[0] < 16) return;   // ~60 movimientos por segundo alcanza
        last[0] = t;
        s.send(u, v, null, 0);
    }
}
