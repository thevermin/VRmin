package org.questvrbridge.client;

import android.content.Context;
import android.view.Gravity;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import static org.questvrbridge.client.Ui.dp;

/**
 * Contenido de una ventana de la Mac (una app, un monitor físico o el monitor virtual).
 * Se usa igual en una ventana del Quest y flotando en la sala VR.
 */
final class WindowUi implements Link.Listener {
    static final int MONITOR_ID = 0x7FFF0002;    // monitor virtual
    static final int DISPLAY_BIT = 0x40000000;   // otra pantalla física de la Mac

    static boolean isDisplay(int id) { return (id & DISPLAY_BIT) != 0 && id != MONITOR_ID; }

    interface Host {
        boolean inVR();
        void close();
        /** Dónde está la imagen de la Mac dentro de la ventana (para el 3D), de 0 a 1. */
        default void onVideoRect(float x0, float y0, float x1, float y1) { }
    }

    final int id;
    private final Context ctx;
    private final Host host;
    private boolean locked;
    private boolean depth3d;   // 3D de esta ventana (lo calcula la IA de la Mac)
    private final StreamDecoder decoder = new StreamDecoder();
    private final FrameLayout root;
    private final Ui.AspectFrame frame;
    private final TextView hint;
    private final DwellButton lockButton;
    private final LinearLayout ornament;
    private int missing = 0;
    private final Runnable sendSize = this::sendSize;
    private final Runnable hideOrnament;

    private String lockKey() { return id == MONITOR_ID ? "lock_monitor" : "lock_apps"; }

