package org.questvrbridge.client;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Pantalla de carga (como la de SteamVR, con el estilo de VRmin): mientras se abre un juego, la sala pasa al vacío
 * de carga y adelante flota un panel con la portada, el nombre, en qué paso va (abriendo Steam, abriendo el juego…)
 * y consejos que cambian solos. Si el juego se cierra, avisa un momento antes de volver a la sala.
 * Lo que se está abriendo lo dice la Mac («launching» en su estado), tanto si lo abriste desde acá como desde VRmin Host.
 */
final class Loading {
    interface Host {
        /** Crea el panel de la pantalla de carga (y le pone la vista de build) o lo cierra. */
        void loadingPanel(boolean on);
        void nativeLoading(boolean on);
    }

    private static final int OFF = 0, OPENING = 1, CLOSED = 2, FAILED = 3;
    private static final long TIP_MS = 5000, CLOSED_MS = 3500, GIVE_UP_MS = 4 * 60_000;

    private final Context app;
    private final Host host;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private int phase = OFF;
    private String gameId = "", gameName = "", launchKey = "", dismissedId = "";
    private long dismissedAt = 0;
    private long since = 0;
    private boolean macGame = false, inGame = false;
    private Context ctx;   // (el de la pantalla virtual del panel)
    private TextView name, status, tip, sub;
    private ImageView cover;
    private View bar;
    private final List<String> tips = new ArrayList<>();
    private int tipAt = 0;

    Loading(Context app, Host host) { this.app = app; this.host = host; }

    /** Se está viendo la pantalla de carga (mientras tanto no se abre ningún menú). */
    boolean active() { return phase != OFF; }

    /** Estado de la Mac (lobby2d). */
    void onState(JSONObject o) {
        boolean g = o.optBoolean("game");
        JSONObject l = o.optJSONObject("launching");
        String st = o.optBoolean("busy") ? o.optString("status", "") : "";
        if (l != null && !g && !inGame) {
            String key = l.optString("id") + "@" + l.optLong("t");
            // (lo que ya se cerró con «Volver a la sala», o ya empezó, no vuelve a aparecer por un rato)
            boolean dismissed = l.optString("id").equals(dismissedId) && SystemClock.uptimeMillis() - dismissedAt < 4 * 60_000;
            if (!key.equals(launchKey) && !dismissed) { launchKey = key; begin(l.optString("id"), l.optString("name")); }
        }
        if (phase == OPENING) {
            if (isFailure(st)) fail(st);
            else if (g) setStatus(I18n.t("Entrando en VR…"));
            else if (!st.isEmpty()) setStatus(st);
            if (SystemClock.uptimeMillis() - since > GIVE_UP_MS) end();
        }
        // El juego se cerró (la Mac dejó de tenerlo): un momento la pantalla de carga con el aviso, y vuelve la sala
        if (macGame && !g && phase == OFF && !gameName.isEmpty()) closed();
        if (g && !o.optString("gameName", "").isEmpty() && phase == OFF) gameName = o.optString("gameName");
        macGame = g;
    }

    /** Empieza o termina el juego en el visor (ya se ve su imagen). */
    void onGameState(boolean on) {
        inGame = on;
        if (on && phase != OFF) end();
    }

    private static boolean isFailure(String s) {
        return s.startsWith("No se pudo") || s.startsWith("No pude") || s.startsWith("Couldn't");
    }

    private void begin(String id, String n) {
        gameId = id; gameName = n; since = SystemClock.uptimeMillis();
        if (phase == OFF) open();
        phase = OPENING;
        show(n, I18n.t("Abriendo el juego…"), true);
    }

    private void closed() {
        phase = CLOSED;
        open();
        show(gameName, I18n.t("El juego se cerró"), false);
        if (sub != null) sub.setText(I18n.t("Volviendo a la sala…"));
        ui.postDelayed(() -> { if (phase == CLOSED) end(); }, CLOSED_MS);
    }

    private void fail(String msg) {
        phase = FAILED;
        setStatus(msg);
        if (bar != null) bar.setVisibility(View.INVISIBLE);
        ui.postDelayed(() -> { if (phase == FAILED) end(); }, 12_000);
    }

