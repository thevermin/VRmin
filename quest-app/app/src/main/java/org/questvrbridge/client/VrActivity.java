package org.questvrbridge.client;

import android.app.NativeActivity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/**
 * Sala VR: la parte nativa (OpenXR) dibuja la habitación, la TV y las ventanas flotantes.
 * Lo que se ve en la TV (el mismo menú de la ventana del Quest) y en cada ventana es interfaz de Android
 * dibujada en una pantalla virtual; el puntero de los controles llega desde la parte nativa.
 */
public class VrActivity extends NativeActivity implements MenuUi.Host {
    static { System.loadLibrary("qvbclient"); }

    static native Surface nativeCreatePanel(int id, int w, int h, float meters, boolean tv);
    static native void nativeDestroyPanel(int id);
    static native void nativeSend(String json);
    static native void nativeExitVR();
    static native void nativeSet3DMode(int mode);
    static native void nativeSetHandModel(int model);
    static native void nativeSetHandColor(int color);
    static native void nativeSetLaser(int color, float gain);
    static native void nativeSetWatch(boolean on);
    static native void nativeSetTimeZone(int minutes);
    static native void nativeSetWristFollow(boolean follow);
    static native void nativeSetRealHands(boolean on);
    static native void nativeSetCameraAllowed(boolean on);
    static native void nativeSetHandSat(float sat);
    static native void nativeSetEdgeSoft(float v);
    static native void nativeToggleInputLock();
    static native void nativeSetPanelInGame(int id, boolean on);
    static native void nativeSetGameHands(boolean on);
    static native void nativeCloseGameMenu();
    static native void nativeSetGameHidden(boolean on);
    static native void nativeSetVersion(String v);
    static native void nativeSetHandGestures(boolean on);
    static native void nativeSetHeight(float m);
    static native void nativeSetHandMove(boolean on);
    static native void nativeRecenter();
    static native void nativeSetFrameGen(int mode);
    static native void nativeSetLaserNoTouch(boolean on);
    static native void nativeSetPortal(boolean on);
    static native void nativeSetImage(float b, float c, float s, float sharp, float warm);
    static native void nativeSetHud(int id);
    static native void nativeSizePanel(int id, float factor);
    static native String nativeStats();
    static native void nativeSet3DStrength(float s);
    static native void nativeSetLeds(boolean on);
    static native void nativeSetLight(float g);
    static native void nativeAttachPanel(int child, int parent);
    static native void nativeSetDock(int id, int dock);
    static native void nativeSetZoom(int id, float z);
    static native String nativeFovCenters();
    static native void nativeSetPanelNoTouch(int id, boolean on);
    static native void nativeSetHandTracking(boolean on);
    static native void nativeScalePanel(int id, float f);
    static native void nativeResetPanel(int id);
    static native void nativeSetFollow(int id, boolean on);
    static native void nativeSetUsbNet(long handle, String ifname);
    static native void nativeSetHandAngles(float pitchL, float rollL, float pitchR, float rollR);
    static native void nativeHandCalibrate();
    static native float[] nativeHandCalResult();
    static native void nativeSetHandCal(float[] q);
    static native void nativeSetKeyboardAnchor(int id);
    static native void nativeSetVigia(boolean on);
    static native void nativeVigiaCalibrate();
    static native void nativeSetVigiaHeight(float m);
    static native void nativeSetCableOnly(boolean on);
    static native void nativeSetGestureMap(int profile, int hand, int[] map);
    /// Modo Centinela: el visor apoyado en el escritorio te mira y sigue los controles; el juego se ve en la Mac
    static volatile boolean vigiaOn = false;
    static volatile long vigiaSince = 0;
    static volatile boolean macVigia = false;
    @Override protected void onNewIntent(android.content.Intent i) {
        super.onNewIntent(i);
        setIntent(i);
        String m = i.getStringExtra("mac");
        if (m != null && !m.isEmpty() && (Link.I.target() == null || !m.equals(Link.I.target().name))) Link.I.connectTo(new Link.Mac(m, "", 9100));
        if (i.getBooleanExtra("vigia", false)) setVigia(true);
    }
    static void setVigia(boolean on) { vigiaOn = on; vigiaSince = SystemClock.uptimeMillis(); nativeSetVigia(on); }
    static native void nativeSetLocomotion(int turnMode, int snapDeg, int turnSpeed, int moveMode, float moveSpeed, boolean vignette);
    static native void nativeCurvePanel(int id);
    static native void nativeSetCurve(int id, float amount);
    static native void nativeConnectTo(String name);
    static native void nativeDisconnect();
    static native void nativeSetEnvironment(int e);
    static native void nativeSetLoading(boolean on);
    static native void nativeDeskRect(boolean visible, float x0, float y0, float x1, float y1);
    static native void nativeSetPanel3D(int id, int mode, float strength);
    static native void nativePanelRect(int id, float x0, float y0, float x1, float y1);

    private static final int TV_ID = 1;
    private static final float DENSITY = 1.25f;   // píxeles por dp en las pantallas virtuales (nitidez)

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Map<Integer, Panel> panels = new HashMap<>();
    private final Map<Integer, Integer> toolbars = new HashMap<>();   // ventana -> su barra de botones
    private int nextToolbarId = 0x30000000;

    private static final class Panel {
        VirtualDisplay vd; Presentation pres; View root;
        MenuUi menu; WindowUi win;
        int d3Mode;                   // 3D de esta ventana: 0 apagado, 1 IA, 2 SBS, 3 arriba/abajo
        int d3Str = -1;               // su intensidad (-1: la general)
        int sizeP = -1;               // tamaño en % (0% = ×0,5 … 100% = ×2,5; -1: tamaño original = 25%)
        int curveP = 0;               // curva en % (0 = plana)
        Runnable hudPaint;            // (barras) repinta su botón de rendimiento
        int zoomP = 0;                // 3D real: cuánto se amplía el video (0 = su tamaño · 100 = ×4)
        boolean inGame;               // se ve durante el juego (abierta desde el menú del juego)
        boolean paused;               // ventana de la sala pausada mientras hay un juego
        long downTime;
        int w, h;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        I18n.init(this);
        Link.I.setContext(this);
        Link.I.useBridge(new Link.Bridge() {
            @Override public void send(String json) { nativeSend(json); }
            @Override public void connect(String name) { nativeConnectTo(name); }
            @Override public void disconnect() { nativeDisconnect(); }
        });
        // La Mac que ya estaba elegida en la ventana del Quest (si hay)
        String mac = getIntent().getStringExtra("mac");
        if (mac != null && !mac.isEmpty()) Link.I.connectTo(new Link.Mac(mac, "", 9100));
        // «Solo Modo Centinela» desde VRmin Host (por el cable): la sala abre directo en el Modo Centinela
        if (getIntent().getBooleanExtra("vigia", false)) setVigia(true);
        // Tienda de Meta: sin compra verificada, la sala no abre
        Licencia.comprobar(this, (comprada, detalle) -> { if (!comprada) nativeExitVR(); });
        // La TV: el menú (Escritorio y Juegos) y la pantalla principal de la Mac
        Panel tv = createPanel(TV_ID, 1600, 1000, 2.2f, true);
        if (tv != null) {
            tv.menu = new MenuUi(tv.pres.getContext(), this);
            show(tv, tv.menu.view());
            tv.menu.start();
            if (!toolbars.containsKey(TV_ID)) createToolbar(TV_ID, tv);   // (puede haberla creado ya el cambio de ambiente)
        }
        askCamera();
        nativeSetGameHands(prefs().getBoolean("gameHands", false));
        nativeSetHandGestures(prefs().getBoolean("handGestures", true));
        applyHandAngles();
        nativeSetHeight(MenuUi.effectiveHeight(prefs()));
        nativeSetVigiaHeight(MenuUi.vigiaHeight(prefs()));
        // Centro óptico de cada ojo (lo usa el foveated encoding): se guarda unos segundos después de entrar
        ui.postDelayed(() -> { try { prefs().edit().putString("fovCenters", nativeFovCenters()).apply(); } catch (Throwable ignored) { } }, 6000);
        nativeSetCableOnly(prefs().getBoolean("cableOnly", false));
        applyGestures();
        nativeSetHandMove(prefs().getBoolean("handMove", true));
        nativeSetFrameGen(0);   // (generar cuadros: apagado; no mejoraba lo que se ve)
        setLocomotion();
        // Cable USB-C (Horizon OS 2.5+): si está, la Mac se encuentra y se conecta por ahí
        UsbLink.I.start(this, (h, ifn) -> nativeSetUsbNet(h, ifn));
        // Teclado: se abre solo cuando en la Mac hay un campo de texto activo (si está activado)
        Link.I.addListener(new Link.Listener() {
            @Override public void onState(org.json.JSONObject o) {
                // Modo Centinela cerrado desde la Mac (Esc o «Salir»)
                if (o != null && o.has("vigia")) {
                    // (solo si la Mac ya lo había mostrado: así no se apaga antes de que le llegue el aviso)
                    if (o.optBoolean("vigia")) macVigia = true;
                    else if (macVigia) { macVigia = false; if (vigiaOn) setVigia(false); }
                }
                // Otro juego: sus propios ángulos de las manos
                if (o != null) { String g = o.optString("gameName", ""); if (!g.equals(handGame)) { handGame = g; applyHandAngles(); } }
                if (o != null) loading.onState(o);
                if (o == null || !o.has("typing")) return;
                boolean t = o.optBoolean("typing");
                if (t != lastTyping) {
                    lastTyping = t;
                    // Solo si recién tocaste una pantalla de la Mac (así no se abre solo al conectar, si el foco ya estaba en un texto)
                    long now = SystemClock.uptimeMillis();
                    boolean byClick = now - lastMacClick < 3000
                            && now - lastPanelOpen > 8000   // no al abrir una pantalla o ventana (ni al conectar)
                            && lastMacClick - lastPanelOpen > 1500;
                    if (t && byClick && prefs().getBoolean("kbAuto", true) && !panels.containsKey(KEYBOARD_ID)) { openKeyboard(lastMacPanel); kbAutoOpened = true; }
                    else if (!t && kbAutoOpened) closeKeyboard();
                }
            }
            @Override public void onConnection(boolean connected, String name) { lastPanelOpen = SystemClock.uptimeMillis(); }
        });
        try { nativeSetVersion(getPackageManager().getPackageInfo(getPackageName(), 0).versionName); } catch (Exception ignored) { }
    }