    WindowUi(Context ctx, Host host, int id, String app, String title, int w, int h) {
        this.ctx = ctx;
        this.host = host;
        this.id = id;
        // Monitores: forma fija por defecto (agrandar la ventana no cambia la resolución). Apps: se adaptan.
        locked = isDisplay(id) || ctx.getSharedPreferences("qvb", Context.MODE_PRIVATE).getBoolean(lockKey(), id == MONITOR_ID);

        root = new FrameLayout(ctx);
        root.setBackgroundColor(Ui.BG);
        hint = Ui.label(ctx, I18n.t("Conectando con la Mac…"), 20, Ui.TEXT2, false);
        hint.setGravity(Gravity.CENTER);
        root.addView(hint, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        frame = new Ui.AspectFrame(ctx);
        frame.setAspect(w / (float) Math.max(1, h));
        root.addView(frame, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        final StreamDecoder.Host dh = new StreamDecoder.Host() {
            @Override public void requestKey() { Link.I.send("{\"type\":\"winkey\",\"id\":" + WindowUi.this.id + "}"); }
            @Override public void onSize(int ww, int hh) { root.post(() -> { frame.setAspect(ww / (float) hh); hint.setVisibility(View.GONE); }); }
        };
        View view = VideoSurface.create(ctx, host.inVR(), new VideoSurface.Callback() {
            @Override public void created(Surface s) { decoder.attach(s, dh); }
            @Override public void destroyed() { decoder.detach(); }
        });
        frame.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        Pointer.attach(view, Pointer.OPEN, (u, v, down, scroll) -> {
            if (v < 0.1f) showOrnament();   // apuntar arriba muestra el nombre
            Link.I.send(Pointer.json("wmouse", WindowUi.this.id, u, v, down, scroll));
        });

        // Nombre de la ventana arriba al centro (solo se muestra, no tapa clics)
        ornament = new LinearLayout(ctx);
        ornament.setOrientation(LinearLayout.HORIZONTAL);
        ornament.setGravity(Gravity.CENTER_VERTICAL);
        ornament.setPadding(dp(ctx, 18), dp(ctx, 6), dp(ctx, 18), dp(ctx, 6));
        ornament.setBackground(Ui.glass(ctx, 999, 215));
        TextView name = Ui.label(ctx, title == null || title.isEmpty() ? app : app + " — " + title, 15, Ui.TEXT, true);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        name.setMaxWidth(dp(ctx, 520));
        ornament.addView(name);
        FrameLayout.LayoutParams olp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        olp.topMargin = dp(ctx, 12);
        root.addView(ornament, olp);
        hideOrnament = () -> ornament.animate().alpha(0f).setDuration(400).withEndAction(() -> ornament.setVisibility(View.INVISIBLE)).start();
        ornament.postDelayed(hideOrnament, 4000);

        // Candado de la forma en la esquina derecha: se activa apuntándole un segundo (antes, el clic va a la Mac)
        lockButton = new DwellButton(ctx, "");
        lockButton.setTextSize(18);
        lockButton.setOnClickListener(v -> setLocked(!locked));
        FrameLayout.LayoutParams llp = new FrameLayout.LayoutParams(dp(ctx, 52), dp(ctx, 52), Gravity.TOP | Gravity.END);
        llp.setMargins(0, dp(ctx, 12), dp(ctx, 12), 0);
        // (en la sala VR el candado y el cerrar van por fuera de la ventana, en su barra de botones)
        if (!isDisplay(id) && !host.inVR()) root.addView(lockButton, llp);   // un monitor físico no puede cambiar de forma
        paintLock();

        frame.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int W = root.getWidth(), H = root.getHeight();
            if (W > 0 && H > 0) host.onVideoRect(l / (float) W, t / (float) H, r / (float) W, b / (float) H);
        });
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r - l != or - ol || b - t != ob - ot) { v.removeCallbacks(sendSize); v.postDelayed(sendSize, 400); }
        });
    }

    View view() { return root; }

    private void paintLock() { Icons.on(lockButton, locked ? "lock" : "unlock", 22); }

    boolean canLock() { return !isDisplay(id); }
    boolean isLocked() { return locked; }
    void toggleLock() { setLocked(!locked); }

    private void setLocked(boolean l) {
        locked = l;
        ctx.getSharedPreferences("qvb", Context.MODE_PRIVATE).edit().putBoolean(lockKey(), l).apply();
        paintLock();
        if (!l) sendSize();   // al destrabar, adaptarse ya al tamaño actual
        showOrnament();
    }

    private void showOrnament() {
        ornament.removeCallbacks(hideOrnament);
        if (ornament.getVisibility() != View.VISIBLE || ornament.getAlpha() < 1f) {
            ornament.animate().cancel();
            ornament.setVisibility(View.VISIBLE);
            ornament.setAlpha(1f);
        }
        ornament.postDelayed(hideOrnament, 2500);
    }

    /// Con la forma destrabada, la Mac adapta su ventana (o el monitor virtual) a la forma de esta ventana
    private void sendSize() {
        if (locked) return;
        float d = ctx.getResources().getDisplayMetrics().density;
        int w = (int) (root.getWidth() / d), h = (int) (root.getHeight() / d);
        if (w < 100 || h < 100) return;
        Link.I.send("{\"type\":\"wsize\",\"id\":" + id + ",\"w\":" + w + ",\"h\":" + h + "}");
    }

    private void request() {
        if (id == MONITOR_ID) Link.I.send("{\"type\":\"monitor\",\"on\":true}");
        Link.I.openWindow(id, decoder);
        sendSize();
        if (depth3d) send3D();
    }

    /// 3D de esta ventana: la Mac calcula su profundidad y la manda junto con el video
    void set3D(boolean on) { depth3d = on; send3D(); }

    private void send3D() { Link.I.send("{\"type\":\"depth3d\",\"id\":" + id + ",\"on\":" + depth3d + "}"); }

    void start() { Link.I.addListener(this); request(); }

    void stop() { Link.I.closeWindow(id); Link.I.removeListener(this); }

    /// Al cerrar del todo: el monitor virtual se quita de la Mac (sus ventanas vuelven a la pantalla principal)
    void destroyed() { if (id == MONITOR_ID) Link.I.send("{\"type\":\"monitor\",\"on\":false}"); }

    @Override
    public void onConnection(boolean connected, String name) {
        if (!connected) { hint.setText(I18n.t("Sin conexión con la Mac")); hint.setVisibility(View.VISIBLE); }
        else request();   // (re)pedir el video de esta ventana
    }

    @Override
    public void onState(JSONObject o) {
        if (id == MONITOR_ID) return;
        // Si la ventana (o el monitor) ya no está en la Mac, cerrar también esta
        JSONArray ws = o.optJSONArray(isDisplay(id) ? "displays" : "windows");
        boolean found = false;
        for (int i = 0; ws != null && i < ws.length(); i++) if (ws.optJSONObject(i).optInt("id") == id) { found = true; break; }
        // (la pantalla principal de la Mac viene aparte, en «mainDisplay»)
        JSONObject md = o.optJSONObject("mainDisplay");
        if (!found && md != null && md.optInt("id") == id) found = true;
        missing = found ? 0 : missing + 1;
        if (missing >= 3) host.close();
    }
}
