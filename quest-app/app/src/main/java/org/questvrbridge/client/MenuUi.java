package org.questvrbridge.client;

import android.content.Context;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import static org.questvrbridge.client.Ui.dp;

/**
 * Menú principal (el mismo en la ventana del Quest y en la TV de la sala VR), inspirado en Virtual Desktop: una barra lateral con
 * Computadoras (elegir la Mac y conectar), Escritorio (pantalla principal, ventanas o monitores)
 * y Juegos (biblioteca con portadas). El escritorio solo se ve cuando lo activás.
 */
final class MenuUi implements Link.Listener {
    /** Lo que cambia entre la ventana del Quest y la TV de la sala inmersiva. */
    interface Host {
        boolean inVR();
        void enterVR();
        void exitVR();
        void openWindow(int id, String app, String title, int w, int h);
        void closeAllWindows();
        /** Dónde está el escritorio dentro de la TV (para el 3D), y si se ve sin el menú encima. */
        default void onDeskRect(boolean visible, float x0, float y0, float x1, float y1) { }
        /** 3D de la pantalla principal: 0 apagado, 1 IA, 2 lado a lado (SBS), 3 arriba/abajo. */
        default void set3DMode(int mode) { }
        default void set3DStrength(float s) { }
        /** Ajustes de imagen: brillo, contraste, saturación, nitidez, calidez. */
        default void setImage(float b, float c, float s, float sharp, float warm) { }
        default void setHud(boolean on) { }
        /** Qué objeto se ve en las manos: 0 control remoto, 1 celular, 2 banana, 3 lápiz. */
        default void setHandModel(int m) { }
        /** Tono de piel de las manos: 0 (muy clara) … 5 (muy oscura). */
        default void setHandColor(int c) { }
        /** Láser: color (0 ámbar, el clásico; 1… otros) e intensidad (1 = normal). */
        default void setLaser(int color, float gain) { }
        /** Reloj en la mano sin láser (mirarlo abre o cierra el menú flotante encima del reloj). */
        default void setWatch(boolean on) { }
        default void setHandColor(float sat) { }
        default void setEdgeSoft(float v) { }
        default void setPortal(boolean on) { }
        default void setLaserNoTouch(boolean on) { }
        default void setHandGestures(boolean on) { }
        /** Altura en la sala y en los juegos (0 = la real). */
        default void setHeight(float m) { }
        default void setCableOnly(boolean on) { }
        default void applyGestures() { }
        default void setHandMove(boolean on) { }
        /** Un juego sigue abierto pero se dejó para ir a la sala (desde «Menú principal» del menú del juego). */
        default boolean gameHidden() { return false; }
        /** Volver a ver el juego que quedó abierto. */
        default void showGame() { }
        /** Cerrar la app del todo. */
        default void quitApp() { }
        /** Girar la sala y las ventanas para que queden delante de donde mirás. */
        default void recenter() { }
        default void setLocomotion() { }
        default void toggleKeyboard() { }
        default void closeKeyboard() { }
        default void setFrameGen(int mode) { }
        /** Ver tus manos y controles reales en la sala (passthrough). */
        default void setRealHands(boolean on) { }
        /** Cerrar el menú del reloj (solo en ese menú). */
        default void closeWristMenu() { }
        /** El menú del reloj: suelto (se mueve como una ventana) o siguiendo al reloj. */
        default boolean isWristMenuFree() { return false; }
        default void setWristMenuFree(boolean free) { }
        /** Si esa ventana (o monitor) ya está abierta en el Quest. */
        default boolean isWindowOpen(int id) { return false; }
        default void setLeds(boolean on) { }
        default void setLight(float g) { }
        default void setEnvironment(int e) { }
        /** Modo Panel: abrir VRmin en el visor directo en el Modo Centinela. */
        default void enterSentinel() { enterVR(); }
        /** Apagar del todo el seguimiento de manos (solo controles). */
        default void setHandTracking(boolean on) { }
    }

    final Context ctx;
    final Host host;
    /** Menú del reloj: sin el escritorio de la Mac (ese se ve en la pantalla principal) y con un botón para cerrarlo. */
    final boolean compact;
    private static final int TAB_MACS = 0, TAB_DESK = 1, TAB_GAMES = 2, TAB_SET = 3, TAB_ROOM = 4;