    // ------------------------------------------------------------------ teclado
    private static final int KEYBOARD_ID = 0x2FFFFFC0;
    private boolean lastTyping = false, kbAutoOpened = false, inGameNow = false;
    // Pantalla de carga mientras se abre un juego (y un momento al cerrarse)
    private static final int LOADING_ID = 0x2FFFFFA0;
    private final Loading loading = new Loading(this, new Loading.Host() {
        @Override public void nativeLoading(boolean on) { nativeSetLoading(on); }
        @Override public void loadingPanel(boolean on) {
            if (on) {
                if (panels.containsKey(LOADING_ID)) return;
                Panel p = createPanel(LOADING_ID, 620, 780, 1.25f, false);
                if (p == null) return;
                p.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
                show(p, loading.build(p.pres.getContext()));
                nativeSetFollow(LOADING_ID, true);   // siempre al frente: te sigue cuando girás
            } else {
                Panel p = panels.remove(LOADING_ID);
                if (p == null) return;
                release(p);
                nativeDestroyPanel(LOADING_ID);
            }
        }
    });
    private String handGame = "";
    private long lastMacClick = 0, lastPanelOpen = 0;
    private int lastMacPanel = TV_ID;

    @Override public void setLocomotion() {
        android.content.SharedPreferences pr = prefs();
        nativeSetLocomotion(pr.getInt("turnMode", 0), pr.getInt("snapDeg", 30), pr.getInt("turnSpeed", 120),
                pr.getInt("moveMode", 0), pr.getFloat("moveSpeed", 2.0f), pr.getBoolean("vignette", true));
    }

    @Override public void toggleKeyboard() { toggleKeyboardFor(lastMacPanel); }
    private void toggleKeyboardFor(int anchor) { if (panels.containsKey(KEYBOARD_ID)) closeKeyboard(); else { openKeyboard(anchor); kbAutoOpened = false; } }

    /// Abre el teclado debajo de esa pantalla o ventana (más chico que una ventana: unos 48 cm de ancho)
    /// Ángulo de las manos y el láser en los juegos, por mano
    private void applyHandAngles() {
        android.content.SharedPreferences pr = prefs();
        // (0.65 guardaba 85° / −130° / +80° al calibrar; ahora eso es el «0» de las manos: no sumarlo dos veces)
        if (!pr.getBoolean("handBase2", false)) {
            android.content.SharedPreferences.Editor ed = pr.edit().putBoolean("handBase2", true);
            if (pr.getInt("handPitchOffL", 0) == -85 && pr.getInt("handRollOffL", 0) == -130) ed.remove("handPitchOffL").remove("handRollOffL");
            if (pr.getInt("handPitchOffR", 0) == -85 && pr.getInt("handRollOffR", 0) == 80) ed.remove("handPitchOffR").remove("handRollOffR");
            ed.apply();
        }
        nativeSetHandAngles(pr.getInt(GameMenuUi.handKey("handPitchOffL"), 0), pr.getInt(GameMenuUi.handKey("handRollOffL"), 0),
                pr.getInt(GameMenuUi.handKey("handPitchOffR"), 0), pr.getInt(GameMenuUi.handKey("handRollOffR"), 0));   // (las de este juego)
        // Calibración de las manos (con los controles): por mano, de la palma al agarre y al rayo del control.
        // (la de antes, con las palmas hacia el visor, no se usa más)
        float[] q = new float[0];
        String[] c = pr.getString("handCal2", "").split(",");
        if (c.length == 30) try { q = new float[30]; for (int i = 0; i < 30; i++) q[i] = Float.parseFloat(c[i]); } catch (NumberFormatException e) { q = new float[0]; }
        nativeSetHandCal(q);
    }

    /// Calibrar las manos: mide 1,5 s y avisa qué manos quedaron (bit 0 izquierda, bit 1 derecha; 0 = ninguna)
    private void handCalibrate(java.util.function.IntConsumer done) {
        nativeHandCalibrate();
        final long t0 = android.os.SystemClock.uptimeMillis();
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable poll = new Runnable() { @Override public void run() {
            float[] r = nativeHandCalResult();
            if (r[0] < 0 && android.os.SystemClock.uptimeMillis() - t0 < 4000) { h.postDelayed(this, 200); return; }
            int st = Math.max(0, (int) r[0]);
            if (st != 0 && st != 8) {   // (las dos manos: la que no se recalibró queda como estaba)
                StringBuilder b = new StringBuilder();
                for (int i = 1; i < r.length; i++) { if (i > 1) b.append(','); b.append(r[i]); }
                prefs().edit().putString("handCal2", b.toString()).apply();
            }
            done.accept(st);
        } };
        h.postDelayed(poll, 2500);
    }

    private void openKeyboard(int anchor) {
        if (panels.containsKey(KEYBOARD_ID)) return;
        nativeSetKeyboardAnchor(anchor);
        Panel p = createPanel(KEYBOARD_ID, 1180, 470, 0.48f, false);
        if (p == null) return;
        p.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        if (inGameNow) { p.inGame = true; nativeSetPanelInGame(KEYBOARD_ID, true); }
        KeyboardUi k = new KeyboardUi(p.pres.getContext(), this::closeKeyboard);
        show(p, k.view());
    }

    @Override public void closeKeyboard() {
        kbAutoOpened = false;
        Panel p = panels.remove(KEYBOARD_ID);
        if (p == null) return;
        release(p);
        nativeDestroyPanel(KEYBOARD_ID);
    }

    // ------------------------------------------------------------------ juego VR
    private static final int GAME_MENU_ID = 0x2FFFFFD0;
    private GameMenuUi gameMenu;
    private final java.util.Set<Integer> following = new java.util.HashSet<>();   // ventanas fijadas a la vista

    /** Empieza o termina un juego VR (llega en el hilo de dibujo). */
    void onGameState(boolean on) { runOnUiThread(() -> setGameState(on)); }
    /** Mantener el botón de menú izquierdo durante un juego: abrir o cerrar su menú. */
    void onGameMenu(boolean open) { runOnUiThread(() -> { if (open) openGameMenu(); else closeGameMenuPanel(false); }); }

    /// Durante el juego, las ventanas de la sala dejan de recibir imagen (la Mac deja de mandarla) y vuelven al salir
    private void setGameState(boolean on) {
        inGameNow = on;
        loading.onGameState(on);
        for (Panel p : panels.values()) {
            if (p.win == null || p.inGame) continue;
            if (on && !p.paused) { p.win.stop(); p.paused = true; }
            else if (!on && p.paused) { p.win.start(); p.paused = false; }
        }
        if (!on) closeGameMenuPanel(false);
    }

    private void openGameMenu() {
        if (panels.containsKey(GAME_MENU_ID)) return;
        Panel p = createPanel(GAME_MENU_ID, 1300, 820, 0.95f, false);
        if (p == null) return;
        p.inGame = true;
        nativeSetPanelInGame(GAME_MENU_ID, true);
        p.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        gameMenu = new GameMenuUi(p.pres.getContext(), new GameMenuUi.Host() {
            @Override public void closeGameMenu() { closeGameMenuPanel(true); }
            @Override public void openGameWindow(int id, String app, String title, int w, int h) { VrActivity.this.openGameWindow(id, app, title, w, h); }
            @Override public boolean isWindowOpen(int id) { Panel q = panels.get(id); return q != null && q.inGame; }
            @Override public void setHud(boolean on) { VrActivity.this.setHud(on); }
            @Override public void setGameHands(boolean on) { nativeSetGameHands(on); }
            @Override public void setHandGestures(boolean on) { nativeSetHandGestures(on); }
            @Override public void setHandAngles(float pitch, float roll) { applyHandAngles(); }
            @Override public void setVigia(boolean on) { VrActivity.setVigia(on); if (on) closeGameMenuPanel(true); }
            @Override public boolean vigia() { return vigiaOn; }
            @Override public void vigiaCalibrate() { nativeVigiaCalibrate(); closeGameMenuPanel(true); }
            @Override public void applyHeight() { VrActivity.this.setHeight(MenuUi.effectiveHeight(prefs())); }
            @Override public void applyGestures() { VrActivity.this.applyGestures(); }
            @Override public void handCalibrate(java.util.function.IntConsumer done) { VrActivity.this.handCalibrate(done); }
            @Override public void resetHandCal() { prefs().edit().remove("handCal").remove("handCal2").apply(); applyHandAngles(); }
            @Override public void toMainMenu() { gameHidden = true; nativeSetGameHidden(true); closeGameMenuPanel(true); }
        });
        show(p, gameMenu.view());
        gameMenu.start();
    }