    private void end() {
        if (phase == OPENING) { dismissedId = gameId; dismissedAt = SystemClock.uptimeMillis(); }
        phase = OFF;
        ui.removeCallbacksAndMessages(null);
        host.nativeLoading(false);
        host.loadingPanel(false);
        ctx = null; name = null; status = null; sub = null; tip = null; cover = null; bar = null;
    }

    private void setStatus(String s) { if (status != null && !s.equals(status.getText().toString())) status.setText(s); }

    // ------------------------------------------------------------------ panel
    private void open() {
        if (ctx != null) return;
        host.nativeLoading(true);
        host.loadingPanel(true);   // (VrActivity crea el panel y arma la vista con build)
    }

    /** VrActivity ya creó el panel: arma la vista con el contexto de su pantalla virtual. */
    View build(Context c) {
        ctx = c;
        int dp16 = Ui.dp(c, 16);
        FrameLayout root = new FrameLayout(c);
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(Gravity.CENTER_HORIZONTAL);
        card.setPadding(dp16 * 2, dp16 * 2, dp16 * 2, dp16 + dp16 / 2);
        // sin rectángulo: apenas una sombra que se desvanece hacia los bordes, para que el texto se lea sobre el vacío
        GradientDrawable bg = new GradientDrawable();
        bg.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        bg.setGradientRadius(Ui.dp(c, 400));
        bg.setColors(new int[]{Color.argb(120, 4, 4, 5), Color.argb(70, 4, 4, 5), Color.argb(0, 4, 4, 5)});
        card.setBackground(bg);
        root.addView(card, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        cover = new ImageView(c);
        cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        cover.setClipToOutline(true);
        GradientDrawable ph = new GradientDrawable();
        ph.setColor(Color.argb(40, 230, 168, 74));
        ph.setCornerRadius(Ui.rad(c, 16));
        cover.setBackground(ph);
        card.addView(cover, new LinearLayout.LayoutParams(Ui.dp(c, 460), Ui.dp(c, 215)));

        name = Ui.label(c, "", 30, Ui.TEXT, true);
        name.setGravity(Gravity.CENTER);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        name.setPadding(0, dp16 + dp16 / 2, 0, 0);
        card.addView(name, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // barrita que va y viene (no sabemos cuánto falta)
        FrameLayout track = new FrameLayout(c);
        GradientDrawable tb = new GradientDrawable();
        tb.setColor(Color.argb(40, 255, 255, 255));
        tb.setCornerRadius(Ui.dp(c, 3));
        track.setBackground(tb);
        bar = new View(c);
        GradientDrawable bb = new GradientDrawable();
        bb.setColor(Ui.ACCENT);
        bb.setCornerRadius(Ui.dp(c, 3));
        bar.setBackground(bb);
        track.addView(bar, new FrameLayout.LayoutParams(Ui.dp(c, 90), Ui.dp(c, 5)));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(Ui.dp(c, 260), Ui.dp(c, 5));
        tlp.topMargin = dp16;
        card.addView(track, tlp);
        android.animation.ObjectAnimator a = android.animation.ObjectAnimator.ofFloat(bar, "translationX", 0, Ui.dp(c, 170));
        a.setDuration(1100);
        a.setRepeatMode(android.animation.ValueAnimator.REVERSE);
        a.setRepeatCount(android.animation.ValueAnimator.INFINITE);
        a.setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator());
        a.start();

        status = Ui.label(c, "", 19, Ui.TEXT2, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp16, 0, 0);
        card.addView(status, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        sub = Ui.label(c, "", 15, Ui.TEXT3, false);
        sub.setGravity(Gravity.CENTER);
        card.addView(sub, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        View spacer = new View(c);
        card.addView(spacer, new LinearLayout.LayoutParams(1, 0, 1f));

        // Consejo (cambia solo cada 5 s; la flecha pasa al siguiente)
        LinearLayout tipBox = new LinearLayout(c);
        tipBox.setOrientation(LinearLayout.HORIZONTAL);
        tipBox.setGravity(Gravity.CENTER_VERTICAL);
        tipBox.setPadding(dp16 + dp16 / 2, dp16, dp16 / 2, dp16);
        GradientDrawable tbg = new GradientDrawable();
        tbg.setColor(Color.argb(40, 255, 255, 255));
        tbg.setCornerRadius(Ui.rad(c, 20));
        tipBox.setBackground(tbg);
        LinearLayout tipText = new LinearLayout(c);
        tipText.setOrientation(LinearLayout.VERTICAL);
        tipText.addView(Ui.label(c, I18n.t("Consejo"), 14, Ui.ACCENT_TEXT, true));
        tip = Ui.label(c, "", 17, Ui.TEXT, false);
        tip.setMinLines(2);
        tipText.addView(tip);
        tipBox.addView(tipText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView next = Ui.circle(c, "›");
        next.setTextSize(26);
        next.setOnClickListener(v -> { nextTip(); scheduleTip(); });
        LinearLayout.LayoutParams nlp = new LinearLayout.LayoutParams(Ui.dp(c, 52), Ui.dp(c, 52));
        nlp.leftMargin = dp16 / 2;
        tipBox.addView(next, nlp);
        card.addView(tipBox, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout foot = new LinearLayout(c);
        foot.setOrientation(LinearLayout.HORIZONTAL);
        foot.setGravity(Gravity.CENTER_VERTICAL);
        foot.setPadding(0, dp16, 0, 0);
        foot.addView(Ui.label(c, "VRmin", 15, Ui.TEXT3, true), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView back = Ui.pill(c, I18n.t("Volver a la sala"));
        back.setOnClickListener(v -> end());   // (el juego se sigue abriendo: al llegar su imagen se pasa al juego igual)
        foot.addView(back);
        card.addView(foot, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        tips.clear();
        for (String t : TIPS) tips.add(I18n.t(t));
        Collections.shuffle(tips);
        tipAt = 0;
        tip.setText(tips.get(0));
        scheduleTip();
        return root;
    }

    private void show(String n, String st, boolean busy) {
        if (name == null) return;
        name.setText(n);
        status.setText(st);
        sub.setText("");
        bar.setVisibility(busy ? View.VISIBLE : View.INVISIBLE);
        loadCover(gameId);
    }

    private final Runnable tipTick = () -> { nextTip(); scheduleTip(); };
    private void scheduleTip() { ui.removeCallbacks(tipTick); ui.postDelayed(tipTick, TIP_MS); }
    private void nextTip() {
        if (tip == null || tips.isEmpty()) return;
        tipAt = (tipAt + 1) % tips.size();
        tip.setText(tips.get(tipAt));
    }

    private void loadCover(String appId) {
        if (cover == null || appId == null || appId.isEmpty()) return;
        final ImageView into = cover;
        into.setTag(appId);
        new Thread(() -> {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL("https://cdn.cloudflare.steamstatic.com/steam/apps/" + appId + "/header.jpg").openConnection();
                c.setConnectTimeout(4000);
                c.setReadTimeout(6000);
                try (InputStream in = c.getInputStream()) {
                    Bitmap bm = BitmapFactory.decodeStream(in);
                    if (bm != null) into.post(() -> { if (appId.equals(into.getTag())) into.setImageBitmap(bm); });
                }
            } catch (Exception ignored) { }
        }).start();
    }

    /// Consejos (se muestran en orden al azar)
    static final String[] TIPS = {
        "Mantené apretado el botón de menú del control izquierdo para abrir el menú de juego.",
        "Si las manos del juego no coinciden con las tuyas: menú de juego › Manos y láser. Ahí podés calibrarlas y ajustar la inclinación y el giro de cada mano (se guarda para cada juego).",
        "Mantené apretado el botón de Meta para volver a centrar la vista.",
        "Si el juego va lento, bajá la nitidez desde el menú de juego.",
        "Con un cable USB-C entre el Quest y la Mac hay menos demora que por Wi-Fi.",
        "Podés simular controles de Index o de Vive en los ajustes avanzados de VRmin Host.",
        "Si un juego se tilda, cerralo con «Forzar cierre» desde VRmin Host.",
        "La primera vez que abrís un juego tarda más: prepara sus gráficos y después abre más rápido.",
        "Con el Modo Centinela jugás sin ponerte el visor: lo apoyás mirándote y el juego se ve en la Mac (menú de juego › Modo Centinela).",
        "La lista de juegos que funcionan está en las Discusiones de VRmin en GitHub.",
        "¿Encontraste un problema? Contalo en github.com/thevermin/VRmin/issues",
        "VRmin es gratis: si te gusta, podés apoyarlo en ko-fi.com/thevermin",
    };
}