    private FrameLayout rootView;
    private LinearLayout panel, content;
    private TextView[] tabs = new TextView[5];
    private TextView vrButton;
    // Ajustes rápidos: íconos arriba de todo en Ajustes (como el centro de control del iPhone)
    static final String[] QUICK_IDS = {"front", "hands", "portal", "keyboard", "move", "palm", "hud", "cable", "rec"};
    static final String[] QUICK_NAMES = {"Al frente", "Manos reales", "Portal", "Teclado", "Moverte con las manos", "Menú en la mano", "Rendimiento", "Solo cable", "Grabar"};
    static final String[] QUICK_ICONS = {"recenter", "hand", "portal", "keyboard", "move", "watch", "stats", "cable", "record"};
    static final String[] QUICK_HINTS = {"Gira la sala y las ventanas hacia donde estás mirando", "Tus manos (o tus controles) con la cámara del visor",
            "Una zona del cuarto real que dibujás con el dedo", "Abre o cierra el teclado virtual",
            "Sin controles, la mano derecha en forma de pistola te teletransporta",
            "Abrir el menú mirando el borde de la mano", "Cuadros por segundo, Mbps y demora", "Conectarse solo por el cable USB-C",
            "Graba un video en la Mac (y el visor, si está por cable)"};
    /** Grabando (lo graba VRmin Host en la Mac). */
    private static boolean recOn = false;
    private java.util.Set<String> quickItems() {
        String sv = prefs().getString("quickItems3", "front,hands,portal,keyboard,move,hud,rec");
        return new java.util.LinkedHashSet<>(java.util.Arrays.asList(sv.isEmpty() ? new String[0] : sv.split(",")));
    }
    private boolean quickOn(String id) {
        switch (id) {
            case "hands": return prefs().getBoolean("realHands", true);
            case "portal": return portalOn;
            case "move": return prefs().getBoolean("handMove", true);
            case "palm": return prefs().getBoolean("palmMenu", true);
            case "hud": return prefs().getBoolean("hud", false);
            case "cable": return prefs().getBoolean("cableOnly", false);
            case "rec": return recOn;
            default: return false;
        }
    }
    private void quickDo(String id) {
        android.content.SharedPreferences pr = prefs();
        switch (id) {
            case "front": host.recenter(); break;
            case "keyboard": host.toggleKeyboard(); break;
            case "hands": { boolean on = !quickOn(id); pr.edit().putBoolean("realHands", on).apply(); host.setRealHands(on); break; }
            case "portal": { portalOn = !portalOn; host.setPortal(portalOn); if (portalOn) showPortalHint(); else hidePortalHint(); break; }
            case "move": { boolean on = !quickOn(id); pr.edit().putBoolean("handMove", on).apply(); host.setHandMove(on); break; }
            case "palm": { boolean on = !quickOn(id); pr.edit().putBoolean("palmMenu", on).apply(); host.setWatch(on); break; }
            case "hud": { boolean on = !quickOn(id); pr.edit().putBoolean("hud", on).apply(); host.setHud(on); break; }
            case "cable": { boolean on = !quickOn(id); pr.edit().putBoolean("cableOnly", on).apply(); host.setCableOnly(on); break; }
            case "rec": recOn = !recOn; Link.I.send("{\"type\":\"record\",\"on\":" + recOn + "}"); break;
        }
        drawnKey = "";
        render();
    }
    /// Ajustes rápidos: una grilla de íconos con el nombre debajo (los encendidos, en ámbar)
    private void quickGrid(LinearLayout into) {
        java.util.Set<String> items = quickItems();
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(ctx, 10), dp(ctx, 12), dp(ctx, 10), dp(ctx, 6));
        box.setBackground(Ui.glass(ctx, 24, 40));
        final int perRow = 5;
        LinearLayout row = null;
        int n = 0;
        for (int i = 0; i < QUICK_IDS.length; i++) {
            final String id = QUICK_IDS[i];
            if (!items.contains(id)) continue;
            if (n % perRow == 0) {
                row = new LinearLayout(ctx);
                row.setOrientation(LinearLayout.HORIZONTAL);
                box.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
            n++;
            boolean on = quickOn(id);
            LinearLayout cell = new LinearLayout(ctx);
            cell.setOrientation(LinearLayout.VERTICAL);
            cell.setGravity(Gravity.CENTER_HORIZONTAL);
            cell.setPadding(dp(ctx, 4), dp(ctx, 6), dp(ctx, 4), dp(ctx, 8));
            cell.setBackground(Ui.hoverable(ctx, 0, 18, false));
            cell.setClickable(true);
            cell.setOnClickListener(v -> quickDo(id));
            View tile = new View(ctx);
            GradientDrawable g = new GradientDrawable();
            g.setShape(GradientDrawable.OVAL);
            g.setColor(on ? (id.equals("rec") ? Color.rgb(235, 72, 64) : Ui.ACCENT) : Color.argb(34, 255, 255, 255));
            tile.setBackground(g);
            tile.setForeground(Icons.of(ctx, QUICK_ICONS[i], 26, on ? (id.equals("rec") ? Color.WHITE : Ui.ACCENT_DARK) : Ui.TEXT));
            tile.setForegroundGravity(Gravity.CENTER);
            cell.addView(tile, new LinearLayout.LayoutParams(dp(ctx, 58), dp(ctx, 58)));
            TextView name = Ui.label(ctx, I18n.t(id.equals("rec") && on ? "Grabando" : QUICK_NAMES[i]), 13, on ? Ui.ACCENT_TEXT : Ui.TEXT2, true);
            name.setGravity(Gravity.CENTER);
            name.setMaxLines(2);
            name.setPadding(0, dp(ctx, 6), 0, 0);
            cell.addView(name, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            row.addView(cell, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
        if (row != null) while (n % perRow != 0) { row.addView(new View(ctx), new LinearLayout.LayoutParams(0, 1, 1f)); n++; }
        // «Editar»: elegir cuáles aparecen
        TextView edit = Ui.label(ctx, I18n.t("Editar"), 14, Ui.ACCENT_TEXT, true);
        edit.setPadding(dp(ctx, 10), dp(ctx, 4), dp(ctx, 10), dp(ctx, 4));
        edit.setBackground(Ui.hoverable(ctx, 0, 999, false));
        edit.setClickable(true);
        edit.setOnClickListener(v -> openPage("quick"));
        LinearLayout.LayoutParams elp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        elp.gravity = Gravity.END;
        box.addView(edit, elp);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 6); lp.bottomMargin = dp(ctx, 6);
        into.addView(box, lp);
    }

    /// Los modos, en la barra lateral (el de ahora, en gris): Panel, Studio, Juego y Centinela
    private void modesBox(LinearLayout side) {
        TextView h = Ui.label(ctx, I18n.t("Modos"), 13, Ui.TEXT3, true);
        h.setPadding(dp(ctx, 14), 0, 0, dp(ctx, 2));
        side.addView(h);
        final int cur = host.inVR() ? 1 : 0;
        String[][] modes = {{"panel", "Modo Panel"}, {"studio", "Modo Studio"}, {"gamepad", "Modo Juego"}, {"eye", "Modo Centinela"}};
        for (int i = 0; i < modes.length; i++) {
            final int k = i;
            TextView t = Ui.label(ctx, I18n.t(modes[i][1]), 15, Ui.TEXT, true);
            t.setGravity(Gravity.CENTER_VERTICAL);
            Ui.leadingIcon(t, modes[i][0], 20, k == cur ? Ui.TEXT3 : Ui.ACCENT_TEXT);
            t.setPadding(dp(ctx, 14), dp(ctx, 8), dp(ctx, 14), dp(ctx, 8));
            if (k == cur) {   // ya estás en este modo
                t.setTextColor(Ui.TEXT3);
                t.setText(I18n.t(modes[i][1]) + I18n.t(" · ahora"));
            } else {
                t.setBackground(Ui.hoverable(ctx, 0, 999, false));
                t.setClickable(true);
                t.setOnClickListener(v -> {
                    if (k == 0) host.exitVR();
                    else if (k == 1) goVR();
                    else if (k == 2) setTab(TAB_GAMES);
                    else startSentinel();
                });
            }
            side.addView(t, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
    }

    /// Modo Centinela: primero su tutorial (con «Empezar»); si lo apagaste, empieza directo
    private void startSentinel() {
        Tutorials.orStart(rootView, "centinela", I18n.t("Empezar el Modo Centinela"), () -> {
            if (!host.inVR()) { if (launching) return; launching = true; }
            host.enterSentinel();
        });
    }

    /// Abre el menú en una página de Ajustes (el botón de ajustes de la barra de cada pantalla)
    void openSettings(String page) {
        if (tab != TAB_SET) setTab(TAB_SET);
        openPage(page);
        setPanel(true);
    }

    /// Paso con ícono a la izquierda
    private LinearLayout stepRow(String icon, String text, float sp) {
        LinearLayout r = new LinearLayout(ctx);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(ctx, 6), 0, dp(ctx, 6));
        r.addView(Ui.iconTile(ctx, icon), new LinearLayout.LayoutParams(dp(ctx, 42), dp(ctx, 42)));
        TextView t = Ui.label(ctx, text, sp, Ui.TEXT, false);
        t.setPadding(dp(ctx, 14), 0, 0, 0);
        r.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return r;
    }

    /// Tutoriales que aparecen solos al llegar a cada lugar (si no los apagaste)
    private void autoTutorial() {
        if (compact) return;
        final String id;
        if (!host.inVR() && Tutorials.wanted(ctx, "modos")) id = "modos";
        else id = tab == TAB_MACS ? (Link.I.isConnected() ? null : "conectar")
                : tab == TAB_DESK ? (host.inVR() ? null : "escritorio")
                : tab == TAB_GAMES ? "juego_abrir"
                : tab == TAB_SET && host.inVR() ? "ajustes_rapidos"
                : tab == TAB_ROOM ? "salas" : null;
        if (id == null || !Tutorials.wanted(ctx, id)) return;
        rootView.postDelayed(() -> Tutorials.auto(rootView, id,
                // después de los modos, cómo conectar la Mac (si todavía no está conectada)
                id.equals("modos") ? () -> { if (!Link.I.isConnected()) Tutorials.auto(rootView, "conectar"); } : null), 700);
    }

    private boolean autoVr = false;   // ya entramos en VR solos por este juego
    private static final String[] TAB_NAMES = {"Computadoras", "Escritorio", "Juegos", "Ajustes", "Salas"};
    private String[] tabIcons;
    private TextView footer;
    private DwellButton menuButton;
    private View scrim;
    private Ui.AspectFrame deskFrame;
    private int tab = TAB_MACS;
    private int gestureProfile = 0;   // página de gestos: 0 sala VR, 1 juegos
    private boolean panelOpen = true, deskShowing = false, launching = false, keepMenu = false;
    private boolean monitorsMode;
    private String drawnKey = "";

    private static final Map<String, Bitmap> covers = new ConcurrentHashMap<>();
    private static final ExecutorService loader = Executors.newFixedThreadPool(3);

    // ------------------------------------------------------------------ armado
    MenuUi(Context ctx, Host host) { this(ctx, host, false); }

    MenuUi(Context ctx, Host host, boolean compact) {
        I18n.init(ctx);
        this.ctx = ctx;
        this.host = host;
        this.compact = compact;
        monitorsMode = ctx.getSharedPreferences("qvb", Context.MODE_PRIVATE).getBoolean("monitors", false);
        rootView = new FrameLayout(ctx);
        ALL.add(new java.lang.ref.WeakReference<>(this));
        rootView.setBackgroundColor(Ui.BG);

        // Pantalla principal de la Mac (oculta hasta que la actives)
        deskFrame = new Ui.AspectFrame(ctx);
        deskFrame.setVisibility(View.GONE);
        rootView.addView(deskFrame, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        final StreamDecoder.Host dhost = new StreamDecoder.Host() {
            @Override public void requestKey() { Link.I.send("{\"type\":\"deskkey\"}"); }
            @Override public void onSize(int w, int h) { rootView.post(() -> deskFrame.setAspect(w / (float) h)); }
        };
        if (!compact) {   // el menú del reloj no muestra el escritorio (no se lo saca a la pantalla principal)
            View deskView = VideoSurface.create(ctx, host.inVR(), new VideoSurface.Callback() {
                @Override public void created(android.view.Surface s) { Link.I.desk.attach(s, dhost); }
                @Override public void destroyed() { Link.I.desk.detach(); }
            });
            deskFrame.addView(deskView, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            Pointer.attach(deskView, new Pointer.Gate() {
                @Override public boolean blocked() { return panelOpen; }
                @Override public void tapWhileBlocked() { setPanel(false); }
            }, (u, v, down, scroll) -> Link.I.send(Pointer.json("mouse", -1, u, v, down, scroll)));
        }

        // Velo oscuro sobre el escritorio cuando el menú está abierto (para que el menú resalte)
        scrim = new View(ctx);
        scrim.setBackgroundColor(Color.argb(150, 0, 0, 0));
        scrim.setVisibility(View.GONE);
        rootView.addView(scrim, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // Panel de vidrio: barra lateral + contenido, con sombra y un borde que brilla
        panel = new LinearLayout(ctx);
        panel.setOrientation(LinearLayout.HORIZONTAL);
        GradientDrawable pbg = Ui.glass(ctx, 40, 238);
        pbg.setStroke(dp(ctx, 1), Color.argb(60, 255, 255, 255));
        panel.setBackground(pbg);
        panel.setElevation(dp(ctx, 30));
        panel.setOutlineAmbientShadowColor(Color.BLACK);
        panel.setOutlineSpotShadowColor(Color.BLACK);
        panel.setClipToOutline(true);
        panel.setClickable(true);
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        plp.setMargins(dp(ctx, 16), dp(ctx, 16), dp(ctx, 16), dp(ctx, 16));
        rootView.addView(panel, plp);

        LinearLayout side = new LinearLayout(ctx);
        side.setOrientation(LinearLayout.VERTICAL);
        side.setPadding(dp(ctx, 16), dp(ctx, 28), dp(ctx, 16), dp(ctx, 18));
        GradientDrawable sbg = new GradientDrawable();
        sbg.setColor(Color.argb(46, 0, 0, 0));
        { float r = Ui.rad(ctx, 40); sbg.setCornerRadii(new float[]{r, r, 0, 0, 0, 0, r, r}); }   // (igual que las esquinas del panel)
        side.setBackground(sbg);
        panel.addView(side, new LinearLayout.LayoutParams(dp(ctx, 250), ViewGroup.LayoutParams.MATCH_PARENT));
        // Logo centrado en la barra lateral
        Ui.Logo brand = new Ui.Logo(ctx, 30);
        LinearLayout.LayoutParams brlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        brlp.bottomMargin = dp(ctx, 18);
        side.addView(brand, brlp);
        String[] names = TAB_NAMES;
        tabIcons = new String[]{"laptop", "display", "gamepad", "sliders", "room"};
        // Salas va antes de Ajustes, y solo en la sala VR (en las ventanas del Quest no hay sala)
        int[] order = host.inVR() ? new int[]{TAB_MACS, TAB_DESK, TAB_GAMES, TAB_ROOM, TAB_SET} : new int[]{TAB_MACS, TAB_DESK, TAB_GAMES, TAB_SET};
        for (int i : order) {
            final int t = i;
            tabs[i] = Ui.label(ctx, I18n.t(names[i]), 17, Ui.TEXT, true);
            tabs[i].setGravity(Gravity.CENTER_VERTICAL);
            Ui.leadingIcon(tabs[i], tabIcons[i], 22, Ui.TEXT2);
            tabs[i].setPadding(dp(ctx, 14), dp(ctx, 11), dp(ctx, 14), dp(ctx, 11));
            tabs[i].setClickable(true);
            tabs[i].setOnClickListener(v -> setTab(t));
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            tlp.topMargin = dp(ctx, 4);
            side.addView(tabs[i], tlp);
        }
        // Ayuda: todos los tutoriales
        TextView help = Ui.label(ctx, I18n.t("Ayuda"), 17, Ui.TEXT2, true);
        help.setGravity(Gravity.CENTER_VERTICAL);
        Ui.leadingIcon(help, "help", 22, Ui.TEXT2);
        help.setPadding(dp(ctx, 14), dp(ctx, 11), dp(ctx, 14), dp(ctx, 11));
        help.setBackground(Ui.hoverable(ctx, 0, 999, false));
        help.setClickable(true);
        help.setOnClickListener(v -> Tutorials.showHelp(rootView));
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hlp.topMargin = dp(ctx, 4);
        side.addView(help, hlp);
        // «Algo anda mal»: la Mac junta los registros en un .zip en su Escritorio
        TextView bug = Ui.label(ctx, I18n.t("Algo anda mal"), 17, Ui.TEXT2, true);
        bug.setGravity(Gravity.CENTER_VERTICAL);
        Ui.leadingIcon(bug, "bulb", 22, Ui.TEXT2);
        bug.setPadding(dp(ctx, 14), dp(ctx, 11), dp(ctx, 14), dp(ctx, 11));
        bug.setBackground(Ui.hoverable(ctx, 0, 999, false));
        bug.setClickable(true);
        bug.setOnClickListener(v -> {
            if (Link.I.state() == null) { Tutorials.confirm(rootView, I18n.t("Algo anda mal"), I18n.t("Conectate a tu Mac primero: el reporte lo arma VRmin Host con los registros de la Mac."), I18n.t("Ir a Computadoras"), () -> setTab(TAB_MACS)); return; }
            Tutorials.confirm(rootView, I18n.t("Algo anda mal"),
                    I18n.t("VRmin Host junta los registros de la Mac, del visor y de los juegos en un .zip en el Escritorio de la Mac. Mandalo cuando pidas ayuda y contá qué pasó. No incluye claves ni datos personales."),
                    I18n.t("Armar el reporte"), () -> {
                        Link.I.send("{\"type\":\"report\"}");
                        rootView.postDelayed(() -> Tutorials.confirm(rootView, I18n.t("Algo anda mal"), I18n.t("Listo: VRmin Host armó un reporte en el Escritorio de la Mac («VRmin reporte…zip»). Mandalo cuando pidas ayuda y contá qué pasó."), I18n.t("Entendido"), () -> { }), 400);
                    });
        });
        side.addView(bug, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        side.addView(new View(ctx), new LinearLayout.LayoutParams(1, 0, 1f));
        if (!compact) modesBox(side);   // los modos (en lugar de «Entrar en Studio» / «Salir de Studio»)
        footer = Ui.label(ctx, "", 13, Ui.TEXT2, false);
        footer.setPadding(dp(ctx, 10), 0, dp(ctx, 6), dp(ctx, 10));
        side.addView(footer);
        TextView quitApp = Ui.pill(ctx, I18n.t("Cerrar VRmin"));
        // (sin ícono: con un ícono a la izquierda y el botón a todo lo ancho, el texto queda corrido a la derecha)
        quitApp.setOnClickListener(v -> Tutorials.confirm(rootView, I18n.t("¿Cerrar VRmin?"),
                I18n.t("La app se cierra del todo. Si hay un juego abierto en la Mac, sigue abierto."), I18n.t("Cerrar"), host::quitApp));
        LinearLayout.LayoutParams qalp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        qalp.bottomMargin = dp(ctx, 8);
        side.addView(quitApp, qalp);
        TextView vr = vrButton = host.inVR() ? Ui.pill(ctx, I18n.t("Salir de Studio")) : Ui.primary(ctx, I18n.t("Entrar en Studio"));
        Ui.leadingIcon(vr, host.inVR() ? "exit" : "visor", 20, host.inVR() ? Ui.TEXT : Ui.ACCENT_DARK);
        vr.setOnClickListener(v -> { if (host.inVR()) host.exitVR(); else goVR(); });
        if (compact) {   // soltar el menú del reloj (queda como una ventana) o volver a engancharlo
            wristButton = Ui.pill(ctx, "");
            wristButton.setOnClickListener(v -> host.setWristMenuFree(!host.isWristMenuFree()));
            refreshWristButton();
            LinearLayout.LayoutParams wlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            wlp.bottomMargin = dp(ctx, 8);
            side.addView(wristButton, wlp);
        }
        if (compact) {   // cerrar el menú del reloj
            TextView close = Ui.pill(ctx, I18n.t("Cerrar menú"));
            Ui.leadingIcon(close, "close", 20, Ui.TEXT);
            close.setOnClickListener(v -> host.closeWristMenu());
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            clp.bottomMargin = dp(ctx, 8);
            side.addView(close, clp);
        }
        if (compact) side.addView(vr);

        ScrollView sv = new ScrollView(ctx);
        sv.setVerticalScrollBarEnabled(false);
        content = new LinearLayout(ctx);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(ctx, 34), dp(ctx, 30), dp(ctx, 34), dp(ctx, 30));
        sv.addView(content);
        panel.addView(sv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        // Botón de la esquina izquierda (solo con el escritorio a la vista): se activa apuntándole un segundo
        menuButton = new DwellButton(ctx, "");
        Icons.on(menuButton, "menu", 32);
        menuButton.setElevation(dp(ctx, 40));
        menuButton.setVisibility(View.GONE);
        menuButton.setOnClickListener(v -> setPanel(!panelOpen));
        FrameLayout.LayoutParams blp = new FrameLayout.LayoutParams(dp(ctx, 68), dp(ctx, 68), Gravity.TOP | Gravity.START);
        blp.setMargins(dp(ctx, 14), dp(ctx, 14), 0, 0);
        rootView.addView(menuButton, blp);


        deskFrame.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> reportDesk());
        if (host.inVR() && !compact) {   // (el menú del reloj no vuelve a aplicar los ajustes: ya están puestos)
            host.set3DMode(mode3D());
            applyImage();
            host.setHud(prefs().getBoolean("hud", false));
            host.setHandModel(prefs().getInt("hands", 0));
            host.setHandColor(prefs().getInt("skinTone", 1));
            applyLaser();
            host.setWatch(prefs().getBoolean("palmMenu", true));   // mirar el borde de la mano abre el menú
            host.setRealHands(prefs().getBoolean("realHands", true));
            host.setHandColor(prefs().getInt("handSat", 50) / 50f * 0.8f);
            host.setEdgeSoft(prefs().getInt("edgeSoft", 60) / 100f);
            host.setLaserNoTouch(false);   // el láser siempre toca al abrir la app (cada pantalla tiene su botón para apagarlo)
            host.setHandTracking(prefs().getBoolean("handTracking", true));
            host.set3DStrength(prefs().getInt("3dStrength", 30) / 100f);
            host.setLeds(prefs().getBoolean("leds", true));
            host.setLight(prefs().getInt("light", 50) / 50f);
            host.setEnvironment(prefs().getInt("env", 0));
        }
        setTab(Link.I.isConnected() || (host.inVR() && Link.I.target() != null) ? TAB_DESK : TAB_MACS);
    }

    private TextView wristButton;

    /** Menú del reloj: el botón dice qué va a hacer (soltarlo o volver a ponerlo sobre el reloj). */
    void refreshWristButton() {
        if (wristButton == null) return;
        boolean free = host.isWristMenuFree();
        wristButton.setText(I18n.t(free ? "Volver a la mano" : "Soltar de la mano"));
        Ui.leadingIcon(wristButton, free ? "lock" : "unlock", 20, Ui.TEXT);
    }

    /** Abre o cierra el menú encima del escritorio (desde la barra de botones de la sala VR). */
    void togglePanel() { setPanel(!panelOpen); }

    private void setPanel(boolean open) {
        boolean was = panel.getVisibility() == View.VISIBLE;
        panelOpen = open || !deskShowing;
        // El menú aparece creciendo apenas y se va achicándose (como en el visor)
        panel.animate().cancel();
        if (panelOpen && !was) {
            panel.setVisibility(View.VISIBLE);
            panel.setAlpha(0f); panel.setScaleX(0.96f); panel.setScaleY(0.96f);
            panel.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(200)
                    .setInterpolator(new android.view.animation.DecelerateInterpolator(1.6f)).start();
        } else if (!panelOpen && was) {
            panel.animate().alpha(0f).scaleX(0.96f).scaleY(0.96f).setDuration(150)
                    .withEndAction(() -> { if (!panelOpen) panel.setVisibility(View.GONE); panel.setAlpha(1f); panel.setScaleX(1f); panel.setScaleY(1f); }).start();
        } else {
            panel.setVisibility(panelOpen ? View.VISIBLE : View.GONE);
            panel.setAlpha(1f); panel.setScaleX(1f); panel.setScaleY(1f);
        }
        scrim.setVisibility(panelOpen && deskShowing ? View.VISIBLE : View.GONE);
        // La primera vez que se ve la pantalla principal en Studio: cómo volver al menú, y los botones de las pantallas
        if (!panelOpen && deskShowing && host.inVR() && !compact)
            rootView.postDelayed(() -> Tutorials.auto(rootView, "studio_menu", () -> Tutorials.auto(rootView, "barra")), 500);
        // En la sala VR el botón del menú va por fuera de la pantalla (en la barra de arriba)
        menuButton.setVisibility(deskShowing && !host.inVR() ? View.VISIBLE : View.GONE);
        Icons.on(menuButton, panelOpen ? "close" : "menu", 32);
        menuButton.setInstant(panelOpen);   // con el menú abierto no hay nada de la Mac debajo: se toca directo
        rootView.post(this::reportDesk);
    }

    /// Le cuenta a la sala VR dónde está la imagen del escritorio (el 3D solo se aplica ahí, con el menú cerrado)
    private void reportDesk() {
        if (compact) return;
        int W = rootView.getWidth(), H = rootView.getHeight();
        if (W <= 0 || H <= 0) return;
        boolean vis = deskShowing && !panelOpen;
        host.onDeskRect(vis, deskFrame.getLeft() / (float) W, deskFrame.getTop() / (float) H,
                deskFrame.getRight() / (float) W, deskFrame.getBottom() / (float) H);
    }

    private void setTab(int t) {
        tab = t;
        for (int i = 0; i < tabs.length; i++) {
            if (tabs[i] == null) continue;
            // la sección elegida se tiñe de ámbar (fondo suave, texto e ícono ámbar claro)
            tabs[i].setBackground(Ui.hoverable(ctx, i == t ? Ui.ACCENT_SEL : 0, 999, false));
            tabs[i].setTextColor(i == t ? Ui.ACCENT_TEXT : Ui.TEXT2);
            tabs[i].setText(I18n.t(TAB_NAMES[i]));   // (por si cambió el idioma)
            Ui.leadingIcon(tabs[i], tabIcons[i], 22, i == t ? Ui.ACCENT_TEXT : Ui.TEXT2);
        }
        setPage = null;
        openChoice = null;
        drawnKey = "";
        rootView.post(() -> slideIn(0));
        render();
        autoTutorial();
    }

    View view() { return rootView; }

    void start() { launching = false; Link.I.addListener(this); }

    void stop() { Link.I.removeListener(this); }

    // ------------------------------------------------------------------ acciones
    /// Pasa a la parte inmersiva (otro proceso). La conexión de las ventanas se corta para que la use la VR.
    private void goVR() {
        if (launching || host.inVR()) return;
        launching = true;
        host.enterVR();
    }

    private void openMacWindow(int id, String app, String title, int w, int h) {
        host.openWindow(id, app, title, w, h);
        drawnKey = "";
        content.postDelayed(this::render, 150);   // el botón «Agregar monitor» pasa al siguiente
    }

    /** Desde el botón 3D de la barra de la sala VR (queda igual que si se tocara acá). */
    void set3DModeFromBar(int mode) { set3DMode(mode); drawnKey = ""; render(); }

    /// Modo 3D guardado de la pantalla principal (las versiones anteriores guardaban solo prendido/apagado)
    int mode3D() { return prefs().getInt("3dMode", prefs().getBoolean("3d", false) ? 1 : 0); }

    void set3DStrengthFromBar(int p) {
        prefs().edit().putInt("3dStrength", p).apply();
        host.set3DStrength(p / 100f);
        drawnKey = "";   // el control del menú se actualiza con el próximo estado (no en cada movimiento)
    }

    private void set3DMode(int mode) {
        prefs().edit().putInt("3dMode", mode).putBoolean("3d", mode > 0).apply();
        // Solo el modo IA necesita que la Mac calcule la profundidad
        Link.I.send("{\"type\":\"depth3d\",\"on\":" + (mode == 1) + "}");
        host.set3DMode(mode);
    }

    // ------------------------------------------------------------------ Ajustes de imagen
    /// brillo, contraste, saturación, nitidez, calidez
    static final String[] IMG_NAMES = {"Normal", "Vívido", "Cine", "Nítido", "Noche", "Personal"};
    static final float[][] IMG_PRESETS = {
            {1f, 1f, 1f, 0f, 0f},
            {1.03f, 1.12f, 1.3f, 0.3f, 0f},
            {0.97f, 1.08f, 0.9f, 0.15f, 0.35f},
            {1f, 1.05f, 1.05f, 0.9f, 0f},
            {0.8f, 0.95f, 0.9f, 0f, 0.6f},
    };

    /// Valores del filtro personalizado a partir de sus controles (0–100; 50 = sin cambio, nitidez desde 0)
    private float[] customImage() {
        return new float[]{
                0.6f + prefs().getInt("imgB", 50) / 100f * 0.8f,
                0.6f + prefs().getInt("imgC", 50) / 100f * 0.8f,
                prefs().getInt("imgS", 50) / 50f,
                prefs().getInt("imgSh", 0) / 100f * 1.5f,
                (prefs().getInt("imgW", 50) - 50) / 50f};
    }

    private void applyImage() {
        int k = prefs().getInt("imgPreset", 0);
        float[] v = k < IMG_PRESETS.length ? IMG_PRESETS[k] : customImage();
        host.setImage(v[0], v[1], v[2], v[3], v[4]);
    }

    private void setMode(boolean monitors) {
        if (monitors == monitorsMode) return;
        monitorsMode = monitors;
        ctx.getSharedPreferences("qvb", Context.MODE_PRIVATE).edit().putBoolean("monitors", monitors).apply();
        host.closeAllWindows();   // cerrar las ventanas del otro modo
        drawnKey = "";
        render();
    }

    // ------------------------------------------------------------------ eventos de la conexión
    @Override
    public void onMacs(List<Link.Mac> macs) { if (tab == TAB_MACS) render(); }

    private final StringBuilder pairDigits = new StringBuilder();   // código que se va escribiendo

    @Override
    public void onPairing(String mac, boolean wrong) {
        if (mac != null) { pairDigits.setLength(0); if (tab != TAB_MACS) setTab(TAB_MACS); }
        drawnKey = "";
        render();
    }

    @Override
    public void onConnection(boolean connected, String name) {
        setFooter(connected ? I18n.t("Conectado a ") + name : I18n.t("Sin conexión"), connected);
        if (connected && tab == TAB_MACS) setTab(TAB_DESK);
        if (connected) sendDeskCfg();
        // La pantalla principal se conecta sola: el menú queda abierto y el botón del menú la muestra
        if (connected && !compact && !deskShowing) { keepMenu = true; Link.I.send("{\"type\":\"desk\",\"on\":true}"); }
        if (connected && host.inVR() && mode3D() == 1)
            Link.I.send("{\"type\":\"depth3d\",\"on\":true}");
        if (!connected) {
            showDesk(false);
            if (Link.I.target() == null) setTab(TAB_MACS);
        }
        drawnKey = "";
        render();
    }

    @Override
    public void onState(JSONObject o) {
        String st = o.optString("status");
        // Un juego VR abierto desde la ventana del Quest: solo se ve dentro de VR, así que entramos solos (una vez por juego)
        if (o.optBoolean("game") && !host.inVR() && !compact) {
            if (!autoVr) { autoVr = true; host.enterVR(); }
        } else if (!o.optBoolean("game")) autoVr = false;
        setFooter(!st.isEmpty() ? st : o.optBoolean("game") ? (host.inVR() ? I18n.t("Hay un juego abierto") : I18n.t("Hay un juego abierto: tocá «Entrar en Studio»")) : I18n.t("Conectado"), true);
        showDesk(o.optBoolean("desk") && o.optString("deskErr").isEmpty());
        render();
    }

    private void showDesk(boolean show) {
        if (compact || show == deskShowing) return;
        deskShowing = show;
        // El escritorio de la Mac aparece suave (y un poquito más grande al principio, como si se acercara)
        deskFrame.animate().cancel();
        if (show) {
            deskFrame.setVisibility(View.VISIBLE);
            deskFrame.setAlpha(0f); deskFrame.setScaleX(1.03f); deskFrame.setScaleY(1.03f);
            deskFrame.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(320)
                    .setInterpolator(new android.view.animation.DecelerateInterpolator(1.8f)).start();
        } else {
            deskFrame.animate().alpha(0f).setDuration(150)
                    .withEndAction(() -> { if (!deskShowing) deskFrame.setVisibility(View.GONE); deskFrame.setAlpha(1f); }).start();
        }
        setPanel(keepMenu || !show);   // al activar el escritorio, el menú se esconde (salvo si se conectó solo)
        keepMenu = false;
    }

    // ------------------------------------------------------------------ contenido de cada pestaña
    private void render() {
        JSONObject st = Link.I.state();
        boolean conn = Link.I.isConnected();
        String key = tab + "|" + conn + "|" + monitorsMode + "|";
        if (tab == TAB_MACS) {
            StringBuilder k = new StringBuilder();
            for (Link.Mac m : Link.I.macs()) k.append(m.name).append(m.ip).append(m.usb);
            Link.Mac t = Link.I.target();
            key += k + "|" + (t == null ? "" : t.name) + "|" + Link.I.pairingMac() + Link.I.pairWrong() + pairDigits;
        } else if (tab == TAB_SET && (st == null || !conn)) {
            key += setKey;
        } else if (st != null) {
            key += tab == TAB_ROOM ? setKey
                    : tab == TAB_SET ? st.optJSONObject("cfg") + "" + st.optJSONObject("adv") + setKey
                    : tab == TAB_GAMES ? st.optJSONArray("games") + "" + st.optBoolean("busy") + st.optBoolean("game") + st.optJSONObject("adv") + st.optJSONObject("cfg") + gameCfgName + gamesQuality
                    : st.optBoolean("desk") + st.optString("deskErr") + st.optJSONArray("windows") + st.optJSONArray("displays") + st.optBoolean("monitor") + st.optString("monitorErr")
                    + st.optBoolean("depth3d") + st.optString("depthStatus") + mode3D() + openMonitors(st);
        }
        if (key.equals(drawnKey)) return;
        drawnKey = key;
        content.removeAllViews();
        if (tab == TAB_MACS) renderMacs();
        else if (tab == TAB_ROOM) renderRoom();
        else if (tab == TAB_SET) renderSettings(conn ? st : null);   // los ajustes del Quest se usan sin conectarse a la Mac
        else if (!conn || st == null) {
            header(I18n.t("Sin conexión"), I18n.t("Primero elegí tu Mac en «Computadoras»."));
            TextView go = Ui.pill(ctx, I18n.t("Ir a Computadoras"));
            go.setOnClickListener(v -> setTab(TAB_MACS));
            content.addView(go, wrap());
        } else if (tab == TAB_DESK) renderDesk(st);
        else if (tab == TAB_SET) renderSettings(st);
        else renderGames(st);
    }

    /// Qué monitores ya están abiertos (para que el botón «Agregar monitor» se actualice al cerrar uno)
    private String openMonitors(JSONObject st) {
        StringBuilder b = new StringBuilder(host.isWindowOpen(WindowUi.MONITOR_ID) ? "v" : "-");
        JSONArray ds = st.optJSONArray("displays");
        for (int i = 0; ds != null && i < ds.length(); i++) b.append(host.isWindowOpen(ds.optJSONObject(i).optInt("id")) ? '1' : '0');
        return b.toString();
    }

    /// Estado abajo de la barra lateral, con un puntito ámbar cuando hay conexión
    private void setFooter(String text, boolean on) {
        footer.setText(text);
        if (on) Ui.leadingIcon(footer, "dot", 16, Ui.ACCENT); else footer.setCompoundDrawablesRelative(null, null, null, null);
        footer.setCompoundDrawablePadding(dp(ctx, 4));
    }

    private LinearLayout.LayoutParams wrap() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 12);
        return lp;
    }

    private void header(String title, String sub) {
        TextView h = Ui.label(ctx, title, 34, Ui.TEXT, true);
        h.setLetterSpacing(-0.01f);
        content.addView(h);
        if (sub != null) {
            TextView s = Ui.label(ctx, sub, 16, Ui.TEXT2, false);
            s.setPadding(0, dp(ctx, 4), 0, dp(ctx, 10));
            content.addView(s);
        }
    }

    /// Emparejar: teclado numérico para el código que muestra la Mac
    private void renderPairing(String mac) {
        header(I18n.t("Emparejar con ") + mac, I18n.t("Escribí el código de 6 números que aparece en la Mac. Solo hace falta la primera vez."));
        StringBuilder shown = new StringBuilder();
        for (int i = 0; i < 6; i++) { if (i > 0) shown.append("  "); shown.append(i < pairDigits.length() ? pairDigits.charAt(i) : '·'); }
        TextView code = Ui.label(ctx, shown.toString(), 40, Ui.TEXT, true);
        code.setPadding(0, dp(ctx, 8), 0, dp(ctx, 4));
        content.addView(code);
        if (Link.I.pairWrong()) {
            TextView w = Ui.label(ctx, I18n.t("Ese código no es. Fijate el que muestra la Mac y probá de nuevo."), 15, Ui.ACCENT, true);
            content.addView(w);
        }
        android.widget.GridLayout g = new android.widget.GridLayout(ctx);
        g.setColumnCount(3);
        String[] keys = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "⌫", "0", "✓"};
        for (String k : keys) {
            TextView b = "✓".equals(k) ? Ui.primary(ctx, I18n.t("Listo")) : Ui.pill(ctx, k);
            b.setTextSize(22);
            b.setGravity(Gravity.CENTER);
            b.setOnClickListener(v -> {
                if ("⌫".equals(k)) { if (pairDigits.length() > 0) pairDigits.setLength(pairDigits.length() - 1); }
                else if ("✓".equals(k)) { if (pairDigits.length() == 6) { Link.I.submitCode(pairDigits.toString()); pairDigits.setLength(0); } }
                else if (pairDigits.length() < 6) pairDigits.append(k);
                drawnKey = "";
                render();
            });
            android.widget.GridLayout.LayoutParams lp = new android.widget.GridLayout.LayoutParams();
            lp.width = dp(ctx, 110); lp.height = dp(ctx, 64);
            lp.setMargins(dp(ctx, 6), dp(ctx, 6), dp(ctx, 6), dp(ctx, 6));
            g.addView(b, lp);
        }
        content.addView(g, wrap());
        TextView cancel = Ui.pill(ctx, I18n.t("Cancelar"));
        cancel.setOnClickListener(v -> { pairDigits.setLength(0); Link.I.cancelPairing(); drawnKey = ""; render(); });
        content.addView(cancel, wrap());
    }

    private void renderMacs() {
        String pm = Link.I.pairingMac();
        Link.Mac tg = Link.I.target();
        if (pm != null && tg != null && pm.equals(tg.name)) { renderPairing(pm); return; }
        header(I18n.t("Computadoras"), I18n.t("Macs con VRmin Host abierto en tu red Wi-Fi."));
        // Conexión: a mano, solo por cable (el mismo ajuste que en Ajustes › Conexión)
        toggle(content, null, I18n.t("Solo por cable USB-C"), I18n.t("Los juegos y las pantallas van siempre por el cable (sin Wi-Fi). Sin cable no se conecta."),
                prefs().getBoolean("cableOnly", false), on -> { prefs().edit().putBoolean("cableOnly", on).apply(); host.setCableOnly(on); });
        List<Link.Mac> list = Link.I.macs();
        Link.Mac t = Link.I.target();
        boolean conn = Link.I.isConnected();
        if (t != null && !list.isEmpty() && list.stream().noneMatch(m -> m.name.equals(t.name))) list.add(0, t);
        if (list.isEmpty() && t != null) list.add(t);
        if (list.isEmpty()) {
            TextView e = Ui.label(ctx, I18n.t("Buscando…\n\nAbrí VRmin Host en la Mac. El Quest y la Mac tienen que estar en la misma red."), 17, Ui.TEXT2, false);
            e.setPadding(0, dp(ctx, 20), 0, 0);
            content.addView(e);
            return;
        }
        for (Link.Mac m : list) {
            boolean mine = t != null && t.name.equals(m.name);
            final String where = m.usb ? I18n.t("por cable USB") : m.ip;
            LinearLayout r = Ui.row(ctx, "laptop", m.name, mine ? (conn ? I18n.t("Conectado · ") + where : m.idle ? I18n.t("Encendiendo el servidor en la Mac…") : I18n.t("Conectando… · ") + where)
                    : m.idle ? (m.usb ? I18n.t("Lista por cable USB · se enciende sola al conectar") : I18n.t("Lista · se enciende sola al conectar")) : I18n.t("Servidor encendido · ") + where);
            TextView b = mine ? Ui.pill(ctx, I18n.t("Desconectar")) : Ui.primary(ctx, I18n.t("Conectar"));
            b.setOnClickListener(v -> {
                if (mine) Link.I.disconnect(); else { Link.I.disconnect(); Link.I.connectTo(m); }
                drawnKey = "";
                render();
            });
            r.addView(b);
            r.setClickable(false);
            content.addView(r);
        }
    }

    private void renderDesk(JSONObject st) {
        header(I18n.t("Escritorio"), I18n.t("La pantalla de tu Mac y sus ventanas, cada una donde quieras."));
        boolean desk = st.optBoolean("desk");
        String deskErr = st.optString("deskErr");
        content.addView(Ui.section(ctx, I18n.t("Pantalla principal")));
        LinearLayout r = Ui.row(ctx, "display", I18n.t("Pantalla principal de la Mac"),
                !deskErr.isEmpty() ? I18n.t("Falta el permiso de grabación de pantalla en la Mac (Ajustes del Sistema → Privacidad y seguridad)")
                        : desk ? I18n.t("Se muestra en esta ventana · el botón del menú vuelve acá") : I18n.t("Se muestra en esta misma ventana"));
        TextView b = desk ? Ui.pill(ctx, I18n.t("Ocultar")) : Ui.primary(ctx, I18n.t("Mostrar"));
        b.setOnClickListener(v -> Link.I.send("{\"type\":\"desk\",\"on\":" + !desk + "}"));
        r.addView(b);
        r.setClickable(false);
        content.addView(r);

        content.addView(Ui.section(ctx, I18n.t("Más ventanas")));
        LinearLayout seg = new LinearLayout(ctx);
        seg.setOrientation(LinearLayout.HORIZONTAL);
        seg.setPadding(dp(ctx, 4), dp(ctx, 4), dp(ctx, 4), dp(ctx, 4));
        GradientDrawable trk = new GradientDrawable();
        trk.setColor(Color.argb(56, 0, 0, 0));
        trk.setCornerRadius(Ui.rad(ctx, 999));
        seg.setBackground(trk);
        TextView a = Ui.pill(ctx, I18n.t("Apps en ventanas")), m = Ui.pill(ctx, I18n.t("Monitores"));
        a.setBackground(Ui.hoverable(ctx, monitorsMode ? 0 : Ui.ACCENT_SEG, 999, false));
        m.setBackground(Ui.hoverable(ctx, monitorsMode ? Ui.ACCENT_SEG : 0, 999, false));
        a.setTextColor(monitorsMode ? Ui.TEXT2 : Ui.ACCENT_TEXT);
        m.setTextColor(monitorsMode ? Ui.ACCENT_TEXT : Ui.TEXT2);
        a.setOnClickListener(v -> setMode(false));
        m.setOnClickListener(v -> setMode(true));
        seg.addView(a, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        seg.addView(m, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(dp(ctx, 460), ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.topMargin = dp(ctx, 6);
        slp.bottomMargin = dp(ctx, 4);
        content.addView(seg, slp);

        if (monitorsMode) {
            // Un solo botón: primero suma los monitores físicos de la Mac, y recién después el virtual (nunca al revés)
            JSONArray ds = st.optJSONArray("displays");
            int n = ds == null ? 0 : ds.length();
            JSONObject next = nextMonitor(st);
            boolean virtOpen = host.isWindowOpen(WindowUi.MONITOR_ID);
            String err = st.optString("monitorErr");
            String sub = next != null ? I18n.t("Siguiente: ") + next.optString("name") + I18n.t(" (monitor de la Mac · ") + next.optInt("w") + "×" + next.optInt("h") + ")"
                    : !virtOpen ? (!err.isEmpty() ? err : n > 0 ? I18n.t("Ya están todos los monitores de la Mac. Siguiente: un monitor virtual")
                            : I18n.t("Siguiente: un monitor virtual (una pantalla más que solo existe en el Quest)"))
                    : I18n.t("Ya agregaste todos los monitores");
            LinearLayout add = Ui.row(ctx, "addDisplay", I18n.t("Agregar monitor"), sub);
            final JSONObject nx = next;
            if (nx != null) add.setOnClickListener(v -> openMacWindow(nx.optInt("id"), nx.optString("name"), "", nx.optInt("w"), nx.optInt("h")));
            else if (!virtOpen) add.setOnClickListener(v -> openMacWindow(WindowUi.MONITOR_ID, I18n.t("Monitor virtual"), "", 1600, 1000));
            else add.setAlpha(0.5f);
            content.addView(add);
            // Los que ya están abiertos: tocarlos los trae de nuevo
            for (int i = 0; i < n; i++) {
                JSONObject d = ds.optJSONObject(i);
                final int id = d.optInt("id"), w = d.optInt("w"), h = d.optInt("h");
                if (!host.isWindowOpen(id)) continue;
                final String name = d.optString("name");
                LinearLayout row = Ui.row(ctx, "display", name, I18n.t("Agregado · monitor de la Mac · ") + w + "×" + h);
                row.setOnClickListener(v -> openMacWindow(id, name, "", w, h));
                content.addView(row);
            }
            if (virtOpen) {
                LinearLayout row = Ui.row(ctx, "display", I18n.t("Monitor virtual"), I18n.t("Agregado · solo existe en el Quest"));
                row.setOnClickListener(v -> openMacWindow(WindowUi.MONITOR_ID, I18n.t("Monitor virtual"), "", 1600, 1000));
                content.addView(row);
            }
        } else {
            JSONArray ws = st.optJSONArray("windows");
            int n = ws == null ? 0 : ws.length();
            if (n == 0) content.addView(Ui.label(ctx, I18n.t("No hay ventanas abiertas en la Mac (o falta el permiso de grabación de pantalla)."), 15, Ui.TEXT2, false));
            for (int i = 0; i < n; i++) {
                JSONObject w = ws.optJSONObject(i);
                final int id = w.optInt("id"), ww = w.optInt("w"), hh = w.optInt("h");
                final String app = w.optString("app"), title = w.optString("title");
                LinearLayout row = Ui.row(ctx, "window", app, title);
                row.setOnClickListener(v -> openMacWindow(id, app, title, ww, hh));
                content.addView(row);
            }
        }

        // 3D, al final (solo en la sala VR: una ventana plana del Quest no puede mostrar profundidad)
        if (host.inVR()) {
            content.addView(Ui.section(ctx, "3D"));
            String ds = st.optString("depthStatus");
            int mode = mode3D();
            option(content, I18n.t("Pantalla principal en 3D"),
                    mode == 1 && !ds.isEmpty() ? ds
                            : mode == 1 ? I18n.t("IA: la Mac calcula la profundidad de cada imagen (también juegos planos)")
                            : mode == 2 ? I18n.t("Para videos 3D lado a lado (SBS): cada ojo ve su mitad")
                            : mode == 3 ? I18n.t("Para videos 3D arriba/abajo: cada ojo ve su mitad")
                            : I18n.t("Cada ventana tiene además su propio botón 3D en su barra"),
                    new String[]{I18n.t("Apagado"), I18n.t("IA\nAutomático"), I18n.t("SBS\nLado a lado"), I18n.t("Arriba/abajo\nVertical")}, mode,
                    i -> { set3DMode(i); drawnKey = ""; render(); });
            if (mode == 1) content.addView(strengthSlider());
        }
    }

    // ------------------------------------------------------------------ 3D: intensidad
    private android.content.SharedPreferences prefs() { return ctx.getSharedPreferences("qvb", Context.MODE_PRIVATE); }

    /// Control deslizante genérico (0–100), guardado en las preferencias
    private View slider(String label, String key, int def, java.util.function.IntConsumer onChange) {
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(ctx, 4), 0, dp(ctx, 4));
        row.addView(Ui.label(ctx, label, 16, Ui.TEXT, true), new LinearLayout.LayoutParams(dp(ctx, 200), ViewGroup.LayoutParams.WRAP_CONTENT));
        android.widget.SeekBar sb = new android.widget.SeekBar(ctx);
        sb.setMax(100);
        int cur = prefs().getInt(key, def);
        sb.setProgress(cur);
        Ui.tint(sb);
        TextView val = Ui.label(ctx, cur + "%", 15, Ui.TEXT, true);
        sb.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar b, int p, boolean user) {
                val.setText(p + "%");
                if (user) prefs().edit().putInt(key, p).apply();   // (antes de avisar: algunos leen el valor guardado)
                onChange.accept(p);
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar b) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar b) { }
        });
        row.addView(sb, new LinearLayout.LayoutParams(0, dp(ctx, 48), 1f));
        row.addView(val, new LinearLayout.LayoutParams(dp(ctx, 56), ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    private View strengthSlider() { return slider(I18n.t("Intensidad del 3D"), "3dStrength", 30, p -> host.set3DStrength(p / 100f)); }

    // ------------------------------------------------------------------ Ajustes
    private String setKey = "";
    private int selEye = -1, selFps = -1, selMbps = -1, selAudio = -1;
    /// Página de Ajustes abierta (null = la lista de categorías) y la opción desplegada
    private String setPage = null, openChoice = null;
    private String advKey = "";   // Avanzado: para qué juego ("" = todos), por su carpeta de Steam

    /// Bloque de vidrio redondeado (para controles deslizantes y textos dentro de una página)
    private LinearLayout block(LinearLayout into) {
        LinearLayout c = new LinearLayout(ctx);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(ctx, 20), dp(ctx, 12), dp(ctx, 20), dp(ctx, 12));
        GradientDrawable cb = new GradientDrawable();
        cb.setColor(Ui.FILL);
        cb.setCornerRadius(Ui.rad(ctx, 22));
        c.setBackground(cb);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 10);
        into.addView(c, lp);
        return c;
    }

    /// Texto chico de ayuda debajo de un grupo
    private void note(LinearLayout into, String text) {
        TextView t = Ui.label(ctx, text, 14, Ui.TEXT2, false);
        t.setPadding(dp(ctx, 8), dp(ctx, 8), dp(ctx, 8), 0);
        into.addView(t);
    }

    /// "1280\nAlta" → "1280 · Alta" (para mostrar el valor elegido en una sola línea)
    private static String oneLine(String label) { return label.replace("\n", " · "); }

    /**
     * Una opción como lista: muestra el nombre y lo elegido; al tocarla se despliega la lista (una opción por fila,
     * con ✓ en la elegida). Con open = true la lista se ve siempre (páginas con una sola opción).
     * Las etiquetas pueden ser «nombre\nexplicación».
     */
    private void option(LinearLayout into, String title, String hint, String[] labels, int selected, java.util.function.IntConsumer pick) {
        choice(into, title, hint, labels, selected, pick, false);
    }

    private void choice(LinearLayout into, String title0, String hint, String[] labels, int selected, java.util.function.IntConsumer pick, boolean alwaysOpen) {
        final String title = title0 == null ? "" : title0;
        boolean open = alwaysOpen || title.equals(openChoice);
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Ui.FILL);
        bg.setCornerRadius(Ui.rad(ctx, 22));
        box.setBackground(bg);
        box.setPadding(dp(ctx, 6), dp(ctx, 6), dp(ctx, 6), dp(ctx, 6));

        // Encabezado: nombre (y explicación) a la izquierda; lo elegido y la flechita a la derecha
        LinearLayout head = new LinearLayout(ctx);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.setPadding(dp(ctx, 14), dp(ctx, 10), dp(ctx, 12), dp(ctx, 10));
        LinearLayout txt = new LinearLayout(ctx);
        txt.setOrientation(LinearLayout.VERTICAL);
        txt.addView(Ui.label(ctx, title, 17, Ui.TEXT, true));
        if (hint != null) txt.addView(Ui.label(ctx, hint, 13, Ui.TEXT2, false));
        head.addView(txt, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (!alwaysOpen) {
            head.setBackground(Ui.hoverable(ctx, 0, 16, false));
            head.setClickable(true);
            TextView val = Ui.label(ctx, selected >= 0 && selected < labels.length ? labels[selected].split("\n", 2)[0] : "", 16, open ? Ui.TEXT3 : Ui.ACCENT_TEXT, true);
            val.setPadding(dp(ctx, 12), 0, dp(ctx, 8), 0);
            head.addView(val);
            ImageView chev = new ImageView(ctx);
            chev.setImageDrawable(Icons.of(ctx, open ? "down" : "chevron", 18, Ui.TEXT2));
            head.addView(chev, new LinearLayout.LayoutParams(dp(ctx, 22), dp(ctx, 22)));
            head.setOnClickListener(v -> { openChoice = open ? null : title; redraw(); });
        }
        if (!title.isEmpty() || !alwaysOpen) box.addView(head);   // (sin título: solo las opciones)

        if (open) {
            for (int i = 0; i < labels.length; i++) {
                final int k = i;
                boolean sel = i == selected;
                String[] parts = labels[i].split("\n", 2);
                LinearLayout row = new LinearLayout(ctx);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(dp(ctx, 14), dp(ctx, 9), dp(ctx, 14), dp(ctx, 9));
                row.setBackground(Ui.hoverable(ctx, sel ? Ui.ACCENT_SEL : 0, 16, false));
                row.setClickable(true);
                LinearLayout col = new LinearLayout(ctx);
                col.setOrientation(LinearLayout.VERTICAL);
                col.addView(Ui.label(ctx, parts[0], 16, sel ? Ui.ACCENT_TEXT : Ui.TEXT, sel));
                if (parts.length > 1) col.addView(Ui.label(ctx, parts[1], 13, Ui.TEXT2, false));
                row.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                ImageView chk = new ImageView(ctx);
                if (sel) chk.setImageDrawable(Icons.of(ctx, "check", 20, Ui.ACCENT_TEXT));
                row.addView(chk, new LinearLayout.LayoutParams(dp(ctx, 24), dp(ctx, 24)));
                row.setOnClickListener(v -> { openChoice = null; pick.accept(k); redraw(); });
                LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                rlp.topMargin = dp(ctx, 2);
                box.addView(row, rlp);
            }
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 10);
        into.addView(box, lp);
    }

    /// Interruptor (encendido/apagado) como los de iPhone, en ámbar
    private View switchView(boolean on) {
        FrameLayout sw = new FrameLayout(ctx);
        GradientDrawable track = new GradientDrawable();
        track.setColor(on ? Ui.ACCENT : Color.argb(70, 255, 255, 255));
        track.setCornerRadius(Ui.rad(ctx, 999));
        sw.setBackground(track);
        View thumb = new View(ctx);
        GradientDrawable th = new GradientDrawable();
        th.setCornerRadius(Ui.dp(ctx, 5));   // (casi cuadrado, como el resto)
        th.setColor(Color.WHITE);
        thumb.setBackground(th);
        thumb.setElevation(dp(ctx, 2));
        FrameLayout.LayoutParams tlp = new FrameLayout.LayoutParams(dp(ctx, 26), dp(ctx, 26), Gravity.CENTER_VERTICAL | (on ? Gravity.END : Gravity.START));
        tlp.setMargins(dp(ctx, 3), 0, dp(ctx, 3), 0);
        sw.addView(thumb, tlp);
        return sw;
    }

    /// Fila con interruptor: nombre (y explicación) e interruptor a la derecha; toda la fila se puede tocar
    private static boolean portalOn = false;
    private View portalHint;   // cartel «cómo dibujar el portal» encima del menú (se va solo cuando quedó listo)
    private static final java.util.List<java.lang.ref.WeakReference<MenuUi>> ALL = new java.util.concurrent.CopyOnWriteArrayList<>();

    /** El portal quedó listo: se van los carteles de todos los menús. */
    static void portalDone() {
        for (java.lang.ref.WeakReference<MenuUi> r : ALL) { MenuUi m = r.get(); if (m != null) m.rootView.post(m::hidePortalHint); }
    }

    private void showPortalHint() {
        hidePortalHint();
        FrameLayout dim = new FrameLayout(ctx);
        dim.setBackgroundColor(Color.argb(150, 0, 0, 0));
        dim.setClickable(true);   // (encima del menú, quieto: no se mueve ni se arrastra)
        LinearLayout card = new LinearLayout(ctx);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(Ui.glass(ctx, 26, 245));
        card.setPadding(dp(ctx, 30), dp(ctx, 26), dp(ctx, 30), dp(ctx, 24));
        TextView t = Ui.label(ctx, I18n.t("Dibujá el portal"), 24, Ui.TEXT, true);
        Ui.leadingIcon(t, "dot", 18, Ui.ACCENT);
        t.setCompoundDrawablePadding(dp(ctx, 8));
        card.addView(t);
        TextView b = Ui.label(ctx, I18n.t("Fuera de las ventanas, pellizcá con la mano del láser (o apretá el gatillo) y, sin soltar, recorré el contorno de lo que querés ver con la punta del índice o del control. Al soltar, el portal queda listo."), 17, Ui.TEXT2, false);
        b.setPadding(0, dp(ctx, 10), 0, dp(ctx, 16));
        card.addView(b);
        TextView cancel = Ui.pill(ctx, I18n.t("Cancelar"));
        cancel.setOnClickListener(v -> { portalOn = false; host.setPortal(false); hidePortalHint(); redraw(); });
        card.addView(cancel, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        FrameLayout.LayoutParams clp = new FrameLayout.LayoutParams(dp(ctx, 620), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        dim.addView(card, clp);
        portalHint = dim;
        rootView.addView(dim, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // encima de todo lo del menú (el panel tiene sombra/elevación propia y si no lo tapaba)
        dim.setElevation(dp(ctx, 200)); dim.setTranslationZ(dp(ctx, 200));
        dim.bringToFront();
        dim.setAlpha(0f);
        dim.animate().alpha(1f).setDuration(180).start();
    }

    private void hidePortalHint() {
        if (portalHint != null) { rootView.removeView(portalHint); portalHint = null; }
    }

    /// El láser toca las pantallas y ventanas (apagado: solo el menú). Antes se guardaba al revés («laserNoTouch»).
    private boolean laserTouch() { return prefs().getBoolean("laserTouch", !prefs().getBoolean("laserNoTouch", false)); }

    private void laserToggle() {
        toggle(content, null, I18n.t("El láser toca las pantallas"),
                I18n.t("Apagado, el láser solo funciona sobre el menú: así usás el mouse y el teclado de la Mac sin clics sin querer (el menú se abre mirando el borde de la mano o manteniendo el botón de menú izquierdo)."),
                laserTouch(), on -> { prefs().edit().putBoolean("laserTouch", on).apply(); host.setLaserNoTouch(!on); });
    }   // el portal no se guarda: se apaga al cerrar la app

    private void toggle(LinearLayout into, String icon, String title, String hint, boolean on, java.util.function.Consumer<Boolean> set) {
        LinearLayout r = icon != null ? Ui.row(ctx, icon, title, hint) : new LinearLayout(ctx);
        if (icon == null) {
            r.setOrientation(LinearLayout.HORIZONTAL);
            r.setGravity(Gravity.CENTER_VERTICAL);
            r.setPadding(dp(ctx, 20), dp(ctx, 14), dp(ctx, 16), dp(ctx, 14));
            r.setBackground(Ui.hoverable(ctx, Ui.FILL, 22, false));
            r.setClickable(true);
            LinearLayout txt = new LinearLayout(ctx);
            txt.setOrientation(LinearLayout.VERTICAL);
            txt.addView(Ui.label(ctx, title, 17, Ui.TEXT, true));
            if (hint != null) txt.addView(Ui.label(ctx, hint, 13, Ui.TEXT2, false));
            r.addView(txt, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(ctx, 10);
            r.setLayoutParams(lp);
        }
        r.addView(switchView(on), new LinearLayout.LayoutParams(dp(ctx, 56), dp(ctx, 32)));
        r.setOnClickListener(v -> { set.accept(!on); redraw(); });
        into.addView(r);
    }

    /// Fila de la lista de Ajustes: ícono, nombre, lo elegido (en gris) y una flechita; abre esa página
    private void navRow(String page, String icon, String title, String value) {
        LinearLayout r = Ui.row(ctx, icon, title, null);
        TextView v = Ui.label(ctx, value, 15, Ui.TEXT2, false);
        v.setSingleLine(true);
        v.setPadding(dp(ctx, 12), 0, dp(ctx, 6), 0);
        r.addView(v);
        ImageView chev = new ImageView(ctx);
        chev.setImageDrawable(Icons.of(ctx, "chevron", 18, Ui.TEXT3));
        r.addView(chev, new LinearLayout.LayoutParams(dp(ctx, 22), dp(ctx, 22)));
        r.setOnClickListener(x -> openPage(page));
        content.addView(r);
    }

    private void openPage(String page) { setPage = page; openChoice = null; redraw(); slideIn(page == null ? -1 : 1); }

    /// Animación al cambiar de página o de sección: el contenido entra deslizándose y aparece
    private void slideIn(int dir) {
        if (content == null) return;
        content.animate().cancel();
        content.setAlpha(0f);
        if (dir != 0) { content.setTranslationX(dir * dp(ctx, 28)); content.setTranslationY(0); }
        else { content.setTranslationX(0); content.setTranslationY(dp(ctx, 14)); }
        content.animate().alpha(1f).translationX(0).translationY(0).setDuration(200)
                .setInterpolator(new android.view.animation.DecelerateInterpolator(1.6f)).start();
    }

    /// Arriba de cada página: «‹ Ajustes» para volver, y el título
    private void pageHeader(String title, String sub) {
        TextView back = Ui.label(ctx, I18n.t("Ajustes"), 16, Ui.ACCENT_TEXT, true);
        Ui.leadingIcon(back, "back", 18, Ui.ACCENT_TEXT);
        back.setCompoundDrawablePadding(dp(ctx, 4));
        back.setPadding(dp(ctx, 6), dp(ctx, 6), dp(ctx, 14), dp(ctx, 6));
        back.setBackground(Ui.hoverable(ctx, 0, 999, false));
        back.setClickable(true);
        back.setOnClickListener(v -> openPage(null));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.bottomMargin = dp(ctx, 8);
        content.addView(back, blp);
        header(title, sub);
    }

    private static int indexOf(int[] a, int v) { int best = 0; for (int i = 0; i < a.length; i++) if (Math.abs(a[i] - v) < Math.abs(a[best] - v)) best = i; return best; }

    private void redraw() {
        setKey = selEye + "," + selFps + "," + selMbps + "," + selAudio + "," + setPage + "," + openChoice + "," + prefs().getAll().hashCode() + "," + I18n.lang();
        drawnKey = "";
        render();
    }

    /// Ajustes del escritorio: se mandan al conectar y cada vez que cambian (se aplican al instante)
    void sendDeskCfg() {
        Link.I.send("{\"type\":\"deskcfg\",\"maxw\":" + prefs().getInt("deskMaxW", 2560) + ",\"fps\":" + prefs().getInt("deskFps", 60)
                + ",\"mbps\":" + prefs().getInt("deskMbps", 20) + ",\"audio\":\"" + prefs().getString("deskAudio", "mac") + "\"}");
    }

    // Opciones (lo que se guarda y cómo se muestra)
    private static final int[] EYES = {1024, 1280, 1536, 1832, 2048, 2304, 2560}, FPSS = {72, 90, 120}, MBPSS = {40, 70, 100, 150, 200};
    private static final String[] EYE_NAMES = {"1024\nNormal", "1280\nAlta", "1536\nMuy alta", "1832\nMáxima · la Mac trabaja mucho más", "2048\nPara Macs Pro o Max · con foveated encoding",
            "2304\nPara Macs Max o Ultra · con HEVC", "2560\nPara Macs Ultra · con HEVC y foveated encoding"};
    private static final String[] MBPS_NAMES = {"40 Mbps\nBaja", "70 Mbps\nMedia", "100 Mbps\nAlta", "150 Mbps\nMuy alta", "200 Mbps\nMáxima · mejor por cable"};
    private static final String[] CODECS = {"hevc", "h264"};
    private static final String[] AUDIOS = {"quest", "mac", "both"};
    private static final int[] D_MAXW = {1280, 1920, 2560}, D_FPS = {30, 60}, D_MBPS = {10, 20, 30, 40};
    private static final String[] HAND_NAMES = {"Control remoto", "Celular", "Banana", "Lápiz"};
    /// Colores del láser (los mismos que dibuja la sala; el primero, ámbar, es el clásico)
    private static final int[] LASER_COLORS = {Color.rgb(230, 168, 74), Color.rgb(240, 240, 242), Color.rgb(89, 191, 255), Color.rgb(89, 242, 140),
            Color.rgb(255, 84, 77), Color.rgb(184, 115, 255), Color.rgb(255, 115, 191), Color.rgb(242, 235, 89)};
    /// Tonos de piel (los mismos que la sala), de más claro a más oscuro
    private static final int[] SKIN_COLORS = {Color.rgb(247, 219, 199), Color.rgb(235, 191, 161), Color.rgb(204, 153, 115),
            Color.rgb(161, 110, 77), Color.rgb(112, 74, 48), Color.rgb(74, 48, 33)};

    /// Color e intensidad del láser (la intensidad va de 0 a 100; 60 = normal)
    private void applyLaser() {
        int k = prefs().getInt("laserK", 60);
        host.setLaser(prefs().getInt("laserCol", 0), Math.max(0.15f, k / 60f));
    }

    /// Elegir un color: cuadraditos (sin nombres), siempre a la vista; el elegido con un borde ámbar y ✓
    private void swatches(LinearLayout into, String title, int[] colors, int selected, java.util.function.IntConsumer pick) {
        LinearLayout b = block(into);
        b.addView(Ui.label(ctx, title, 17, Ui.TEXT, true));
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(ctx, 10), 0, dp(ctx, 4));
        for (int i = 0; i < colors.length; i++) {
            final int k = i;
            FrameLayout sq = new FrameLayout(ctx);
            GradientDrawable g = new GradientDrawable();
            g.setColor(colors[i]);
            g.setCornerRadius(Ui.rad(ctx, 14));
            if (i == selected) g.setStroke(dp(ctx, 3), Ui.ACCENT_TEXT);
            else g.setStroke(dp(ctx, 1), Color.argb(50, 255, 255, 255));
            sq.setBackground(g);
            sq.setForeground(Ui.hoverable(ctx, 0, 14, false));
            sq.setClickable(true);
            if (i == selected) {
                ImageView chk = new ImageView(ctx);
                // ✓ en oscuro o en claro, según lo claro que sea el color
                int lum = (Color.red(colors[i]) * 299 + Color.green(colors[i]) * 587 + Color.blue(colors[i]) * 114) / 1000;
                chk.setImageDrawable(Icons.of(ctx, "check", 22, lum > 150 ? Color.rgb(30, 24, 16) : Color.WHITE));
                sq.addView(chk, new FrameLayout.LayoutParams(dp(ctx, 24), dp(ctx, 24), Gravity.CENTER));
            }
            sq.setOnClickListener(v -> { pick.accept(k); redraw(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(ctx, 52), dp(ctx, 52));
            lp.rightMargin = dp(ctx, 12);
            row.addView(sq, lp);
        }
        b.addView(row);
    }
    // Foveated encoding: lo que se guarda en la Mac es "nivel,centro,cx izq,cx der,cy" (el centro óptico lo mide la sala VR)
    static int fovLevel(JSONObject cfg) { try { return Integer.parseInt(cfg.optString("fov", "0").split(",")[0].trim()); } catch (Exception e) { return 0; } }
    static int fovSize(JSONObject cfg) { try { String[] p = cfg.optString("fov", "0").split(","); return p.length > 1 ? Integer.parseInt(p[1].trim()) : 1; } catch (Exception e) { return 1; } }
    static String fovString(Context c, int level, int size) {
        return level <= 0 ? "0" : level + "," + size + "," + c.getSharedPreferences("qvb", Context.MODE_PRIVATE).getString("fovCenters", "0.5,0.5,0.5");
    }
    static final String[] FOV_NAMES = {"Apagado\nLa imagen completa en todos lados", "Suave\nCasi no se nota", "Medio\nRecomendado", "Fuerte\nLo más rápido; el borde se ve más borroso"};
    static final String[] FOVC_NAMES = {"Chico", "Medio", "Grande"};
    private int selFov = -1, selFovC = -1, selCodec = -1;
    private static final String[] MODE_NAMES = {"Apagado", "IA", "Lado a lado (SBS)", "Arriba/abajo"};
    /// Perfiles rápidos de los juegos: posiciones en EYES, FPSS y MBPSS
    private static final int[][] PROFILES = {{0, 0, 0}, {0, 1, 1}, {1, 1, 2}};

    private static final float[] SEATED_HEIGHTS = {1.60f, 1.70f, 1.75f, 1.80f, 1.90f};
    /// Altura que se aplica: sentado = la elegida; de pie = la real (0) o la elegida
    float effectiveHeight() { return effectiveHeight(prefs()); }
    static float effectiveHeight(android.content.SharedPreferences pr) {
        float h = pr.getFloat("heightM", 1.75f);
        if (h <= 0.5f) h = 1.75f;
        boolean standing = pr.getInt("posture", pr.getFloat("heightM", 1.75f) <= 0.5f ? 1 : 0) == 1;
        if (standing) return pr.getBoolean("standReal", true) ? 0f : h;
        return h;
    }
    /// Altura de la cabeza en el Modo Centinela (la elegida; 1,75 m si no se eligió ninguna)
    static float vigiaHeight(android.content.SharedPreferences pr) {
        float h = pr.getFloat("heightM", 1.75f);
        return h <= 0.5f ? 1.75f : h;
    }
    static final float[] HEIGHTS = {1.60f, 1.70f, 1.75f, 1.80f, 1.90f};
    private static final int[] SNAPS = {15, 30, 45, 90}, TURN_SPEEDS = {60, 120, 200};
    private static final float[] MOVE_SPEEDS = {1.2f, 2.0f, 3.2f};
    private String moveSummary() {
        android.content.SharedPreferences pr = prefs();
        String t = pr.getInt("turnMode", 0) == 0 ? I18n.t("Giro por saltos de ") + pr.getInt("snapDeg", 30) + "°" : I18n.t("Giro suave");
        int m = pr.getInt("moveMode", 0);
        return t + " · " + I18n.t(m == 0 ? "Teletransporte" : m == 1 ? "Caminar" : "Teletransporte y caminar");
    }
    private String heightName(float m) { return m <= 0.5f ? I18n.t("Real") : String.format(java.util.Locale.US, "%.2f m", m); }

    private String audioName(int i) { return I18n.t(i == 0 ? "Solo visor" : i == 1 ? "Solo Mac" : "En los dos"); }

    private void renderSettings(JSONObject st) {
        final boolean online = st != null;   // sin conexión: los ajustes del Quest igual se pueden cambiar
        JSONObject cfg = online ? st.optJSONObject("cfg") : null;
        if (cfg == null) cfg = new JSONObject();
        if (online) {   // (los de los juegos vienen de la Mac: sin conexión no se tocan)
            if (selEye < 0) selEye = indexOf(EYES, cfg.optInt("eye", 1024));
            if (selFps < 0) selFps = indexOf(FPSS, cfg.optInt("fps", 90));
            if (selMbps < 0) selMbps = indexOf(MBPSS, cfg.optInt("mbps", 70));
            if (selAudio < 0) { String a = cfg.optString("audio", "quest"); selAudio = a.equals("mac") ? 1 : a.equals("both") ? 2 : 0; }
            if (selFov < 0) { selFov = fovLevel(cfg); selFovC = fovSize(cfg); }
        }
        if (setPage != null && !host.inVR() && (setPage.equals("image") || setPage.equals("hands") || setPage.equals("3d") || setPage.equals("height") || setPage.equals("move"))) setPage = null;

        if (setPage == null) {
            header(I18n.t("Ajustes"), I18n.t("Todo se guarda solo."));
            if (host.inVR()) quickGrid(content);
            content.addView(Ui.section(ctx, I18n.t("General")));
            navRow("lang", "globe", I18n.english() ? "Language" : "Idioma", I18n.english() ? "English" : "Español");
            if (host.inVR()) {
                boolean stand = prefs().getInt("posture", prefs().getFloat("heightM", 1.75f) <= 0.5f ? 1 : 0) == 1;
                navRow("height", "visor", I18n.t("Tu altura"), (stand ? I18n.t("De pie · ") : I18n.t("Sentado · ")) + heightName(effectiveHeight()));
            }
            content.addView(Ui.section(ctx, I18n.t("Calidad")));
            navRow("games", "gamepad", I18n.t("Juegos VR"), online ? I18n.t("Calidad y rendimiento, para todos o para cada juego") : I18n.t("Con la Mac conectada"));
            navRow("desk", "display", I18n.t("Escritorio y ventanas"), prefs().getInt("deskMaxW", 2560) + " px · " + prefs().getInt("deskFps", 60) + " fps");
            if (host.inVR()) {
                content.addView(Ui.section(ctx, I18n.t("Movimiento")));
                navRow("move", "room", I18n.t("Moverte por la sala"), moveSummary());
                content.addView(Ui.section(ctx, I18n.t("Teclado")));
                toggle(content, null, I18n.t("Abrir el teclado solo"), I18n.t("Cuando tocás un lugar para escribir en la Mac. También está el botón del teclado en la barra de cada pantalla."),
                        prefs().getBoolean("kbAuto", true), on -> prefs().edit().putBoolean("kbAuto", on).apply());
                content.addView(Ui.section(ctx, I18n.t("Conexión")));
                toggle(content, null, I18n.t("Solo por cable USB-C"), I18n.t("Los juegos y las pantallas van siempre por el cable (sin Wi-Fi). Sin cable no se conecta."),
                        prefs().getBoolean("cableOnly", false), on -> { prefs().edit().putBoolean("cableOnly", on).apply(); host.setCableOnly(on); });
                content.addView(Ui.section(ctx, I18n.t("Studio")));
                navRow("image", "filter", I18n.t("Imagen"), I18n.t(IMG_NAMES[Math.min(IMG_NAMES.length - 1, prefs().getInt("imgPreset", 0))]));
                navRow("hands", "hand", I18n.t("Manos y controles"), I18n.t(prefs().getBoolean("realHands", true) ? "Manos reales" : "Solo el láser"));
                navRow("gestures", "hand", I18n.t("Gestos de las manos"), I18n.t("Joystick virtual y botones"));
                toggle(content, "stats", I18n.t("Indicador de rendimiento"), I18n.t("Cuadros por segundo, Mbps y demora"), prefs().getBoolean("hud", false),
                        on -> { prefs().edit().putBoolean("hud", on).apply(); host.setHud(on); });
            }
            TextView about = Ui.label(ctx, I18n.t("VRmin ") + version() + I18n.t(" · en la Mac: VRmin Host"), 13, Ui.TEXT3, false);
            about.setPadding(dp(ctx, 6), dp(ctx, 26), 0, 0);
            content.addView(about);
            return;
        }

        switch (setPage) {
            case "quick": {
                pageHeader(I18n.t("Ajustes rápidos"), I18n.t("Los íconos de arriba de todo en Ajustes."));
                java.util.Set<String> items = quickItems();
                for (int i = 0; i < QUICK_IDS.length; i++) {
                    final String id = QUICK_IDS[i];
                    toggle(content, null, I18n.t(QUICK_NAMES[i]), I18n.t(QUICK_HINTS[i]), items.contains(id), on -> {
                        java.util.Set<String> it = quickItems();
                        if (on) it.add(id); else it.remove(id);
                        StringBuilder sb = new StringBuilder();
                        for (String q : QUICK_IDS) if (it.contains(q)) { if (sb.length() > 0) sb.append(','); sb.append(q); }   // (siempre en el mismo orden)
                        prefs().edit().putString("quickItems3", sb.toString()).apply();
                    });
                }
                break;
            }
            case "gestures": {
                pageHeader(I18n.t("Gestos de las manos"), I18n.t("Sin controles: qué hace cada gesto, en Studio y en los juegos."));
                GestureUi gu = new GestureUi(ctx, prefs(), host::applyGestures, this::redraw);
                gu.profilePicker(content, gestureProfile, i -> { gestureProfile = i; redraw(); });
                GestureUi.note(ctx, content);
                gu.actions(content, gestureProfile, 0);
                gu.actions(content, gestureProfile, 1);
                gu.macrosSection(content, gestureProfile);
                break;
            }
            case "lang":
                pageHeader(I18n.english() ? "Language" : "Idioma", I18n.t("El idioma de VRmin. Algunas ventanas cambian al volver a abrirlas."));
                choice(content, null, null, new String[]{"English", "Español"}, I18n.english() ? 0 : 1, i -> {
                    I18n.setLang(i == 0 ? "en" : "es");
                    vrButton.setText(I18n.t(host.inVR() ? "Salir de Studio" : "Entrar en Studio"));
                    setTab(tab);
                    setPage = "lang";
                }, true);
                break;

            case "move": {
                pageHeader(I18n.t("Moverte por la sala"), I18n.t("Cómo girar y desplazarte con los joysticks (o con las manos)."));
                android.content.SharedPreferences pr = prefs();
                java.util.function.Consumer<Runnable> save = r -> { r.run(); host.setLocomotion(); };
                content.addView(Ui.section(ctx, I18n.t("Girar")));
                option(content, I18n.t("Forma de girar"), I18n.t("Joystick derecho a los costados"),
                        new String[]{I18n.t("Por saltos\nMarea menos"), I18n.t("Suave\nComo caminar")}, pr.getInt("turnMode", 0),
                        i -> save.accept(() -> pr.edit().putInt("turnMode", i).apply()));
                if (pr.getInt("turnMode", 0) == 0) {
                    option(content, I18n.t("Tamaño del salto"), null, new String[]{"15°", "30°", "45°", "90°"},
                            indexOf(SNAPS, pr.getInt("snapDeg", 30)), i -> save.accept(() -> pr.edit().putInt("snapDeg", SNAPS[i]).apply()));
                } else {
                    option(content, I18n.t("Velocidad del giro"), null, new String[]{I18n.t("Lenta"), I18n.t("Normal"), I18n.t("Rápida")},
                            indexOf(TURN_SPEEDS, pr.getInt("turnSpeed", 120)), i -> save.accept(() -> pr.edit().putInt("turnSpeed", TURN_SPEEDS[i]).apply()));
                }
                content.addView(Ui.section(ctx, I18n.t("Desplazarte")));
                option(content, I18n.t("Forma de moverte"), I18n.t("Teletransporte: joystick derecho hacia atrás, apuntá y soltá. Caminar: joystick izquierdo."),
                        new String[]{I18n.t("Teletransporte"), I18n.t("Caminar"), I18n.t("Los dos")}, pr.getInt("moveMode", 0),
                        i -> save.accept(() -> pr.edit().putInt("moveMode", i).apply()));
                if (pr.getInt("moveMode", 0) != 0) {
                    int ms = 0; float cur = pr.getFloat("moveSpeed", 2.0f);
                    for (int i = 0; i < MOVE_SPEEDS.length; i++) if (Math.abs(MOVE_SPEEDS[i] - cur) < 0.01f) ms = i;
                    option(content, I18n.t("Velocidad al caminar"), null, new String[]{I18n.t("Lenta"), I18n.t("Normal"), I18n.t("Rápida")}, ms,
                            i -> save.accept(() -> pr.edit().putFloat("moveSpeed", MOVE_SPEEDS[i]).apply()));
                }
                content.addView(Ui.section(ctx, I18n.t("Comodidad")));
                toggle(content, null, I18n.t("Oscurecer los bordes al moverte"), I18n.t("Mientras girás o caminás de forma continua (ayuda a no marearte)"),
                        pr.getBoolean("vignette", true), on -> save.accept(() -> pr.edit().putBoolean("vignette", on).apply()));
                break;
            }

            case "height": {
                pageHeader(I18n.t("Tu altura"), I18n.t("Para la sala y los juegos."));
                android.content.SharedPreferences pr = prefs();
                boolean standing = pr.getInt("posture", pr.getFloat("heightM", 1.75f) <= 0.5f ? 1 : 0) == 1;
                choice(content, I18n.t("Cómo jugás"), null, new String[]{I18n.t("Sentado\nVRmin te pone a la altura de abajo, como si estuvieras de pie"),
                                I18n.t("De pie\nTu altura real (la que mide el visor)")}, standing ? 1 : 0,
                        i -> { pr.edit().putInt("posture", i).apply(); host.setHeight(effectiveHeight()); }, true);
                {
                    float cur = pr.getFloat("heightM", 1.75f);
                    if (cur <= 0.5f) cur = 1.75f;
                    boolean real = standing && pr.getBoolean("standReal", true);
                    int off = standing ? 1 : 0;   // de pie: la primera opción es «Real»
                    int sel = real ? 0 : off;
                    for (int i = 0; i < SEATED_HEIGHTS.length; i++) if (!real && Math.abs(SEATED_HEIGHTS[i] - cur) < 0.01f) sel = i + off;
                    String[] names = new String[SEATED_HEIGHTS.length + off];
                    if (standing) names[0] = I18n.t("Real\nLa que mide el visor");
                    for (int i = 0; i < SEATED_HEIGHTS.length; i++) names[i + off] = heightName(SEATED_HEIGHTS[i]);
                    choice(content, I18n.t("Altura"), null, names, sel, i -> {
                        if (i < off) pr.edit().putBoolean("standReal", true).apply();
                        else pr.edit().putFloat("heightM", SEATED_HEIGHTS[i - off]).putBoolean("standReal", false).apply();
                        host.setHeight(effectiveHeight());
                    }, true);
                    note(content, standing ? I18n.t("Con una altura elegida, el juego te pone a esa altura aunque midas distinto. Se mide en la posición en que estás al elegirla; si cambiás, mantené apretado el botón de Meta (recentrar).")
                                           : I18n.t("Se mide sentado, en la posición en que estás al elegirla. Si te acomodás distinto, mantené apretado el botón de Meta (recentrar) y se vuelve a medir."));
                }
                break;
            }

            case "games": {
                pageHeader(I18n.t("Juegos VR"), I18n.t("Primero elegí si es para todos los juegos o para uno. Un juego con ajustes propios siempre usa los suyos."));
                if (!online) {   // estos ajustes los guarda la Mac
                    note(content, I18n.t("Estos ajustes se guardan en la Mac: conectate a tu Mac en «Computadoras» para cambiarlos."));
                    TextView go = Ui.pill(ctx, I18n.t("Ir a Computadoras"));
                    go.setOnClickListener(v -> setTab(TAB_MACS));
                    content.addView(go, wrap());
                    break;
                }
                gameAdvBlock(st, true);
                break;
            }

            case "desk": {
                pageHeader(I18n.t("Escritorio y ventanas"), online ? I18n.t("Se aplica al instante.") : I18n.t("Se guarda ahora y se aplica al conectarte a la Mac."));
                option(content, I18n.t("Nitidez"), I18n.t("Ancho máximo de la imagen"), new String[]{I18n.t("1280 px\nNormal"), I18n.t("1920 px\nAlta"), I18n.t("2560 px\nMáxima")}, indexOf(D_MAXW, prefs().getInt("deskMaxW", 2560)),
                        i -> { prefs().edit().putInt("deskMaxW", D_MAXW[i]).apply(); sendDeskCfg(); });
                option(content, I18n.t("Fluidez"), I18n.t("Cuadros por segundo"), new String[]{I18n.t("30 fps\nAhorra"), I18n.t("60 fps\nSuave")}, indexOf(D_FPS, prefs().getInt("deskFps", 60)),
                        i -> { prefs().edit().putInt("deskFps", D_FPS[i]).apply(); sendDeskCfg(); });
                option(content, I18n.t("Calidad de imagen"), null, new String[]{I18n.t("10 Mbps\nBaja"), I18n.t("20 Mbps\nMedia"), I18n.t("30 Mbps\nAlta"), I18n.t("40 Mbps\nMuy alta")}, indexOf(D_MBPS, prefs().getInt("deskMbps", 20)),
                        i -> { prefs().edit().putInt("deskMbps", D_MBPS[i]).apply(); sendDeskCfg(); });
                String curA = prefs().getString("deskAudio", "mac");
                option(content, I18n.t("Sonido de la Mac"), I18n.t("Fuera de los juegos (videos, música…)"), new String[]{audioName(0), audioName(1), audioName(2)},
                        curA.equals("quest") ? 0 : curA.equals("both") ? 2 : 1,
                        i -> { prefs().edit().putString("deskAudio", AUDIOS[i]).apply(); sendDeskCfg(); });
                break;
            }

            case "image": {
                pageHeader(I18n.t("Imagen"), I18n.t("Para el escritorio, las ventanas y los juegos (los menús no cambian)."));
                int k = prefs().getInt("imgPreset", 0);
                option(content, I18n.t("Filtro"), null, new String[]{I18n.t("Normal"), I18n.t("Vívido\nColores fuertes"), I18n.t("Cine\nCálido"), I18n.t("Nítido\nMás detalle"), I18n.t("Noche\nCansa menos la vista"), I18n.t("Personal\nA tu gusto")}, k,
                        i -> { prefs().edit().putInt("imgPreset", i).apply(); applyImage(); });
                if (k == IMG_PRESETS.length) {
                    content.addView(Ui.section(ctx, I18n.t("Personal")));
                    LinearLayout b = block(content);
                    java.util.function.IntConsumer ap = p -> applyImage();
                    b.addView(slider(I18n.t("Brillo"), "imgB", 50, ap));
                    b.addView(slider(I18n.t("Contraste"), "imgC", 50, ap));
                    b.addView(slider(I18n.t("Saturación"), "imgS", 50, ap));
                    b.addView(slider(I18n.t("Nitidez"), "imgSh", 0, ap));
                    b.addView(slider(I18n.t("Calidez"), "imgW", 50, ap));
                }
                note(content, I18n.t("También podés cambiar de filtro con el botón de la barra de cada pantalla."));
                break;
            }

            case "hands":
                pageHeader(I18n.t("Manos y controles"), I18n.t("Cómo se ven tus manos y el láser en la sala."));
                toggle(content, null, I18n.t("Seguimiento de manos"), I18n.t("Apagado, VRmin usa solo los controles: no sigue tus manos, ni sus gestos, ni el menú en la mano."),
                        prefs().getBoolean("handTracking", true), on -> { prefs().edit().putBoolean("handTracking", on).apply(); host.setHandTracking(on); });
                toggle(content, null, I18n.t("Ver tus manos reales"), I18n.t("Con los controles se ven los controles; sin controles, tus manos (nunca las dos cosas a la vez)"),
                        prefs().getBoolean("realHands", true), on -> { prefs().edit().putBoolean("realHands", on).apply(); host.setRealHands(on); });
                note(content, I18n.t("Ver tus manos reales puede bajar un poco el rendimiento. Si notás tirones, apagalo."));
                block(content).addView(slider(I18n.t("Color de las manos"), "handSat", 50, p -> host.setHandColor(p / 50f * 0.8f)));
                block(content).addView(slider(I18n.t("Suavidad del borde"), "edgeSoft", 60, p -> host.setEdgeSoft(p / 100f)));
                content.addView(Ui.section(ctx, I18n.t("Portal")));
                toggle(content, null, I18n.t("Portal al cuarto real"), I18n.t("Dibujá un círculo alrededor de algo (por ejemplo, tu teclado) y esa zona se ve hasta que lo apagues"),
                        portalOn, on -> { portalOn = on; host.setPortal(on); if (on) showPortalHint(); else hidePortalHint(); });
                note(content, I18n.t("Para dibujarlo: fuera de las ventanas, pellizcá con la mano del láser (o apretá el gatillo) y recorré el contorno con la punta del índice o del control. Al soltar, el portal queda listo. Para hacer otro, apagalo y volvé a encenderlo."));
                note(content, I18n.t("Para que el borde de tus manos se vea suave, VRmin usa las cámaras del visor (te pide permiso una vez). Sin ese permiso se ven con el borde marcado."));
                toggle(content, null, I18n.t("Abrir el menú mirando la mano"), I18n.t("Mirá el borde de la mano, del lado del meñique, durante un momento"),
                        prefs().getBoolean("palmMenu", true), on -> { prefs().edit().putBoolean("palmMenu", on).apply(); host.setWatch(on); });
                note(content, I18n.t("Sin controles, apuntá con la mano y juntá el índice con el pulgar para hacer clic."));
                note(content, I18n.t("El láser sale de una sola mano. Para pasarlo a la otra, apretá su gatillo (o pellizcá con esa mano)."));
                content.addView(Ui.section(ctx, I18n.t("Moverte")));
                toggle(content, null, I18n.t("Moverte con las manos"), I18n.t("Sin controles, con la mano derecha en forma de pistola (índice estirado, los otros tres dedos cerrados): apuntá al piso adelante y bajá el pulgar para ir ahí; apuntá a un costado y bajá el pulgar para girar."),
                        prefs().getBoolean("handMove", true), on -> { prefs().edit().putBoolean("handMove", on).apply(); host.setHandMove(on); });
                content.addView(Ui.section(ctx, I18n.t("Juegos")));
                toggle(content, null, I18n.t("Jugar con las manos (experimental)"), I18n.t("No se recomienda en juegos rápidos.") + " " + I18n.t("Sin controles, tus manos hacen de control: pellizco con el índice = gatillo · cerrar mayor, anular y meñique = agarre · pellizco con el mayor = B/Y · pellizco con el anular = A/X · pellizco con el meñique izquierdo = menú (mantenelo para este menú) · pulgar sobre la palma = apretar el joystick · puño con el pulgar encima y mover la mano = joystick"),
                        prefs().getBoolean("handGestures", true), on -> { prefs().edit().putBoolean("handGestures", on).apply(); host.setHandGestures(on); });
                content.addView(Ui.section(ctx, I18n.t("Láser")));
                note(content, I18n.t("Para que el láser no haga clic en una pantalla, usá el botón del láser en la barra de esa pantalla."));
                note(content, I18n.t("Para poner en pausa los controles y las manos (por ejemplo, para comer mirando una película): mantené A y B (o X e Y) un segundo, juntá el pulgar con el meñique derecho o tocá el botón de pausa en la barra de una pantalla. Para volver, el mismo atajo o el mismo gesto."));
                swatches(content, I18n.t("Color"), LASER_COLORS, Math.min(LASER_COLORS.length - 1, prefs().getInt("laserCol", 0)),
                        i -> { prefs().edit().putInt("laserCol", i).apply(); applyLaser(); });
                block(content).addView(slider(I18n.t("Intensidad"), "laserK", 60, p -> host.setLaser(prefs().getInt("laserCol", 0), Math.max(0.15f, p / 60f))));
                break;

            case "3d":
                pageHeader("3D", I18n.t("Profundidad para la pantalla principal de la Mac."));
                choice(content, I18n.t("Pantalla principal"), null, new String[]{I18n.t("Apagado"), I18n.t("IA\nLa Mac calcula la profundidad (también juegos planos)"),
                        I18n.t("Lado a lado (SBS)\nPara videos 3D lado a lado"), I18n.t("Arriba/abajo\nPara videos 3D arriba y abajo")}, mode3D(), this::set3DMode, true);
                if (mode3D() == 1) block(content).addView(strengthSlider());   // en SBS y arriba/abajo no hay intensidad
                note(content, I18n.t("Cada ventana tiene su propio botón 3D en su barra (tocalo para elegir el modo; mantenelo apretado para la intensidad)."));
                break;

            default:
                setPage = null;
                redraw();
        }
    }

    // Ajustes avanzados de los juegos (los guarda la Mac): escala de render, automática, campo de visión, controles de Index
    static final int[] ADV_SCALES = {50, 60, 70, 80, 90, 100};
    static final int[] ADV_FOVS = {80, 85, 90, 95, 100};
    static void sendAdv(String scope, String field, String value) {
        Link.I.send("{\"type\":\"gamecfg\",\"scope\":\"" + scope + "\",\"" + field + "\":" + value + "}");
    }
    private void advanced(LinearLayout into, JSONObject v, String scope) {
        int sc = (int) Math.round((v == null ? 1 : v.optDouble("scale", 1)) * 100), fk = (int) Math.round((v == null ? 1 : v.optDouble("fovk", 1)) * 100);
        into.addView(Ui.section(ctx, I18n.t("Rendimiento")));
        String[] sn = new String[ADV_SCALES.length];
        for (int i = 0; i < sn.length; i++) sn[i] = ADV_SCALES[i] + " %" + (ADV_SCALES[i] == 100 ? I18n.t("\nSin escalar") : ADV_SCALES[i] == 70 ? I18n.t("\nBuen equilibrio") : "");
        final String again = "\n" + I18n.t("Hay que volver a abrir el juego para que se aplique.");
        option(into, I18n.t("Escala de render"), I18n.t("El juego dibuja menos píxeles y la Mac los agranda con MetalFX: más cuadros por segundo") + again,
                sn, indexOf(ADV_SCALES, sc), i -> sendAdv(scope, "scale", String.valueOf(ADV_SCALES[i] / 100.0)));
        toggle(into, null, I18n.t("Escala automática"), I18n.t("Si el juego no llega a los cuadros por segundo, baja la escala; si sobra, la sube. Aprende de cada partida.") + again,
                v != null && v.optBoolean("auto"), on -> sendAdv(scope, "auto", String.valueOf(on)));
        String[] fn = new String[ADV_FOVS.length];
        for (int i = 0; i < fn.length; i++) fn[i] = ADV_FOVS[i] + " %" + (ADV_FOVS[i] == 100 ? I18n.t("\nCompleto") : ADV_FOVS[i] == 90 ? I18n.t("\nCasi no se nota") : "");
        option(into, I18n.t("Campo de visión"), I18n.t("Las esquinas casi no se ven por las lentes: si el juego no las dibuja, rinde más") + again,
                fn, indexOf(ADV_FOVS, fk), i -> sendAdv(scope, "fovk", String.valueOf(ADV_FOVS[i] / 100.0)));
        into.addView(Ui.section(ctx, I18n.t("Controles")));
        toggle(into, null, I18n.t("Emular controles de Valve Index"), I18n.t("Para juegos que funcionan mejor con Index: los dedos se mueven según cómo tocás los botones") + again,
                v != null && v.optBoolean("index"), on -> sendAdv(scope, "index", String.valueOf(on)));
        toggle(into, null, I18n.t("Emular controles de HTC Vive"), I18n.t("Para juegos hechos para los controles de Vive: el stick hace de trackpad (moverlo es deslizar el dedo, hundirlo es hacer clic)") + again,
                v != null && v.optBoolean("vive"), on -> sendAdv(scope, "vive", String.valueOf(on)));
    }

    /// El próximo monitor físico de la Mac que todavía no está abierto (null si ya están todos)
    private JSONObject nextMonitor(JSONObject st) {
        JSONArray ds = st.optJSONArray("displays");
        for (int i = 0; ds != null && i < ds.length(); i++) if (!host.isWindowOpen(ds.optJSONObject(i).optInt("id"))) return ds.optJSONObject(i);
        return null;
    }

    /** Botón de la barra: agrega el próximo monitor (primero los físicos, después el virtual). Devuelve qué hizo. */
    String addMonitorFromBar() {
        JSONObject st = Link.I.state();
        if (!Link.I.isConnected() || st == null) return I18n.t("Sin conexión con la Mac");
        JSONObject nx = nextMonitor(st);
        if (nx != null) {
            openMacWindow(nx.optInt("id"), nx.optString("name"), "", nx.optInt("w"), nx.optInt("h"));
            return I18n.t("Monitor agregado: ") + nx.optString("name");
        }
        if (!host.isWindowOpen(WindowUi.MONITOR_ID)) {
            openMacWindow(WindowUi.MONITOR_ID, I18n.t("Monitor virtual"), "", 1600, 1000);
            return I18n.t("Monitor virtual agregado");
        }
        return I18n.t("Ya agregaste todos los monitores");
    }

    /** Botón de la barra: pasa al filtro de imagen siguiente. Devuelve su nombre. */
    String nextImageFromBar() {
        int k = (prefs().getInt("imgPreset", 0) + 1) % IMG_NAMES.length;
        prefs().edit().putInt("imgPreset", k).apply();
        applyImage();
        redraw();
        return I18n.t("Filtro: ") + I18n.t(IMG_NAMES[k]);
    }

    /// Versión de la app (para «Acerca de»)
    private String version() {
        try { return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName; }
        catch (Exception e) { return "?"; }
    }

    /** Desde el botón de la barra de la sala VR. */
    void setHudFromBar(boolean on) { prefs().edit().putBoolean("hud", on).apply(); host.setHud(on); drawnKey = ""; render(); }

    /// Salas: el lugar donde estás (su propia sección del menú, separada de los ajustes)
    private void renderRoom() {
        header(I18n.t("Salas"), I18n.t("El lugar donde estás en el Modo Studio."));
        choice(content, I18n.t("Lugar"), null, new String[]{I18n.t("Living\nCon TV y luces"), I18n.t("Espacio\nSin TV fija: todo flota"), I18n.t("Cine\nPantalla gigante"), I18n.t("Passthrough\nTu cuarto real: todo flota, sin moverte"), I18n.t("Carga\nEl vacío de la pantalla de carga")},
                prefs().getInt("env", 0), i -> { prefs().edit().putInt("env", i).apply(); host.setEnvironment(i); }, true);
        content.addView(Ui.section(ctx, I18n.t("Luces")));
        toggle(content, null, I18n.t("Luces detrás de la TV"), I18n.t("Siguen los colores de la pantalla (solo en el living)"), prefs().getBoolean("leds", true),
                on -> { prefs().edit().putBoolean("leds", on).apply(); host.setLeds(on); });
        block(content).addView(slider(I18n.t("Brillo"), "light", 50, p -> host.setLight(p / 50f)));
    }

    /// Calidad de los juegos (para todos): perfil, nitidez, fluidez, Mbps, sonido y foveated encoding, con «Aplicar»
    private void qualityBlock(JSONObject cfg) {
        int prof = PROFILES.length;   // «A tu gusto» si no coincide con ninguno
        for (int i = 0; i < PROFILES.length; i++)
            if (PROFILES[i][0] == selEye && PROFILES[i][1] == selFps && PROFILES[i][2] == selMbps) prof = i;
        option(content, I18n.t("Perfil"), I18n.t("Lo más fácil: elegí uno y listo"),
                new String[]{I18n.t("Más fluido\nPara Wi-Fi lento o si se entrecorta"), I18n.t("Equilibrado\nRecomendado"),
                        I18n.t("Más nítido\nPide más a la Mac y al Wi-Fi"), I18n.t("A tu gusto\nLo que elijas abajo")}, prof,
                i -> { if (i < PROFILES.length) { selEye = PROFILES[i][0]; selFps = PROFILES[i][1]; selMbps = PROFILES[i][2]; } });
        qualityDetail(selEye, selFps, selMbps, selFov, selFovC, i -> selEye = i, i -> selFps = i, i -> selMbps = i, i -> selFov = i, i -> selFovC = i);
        content.addView(Ui.section(ctx, I18n.t("Sonido y video")));
        option(content, I18n.t("Sonido"), null, new String[]{audioName(0), audioName(1), audioName(2)}, selAudio, i -> selAudio = i);
        option(content, I18n.t("Códec de video"), I18n.t("HEVC da mejor imagen con los mismos Mbps. H.264 queda por ahora, por si HEVC anda mal."),
                new String[]{I18n.t("HEVC\nRecomendado"), I18n.t("H.264\nEl de antes")}, selCodec, i -> selCodec = i);
        boolean changed = EYES[selEye] != cfg.optInt("eye") || FPSS[selFps] != cfg.optInt("fps") || MBPSS[selMbps] != cfg.optInt("mbps")
                || !AUDIOS[selAudio].equals(cfg.optString("audio", "quest")) || !CODECS[selCodec].equals(cfg.optString("codec", "hevc"))
                || selFov != fovLevel(cfg) || (selFov > 0 && selFovC != fovSize(cfg));
        TextView apply = changed ? Ui.primary(ctx, I18n.t("Aplicar cambios")) : Ui.pill(ctx, I18n.t("Aplicado"));
        apply.setEnabled(changed);
        apply.setAlpha(changed ? 1f : 0.6f);
        apply.setOnClickListener(v -> Link.I.send("{\"type\":\"settings\",\"eye\":" + EYES[selEye] + ",\"fps\":" + FPSS[selFps]
                + ",\"mbps\":" + MBPSS[selMbps] + ",\"audio\":\"" + AUDIOS[selAudio] + "\",\"codec\":\"" + CODECS[selCodec]
                + "\",\"fov\":\"" + fovString(ctx, selFov, selFovC) + "\"}"));
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        alp.topMargin = dp(ctx, 18);
        content.addView(apply, alp);
        if (changed) note(content, I18n.t("Al aplicar, la conexión se corta unos segundos."));
    }

    /// Nitidez, fluidez, Mbps y foveated encoding (para todos o para un juego)
    private void qualityDetail(int eye, int fps, int mbps, int fov, int fovC, java.util.function.IntConsumer pEye, java.util.function.IntConsumer pFps,
                               java.util.function.IntConsumer pMbps, java.util.function.IntConsumer pFov, java.util.function.IntConsumer pFovC) {
        content.addView(Ui.section(ctx, I18n.t("Calidad")));
        String[] en = new String[EYE_NAMES.length];
        for (int i = 0; i < en.length; i++) en[i] = I18n.t(EYE_NAMES[i]);
        option(content, I18n.t("Nitidez"), I18n.t("Píxeles por ojo") + "\n" + I18n.t("Si hay un juego abierto, hay que volver a abrirlo para que se aplique."), en, eye, pEye);
        option(content, I18n.t("Fluidez"), I18n.t("Cuadros por segundo"), new String[]{I18n.t("72 Hz\nAhorra"), I18n.t("90 Hz\nRecomendado"), I18n.t("120 Hz\nMás suave")}, fps, pFps);
        String[] mn = new String[MBPS_NAMES.length];
        for (int i = 0; i < mn.length; i++) mn[i] = I18n.t(MBPS_NAMES[i]);
        option(content, I18n.t("Calidad de imagen"), I18n.t("Más = menos manchas, pide más al Wi-Fi"), mn, mbps, pMbps);
        String[] fn = new String[FOV_NAMES.length];
        for (int i = 0; i < fn.length; i++) fn[i] = I18n.t(FOV_NAMES[i]);
        option(content, I18n.t("Foveated encoding"), I18n.t("La Mac manda el centro de cada ojo completo y el borde más chico: menos demora y más nitidez en el centro con los mismos Mbps"),
                fn, fov, pFov);
        if (fov > 0) {
            String[] cn = new String[FOVC_NAMES.length];
            for (int i = 0; i < cn.length; i++) cn[i] = I18n.t(FOVC_NAMES[i]);
            option(content, I18n.t("Tamaño del centro nítido"), I18n.t("Más grande = se nota menos, ahorra menos"), cn, fovC, pFovC);
        }
    }

    /// Lo elegido en la calidad de los juegos arranca en lo que tiene la Mac
    private void initQuality(JSONObject cfg) {
        if (selEye < 0) selEye = indexOf(EYES, cfg.optInt("eye", 1024));
        if (selFps < 0) selFps = indexOf(FPSS, cfg.optInt("fps", 90));
        if (selMbps < 0) selMbps = indexOf(MBPSS, cfg.optInt("mbps", 70));
        if (selAudio < 0) { String a = cfg.optString("audio", "quest"); selAudio = a.equals("mac") ? 1 : a.equals("both") ? 2 : 0; }
        if (selFov < 0) { selFov = fovLevel(cfg); selFovC = fovSize(cfg); }
        if (selCodec < 0) selCodec = cfg.optString("codec", "hevc").equals("h264") ? 1 : 0;
    }

    /// Ajustes avanzados de un juego o de todos. withPicker: con el selector «Configurando» (en Ajustes); sin él, el de advKey
    private void gameAdvBlock(JSONObject st, boolean withPicker) {
        JSONObject adv = st.optJSONObject("adv");
        JSONObject all = adv == null ? null : adv.optJSONObject("all");
        JSONObject own = adv == null ? null : adv.optJSONObject("own");
        // Para qué juego: «Todos los juegos» o uno de los de VR (por su carpeta de Steam)
        java.util.List<String> names = new java.util.ArrayList<>(), dirs = new java.util.ArrayList<>();
        names.add(I18n.t("Todos los juegos")); dirs.add("");
        JSONArray gs = st.optJSONArray("games");
        for (int i = 0; gs != null && i < gs.length(); i++) {
            JSONObject g = gs.optJSONObject(i);
            if (!g.optBoolean("vr", true) || g.optString("dir").isEmpty()) continue;
            boolean has = own != null && own.has(g.optString("dir"));
            names.add(g.optString("name") + (has ? I18n.t("\nCon ajustes propios") : I18n.t("\nUsa los de todos los juegos")));
            dirs.add(g.optString("dir"));
        }
        int cur = Math.max(0, dirs.indexOf(advKey));
        final String key = dirs.get(cur);
        if (withPicker) option(content, I18n.t("Configurando"), I18n.t("Para qué juego son estos ajustes"), names.toArray(new String[0]), cur,
                i -> { advKey = dirs.get(i); redraw(); });
        note(content, I18n.t("Si un juego tiene ajustes propios, siempre se usan esos. Si no, usa los de «Todos los juegos». Para que un juego vuelva a usar los generales, elegilo acá y tocá «Usar los de todos los juegos»."));
        JSONObject v = all;
        if (!key.isEmpty()) {
            try {
                v = new JSONObject(all == null ? "{}" : all.toString());
                JSONObject o = own == null ? null : own.optJSONObject(key);
                if (o != null) for (java.util.Iterator<String> it = o.keys(); it.hasNext(); ) { String k = it.next(); v.put(k, o.get(k)); }
            } catch (Exception ignored) { }
            if (own != null && own.has(key)) {
                TextView r = Ui.pill(ctx, I18n.t("Usar los de todos los juegos"));
                r.setOnClickListener(x -> Link.I.send("{\"type\":\"gamecfg\",\"scope\":\"key:" + key + "\",\"reset\":true}"));
                content.addView(r, wrap());
            }
        }
        JSONObject cfg = st.optJSONObject("cfg");
        if (cfg == null) cfg = new JSONObject();
        if (key.isEmpty()) {
            initQuality(cfg);
            qualityBlock(cfg);
        } else {
            // La calidad de este juego: la suya o la general. Se guarda al elegir y se usa la próxima vez que se abra
            final String sc = "key:" + key;
            JSONObject q = v == null ? new JSONObject() : v;
            int fl = q.has("fov") ? fovLevel(q) : fovLevel(cfg), fs = q.has("fov") ? fovSize(q) : fovSize(cfg);
            qualityDetail(indexOf(EYES, q.optInt("eye", cfg.optInt("eye", 1024))), indexOf(FPSS, q.optInt("fps", cfg.optInt("fps", 90))),
                    indexOf(MBPSS, q.optInt("mbps", cfg.optInt("mbps", 70))), fl, fs,
                    i -> sendAdv(sc, "eye", String.valueOf(EYES[i])), i -> sendAdv(sc, "fps", String.valueOf(FPSS[i])),
                    i -> sendAdv(sc, "mbps", String.valueOf(MBPSS[i])),
                    i -> sendAdv(sc, "fov", "\"" + fovString(ctx, i, fs) + "\""), i -> sendAdv(sc, "fov", "\"" + fovString(ctx, fl, i) + "\""));
            note(content, I18n.t("Se guarda al elegir y se usa la próxima vez que abras este juego (la conexión se corta unos segundos al abrirlo)."));
        }
        advanced(content, v, key.isEmpty() ? "all" : "key:" + key);
    }

    private boolean gamesQuality = false;          // pestaña Juegos: calidad desplegada
    private String gameCfgName = null;             // pestaña Juegos: mostrando los ajustes de este juego (advKey = su carpeta)

    private void renderGames(JSONObject st) {
        boolean busy = st.optBoolean("busy");
        // Ajustes de un juego (desde «Ajustes» sobre su portada): se quedan en esta pestaña
        if (gameCfgName != null) {
            TextView back = Ui.pill(ctx, I18n.t("‹ Juegos"));
            back.setOnClickListener(v -> { gameCfgName = null; advKey = ""; redraw(); });
            content.addView(back, wrap());
            header(I18n.t("Ajustes de %s").replace("%s", gameCfgName), I18n.t("Solo para este juego: calidad y rendimiento. Lo que no cambies acá sale de «Todos los juegos»."));
            gameAdvBlock(st, false);
            return;
        }
        header(I18n.t("Juegos"), busy ? I18n.t("Abriendo un juego… esperá un momento.") : I18n.t("Tus juegos de Steam para VR. Al elegir uno, el Quest pasa a VR."));
        // Calidad de los juegos (la misma que en Ajustes › Juegos VR), plegada
        JSONObject cfg = st.optJSONObject("cfg");
        if (cfg != null) {
            initQuality(cfg);
            LinearLayout q = Ui.row(ctx, "sliders", I18n.t("Calidad de los juegos"), EYES[selEye] + " · " + FPSS[selFps] + " Hz · " + MBPSS[selMbps] + " Mbps");
            q.addView(Ui.label(ctx, gamesQuality ? "⌃" : "›", 22, Ui.TEXT2, false));
            q.setOnClickListener(v -> { gamesQuality = !gamesQuality; redraw(); });
            content.addView(q);
            if (gamesQuality) qualityBlock(cfg);
        }
        JSONArray games = st.optJSONArray("games");
        int n = games == null ? 0 : games.length();
        if (n == 0) { content.addView(Ui.label(ctx, I18n.t("No hay juegos instalados en el Steam de VRmin Host."), 16, Ui.TEXT2, false)); return; }
        // Un juego ya abierto en la Mac (por ejemplo, después de cambiar la calidad): volver a él o cerrarlo
        if (st.optBoolean("game")) {
            String gn = st.optString("gameName");
            LinearLayout card = Ui.row(ctx, "gamepad", gn.isEmpty() ? I18n.t("Hay un juego abierto") : gn,
                    !host.inVR() ? I18n.t("Para verlo, entrá en Studio.") : host.gameHidden() ? I18n.t("Sigue abierto mientras estás en la sala. Tocá «Volver al juego».")
                            : I18n.t("Se vuelve a ver solo en unos segundos. Si no, tocá «Volver al juego».")); 
            TextView back = Ui.primary(ctx, I18n.t(host.inVR() ? "Volver al juego" : "Entrar en Studio"));
            back.setOnClickListener(v -> { if (host.gameHidden()) host.showGame(); else if (host.inVR()) Link.I.send("{\"type\":\"rejoin\"}"); else host.enterVR(); });
            card.addView(back);
            TextView quit = Ui.pill(ctx, I18n.t("Cerrar el juego"));
            quit.setOnClickListener(v -> Link.I.send("{\"type\":\"quitgame\"}"));
            LinearLayout.LayoutParams qlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            qlp.leftMargin = dp(ctx, 8);
            card.addView(quit, qlp);
            content.addView(card);
        }
        // Grilla de portadas (3 por fila): primero los de VR; los planos aparte (se abren como una ventana, en Studio)
        java.util.List<JSONObject> vr = new java.util.ArrayList<>(), flat = new java.util.ArrayList<>();
        for (int i = 0; i < n; i++) { JSONObject g = games.optJSONObject(i); (g.optBoolean("vr", true) ? vr : flat).add(g); }
        gameGrid(vr, busy);
        if (!flat.isEmpty()) {
            content.addView(Ui.section(ctx, I18n.t("Juegos planos")));
            TextView fh = Ui.label(ctx, I18n.t("Se abren como una ventana de la Mac, acá en Studio (sin pasar al modo de juego)."), 14, Ui.TEXT2, false);
            fh.setPadding(dp(ctx, 4), 0, 0, 0);
            content.addView(fh);
            gameGrid(flat, busy);
        }
        TextView tip = Ui.label(ctx, I18n.t("¿No está tu juego? Abrilo directamente desde Steam (en la Mac o desde el escritorio de la Mac acá en el visor)."), 14, Ui.TEXT3, false);
        tip.setPadding(dp(ctx, 4), dp(ctx, 18), 0, 0);
        content.addView(tip);
    }

    private void gameGrid(java.util.List<JSONObject> list, boolean busy) {
        LinearLayout rowL = null;
        int n = list.size();
        for (int i = 0; i < n; i++) {
            if (i % 3 == 0) {
                rowL = new LinearLayout(ctx);
                rowL.setOrientation(LinearLayout.HORIZONTAL);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                lp.topMargin = dp(ctx, 14);
                content.addView(rowL, lp);
            }
            JSONObject g = list.get(i);
            rowL.addView(gameTile(g.optString("id"), g.optString("name"), busy, g.optBoolean("vr", true) ? g.optString("dir") : ""), tileParams(i % 3));
        }
        for (int i = n % 3; rowL != null && i != 0 && i < 3; i++) rowL.addView(new View(ctx), tileParams(i));
    }

    private LinearLayout.LayoutParams tileParams(int col) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        if (col > 0) lp.leftMargin = dp(ctx, 14);
        return lp;
    }

    private View gameTile(String id, String name, boolean busy, String dir) {
        LinearLayout t = new LinearLayout(ctx);
        t.setOrientation(LinearLayout.VERTICAL);
        t.setPadding(dp(ctx, 8), dp(ctx, 8), dp(ctx, 8), dp(ctx, 12));
        t.setBackground(Ui.hoverable(ctx, Color.argb(22, 255, 255, 255), 22, false));
        t.setClickable(true);
        t.setAlpha(busy ? 0.5f : 1f);
        ImageView img = new ImageView(ctx);
        img.setScaleType(ImageView.ScaleType.CENTER_CROP);
        img.setClipToOutline(true);
        GradientDrawable ph = new GradientDrawable();
        ph.setColor(Color.argb(40, 255, 255, 255));
        ph.setCornerRadius(Ui.rad(ctx, 16));
        img.setBackground(ph);
        // Al pasar el láser por la portada: «Jugar» y «Ajustes» (los de este juego, sin salir de esta pestaña)
        FrameLayout cover = new FrameLayout(ctx);
        cover.addView(img, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        LinearLayout over = new LinearLayout(ctx);
        over.setOrientation(LinearLayout.HORIZONTAL);
        over.setGravity(Gravity.CENTER);
        GradientDrawable sc = new GradientDrawable();
        sc.setColor(Color.argb(150, 0, 0, 0));
        sc.setCornerRadius(Ui.rad(ctx, 16));
        over.setBackground(sc);
        over.setVisibility(View.GONE);
        cover.addView(over, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        t.addView(cover, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(ctx, 128)));
        loadCover(id, img);
        TextView l = Ui.label(ctx, name, 16, Ui.TEXT, true);
        l.setSingleLine(true);
        l.setEllipsize(android.text.TextUtils.TruncateAt.END);
        l.setPadding(dp(ctx, 4), dp(ctx, 10), dp(ctx, 4), 0);
        t.addView(l);
        Runnable play = () -> {
            Link.I.send("{\"type\":\"launch\",\"game\":\"" + id + "\"}");
            host.showGame();   // (si había un juego dejado para ir a la sala, se vuelve a ver)
            // El juego necesita el visor en VR para dibujar: pasar ya a la parte inmersiva
            if (!host.inVR()) t.postDelayed(this::goVR, 300);
        };
        if (!busy) {
            t.setOnClickListener(v -> play.run());
            TextView pb = Ui.primary(ctx, I18n.t("Jugar"));
            pb.setOnClickListener(v -> play.run());
            over.addView(pb);
            if (!dir.isEmpty()) {   // (los juegos planos no tienen estos ajustes)
                TextView ab = Ui.pill(ctx, I18n.t("Ajustes"));
                ab.setOnClickListener(v -> { gameCfgName = name; advKey = dir; redraw(); });
                LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                alp.leftMargin = dp(ctx, 10);
                over.addView(ab, alp);
            }
            // (el láser sobre un botón «sale» de la portada: se esconde con un poco de demora, por si entra a otro)
            Runnable hide = () -> over.setVisibility(View.GONE);
            View.OnHoverListener hov = (v, e) -> {
                if (e.getAction() == android.view.MotionEvent.ACTION_HOVER_ENTER || e.getAction() == android.view.MotionEvent.ACTION_HOVER_MOVE) {
                    t.removeCallbacks(hide); over.setVisibility(View.VISIBLE);
                } else if (e.getAction() == android.view.MotionEvent.ACTION_HOVER_EXIT) { t.removeCallbacks(hide); t.postDelayed(hide, 250); }
                return false;
            };
            t.setOnHoverListener(hov);
            for (int k = 0; k < over.getChildCount(); k++) over.getChildAt(k).setOnHoverListener(hov);
        }
        return t;
    }

    /// Portada del juego desde Steam (se guarda en memoria mientras la app esté abierta)
    private void loadCover(String appId, ImageView into) {
        Bitmap b = covers.get(appId);
        if (b != null) { into.setImageBitmap(b); return; }
        into.setTag(appId);
        loader.execute(() -> {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL("https://cdn.cloudflare.steamstatic.com/steam/apps/" + appId + "/header.jpg").openConnection();
                c.setConnectTimeout(4000);
                c.setReadTimeout(6000);
                try (InputStream in = c.getInputStream()) {
                    Bitmap bm = BitmapFactory.decodeStream(in);
                    if (bm == null) return;
                    covers.put(appId, bm);
                    into.post(() -> { if (appId.equals(into.getTag())) into.setImageBitmap(bm); });
                }
            } catch (Exception ignored) { }
        });
    }
}