    private void closeGameMenuPanel(boolean tellNative) {
        Panel p = panels.remove(GAME_MENU_ID);
        if (gameMenu != null) { gameMenu.stop(); gameMenu = null; }
        if (p != null) { release(p); nativeDestroyPanel(GAME_MENU_ID); }
        if (tellNative) nativeCloseGameMenu();
    }

    /// Ventana o monitor de la Mac abierto desde el menú del juego: flota delante tuyo durante el juego
    private void openGameWindow(int id, String app, String title, int w, int h) {
        Panel p = panels.get(id);
        if (p == null) { openWindow(id, app, title, w, h); p = panels.get(id); }
        if (p == null) return;
        if (p.paused && p.win != null) { p.win.start(); p.paused = false; }
        p.inGame = true;
        nativeSetPanelInGame(id, true);
    }

    /// Cámaras del visor: para ver tus manos reales con el borde difuminado (sin permiso, se usa el passthrough de Meta)
    private static final String HEADSET_CAMERA = "horizonos.permission.HEADSET_CAMERA";
    private static final int CAMERA_REQUEST = 41;

    private void askCamera() {
        try {
            if (checkSelfPermission(HEADSET_CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED) nativeSetCameraAllowed(true);
            else requestPermissions(new String[] { HEADSET_CAMERA }, CAMERA_REQUEST);
        } catch (Exception e) {
            nativeSetCameraAllowed(false);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code != CAMERA_REQUEST) return;
        boolean ok = results.length > 0 && results[0] == android.content.pm.PackageManager.PERMISSION_GRANTED;
        nativeSetCameraAllowed(ok);
    }

    @Override
    protected void onDestroy() {
        for (Panel p : panels.values()) release(p);
        panels.clear();
        super.onDestroy();
    }

    private Panel createPanel(int id, int dpW, int dpH, float meters, boolean tv) {
        int w = Math.round(dpW * DENSITY), h = Math.round(dpH * DENSITY);
        Surface s = nativeCreatePanel(id, w, h, meters, tv);
        if (s == null) return null;
        if (id != KEYBOARD_ID && id != GAME_MENU_ID) lastPanelOpen = SystemClock.uptimeMillis();
        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Panel p = new Panel();
        p.w = w; p.h = h;
        p.vd = dm.createVirtualDisplay("vrmin-" + id, w, h, Math.round(160 * DENSITY), s, DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY);
        p.pres = new Presentation(this, p.vd.getDisplay());
        p.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.BLACK));
        panels.put(id, p);
        return p;
    }

    private void show(Panel p, View v) {
        p.root = v;
        p.pres.setContentView(v);
        p.pres.show();
    }

    private void release(Panel p) {
        if (p.menu != null) p.menu.stop();
        if (p.win != null) p.win.stop();
        try { p.pres.dismiss(); } catch (Exception ignored) { }
        p.vd.release();
    }

    // ------------------------------------------------------------------ llamadas desde la parte nativa
    /** Paquete de la Mac para la interfaz (estado, escritorio o ventana). Llega en el hilo de red. */
    void onUiPacket(byte[] data) { Link.I.handlePacket(data, data.length); }

    void onNativeConnection(boolean connected, String name) { Link.I.bridgeConnection(connected, name); }

    /** Primer mensaje para esa Mac (clave o código), o "" si hay que esperar el código. Llega en el hilo de red. */
    String authFor(String mac) { String a = Link.I.authJson(mac); return a == null ? "" : a; }

    /** Miraste el reloj de la mano izquierda: abrir o cerrar el menú (igual que el botón del menú). Llega en el hilo de la VR. */
    void onWatch() { runOnUiThread(this::toggleWristMenu); }

    /// El menú del reloj: el mismo menú, en una pantallita que flota encima del reloj (otra forma de llegar a él)
    private static final int WRIST_ID = 0x2FFFFFE0;

    private boolean wristFree = false;   // el menú del reloj quedó suelto (como una ventana más)

    /// Mirar el reloj: abre el menú encima del reloj; si ya está, lo cierra; si estaba suelto, lo vuelve a traer al reloj
    private void toggleWristMenu() {
        if (loading.active()) return;   // (la pantalla de carga es solo para esperar)
        if (panels.containsKey(WRIST_ID)) {
            if (wristFree) setWristMenuFree(false); else closeWristMenu();
            return;
        }
        wristFree = false;
        nativeSetWristFollow(true);
        Panel p = createPanel(WRIST_ID, 1150, 740, 0.46f, false);
        if (p == null) return;
        p.menu = new MenuUi(p.pres.getContext(), this, true);
        show(p, p.menu.view());
        p.menu.start();
    }

    @Override public boolean isWristMenuFree() { return wristFree; }

    @Override public void setWristMenuFree(boolean free) {
        wristFree = free;
        nativeSetWristFollow(!free);
        Panel p = panels.get(WRIST_ID);
        if (p != null && p.menu != null) p.menu.refreshWristButton();
    }

    @Override public void closeWristMenu() {
        Panel p = panels.remove(WRIST_ID);
        if (p == null) return;
        release(p);
        nativeDestroyPanel(WRIST_ID);
    }

    /// Hora del Quest para el reloj (diferencia con UTC en minutos, con horario de verano)
    private void sendTimeZone() {
        nativeSetTimeZone(java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000);
    }

    @Override protected void onResume() { super.onResume(); sendTimeZone(); }

    /** Macs que se ven en la red (lo manda la parte nativa cada segundo). */
    void onMacs(String json) {
        java.util.List<Link.Mac> list = new java.util.ArrayList<>();
        try {
            org.json.JSONArray a = new org.json.JSONArray(json);
            for (int i = 0; i < a.length(); i++) {
                org.json.JSONObject o = a.getJSONObject(i);
                Link.Mac m = new Link.Mac(o.optString("name"), o.optString("ip"), 9100);
                m.idle = o.optBoolean("idle");
                m.usb = o.optBoolean("usb");
                list.add(m);
            }
        } catch (Exception ignored) { }
        Link.I.setBridgeMacs(list);
    }

    /** Puntero de un control sobre un panel: 0 mover, 1 apretar, 2 arrastrar, 3 soltar, 4 rueda, 5 salió. */
    void onPointer(int id, int action, float u, float v, float scroll) {
        ui.post(() -> {
            Panel p = panels.get(id);
            if (p == null || p.root == null) return;
            // Clic en una pantalla o ventana de la Mac: si enseguida aparece un lugar para escribir, el teclado se abre debajo de ella
            if (action == 1 && (p.win != null || id == TV_ID)) { lastMacClick = SystemClock.uptimeMillis(); lastMacPanel = id; }
            float x = u * p.root.getWidth(), y = v * p.root.getHeight();
            long now = SystemClock.uptimeMillis();
            MotionEvent e;
            switch (action) {
                case 0:
                case 5:
                    e = MotionEvent.obtain(now, now, action == 0 ? MotionEvent.ACTION_HOVER_MOVE : MotionEvent.ACTION_HOVER_EXIT, x, y, 0);
                    e.setSource(InputDevice.SOURCE_MOUSE);
                    p.root.dispatchGenericMotionEvent(e);
                    break;
                case 1:
                    p.downTime = now;
                    e = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, x, y, 0);
                    e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
                    p.root.dispatchTouchEvent(e);
                    break;
                case 2:
                case 3:
                    e = MotionEvent.obtain(p.downTime, now, action == 2 ? MotionEvent.ACTION_MOVE : MotionEvent.ACTION_UP, x, y, 0);
                    e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
                    p.root.dispatchTouchEvent(e);
                    break;
                case 4: {
                    MotionEvent.PointerProperties[] pp = {new MotionEvent.PointerProperties()};
                    pp[0].id = 0; pp[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
                    MotionEvent.PointerCoords[] pc = {new MotionEvent.PointerCoords()};
                    pc[0].x = x; pc[0].y = y;
                    pc[0].setAxisValue(MotionEvent.AXIS_VSCROLL, scroll);
                    e = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, pp, pc, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
                    p.root.dispatchGenericMotionEvent(e);
                    break;
                }
                default: return;
            }
            e.recycle();
        });
    }

    // ------------------------------------------------------------------ MenuUi.Host
    /// Intensidad del 3D: 100% = correr la imagen 1,5% del ancho (antes era 3%, demasiado)
    private static final float MAX_3D = 0.015f;
    private static final int HUD_ID = 0x2FFFFFF0;

    @Override public boolean inVR() { return true; }
    @Override public void enterVR() { }
    private boolean gameHidden = false;   // «Menú principal» desde el menú del juego: el juego sigue abierto detrás
    @Override public boolean gameHidden() { return gameHidden; }
    @Override public void showGame() { if (gameHidden) { gameHidden = false; nativeSetGameHidden(false); } }
    @Override public void exitVR() { nativeExitVR(); }
    /// «Cerrar VRmin» desde Studio: el panel (MainActivity) corre en otro proceso y quedaba abierto. Se cierran
    /// todas las tareas de la app y los otros procesos suyos, y después este.
    @Override public void quitApp() {
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            for (android.app.ActivityManager.AppTask t : am.getAppTasks()) t.finishAndRemoveTask();
        } catch (Exception ignored) { }
        finishAffinity();
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            try {
                android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
                int me = android.os.Process.myPid();
                for (android.app.ActivityManager.RunningAppProcessInfo p : am.getRunningAppProcesses())
                    if (p.pid != me && p.processName.startsWith(getPackageName())) android.os.Process.killProcess(p.pid);
            } catch (Exception ignored) { }
            System.exit(0);
        }, 300);
    }
    @Override public void set3DMode(int mode) { nativeSet3DMode(mode); if (tv3dPaint != null) tv3dPaint.run(); }
    @Override public void set3DStrength(float s) { nativeSet3DStrength(s * MAX_3D); if (tv3dPaint != null) tv3dPaint.run(); }
    @Override public void setLeds(boolean on) { nativeSetLeds(on); }
    @Override public void setHandModel(int m) { nativeSetHandModel(m); }
    @Override public void setHandColor(int c) { nativeSetHandColor(c); }
    @Override public void setLaser(int color, float gain) { nativeSetLaser(color, gain); }
    @Override public void setWatch(boolean on) { nativeSetWatch(on); }
    @Override public void setRealHands(boolean on) { nativeSetRealHands(on); }
    @Override public void setHandTracking(boolean on) { nativeSetHandTracking(on); }
    /// Modo Centinela desde los modos de la barra lateral (en Studio)
    @Override public void enterSentinel() { setVigia(true); }
    @Override public void setHandColor(float sat) { nativeSetHandSat(sat); }
    @Override public void setEdgeSoft(float v) { nativeSetEdgeSoft(v); }
    @Override public void setPortal(boolean on) {
        nativeSetPortal(on);   // (el cartel de cómo dibujarlo lo muestra el menú, encima de sí mismo)
    }

    /// Cómo dibujar el portal: un cartelito delante tuyo que se va solo cuando el portal quedó listo
    private static final int PORTAL_HINT_ID = 0x2FFFFFB0;
    private void showPortalHint() {
        closePortalHint();
        Panel p = createPanel(PORTAL_HINT_ID, 760, 250, 0.55f, false);
        if (p == null) return;
        p.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        Context c = p.pres.getContext();
        android.widget.LinearLayout box = new android.widget.LinearLayout(c);
        box.setOrientation(android.widget.LinearLayout.VERTICAL);
        box.setBackground(Ui.glass(c, 26, 235));
        box.setPadding(Ui.dp(c, 26), Ui.dp(c, 20), Ui.dp(c, 26), Ui.dp(c, 20));
        TextView title = Ui.label(c, I18n.t("Dibujá el portal"), 22, Ui.TEXT, true);
        Ui.leadingIcon(title, "dot", 18, Ui.ACCENT);
        title.setCompoundDrawablePadding(Ui.dp(c, 8));
        box.addView(title);
        TextView body = Ui.label(c, I18n.t("Fuera de las ventanas, pellizcá con la mano del láser (o apretá el gatillo) y, sin soltar, recorré el contorno de lo que querés ver con la punta del índice o del control. Al soltar, el portal queda listo."), 16, Ui.TEXT2, false);
        body.setPadding(0, Ui.dp(c, 8), 0, 0);
        box.addView(body);
        show(p, box);
    }
    private void closePortalHint() {
        Panel p = panels.remove(PORTAL_HINT_ID);
        if (p == null) return;
        release(p);
        nativeDestroyPanel(PORTAL_HINT_ID);
    }
    /// Controles y manos en pausa o activos de nuevo (lo cambia la parte nativa con A+B, X+Y, pulgar y meñique derecho
    /// o el botón de la barra): en pausa, un cartel donde aparece el teclado que queda hasta volver (así cualquiera sabe
    /// cómo); al volver, otro que se va solo
    private static final int LOCK_NOTE_ID = 0x2FFFFFB1;
    void onInputLock(boolean on) {
        runOnUiThread(() -> {
            Panel old = panels.remove(LOCK_NOTE_ID);
            if (old != null) { release(old); nativeDestroyPanel(LOCK_NOTE_ID); }
            nativeSetKeyboardAnchor(lastMacPanel);   // (debajo de la última pantalla que usaste)
            Panel p = createPanel(LOCK_NOTE_ID, 760, 220, 0.5f, false);
            if (p == null) return;
            p.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            Context c = p.pres.getContext();
            android.widget.LinearLayout box = new android.widget.LinearLayout(c);
            box.setOrientation(android.widget.LinearLayout.VERTICAL);
            box.setBackground(Ui.glass(c, 26, 235));
            box.setPadding(Ui.dp(c, 26), Ui.dp(c, 20), Ui.dp(c, 26), Ui.dp(c, 20));
            TextView title = Ui.label(c, I18n.t(on ? "Controles y manos en pausa" : "Controles y manos activos"), 22, Ui.TEXT, true);
            Ui.leadingIcon(title, on ? "pause" : "hand", 20, Ui.ACCENT);
            title.setCompoundDrawablePadding(Ui.dp(c, 8));
            box.addView(title);
            TextView body = Ui.label(c, I18n.t(on ? "Nada toca las pantallas ni los menús. Para volver: mantené A y B (o X e Y) un segundo, o juntá el pulgar con el meñique derecho."
                                                   : "El láser y las manos vuelven a tocar las pantallas y los menús."), 16, Ui.TEXT2, false);
            body.setPadding(0, Ui.dp(c, 8), 0, 0);
            box.addView(body);
            show(p, box);
            if (!on) new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                Panel q = panels.get(LOCK_NOTE_ID);
                if (q == p) { panels.remove(LOCK_NOTE_ID); release(q); nativeDestroyPanel(LOCK_NOTE_ID); }
            }, 3000);
        });
    }

    /** El portal quedó listo (llega en el hilo de dibujo). */
    void onPortalDone() { runOnUiThread(MenuUi::portalDone); }
    @Override public void setLaserNoTouch(boolean on) { nativeSetLaserNoTouch(on); }
    @Override public void setHandGestures(boolean on) { nativeSetHandGestures(on); }
    @Override public void setHeight(float m) { nativeSetHeight(m); nativeSetVigiaHeight(MenuUi.vigiaHeight(prefs())); }
    @Override public void setHandMove(boolean on) { nativeSetHandMove(on); }
    @Override public void setCableOnly(boolean on) { nativeSetCableOnly(on); }
    /// Gestos de las manos (sala VR y juegos, por mano) → parte nativa
    @Override public void applyGestures() {
        for (int p = 0; p < 2; p++) for (int h = 0; h < 2; h++) nativeSetGestureMap(p, h, GestureUi.load(prefs(), p, h));
    }
    @Override public void recenter() { nativeRecenter(); }
    @Override public void setFrameGen(int mode) { nativeSetFrameGen(mode); }
    @Override public void setLight(float g) { nativeSetLight(g); }
    @Override public void setImage(float b, float c, float s, float sharp, float warm) { nativeSetImage(b, c, s, sharp, warm); }
    @Override public boolean isWindowOpen(int id) { return panels.containsKey(id); }
    private int env = 0;

    /// Al cambiar de ambiente, la barra de la pantalla principal se arma de nuevo (con o sin tamaño y curva)
    @Override public void setEnvironment(int e) {
        nativeSetEnvironment(e);
        if (e == env) return;
        env = e;
        Panel tv = panels.get(TV_ID);
        removeToolbars(TV_ID);
        if (tv != null) { tv.sizeP = -1; tv.curveP = 0; createToolbar(TV_ID, tv); }
    }
    @Override public void onDeskRect(boolean visible, float x0, float y0, float x1, float y1) { nativeDeskRect(visible, x0, y0, x1, y1); }

    @Override
    public void closeAllWindows() {
        for (Integer id : new java.util.ArrayList<>(panels.keySet()))
            if (id != TV_ID && id != HUD_ID && id != WRIST_ID && id != GAME_MENU_ID && !toolbars.containsValue(id) && !toolbars2.containsValue(id)) closeWindow(id);
    }

    /// Cada ventana de la Mac flota aparte en la sala (si ya está abierta, no se duplica)
    @Override
    public void openWindow(int id, String app, String title, int w, int h) {
        if (panels.containsKey(id) || id == TV_ID) return;
        float k = Math.min(1.0f, 1300f / Math.max(w, h));
        int dw = Math.max(400, Math.round(w * k)), dh = Math.max(300, Math.round(h * k));
        Panel p = createPanel(id, dw, dh, 1.6f * dw / 1300f, false);
        if (p == null) return;
        p.win = new WindowUi(p.pres.getContext(), new WindowUi.Host() {
            @Override public boolean inVR() { return true; }
            @Override public void close() { closeWindow(id); }
            @Override public void onVideoRect(float x0, float y0, float x1, float y1) { nativePanelRect(id, x0, y0, x1, y1); }
        }, id, app, title, w, h);
        show(p, p.win.view());
        p.win.start();
        createToolbar(id, p);
    }

    // ------------------------------------------------------------------ indicador de rendimiento
    private Panel hud;
    private TextView hudText;
    private final Runnable hudTick = new Runnable() {
        @Override public void run() {
            if (hud == null) return;
            try {
                org.json.JSONObject o = new org.json.JSONObject(nativeStats());
                org.json.JSONObject st = Link.I.state();
                String game = st != null ? st.optString("gameName", "") : "", mac = st != null ? st.optString("macModel", "Mac") : "Mac";
                if (mac.isEmpty()) mac = "Mac";
                StringBuilder b = new StringBuilder();
                if (o.optBoolean("conectado") && o.optBoolean("enJuego"))
                    b.append(I18n.t("FPS ")).append(game.isEmpty() ? I18n.t("del juego") : game).append(" (").append(mac).append("): ").append(o.optInt("juego")).append(" fps\n");
                b.append(I18n.t("FPS visor (")).append(android.os.Build.MODEL).append("): ").append(o.optInt("pantalla", o.optInt("visor"))).append(" fps");
                if (o.optBoolean("generando")) b.append(I18n.t(" · con cuadros generados"));
                b.append(o.optBoolean("usb") ? I18n.t("\nCable USB  ") : I18n.t("\nWi-Fi  ")).append(String.format(java.util.Locale.US, "%.1f", o.optDouble("mbps"))).append(I18n.t(" Mbps"));
                // Tiempos de cada etapa en la Mac (para saber qué limita y cuánto margen queda)
                org.json.JSONObject pf = st != null ? st.optJSONObject("perf") : null;
                if (o.optBoolean("enJuego") && pf != null && pf.has("hz")) {
                    int hz = Math.max(1, pf.optInt("hz", 90));
                    double budget = 1000.0 / hz, wait = pf.optDouble("espera"), gms = pf.optDouble("juego"), copy = pf.optDouble("copia"),
                            macMs = pf.optDouble("mac"), video = pf.optDouble("video"), gpu = pf.optDouble("gpu", 0);
                    int fps = pf.optInt("fps"), got = pf.optInt("recibidos"), skip = pf.optInt("saltados");
                    b.append(String.format(java.util.Locale.US, I18n.t("\nMac: juego %.1f ms · copia %.1f ms (espera GPU %.1f) · video %.1f ms (de %.1f ms por cuadro)"),
                            gms, copy + macMs, gpu, video, budget));
                    int margin = (int) Math.round(Math.max(0, Math.min(100, wait / budget * 100)));
                    String limit;
                    if (skip > 2) limit = I18n.t("el video (bajá Mbps o nitidez)");
                    else if (fps < hz * 0.93 && wait < 0.5) limit = I18n.t("el juego (la Mac no llega: bajá nitidez o Hz)");
                    else if (got < fps * 0.9) limit = I18n.t("la copia de la imagen");
                    else limit = I18n.t("nada: hay margen para subir la calidad");
                    b.append(String.format(java.util.Locale.US, I18n.t("\nMargen %d%%  ·  limita: %s"), margin, limit));
                }
                if (!o.optBoolean("conectado")) b.append(I18n.t("\nSin conexión con la Mac"));
                else if (o.optBoolean("enJuego")) b.append(I18n.t("   ·   demora  ")).append(o.optInt("lat")).append(I18n.t(" ms   ·   perdidos  ")).append(o.optInt("perdidos"));
                else b.append(I18n.t("\nEscritorio y ventanas  ")).append(o.optInt("escritorio")).append(I18n.t(" fps"));
                hudText.setText(b);
            } catch (Exception ignored) { }
            ui.postDelayed(this, 1000);
        }
    };

    @Override public void setHud(boolean on) {
        if (on == (hud != null)) return;
        if (on) {
            hud = createPanel(HUD_ID, 640, 190, 0.64f, false);   // (más alto: con los tiempos de la Mac)
            if (hud == null) return;
            nativeSetHud(HUD_ID);
            Context c = hud.pres.getContext();
            android.widget.FrameLayout root = new android.widget.FrameLayout(c);
            root.setBackgroundColor(Color.TRANSPARENT);
            hudText = Ui.label(c, I18n.t("Midiendo…"), 16, Ui.TEXT, true);
            hudText.setLineSpacing(0, 1.2f);
            hudText.setPadding(Ui.dp(c, 16), Ui.dp(c, 10), Ui.dp(c, 16), Ui.dp(c, 10));
            hudText.setBackground(Ui.glass(c, 18, 200));
            root.addView(hudText, new android.widget.FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            hud.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            show(hud, root);
            ui.post(hudTick);
        } else {
            ui.removeCallbacks(hudTick);
            panels.remove(HUD_ID);
            release(hud);
            nativeDestroyPanel(HUD_ID);
            hud = null; hudText = null;
        }
        for (Panel p : panels.values()) if (p.hudPaint != null) p.hudPaint.run();
    }

    // ------------------------------------------------------------------ barra de botones
    /// La barra cierra sola su panel (3D o tamaño) unos segundos después de que dejás de usarlo
    private static final class BarRoot extends android.widget.FrameLayout {
        Runnable onUse;
        BarRoot(Context c) { super(c); }
        @Override public boolean dispatchTouchEvent(MotionEvent e) { if (onUse != null) onUse.run(); return super.dispatchTouchEvent(e); }
        @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { if (onUse != null) onUse.run(); return super.dispatchGenericMotionEvent(e); }
    }

    private Runnable tv3dPaint;   // repinta el botón 3D de la pantalla principal si algo cambia desde el menú

    /// Barra de botones por fuera de la ventana (arriba a la izquierda), al estilo de visionOS:
    /// menú (pantalla principal), 3D, tamaño, curvar, rendimiento (pantalla principal), traer adelante y cerrar.
    /// Arriba de la barra (transparente) aparece el panel del 3D o del tamaño:
    ///  · tocar 3D o tamaño lo abre (o cierra);
    ///  · mantenerlos apretados y mover el control a los costados ajusta directo, y al soltar se cierra.
    /// Una barra de botones (panel aparte): fila de botones y, del otro lado, el lugar de los paneles (3D, tamaño) y los avisos
    private final class Bar {
        Panel t;
        BarRoot root;
        android.widget.FrameLayout pop;
        android.widget.LinearLayout row;
        TextView flash;
        java.util.function.Consumer<String> say, tip;
        Runnable closePop;
        Context c;
        /// below: la barra va debajo de la pantalla (los paneles se abren hacia abajo, a la derecha)
        Bar(int tid, int parentId, int nb, boolean below) {
            final int popH = 128;
            int dpW = nb * 50 + 12, dpH = 58 + 10 + popH;
            t = createPanel(tid, dpW, dpH, 0.40f * dpW / 302f, false);
            if (t == null) return;
            nativeAttachPanel(tid, parentId);
            nativeSetDock(tid, below ? 1 : 0);
            c = t.pres.getContext();
            row = new android.widget.LinearLayout(c);
            row.setOrientation(android.widget.LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            int pad = Ui.dp(c, 6);
            row.setPadding(pad, pad, pad, pad);
            row.setBackground(Ui.glass(c, 999, 225));
            root = new BarRoot(c);
            root.setBackgroundColor(Color.TRANSPARENT);
            pop = new android.widget.FrameLayout(c);
            pop.setVisibility(View.INVISIBLE);
            int side = below ? android.view.Gravity.END : android.view.Gravity.START;
            root.addView(pop, new android.widget.FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(c, popH), below ? android.view.Gravity.BOTTOM : android.view.Gravity.TOP));
            root.addView(row, new android.widget.FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, Ui.dp(c, 58), (below ? android.view.Gravity.TOP : android.view.Gravity.BOTTOM) | side));
            closePop = () -> pop.setVisibility(View.INVISIBLE);
            flash = Ui.label(c, "", 17, Ui.TEXT, true);
            flash.setPadding(Ui.dp(c, 18), Ui.dp(c, 10), Ui.dp(c, 18), Ui.dp(c, 10));
            flash.setBackground(Ui.glass(c, 999, 235));
            Ui.leadingIcon(flash, "dot", 16, Ui.ACCENT);
            flash.setCompoundDrawablePadding(Ui.dp(c, 6));
            flash.setVisibility(View.GONE);
            pop.addView(flash, new android.widget.FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                    (below ? android.view.Gravity.TOP : android.view.Gravity.BOTTOM) | side));
            final Runnable hideFlash = () -> { if (flash.getVisibility() == View.VISIBLE) pop.setVisibility(View.INVISIBLE); };
            say = msg -> {
                for (int i = 0; i < pop.getChildCount(); i++) pop.getChildAt(i).setVisibility(View.GONE);
                flash.setText(msg);
                flash.setVisibility(View.VISIBLE);
                pop.setVisibility(View.VISIBLE);
                root.removeCallbacks(hideFlash);
                root.postDelayed(hideFlash, 1800);
            };
            // (el nombre del botón no tapa un panel abierto, como el del tamaño o el 3D)
            tip = name -> {
                boolean other = pop.getVisibility() == View.VISIBLE && flash.getVisibility() != View.VISIBLE;
                if (!other) say.accept(name);
            };
            root.onUse = () -> { root.removeCallbacks(closePop); if (pop.getVisibility() == View.VISIBLE) root.postDelayed(closePop, 6000); };
        }
        /// Un botón con ícono; apuntándole un segundo aparece su nombre
        TextView btn(String icon, Runnable action) {
            TextView b = Ui.circle(c, "");
            Icons.on(b, icon, icon.equals("menu") ? 30 : 22);   // (el del menú, más grande)
            b.setBackground(Ui.hoverable(c, Color.argb(30, 255, 255, 255), 12, false));
            if (action != null) b.setOnClickListener(v -> action.run());
            final String name = toolName(icon);
            final Runnable show = () -> { if (name != null) tip.accept(name); };
            b.setOnHoverListener((v, e) -> {
                if (e.getAction() == MotionEvent.ACTION_HOVER_ENTER) v.postDelayed(show, 1000);
                else if (e.getAction() == MotionEvent.ACTION_HOVER_EXIT) v.removeCallbacks(show);
                return false;
            });
            android.widget.LinearLayout.LayoutParams lp = new android.widget.LinearLayout.LayoutParams(Ui.dp(c, 46), Ui.dp(c, 46));
            lp.rightMargin = Ui.dp(c, 4);
            row.addView(b, lp);
            return b;
        }
        void show() {
            t.pres.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            VrActivity.this.show(t, root);
        }
    }

    /// Las dos barras de una pantalla: se quitan juntas
    private final Map<Integer, Integer> toolbars2 = new HashMap<>();   // ventana -> su barra de abajo
    private void removeToolbars(int id) {
        for (Map<Integer, Integer> m : java.util.Arrays.asList(toolbars, toolbars2)) {
            Integer old = m.remove(id);
            if (old != null) { Panel ob = panels.remove(old); if (ob != null) release(ob); nativeDestroyPanel(old); }
        }
    }
    /// El láser no toca estas pantallas (se apuntan igual sus botones). Al abrir la app, todas se tocan.
    private final java.util.Set<Integer> noTouch = new java.util.HashSet<>();

    /// Botones de cada pantalla, en dos barras por fuera de ella:
    ///  · arriba a la izquierda, lo de la ventana: menú, teclado, láser, fijar a la vista, traer adelante, cerrar;
    ///  · abajo a la derecha, la imagen: 3D, tamaño, curva, filtro, cambiar de monitor, agregar monitor, rendimiento, ajustes.
    /// Los paneles del 3D y del tamaño se abren al lado de su barra: tocar abre o cierra; mantener y mover ajusta directo.
    private void createToolbar(int id, Panel owner) {
        final boolean tv = id == TV_ID;
        removeToolbars(id);   // nunca dos barras para la misma pantalla
        // La pantalla principal en un ambiente con pantalla fija (living, cine) no cambia de tamaño, ni se curva, ni se mueve.
        final boolean fixed = tv && env != 1 && env != 3;
        int tidTop = nextToolbarId++, tidBot = nextToolbarId++;   // (positivos: la parte nativa usa -1 como "ninguno")
        Bar top = new Bar(tidTop, id, 7, false);
        if (top.t == null) return;
        toolbars.put(id, tidTop);
        Bar bot = new Bar(tidBot, id, 8, true);
        if (bot.t == null) { removeToolbars(id); return; }
        toolbars2.put(id, tidBot);

        // ---- arriba: la ventana
        top.btn("menu", () -> { MenuUi m = mainMenu(); if (m != null) m.togglePanel(); });   // el menú está en la pantalla principal
        top.btn("keyboard", () -> { toggleKeyboardFor(id); top.say.accept(panels.containsKey(KEYBOARD_ID) ? I18n.t("Teclado abierto") : I18n.t("Teclado cerrado")); });
        final TextView[] bl = new TextView[1];
        final Runnable paintLaser = () -> {
            boolean off = noTouch.contains(id);
            bl[0].setBackground(Ui.hoverable(top.c, off ? Ui.ACCENT : Color.argb(30, 255, 255, 255), 12, false));
            bl[0].setForeground(Icons.of(top.c, off ? "laserOff" : "laser", 22, off ? Ui.ACCENT_DARK : Ui.TEXT));
        };
        bl[0] = top.btn("laser", () -> {
            boolean off = !noTouch.contains(id);
            if (off) noTouch.add(id); else noTouch.remove(id);
            nativeSetPanelNoTouch(id, off);
            paintLaser.run();
            top.say.accept(off ? I18n.t("El láser no toca esta pantalla (sus botones sí)") : I18n.t("El láser toca esta pantalla"));
        });
        paintLaser.run();
        nativeSetPanelNoTouch(id, noTouch.contains(id));
        // Pausar controles y manos (para mirar algo y comer o usar el celular): el aviso dice cómo volver
        top.btn("pause", VrActivity::nativeToggleInputLock);
        // Fijar a la vista: la ventana te sigue suavemente cuando girás la cabeza
        final TextView[] bf = new TextView[1];
        final Runnable paintFollow = () -> {
            boolean on = following.contains(id);
            bf[0].setBackground(Ui.hoverable(top.c, on ? Ui.ACCENT : Color.argb(30, 255, 255, 255), 12, false));
            bf[0].setForeground(Icons.of(top.c, on ? "lock" : "unlock", 22, on ? Ui.ACCENT_DARK : Ui.TEXT));
        };
        bf[0] = top.btn("unlock", () -> {
            boolean on = !following.contains(id);
            if (on) following.add(id); else following.remove(id);
            nativeSetFollow(id, on);
            paintFollow.run();
            top.say.accept(on ? I18n.t("Fijada a tu vista: te sigue cuando girás") : I18n.t("Suelta: se queda en su lugar"));
        });
        paintFollow.run();
        top.btn("recenter", () -> nativeResetPanel(id));
        TextView bx = top.btn("close", null);
        if (tv) grey(bx, () -> top.say.accept(I18n.t("La pantalla principal no se cierra")));
        else bx.setOnClickListener(v -> closeWindow(id));

        // ---- abajo: la imagen
        Context c = bot.c;
        android.widget.FrameLayout pop = bot.pop;
        BarRoot root = bot.root;
        // panel del 3D: modo + intensidad
        android.widget.LinearLayout p3 = popBox(c);
        android.widget.LinearLayout chips = new android.widget.LinearLayout(c);
        chips.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        String[] modes = {I18n.t("Apagado"), I18n.t("IA"), "SBS", I18n.t("Arriba/abajo")};
        TextView[] chip = new TextView[4];
        android.widget.SeekBar str = seek(c);
        TextView strLabel = Ui.label(c, "", 14, Ui.TEXT, true);
        Runnable[] paint3D = new Runnable[1];
        for (int i = 0; i < 4; i++) {
            final int m = i;
            chip[i] = Ui.pill(c, modes[i]);
            chip[i].setTextSize(14);
            chip[i].setPadding(Ui.dp(c, 8), Ui.dp(c, 6), Ui.dp(c, 8), Ui.dp(c, 6));
            chip[i].setOnClickListener(v -> { setMode3D(id, owner, m); paint3D[0].run(); });
            android.widget.LinearLayout.LayoutParams lp = new android.widget.LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            lp.rightMargin = Ui.dp(c, 4);
            chips.addView(chip[i], lp);
        }
        p3.addView(chips);
        View strRowV = seekRow(c, strLabel, str);
        p3.addView(strRowV);
        str.setOnSeekBarChangeListener(seekListener(p -> { setStrength3D(id, owner, p); paint3D[0].run(); }));
        // Ampliar (SBS y arriba/abajo): las barras negras del video se sacan solas; esto lo agranda (se pasa de los bordes)
        android.widget.SeekBar zm = seek(c);
        TextView zmLabel = Ui.label(c, "", 14, Ui.TEXT, true);
        View zmRowV = seekRow(c, zmLabel, zm);
        p3.addView(zmRowV);
        zm.setOnSeekBarChangeListener(seekListener(p -> { owner.zoomP = p; nativeSetZoom(id, 1f + 3f * p / 100f); paint3D[0].run(); }));
        pop.addView(p3, new android.widget.FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, android.view.Gravity.TOP));
        // paneles de tamaño y curva: control en %, de a poco (arrastrar es relativo y suave; − / + de a un paso)
        Object[] sz = stepPanel(c, pop, I18n.t("Tamaño"), () -> sizePos(owner), v -> setSize(id, owner, v), 1, 12f);
        Object[] cv = stepPanel(c, pop, I18n.t("Curva"), () -> owner.curveP, v -> setCurve(id, owner, v), 2, 30f);
        for (Object[] o : new Object[][]{sz, cv}) {   // (se abren hacia abajo, pegados a la barra)
            View v = (View) o[0];
            if (v.getLayoutParams() instanceof android.widget.FrameLayout.LayoutParams) ((android.widget.FrameLayout.LayoutParams) v.getLayoutParams()).gravity = android.view.Gravity.TOP;
        }
        TextView b3 = bot.btn("cube", null);
        paint3D[0] = () -> {
            int mode = mode3D(id, owner);
            // prendido: el botón se llena de ámbar y el ícono pasa a oscuro
            b3.setForeground(Icons.of(c, "cube", 22, mode > 0 ? Ui.ACCENT_DARK : Ui.TEXT));
            b3.setBackground(Ui.hoverable(c, mode > 0 ? Ui.ACCENT : Color.argb(30, 255, 255, 255), 12, false));
            for (int i = 0; i < 4; i++) {
                chip[i].setBackground(Ui.hoverable(c, i == mode ? Ui.ACCENT_SEG : 0, 999, false));
                chip[i].setTextColor(i == mode ? Ui.ACCENT_TEXT : Ui.TEXT2);
            }
            int sv = strength3D(id, owner);
            if (str.getProgress() != sv) str.setProgress(sv);
            // la intensidad es solo para la IA: en SBS y arriba/abajo (3D real) no aparece, para no confundir
            strRowV.setVisibility(mode == 1 ? View.VISIBLE : View.GONE);
            strLabel.setText(I18n.t("Intensidad  ") + sv + "%");
            zmRowV.setVisibility(mode >= 2 ? View.VISIBLE : View.GONE);
            if (zm.getProgress() != owner.zoomP) zm.setProgress(owner.zoomP);
            zmLabel.setText(I18n.t("Ampliar  ×") + String.format(java.util.Locale.US, "%.1f", 1f + 3f * owner.zoomP / 100f));
        };
        paint3D[0].run();
        if (tv) tv3dPaint = paint3D[0];
        holdButton(b3, root, pop, p3, null, str, 0f,
                () -> { if (mode3D(id, owner) == 0) setMode3D(id, owner, 1); },   // mantener = intensidad de la IA (apagado → IA)
                p -> { if (mode3D(id, owner) == 1) setStrength3D(id, owner, p); }, paint3D[0]);
        // Tamaño y curva (en la pantalla principal de un ambiente fijo, en gris)
        TextView bs = bot.btn("size", null), bc = bot.btn("curve", null);
        if (fixed) {
            grey(bs, () -> bot.say.accept(I18n.t("Esta pantalla tiene tamaño fijo")));
            grey(bc, () -> bot.say.accept(I18n.t("Esta pantalla no se puede curvar")));
        } else {
            holdButton(bs, root, pop, (View) sz[0], null, (android.widget.SeekBar) sz[1], 12f, null, v -> setSize(id, owner, v), (Runnable) sz[2]);
            holdButton(bc, root, pop, (View) cv[0], null, (android.widget.SeekBar) cv[1], 30f, null, v -> setCurve(id, owner, v), (Runnable) cv[2]);
        }
        bot.btn("filter", () -> { MenuUi m = mainMenu(); if (m != null) bot.say.accept(m.nextImageFromBar()); });
        // Cambiar de monitor (pantalla principal y ventanas de pantalla): principal → otros monitores → monitor virtual (si
        // está abierto) → principal. Apuntándole un segundo dice a cuál cambiaría.
        final boolean isDisp = tv || ((id & 0x40000000) != 0 && id != WindowUi.MONITOR_ID);
        TextView bm = bot.btn("swapDisplay", null);
        if (!isDisp) grey(bm, () -> bot.say.accept(I18n.t("Solo en las pantallas de la Mac")));
        else {
            bm.setOnClickListener(v -> { String[] n = nextDisplay(id, tv); if (n == null) { bot.say.accept(I18n.t("No hay otro monitor")); return; } switchDisplay(id, tv, Integer.parseInt(n[0])); bot.say.accept(I18n.t("Ahora: ") + n[1]); });
            final Runnable showNext = () -> { String[] n = nextDisplay(id, tv); bot.tip.accept(n == null ? I18n.t("No hay otro monitor") : I18n.t("Cambiar a: ") + n[1]); };
            bm.setOnHoverListener((v, e) -> {
                if (e.getAction() == MotionEvent.ACTION_HOVER_ENTER) v.postDelayed(showNext, 700);
                else if (e.getAction() == MotionEvent.ACTION_HOVER_EXIT) v.removeCallbacks(showNext);
                return false;
            });
        }
        bot.btn("addDisplay", () -> { MenuUi m = mainMenu(); if (m != null) bot.say.accept(m.addMonitorFromBar()); });
        TextView bh = bot.btn("stats", () -> {
            MenuUi m = mainMenu();
            if (m != null) m.setHudFromBar(hud == null);
            bot.say.accept(hud != null ? I18n.t("Rendimiento: visible (arriba a la izquierda)") : I18n.t("Rendimiento: oculto"));
        });
        owner.hudPaint = () -> {
            bh.setBackground(Ui.hoverable(c, hud != null ? Ui.ACCENT : Color.argb(30, 255, 255, 255), 12, false));
            bh.setForeground(Icons.of(c, "stats", 22, hud != null ? Ui.ACCENT_DARK : Ui.TEXT));
        };
        owner.hudPaint.run();
        // Ajustes (el último): abre el menú en «Escritorio y ventanas»
        bot.btn("sliders", () -> { MenuUi m = mainMenu(); if (m != null) m.openSettings("desk"); });

        top.show();
        bot.show();
    }

    /// Pantalla que se ve ahora en cada ventana de pantalla (cuando se cambió con el botón): ventana → pantalla
    private final java.util.Map<Integer, Integer> dispShown = new java.util.HashMap<>();
    /// Las pantallas de la Mac en orden: principal, otros monitores, monitor virtual (si está abierto). {id, nombre}
    private java.util.List<String[]> displayCycle() {
        java.util.List<String[]> out = new java.util.ArrayList<>();
        org.json.JSONObject st = Link.I.state();
        if (st == null) return out;
        org.json.JSONObject md = st.optJSONObject("mainDisplay");
        if (md != null) out.add(new String[]{String.valueOf(md.optLong("id")), I18n.t("Pantalla principal")});
        org.json.JSONArray ds = st.optJSONArray("displays");
        for (int i = 0; ds != null && i < ds.length(); i++) { org.json.JSONObject d = ds.optJSONObject(i); out.add(new String[]{String.valueOf(d.optLong("id")), d.optString("name", I18n.t("Monitor"))}); }
        long mon = st.optLong("monitorId", 0);
        if (mon != 0) out.add(new String[]{String.valueOf(mon), I18n.t("Monitor virtual")});
        return out;
    }
    /// A cuál cambiaría esa pantalla (null si no hay otra)
    private String[] nextDisplay(int id, boolean tv) {
        java.util.List<String[]> l = displayCycle();
        if (l.size() < 2) return null;
        org.json.JSONObject st = Link.I.state();
        long cur = tv ? (st != null ? st.optLong("deskDisplay", 0) : 0) : dispShown.getOrDefault(id, id) & 0xFFFFFFFFL;
        int at = -1;
        for (int i = 0; i < l.size(); i++) if (Long.parseLong(l.get(i)[0]) == cur) at = i;
        return l.get((at + 1) % l.size());
    }
    private void switchDisplay(int id, boolean tv, int to) {
        if (tv) Link.I.send("{\"type\":\"deskdisp\",\"id\":" + (to & 0xFFFFFFFFL) + "}");
        else { dispShown.put(id, to); Link.I.send("{\"type\":\"dispswap\",\"id\":" + (id & 0xFFFFFFFFL) + ",\"to\":" + (to & 0xFFFFFFFFL) + "}"); }
    }

    /// Nombre de lo que hace cada botón de la barra de una pantalla
    private static String toolName(String icon) {
        switch (icon) {
            case "menu": return I18n.t("Menú");
            case "cube": return I18n.t("3D");
            case "size": return I18n.t("Tamaño");
            case "curve": return I18n.t("Curvar");
            case "filter": return I18n.t("Filtro de imagen");
            case "addDisplay": return I18n.t("Agregar monitor");
            case "stats": return I18n.t("Indicador de rendimiento");
            case "unlock": case "lock": return I18n.t("Fijar a tu vista");
            case "keyboard": return I18n.t("Teclado");
            case "laser": return I18n.t("El láser toca la pantalla");
            case "pause": return I18n.t("Pausar controles y manos");
            case "sliders": return I18n.t("Ajustes de pantallas");
            case "swapDisplay": return I18n.t("Cambiar de monitor");
            case "recenter": return I18n.t("Traer adelante");
            case "close": return I18n.t("Cerrar");
            default: return null;
        }
    }

    /// Botón que abre su panel al tocarlo, o ajusta directo mientras se mantiene apretado (y al soltar se cierra)
    private void holdButton(TextView b, BarRoot root, android.widget.FrameLayout pop, View mine, View other,
                            android.widget.SeekBar rail, float gain, Runnable onHold, java.util.function.IntConsumer drag, Runnable paint) {
        final float[] down = new float[1];
        final int[] start = new int[1];
        final boolean[] holding = {false}, moved = {false}, wasOpen = {false};
        final Runnable startHold = () -> {
            holding[0] = true;
            if (onHold != null) onHold.run();
            for (int i = 0; i < pop.getChildCount(); i++) pop.getChildAt(i).setVisibility(View.GONE);
            mine.setVisibility(View.VISIBLE);
            pop.setVisibility(View.VISIBLE);
            paint.run();
        };
        b.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    holding[0] = false; moved[0] = false; down[0] = e.getX(); start[0] = -1;
                    wasOpen[0] = pop.getVisibility() == View.VISIBLE && mine.getVisibility() == View.VISIBLE;
                    v.setPressed(true);
                    v.postDelayed(startHold, 350);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (!holding[0]) return true;
                    if (!moved[0] && Math.abs(e.getX() - down[0]) < Ui.dp(v.getContext(), 8)) return true;
                    moved[0] = true;
                    // posición del control sobre el riel del panel (el ancho de la barra hace de riel)
                    int[] bl = new int[2], sl = new int[2];
                    if (gain > 0) {
                        // de a poco: todo el ancho del riel = «gain» % (no salta)
                        if (start[0] < 0) start[0] = rail.getProgress();
                        int p = Math.round(start[0] + (e.getX() - down[0]) / Math.max(1, rail.getWidth()) * gain);
                        drag.accept(Math.max(0, Math.min(100, p)));
                    } else {
                        v.getLocationInWindow(bl); rail.getLocationInWindow(sl);
                        float x = bl[0] + e.getX() - sl[0];
                        drag.accept(Math.round(Math.max(0f, Math.min(1f, x / Math.max(1, rail.getWidth()))) * 100));
                    }
                    paint.run();
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    v.removeCallbacks(startHold);
                    v.setPressed(false);
                    if (holding[0]) { if (!wasOpen[0]) pop.setVisibility(View.INVISIBLE); }
                    else if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                        // tocar: abrir o cerrar su panel
                        if (wasOpen[0]) pop.setVisibility(View.INVISIBLE);
                        else {
                            for (int i = 0; i < pop.getChildCount(); i++) pop.getChildAt(i).setVisibility(View.GONE);
                            mine.setVisibility(View.VISIBLE); pop.setVisibility(View.VISIBLE); paint.run();
                        }
                    }
                    holding[0] = false;
                    root.onUse.run();
                    return true;
            }
            return false;
        });
    }

    private static android.widget.LinearLayout popBox(Context c) {
        android.widget.LinearLayout l = new android.widget.LinearLayout(c);
        l.setOrientation(android.widget.LinearLayout.VERTICAL);
        l.setPadding(Ui.dp(c, 14), Ui.dp(c, 10), Ui.dp(c, 14), Ui.dp(c, 8));
        l.setBackground(Ui.glass(c, 26, 235));
        l.setVisibility(View.GONE);
        return l;
    }

    private static android.widget.SeekBar seek(Context c) {
        android.widget.SeekBar sb = new android.widget.SeekBar(c);
        sb.setMax(100);
        Ui.tint(sb);
        return sb;
    }

    private static View seekRow(Context c, TextView label, android.widget.SeekBar sb) {
        android.widget.LinearLayout r = new android.widget.LinearLayout(c);
        r.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        r.setGravity(android.view.Gravity.CENTER_VERTICAL);
        r.setPadding(0, Ui.dp(c, 6), 0, 0);
        r.addView(label, new android.widget.LinearLayout.LayoutParams(Ui.dp(c, 150), ViewGroup.LayoutParams.WRAP_CONTENT));
        r.addView(sb, new android.widget.LinearLayout.LayoutParams(0, Ui.dp(c, 40), 1f));
        return r;
    }

    private static android.widget.SeekBar.OnSeekBarChangeListener seekListener(java.util.function.IntConsumer onUser) {
        return new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar b, int p, boolean user) { if (user) onUser.accept(p); }
            @Override public void onStartTrackingTouch(android.widget.SeekBar b) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar b) { }
        };
    }

    // ------------------------------------------------------------------ 3D y tamaño de cada ventana
    private android.content.SharedPreferences prefs() { return getSharedPreferences("qvb", Context.MODE_PRIVATE); }

    private int mode3D(int id, Panel p) {
        if (id == TV_ID) return p.menu != null ? p.menu.mode3D() : 0;
        return p.d3Mode;
    }

    private int strength3D(int id, Panel p) {
        int def = prefs().getInt("3dStrength", 30);
        return id == TV_ID || p.d3Str < 0 ? def : p.d3Str;
    }

    private void setMode3D(int id, Panel p, int mode) {
        if (id == TV_ID) { if (p.menu != null) p.menu.set3DModeFromBar(mode); return; }
        p.d3Mode = mode;
        if (p.win != null) p.win.set3D(mode == 1);   // solo la IA necesita que la Mac calcule la profundidad
        nativeSetPanel3D(id, mode, strength3D(id, p) / 100f * MAX_3D);
    }

    private void setStrength3D(int id, Panel p, int s) {
        if (id == TV_ID) { if (p.menu != null) p.menu.set3DStrengthFromBar(s); return; }
        p.d3Str = s;
        nativeSetPanel3D(id, p.d3Mode, s / 100f * MAX_3D);
    }

    /// Tamaño en %: 0% = ×0,5 · 25% = tamaño original · 100% = ×2,5 (parejo en todo el recorrido)
    private static float sizeFactor(int p) { return 0.5f + 2f * p / 100f; }
    private static int sizePos(Panel p) { return p.sizeP >= 0 ? p.sizeP : 25; }

    private void setCurve(int id, Panel p, int pos) {
        pos = Math.max(0, Math.min(100, pos));
        p.curveP = pos;
        nativeSetCurve(id, pos / 100f);
    }

    private MenuUi mainMenu() { Panel tv = panels.get(TV_ID); return tv == null ? null : tv.menu; }

    /// Botón que no se puede usar en esta pantalla: gris, y al tocarlo explica por qué
    private static void grey(TextView b, Runnable why) {
        b.setAlpha(0.28f);
        b.setOnClickListener(v -> why.run());
    }

    /// Panel con un control en % que se mueve de a poco: arrastrar es relativo (todo el ancho = 30%), − / + de a «step».
    /// Devuelve {panel, control, repintar}.
    private Object[] stepPanel(Context c, android.widget.FrameLayout pop, String name,
                               java.util.function.IntSupplier get, java.util.function.IntConsumer set, int step, float gain) {
        android.widget.LinearLayout box = popBox(c);
        android.widget.SeekBar rail = seek(c);
        TextView label = Ui.label(c, "", 14, Ui.TEXT, true);
        android.widget.LinearLayout row = (android.widget.LinearLayout) seekRow(c, label, rail);
        box.addView(row);
        Runnable paint = () -> {
            int p = get.getAsInt();
            if (rail.getProgress() != p) rail.setProgress(p);
            label.setText(name + "  " + p + "%");
        };
        final float[] sx = new float[1];
        final int[] sp = new int[1];
        rail.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: sx[0] = e.getX(); sp[0] = get.getAsInt(); return true;
                case MotionEvent.ACTION_MOVE: {
                    int p = Math.max(0, Math.min(100, Math.round(sp[0] + (e.getX() - sx[0]) / Math.max(1, v.getWidth()) * gain)));
                    if (p != get.getAsInt()) { set.accept(p); paint.run(); }
                    return true;
                }
                default: return true;
            }
        });
        TextView less = Ui.circle(c, ""), more = Ui.circle(c, "");
        Icons.on(less, "minus", 18); Icons.on(more, "plus", 18);
        for (TextView b : new TextView[]{less, more}) {
            b.setBackground(Ui.hoverable(c, Color.argb(30, 255, 255, 255), 12, false));
        }
        less.setOnClickListener(v -> { set.accept(Math.max(0, get.getAsInt() - step)); paint.run(); });
        more.setOnClickListener(v -> { set.accept(Math.min(100, get.getAsInt() + step)); paint.run(); });
        row.addView(less, 1, new android.widget.LinearLayout.LayoutParams(Ui.dp(c, 40), Ui.dp(c, 40)));
        row.addView(more, new android.widget.LinearLayout.LayoutParams(Ui.dp(c, 40), Ui.dp(c, 40)));
        pop.addView(box, new android.widget.FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, android.view.Gravity.BOTTOM));
        paint.run();
        return new Object[]{box, rail, paint};
    }

    private void setSize(int id, Panel p, int pos) {
        pos = Math.max(0, Math.min(100, pos));
        p.sizeP = pos;
        nativeSizePanel(id, sizeFactor(pos));
    }

    private void closeWindow(int id) {
        Panel p = panels.remove(id);
        if (p == null) return;
        removeToolbars(id);   // sus barras de botones
        noTouch.remove(id);
        if (p.win != null) p.win.destroyed();
        release(p);
        nativeDestroyPanel(id);
    }
}
