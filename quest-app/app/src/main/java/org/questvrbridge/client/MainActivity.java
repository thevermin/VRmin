package org.questvrbridge.client;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Bundle;

import static org.questvrbridge.client.Ui.dp;

/** Ventana principal del Quest (modo ventanas). El contenido es MenuUi, el mismo que en la TV de la sala VR. */
public class MainActivity extends Activity implements MenuUi.Host {
    private MenuUi menu;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        I18n.init(this);
        menu = new MenuUi(this, this);
        setContentView(menu.view());
        verificarCompra();
    }

    /// Tienda de Meta: si no se puede verificar la compra, se muestra un aviso en lugar del menú
    private void verificarCompra() {
        Licencia.comprobar(this, (comprada, detalle) -> {
            if (comprada) return;
            android.widget.LinearLayout l = new android.widget.LinearLayout(this);
            l.setOrientation(android.widget.LinearLayout.VERTICAL);
            l.setGravity(android.view.Gravity.CENTER);
            l.setBackgroundColor(Ui.BG);
            l.setPadding(dp(this, 60), dp(this, 60), dp(this, 60), dp(this, 60));
            l.addView(new Ui.Logo(this, 40));
            android.widget.TextView t = Ui.label(this, I18n.t("No pudimos verificar tu compra de VRmin."), 24, Ui.TEXT, true);
            t.setGravity(android.view.Gravity.CENTER);
            t.setPadding(0, dp(this, 24), 0, dp(this, 8));
            l.addView(t);
            android.widget.TextView d = Ui.label(this, I18n.t("Comprala en la tienda de Meta Horizon, o revisá que estés con la misma cuenta.")
                    + (detalle != null ? "\n(" + detalle + ")" : ""), 16, Ui.TEXT2, false);
            d.setGravity(android.view.Gravity.CENTER);
            l.addView(d);
            android.widget.LinearLayout b = new android.widget.LinearLayout(this);
            b.setGravity(android.view.Gravity.CENTER);
            b.setPadding(0, dp(this, 28), 0, 0);
            android.widget.TextView retry = Ui.primary(this, I18n.t("Reintentar"));
            retry.setOnClickListener(v -> recreate());
            android.widget.TextView close = Ui.pill(this, I18n.t("Cerrar"));
            close.setOnClickListener(v -> finishAndRemoveTask());
            android.widget.LinearLayout.LayoutParams lp = new android.widget.LinearLayout.LayoutParams(-2, -2);
            lp.setMargins(dp(this, 8), 0, dp(this, 8), 0);
            b.addView(retry, lp);
            b.addView(close, lp);
            l.addView(b);
            setContentView(l);
        });
    }

    @Override protected void onStart() {
        super.onStart();
        menu.start();
        Link.I.acquire(this);
        // Al volver de VR, el menú aparece suave
        android.view.View v = menu.view();
        if (v.getAlpha() < 1f) v.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(400).start();
        launchingVR = false;
    }

    private boolean launchingVR;
    private static final long FADE_MS = 550;
    @Override protected void onStop() { super.onStop(); menu.stop(); Link.I.release(); }

    @Override public boolean inVR() { return false; }
    @Override public void exitVR() { }
    @Override public void quitApp() { finishAffinity(); new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> System.exit(0), 300); }
    @Override public void closeAllWindows() { WindowActivity.closeAll(); }
    @Override public boolean isWindowOpen(int id) { for (WindowActivity a : WindowActivity.OPEN) if (a.winId() == id) return true; return false; }

    /// Pasa a la sala inmersiva (otro proceso). La conexión de las ventanas se corta para que la use la VR.
    /// Primero las ventanas se desvanecen suave; recién después arranca la sala VR (con su transición de 2 segundos)
    /// Modo Centinela desde el Modo Panel: la sala VR abre directo en ese modo (como «Empezar» en VRmin Host)
    private boolean sentinel;
    @Override public void enterSentinel() { sentinel = true; enterVR(); }

    @Override
    public void enterVR() {
        if (launchingVR) return;
        launchingVR = true;
        for (WindowActivity a : WindowActivity.OPEN) a.fadeOut(FADE_MS);
        menu.view().animate().alpha(0f).scaleX(0.94f).scaleY(0.94f).setDuration(FADE_MS)
                .setInterpolator(new android.view.animation.DecelerateInterpolator()).start();
        menu.view().postDelayed(this::startVR, FADE_MS + 50);
    }

    private void startVR() {
        WindowActivity.closeAll();   // en VR las ventanas se abren dentro de la sala
        Link.I.pauseForVR();
        Intent i = new Intent();
        i.setComponent(new ComponentName(this, VrActivity.class.getName()));
        Link.Mac t = Link.I.target();
        if (t != null) i.putExtra("mac", t.name);   // la sala VR se conecta a la misma Mac
        if (sentinel) i.putExtra("vigia", true);
        sentinel = false;
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }

    /// Abre una ventana (o pantalla) de la Mac en su propia ventana del Quest; si ya está abierta, la trae al frente
    @Override
    public void openWindow(int id, String app, String title, int w, int h) {
        Intent i = new Intent(this, WindowActivity.class);
        i.setData(Uri.parse("qvb://ventana/" + id));
        i.putExtra("id", id).putExtra("app", app).putExtra("title", title).putExtra("w", w).putExtra("h", h);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_DOCUMENT);
        Bundle opts = null;
        try {
            float k = Math.min(1.0f, 1300f / Math.max(w, h));
            int pw = dp(this, Math.max(400, w * k)), ph = dp(this, Math.max(300, h * k));
            opts = ActivityOptions.makeBasic().setLaunchBounds(new Rect(0, 0, pw, ph)).toBundle();
        } catch (Exception ignored) { }
        startActivity(i, opts);
    }
}
