package org.questvrbridge.client;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import static org.questvrbridge.client.Ui.dp;

/**
 * Menú del juego VR (mantener el botón de menú del control izquierdo): seguir jugando, cerrar el juego,
 * abrir ventanas y monitores de la Mac que flotan durante el juego, y algunos ajustes. No es el menú de la sala.
 */
final class GameMenuUi implements Link.Listener {
    interface Host {
        void closeGameMenu();
        void openGameWindow(int id, String app, String title, int w, int h);
        boolean isWindowOpen(int id);
        void setHud(boolean on);
        void setGameHands(boolean on);
        void setHandGestures(boolean on);
        default void setHandAngles(float pitch, float roll) { }
        default void setVigia(boolean on) { }
        default boolean vigia() { return false; }
        default void vigiaCalibrate() { }
        default void applyHeight() { }
        default void applyGestures() { }
        /** «Menú principal»: el juego sigue abierto y se ve la sala (se vuelve desde Juegos). */
        default void toMainMenu() { }
        /** Calibrar las manos (con los controles agarrados): avisa qué manos quedaron (bit 0 izquierda, bit 1 derecha; 8 = no se pudo). */
        default void handCalibrate(java.util.function.IntConsumer done) { }
        default void resetHandCal() { }
    }

    private final Context ctx;
    private final Host host;
    private LinearLayout list;
    private final FrameLayout root;
    private boolean confirmQuit = false;
    private String drawnKey = "";

    private final LinearLayout left, right, head;
    private boolean showGestures = false;
    // Imagen del juego (se guarda en la Mac): lo elegido acá hasta tocar «Aplicar»
    private static final int[] EYES = {1024, 1280, 1536, 1832, 2048, 2304, 2560}, FPSS = {72, 90, 120}, MBPSS = {40, 70, 100, 150, 200};
    private static final int[][] PROFILES = {{0, 0, 0}, {0, 1, 1}, {1, 1, 2}};
    private int selEye = -1, selFps = -1, selMbps = -1, selFov = 0, selFovC = 1;
    private boolean showDetail = false, applied = false;
    private int qAll = -1;                 // la imagen se cambia para: 1 = todos los juegos · 0 = este juego (-1 = según tenga la suya)
    private boolean screensPage = false;   // página «Ventanas y pantallas»
    private boolean advPage = false;       // página «Avanzado» (de este juego)
    private boolean advAll = false;        // en «Avanzado»: configurando todos los juegos en vez de este
    private boolean handsPage = false;     // página «Manos y láser»
    private boolean gesturesPage = false;  // página «Gestos de las manos»
    private int gestureProfile = 1;        // (en el menú del juego empieza en «Juegos»)
    private boolean showVigia = false;     // instrucciones del Modo Centinela desplegadas
    private String calMsg = null;          // calibración de las manos: lo que se muestra mientras dura

    GameMenuUi(Context c, Host h) {
        ctx = c; host = h;
        root = new FrameLayout(c);
        root.setBackgroundColor(Color.TRANSPARENT);
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(Ui.glass(c, 28, 238));
        card.setPadding(dp(c, 30), dp(c, 24), dp(c, 30), dp(c, 20));
        // Arriba: título y las dos acciones principales
        head = new LinearLayout(c);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        card.addView(head, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        // Debajo: dos columnas (pantallas de la Mac · ajustes), cada una con su desplazamiento
        LinearLayout cols = new LinearLayout(c);
        cols.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        clp.topMargin = dp(c, 18);
        card.addView(cols, clp);
        left = new LinearLayout(c); left.setOrientation(LinearLayout.VERTICAL);
        right = new LinearLayout(c); right.setOrientation(LinearLayout.VERTICAL);
        ScrollView ls = new ScrollView(c); ls.addView(left);
        ScrollView rs = new ScrollView(c); rs.addView(right);
        LinearLayout.LayoutParams lp1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        lp2.leftMargin = dp(c, 26);
        cols.addView(ls, lp1);
        cols.addView(rs, lp2);
        TextView foot = Ui.label(c, I18n.t("Para abrir o cerrar este menú: mantené el botón de menú del control izquierdo (o el pellizco del meñique izquierdo). Mientras está abierto, el juego no recibe los botones."), 13, Ui.TEXT3, false);
        foot.setPadding(dp(c, 4), dp(c, 12), 0, 0);
        card.addView(foot);
        list = left;   // (compatibilidad con el resto de la clase)
        root.addView(card, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        render();
    }

    View view() { return root; }
    void start() { Link.I.addListener(this); Tutorials.auto(root, "juego_menu"); }
    void stop() { Link.I.removeListener(this); }

    private SharedPreferences prefs() { return ctx.getSharedPreferences("qvb", Context.MODE_PRIVATE); }

    @Override public void onState(JSONObject o) { render(); }
    @Override public void onConnection(boolean connected, String name) { render(); }

    /// Fila de una pantalla: ícono, nombre, y a la derecha «Abrir» o «Abierta»
    private void screenRow(String icon, String title, String sub, boolean open, Runnable act) { screenRow(left, icon, title, sub, open, act); }
    private void screenRow(LinearLayout into, String icon, String title, String sub, boolean open, Runnable act) {
        LinearLayout r = Ui.row(ctx, icon, title, sub);
        TextView chip = open ? Ui.pill(ctx, I18n.t("Abierta")) : Ui.primary(ctx, I18n.t("Abrir"));
        chip.setTextSize(14);
        chip.setPadding(dp(ctx, 16), dp(ctx, 6), dp(ctx, 16), dp(ctx, 6));
        if (open) chip.setAlpha(0.6f);
        r.addView(chip);
        r.setOnClickListener(v -> { act.run(); drawnKey = ""; r.postDelayed(this::render, 200); });
        chip.setOnClickListener(v -> r.performClick());
        into.addView(r);
    }

    private void render() {
        JSONObject st = Link.I.state();
        String key = (st == null ? "" : String.valueOf(st.optJSONArray("windows")) + st.optJSONArray("displays") + st.optJSONObject("mainDisplay")) + confirmQuit + showGestures + screensPage + advPage + (st == null ? "" : String.valueOf(st.optJSONObject("adv"))) + handsPage + advAll + gesturesPage + gestureProfile + showVigia + host.vigia()
                + prefs().getBoolean("gameHands", false) + prefs().getBoolean("handGestures", true) + prefs().getBoolean("hud", false) + prefs().getInt("handPitchOffL", 0) + prefs().getInt("handRollOffL", 0) + prefs().getInt("handPitchOffR", 0) + prefs().getInt("handRollOffR", 0)
                + selEye + selFps + selMbps + selFov + selFovC + showDetail + applied + qAll + (st == null ? "" : String.valueOf(st.optJSONObject("cfg")))
                + host.isWindowOpen(WindowUi.MONITOR_ID) + calMsg;
        if (key.equals(drawnKey)) return;
        drawnKey = key;
        head.removeAllViews(); left.removeAllViews(); right.removeAllViews();

        // Arriba
        if (screensPage || handsPage || gesturesPage || advPage) {
            TextView back = Ui.pill(ctx, I18n.t("‹ Volver"));
            back.setOnClickListener(v -> { if (gesturesPage) { gesturesPage = false; handsPage = true; } else { screensPage = false; handsPage = false; advPage = false; } drawnKey = ""; render(); slide(-1); });
            head.addView(back);
            TextView title = Ui.label(ctx, "  " + I18n.t(advPage ? "Avanzado" : gesturesPage ? "Gestos de las manos" : handsPage ? "Manos y láser" : "Ventanas y pantallas"), 28, Ui.TEXT, true);
            head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        } else {
            TextView title = Ui.label(ctx, I18n.t("Juego en curso"), 28, Ui.TEXT, true);
            head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            TextView quit = Ui.pill(ctx, confirmQuit ? I18n.t("Tocá de nuevo para cerrar") : I18n.t("Cerrar el juego"));
            if (confirmQuit) quit.setTextColor(Ui.ACCENT_TEXT);
            quit.setOnClickListener(v -> {
                if (!confirmQuit) { confirmQuit = true; render(); quit.postDelayed(() -> { confirmQuit = false; drawnKey = ""; render(); }, 4000); return; }
                confirmQuit = false;
                Link.I.send("{\"type\":\"quitgame\"}");
                host.closeGameMenu();
            });
            head.addView(quit);
            TextView main = Ui.pill(ctx, I18n.t("Menú principal"));
            main.setOnClickListener(v -> host.toMainMenu());
            LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            mlp.leftMargin = dp(ctx, 12);
            head.addView(main, mlp);
        }
        TextView help = Ui.circle(ctx, "");   // Ayuda: todos los tutoriales
        Icons.on(help, "help", 24);
        help.setOnClickListener(v -> Tutorials.showHelp(root));
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(dp(ctx, 46), dp(ctx, 46));
        hlp.leftMargin = dp(ctx, 12);
        head.addView(help, hlp);
        TextView resume = Ui.primary(ctx, I18n.t("Seguir jugando"));
        resume.setOnClickListener(v -> host.closeGameMenu());
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.leftMargin = dp(ctx, 12);
        head.addView(resume, rlp);

        if (screensPage) { renderScreens(st); return; }
        if (advPage) { renderAdvanced(st); return; }
        if (gesturesPage) { renderGestures(); return; }
        if (handsPage) { renderHands(); return; }

        // Izquierda: ventanas y pantallas (en su propia página) y ajustes
        list = left;
        int open = 0, total = 0;
        if (st != null) {
            JSONObject md = st.optJSONObject("mainDisplay");
            if (md != null) { total++; if (host.isWindowOpen(md.optInt("id"))) open++; }
            JSONArray ds = st.optJSONArray("displays"), ws = st.optJSONArray("windows");
            for (int i = 0; ds != null && i < ds.length(); i++) { total++; if (host.isWindowOpen(ds.optJSONObject(i).optInt("id"))) open++; }
            for (int i = 0; ws != null && i < ws.length(); i++) { total++; if (host.isWindowOpen(ws.optJSONObject(i).optInt("id"))) open++; }
        }
        if (host.isWindowOpen(WindowUi.MONITOR_ID)) open++;
        left.addView(Ui.section(ctx, I18n.t("Tu Mac")));
        LinearLayout nav = Ui.row(ctx, "window", I18n.t("Ventanas y pantallas"),
                st == null ? I18n.t("Sin conexión con la Mac") : open > 0 ? I18n.t("Abiertas: ") + open : I18n.t("Abrí tu pantalla o una ventana encima del juego"));
        nav.addView(Ui.label(ctx, "›", 26, Ui.TEXT2, false));
        nav.setOnClickListener(v -> { screensPage = true; drawnKey = ""; render(); slide(1); });
        left.addView(nav);
        TextView hint = Ui.label(ctx, I18n.t("Quedan flotando mientras jugás. Para moverlas, arrastralas de la barrita con este menú abierto."), 13, Ui.TEXT2, false);
        hint.setPadding(dp(ctx, 4), dp(ctx, 6), 0, 0);
        left.addView(hint);

        left.addView(Ui.section(ctx, I18n.t("Ajustes")));
        LinearLayout hn = Ui.row(ctx, "hand", I18n.t("Manos y láser"),
                prefs().getBoolean("handGestures", true) ? I18n.t("Jugar con las manos · ángulo de las manos del juego") : I18n.t("Jugar con las manos: apagado"));
        hn.addView(Ui.label(ctx, "›", 26, Ui.TEXT2, false));
        hn.setOnClickListener(v -> { handsPage = true; drawnKey = ""; render(); slide(1); });
        left.addView(hn);
        toggle(I18n.t("Indicador de rendimiento"), I18n.t("Cuadros por segundo, Wi-Fi y demora"),
                prefs().getBoolean("hud", false), on -> { prefs().edit().putBoolean("hud", on).apply(); host.setHud(on); });

        // Tu altura (la misma que en Ajustes de la sala): se aplica al instante
        left.addView(Ui.section(ctx, I18n.t("Tu altura")));
        {
            android.content.SharedPreferences pr = prefs();
            boolean standing = pr.getInt("posture", pr.getFloat("heightM", 1.75f) <= 0.5f ? 1 : 0) == 1;
            boolean real = standing && pr.getBoolean("standReal", true);
            float cur = pr.getFloat("heightM", 1.75f);
            pills(new String[]{I18n.t("Sentado"), I18n.t("De pie")}, standing ? 1 : 0,
                    i -> { pr.edit().putInt("posture", i).apply(); host.applyHeight(); });
            String[] names = new String[MenuUi.HEIGHTS.length + (standing ? 1 : 0)];
            int off = standing ? 1 : 0, sel = real ? 0 : -1;
            if (standing) names[0] = I18n.t("Real");
            for (int i = 0; i < MenuUi.HEIGHTS.length; i++) {
                names[i + off] = String.format(java.util.Locale.US, "%.2f", MenuUi.HEIGHTS[i]);
                if (!real && Math.abs(MenuUi.HEIGHTS[i] - cur) < 0.01f) sel = i + off;
            }
            pills(names, sel, i -> {
                if (i < off) pr.edit().putBoolean("standReal", true).apply();
                else pr.edit().putFloat("heightM", MenuUi.HEIGHTS[i - off]).putBoolean("standReal", false).apply();
                host.applyHeight();
            });
            TextView hh = Ui.label(ctx, I18n.t("Metros. En el Modo Centinela la cabeza va siempre a la altura elegida (1,75 si elegiste «Real»)."), 13, Ui.TEXT2, false);
            hh.setPadding(dp(ctx, 4), dp(ctx, 4), 0, 0);
            left.addView(hh);
        }

        // Modo Centinela: jugar sin el visor puesto, mirando la Mac
        left.addView(Ui.section(ctx, I18n.t("Modos")));
        if (!host.vigia()) {
            // Como en la barra lateral: tocarlo muestra su tutorial (con «Empezar»); si lo apagaste, empieza directo
            LinearLayout vg = Ui.row(ctx, "eye", I18n.t("Modo Centinela"), I18n.t("Jugá sin el visor puesto: el juego se ve en la Mac"));
            TextView go = Ui.primary(ctx, I18n.t("Empezar"));
            go.setTextSize(14);
            go.setPadding(dp(ctx, 16), dp(ctx, 6), dp(ctx, 16), dp(ctx, 6));
            vg.addView(go);
            Runnable start = () -> Tutorials.orStart(root, "centinela", I18n.t("Empezar el Modo Centinela"), () -> host.setVigia(true));
            vg.setOnClickListener(v -> start.run());
            go.setOnClickListener(v -> start.run());
            left.addView(vg);
        } else {
            LinearLayout cal = Ui.row(ctx, "recenter", I18n.t("Calibrar la altura"), I18n.t("Parate derecho con los controles delante del pecho: en 3 segundos se toma tu posición"));
            cal.setOnClickListener(v -> host.vigiaCalibrate());
            left.addView(cal);
            toggle(I18n.t("Modo Centinela"), I18n.t("Encendido: el juego se ve en la Mac"), true, on -> { if (!on) host.setVigia(false); });
        }

        // Derecha: imagen del juego
        gameQuality(st);
    }

    /// Página «Avanzado»: lo de este juego (se guarda en la Mac y se usa la próxima vez que lo abras)
    private void renderAdvanced(JSONObject st) {
        JSONObject a = st == null ? null : st.optJSONObject("adv");
        JSONObject g = a == null ? null : a.optJSONObject("game");
        list = left;
        if (g == null) { left.addView(Ui.label(ctx, I18n.t("Sin conexión con la Mac"), 15, Ui.TEXT2, false)); return; }
        // Para qué se configura: este juego (no se puede elegir otro desde acá) o todos los juegos
        JSONObject adv = st == null ? null : st.optJSONObject("adv");
        JSONObject all = adv == null ? null : adv.optJSONObject("all");
        final String scope = advAll ? "all" : "game";
        JSONObject v = advAll ? all : g;
        if (v == null) v = new JSONObject();
        left.addView(Ui.section(ctx, I18n.t("Configurando")));
        leftPills(I18n.t("Para qué juego son estos ajustes"), null,
                new String[]{g.optString("name"), I18n.t("Todos los juegos")}, advAll ? 1 : 0, i -> { advAll = i == 1; drawnKey = ""; render(); });
        TextView d = Ui.label(ctx, I18n.t("Si un juego tiene ajustes propios, siempre se usan esos. Si no, usa los de «Todos los juegos». Para que un juego vuelva a usar los generales, tocá «Usar los de todos los juegos»."), 13, Ui.TEXT2, false);
        d.setPadding(dp(ctx, 4), dp(ctx, 8), 0, dp(ctx, 6));
        left.addView(d);
        if (!advAll && g.optBoolean("own")) {
            TextView r = Ui.pill(ctx, I18n.t("Usar los de todos los juegos"));
            r.setOnClickListener(x -> Link.I.send("{\"type\":\"gamecfg\",\"scope\":\"game\",\"reset\":true}"));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(ctx, 6);
            left.addView(r, lp);
        }
        final String again = "\n" + I18n.t("Hay que volver a abrir el juego para que se aplique.");
        toggle(I18n.t("Escala automática"), I18n.t("Si el juego no llega a los cuadros por segundo, baja la escala; si sobra, la sube. Aprende de cada partida.") + again,
                v.optBoolean("auto"), on -> MenuUi.sendAdv(scope, "auto", String.valueOf(on)));
        toggle(I18n.t("Emular controles de Valve Index"), I18n.t("Para juegos que funcionan mejor con Index: los dedos se mueven según cómo tocás los botones") + again,
                v.optBoolean("index"), on -> MenuUi.sendAdv(scope, "index", String.valueOf(on)));
        right.addView(Ui.section(ctx, I18n.t("Rendimiento")));
        int sc = (int) Math.round(v.optDouble("scale", 1) * 100), fk = (int) Math.round(v.optDouble("fovk", 1) * 100);
        String[] sn = new String[MenuUi.ADV_SCALES.length];
        int si = -1;
        for (int i = 0; i < sn.length; i++) { sn[i] = MenuUi.ADV_SCALES[i] + "%"; if (MenuUi.ADV_SCALES[i] == sc) si = i; }
        pills(I18n.t("Escala de render"), I18n.t("El juego dibuja menos píxeles y la Mac los agranda con MetalFX: más cuadros por segundo") + again,
                sn, si, i -> MenuUi.sendAdv(scope, "scale", String.valueOf(MenuUi.ADV_SCALES[i] / 100.0)));
        String[] fn = new String[MenuUi.ADV_FOVS.length];
        int fi = -1;
        for (int i = 0; i < fn.length; i++) { fn[i] = MenuUi.ADV_FOVS[i] + "%"; if (MenuUi.ADV_FOVS[i] == fk) fi = i; }
        pills(I18n.t("Campo de visión"), I18n.t("Las esquinas casi no se ven por las lentes: si el juego no las dibuja, rinde más") + again,
                fn, fi, i -> MenuUi.sendAdv(scope, "fovk", String.valueOf(MenuUi.ADV_FOVS[i] / 100.0)));
    }

    /// Página «Manos y láser»: a la izquierda jugar con las manos y sus gestos; a la derecha el ángulo de las manos del juego
    private void renderHands() {
        list = left;
        left.addView(Ui.section(ctx, I18n.t("Jugar con las manos")));
        toggle(I18n.t("Jugar con las manos (experimental)"), I18n.t("Sin controles, tus manos hacen de control con gestos. No se recomienda en juegos rápidos."),
                prefs().getBoolean("handGestures", true), on -> { prefs().edit().putBoolean("handGestures", on).apply(); host.setHandGestures(on); });
        LinearLayout gn = Ui.row(ctx, "hand", I18n.t("Gestos de las manos"), I18n.t("Joystick virtual, qué hace cada gesto y macros (con dibujos)"));
        gn.addView(Ui.label(ctx, "›", 26, Ui.TEXT2, false));
        gn.setOnClickListener(v -> { handsPage = false; gesturesPage = true; drawnKey = ""; render(); slide(1); });
        left.addView(gn);
        if (prefs().getBoolean("handGestures", true)) {
            LinearLayout cr = Ui.row(ctx, "hand", I18n.t("Calibrar las manos"), I18n.t("Con los controles en la mano: después, sin controles, el juego ve tus manos como si los tuvieras agarrados"));
            TextView go = Ui.primary(ctx, I18n.t("Calibrar"));
            go.setOnClickListener(v -> { if (calMsg == null) calibrate(5); });
            cr.addView(go);
            left.addView(cr);
            if (calMsg != null) {
                TextView m = Ui.label(ctx, calMsg, 16, Ui.ACCENT_TEXT, true);
                m.setPadding(dp(ctx, 8), dp(ctx, 10), dp(ctx, 8), dp(ctx, 4));
                left.addView(m);
            }
        }
        left.addView(Ui.section(ctx, I18n.t("Cámara")));
        toggle(I18n.t("Manos reales en el juego (experimental)"), I18n.t("Tus manos con la cámara del visor, encima del juego"),
                prefs().getBoolean("gameHands", false), on -> { prefs().edit().putBoolean("gameHands", on).apply(); host.setGameHands(on); });

        // Derecha: el ángulo de las manos (y del láser) del juego, que se nota al instante
        right.addView(Ui.section(ctx, I18n.t("Ángulo de las manos y el láser del juego")));
        TextView hint = Ui.label(ctx, I18n.t("Para cuando jugás con las manos: si la mano o el láser del juego no coinciden con tu mano, probá estas opciones mirando el juego."), 13, Ui.TEXT2, false);
        hint.setPadding(dp(ctx, 4), 0, 0, dp(ctx, 4));
        right.addView(hint);
        for (String side : new String[]{"L", "R"}) {
            TextView hs = Ui.label(ctx, I18n.t(side.equals("L") ? "Mano izquierda" : "Mano derecha"), 15, Ui.ACCENT_TEXT, true);
            hs.setPadding(dp(ctx, 4), dp(ctx, 14), 0, 0);
            right.addView(hs);
            stepper(I18n.t("Inclinación"), I18n.t("Hacia abajo si apunta muy arriba; hacia arriba si apunta muy abajo"), "handPitchOff" + side, 90,
                    I18n.t("abajo"), I18n.t("arriba"));
            stepper(I18n.t("Giro"), I18n.t("Si la palma del juego mira a otro lado que la tuya"), "handRollOff" + side, 180,
                    I18n.t("izquierda"), I18n.t("derecha"));
        }
        TextView reset = Ui.pill(ctx, I18n.t("Volver a lo recomendado"));
        reset.setOnClickListener(v -> { prefs().edit().remove("handPitchOffL").remove("handRollOffL").remove("handPitchOffR").remove("handRollOffR").apply(); host.resetHandCal(); applyAngles(); drawnKey = ""; render(); });
        LinearLayout.LayoutParams rlp2 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp2.topMargin = dp(ctx, 12);
        right.addView(reset, rlp2);
    }

    /// Página «Gestos de las manos»: arriba el perfil (sala VR o juegos); mano izquierda a la izquierda, derecha a la derecha
    private void renderGestures() {
        Runnable redraw = () -> { drawnKey = ""; render(); };
        GestureUi gu = new GestureUi(ctx, prefs(), host::applyGestures, redraw);
        gu.profilePicker(left, gestureProfile, i -> { gestureProfile = i; drawnKey = ""; render(); });
        GestureUi.note(ctx, left);
        gu.actions(left, gestureProfile, 0);
        gu.actions(right, gestureProfile, 1);
        gu.macrosSection(left, gestureProfile);   // (al final, después de los gestos)
    }

    /// Página «Ventanas y pantallas»: a la izquierda las pantallas, a la derecha las ventanas (agrupadas por app)
    private void renderScreens(JSONObject st) {
        if (st == null) { left.addView(Ui.label(ctx, I18n.t("Sin conexión con la Mac"), 15, Ui.TEXT2, false)); return; }
        left.addView(Ui.section(ctx, I18n.t("Pantallas")));
        JSONObject md = st.optJSONObject("mainDisplay");
        if (md != null) {
            final int id = md.optInt("id"), w = md.optInt("w"), h = md.optInt("h");
            final String name = I18n.t("Pantalla principal");
            screenRow(left, "display", name, md.optString("name") + " · " + w + "×" + h, host.isWindowOpen(id), () -> host.openGameWindow(id, name, "", w, h));
        }
        JSONArray ds = st.optJSONArray("displays");
        for (int i = 0; ds != null && i < ds.length(); i++) {
            JSONObject d = ds.optJSONObject(i);
            final int id = d.optInt("id"), w = d.optInt("w"), h = d.optInt("h");
            final String name = d.optString("name");
            screenRow(left, "display", name, I18n.t("Monitor · ") + w + "×" + h, host.isWindowOpen(id), () -> host.openGameWindow(id, name, "", w, h));
        }
        left.addView(Ui.section(ctx, I18n.t("Monitor virtual")));
        screenRow(left, "addDisplay", I18n.t("Monitor virtual"), I18n.t("Una pantalla más, solo en el Quest"), host.isWindowOpen(WindowUi.MONITOR_ID),
                () -> host.openGameWindow(WindowUi.MONITOR_ID, I18n.t("Monitor virtual"), "", 1600, 1000));

        right.addView(Ui.section(ctx, I18n.t("Ventanas")));
        JSONArray ws = st.optJSONArray("windows");
        if (ws == null || ws.length() == 0) {
            right.addView(Ui.label(ctx, I18n.t("No hay ventanas abiertas en la Mac."), 15, Ui.TEXT2, false));
            return;
        }
        // Agrupadas por app, en orden alfabético
        java.util.TreeMap<String, java.util.List<JSONObject>> byApp = new java.util.TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        for (int i = 0; i < ws.length(); i++) {
            JSONObject w = ws.optJSONObject(i);
            byApp.computeIfAbsent(w.optString("app"), k -> new java.util.ArrayList<>()).add(w);
        }
        for (java.util.Map.Entry<String, java.util.List<JSONObject>> e : byApp.entrySet()) {
            TextView appName = Ui.label(ctx, e.getKey(), 14, Ui.TEXT2, true);
            appName.setPadding(dp(ctx, 4), dp(ctx, 12), 0, 0);
            right.addView(appName);
            for (JSONObject w : e.getValue()) {
                final int id = w.optInt("id"), ww = w.optInt("w"), hh = w.optInt("h");
                final String app = w.optString("app"), t = w.optString("title");
                screenRow(right, "window", t.isEmpty() ? app : t, ww + "×" + hh, host.isWindowOpen(id), () -> host.openGameWindow(id, app, t, ww, hh));
            }
        }
    }

    /// Animación al cambiar de página: las columnas entran deslizándose
    private void slide(int dir) {
        for (View v : new View[]{left, right}) {
            v.animate().cancel();
            v.setAlpha(0f); v.setTranslationX(dir * dp(ctx, 30));
            v.animate().alpha(1f).translationX(0).setDuration(200).setInterpolator(new android.view.animation.DecelerateInterpolator(1.6f)).start();
        }
    }

    /// Calibrar las manos: cuenta regresiva con el pedido, después mide (1,5 s) y muestra el resultado
    private void calibrate(int n) {
        String ask = I18n.t("Agarrá los dos controles como para jugar, delante tuyo y a la vista del visor. Quedate quieto…");
        if (n > 0) { calMsg = ask + " " + n; render(); left.postDelayed(() -> calibrate(n - 1), 1000); return; }
        calMsg = I18n.t("Midiendo… no te muevas"); render();
        host.handCalibrate(st -> {
            calMsg = st == 3 ? I18n.t("Listo: las dos manos quedaron calibradas.")
                   : st == 1 ? I18n.t("Solo se calibró la mano izquierda (la derecha no se vio bien). Probá de nuevo.")
                   : st == 2 ? I18n.t("Solo se calibró la mano derecha (la izquierda no se vio bien). Probá de nuevo.")
                   : st == 8 ? I18n.t("Tu visor no pudo seguir las manos y los controles a la vez. Actualizá el visor y probá de nuevo.")
                   : I18n.t("No vi tus manos con los controles. Probá de nuevo, con los controles a la vista.");
            render();
            left.postDelayed(() -> { calMsg = null; render(); }, 6000);
        });
    }

    private void applyAngles() { host.setHandAngles(0, 0); }   // (la parte VR lee los valores de cada mano)

    /// Ajuste fino de a 5°: «−» valor «+» (y se aplica al instante); mantener apretado repite
    private void stepper(String title, String hint, String key, int max, String lessName, String moreName) {
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(ctx, 18), dp(ctx, 12), dp(ctx, 14), dp(ctx, 12));
        box.setBackground(Ui.hoverable(ctx, Ui.FILL, 20, false));
        box.addView(Ui.label(ctx, title, 16, Ui.TEXT, true));
        if (hint != null) box.addView(Ui.label(ctx, hint, 13, Ui.TEXT2, false));
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(ctx, 10), 0, 0);
        final int cur = prefs().getInt(key, 0);
        TextView val = Ui.label(ctx, (cur > 0 ? "+" : "") + cur + "°", 22, cur == 0 ? Ui.TEXT : Ui.ACCENT_TEXT, true);
        val.setGravity(Gravity.CENTER);
        java.util.function.IntConsumer step = d -> {
            int v = Math.max(-max, Math.min(max, prefs().getInt(key, 0) + d));
            prefs().edit().putInt(key, v).apply();
            applyAngles();
            val.setText((v > 0 ? "+" : "") + v + "°");
            val.setTextColor(v == 0 ? Ui.TEXT : Ui.ACCENT_TEXT);
        };
        String[][] btns = {{"−15", "-15"}, {"−5", "-5"}, null, {"+5", "5"}, {"+15", "15"}};
        for (String[] bt : btns) {
            if (bt == null) { row.addView(val, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.4f)); continue; }
            final int d = Integer.parseInt(bt[1]);
            TextView b = Ui.pill(ctx, bt[0]);
            b.setTextSize(15);
            b.setOnClickListener(v -> step.accept(d));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            lp.leftMargin = dp(ctx, 4); lp.rightMargin = dp(ctx, 4);
            row.addView(b, lp);
        }
        box.addView(row);
        LinearLayout ends = new LinearLayout(ctx);
        ends.setOrientation(LinearLayout.HORIZONTAL);
        TextView l = Ui.label(ctx, "← " + lessName, 12, Ui.TEXT3, false), r = Ui.label(ctx, moreName + " →", 12, Ui.TEXT3, false);
        ends.addView(l, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        r.setGravity(Gravity.END);
        ends.addView(r, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        ends.setPadding(dp(ctx, 4), dp(ctx, 4), dp(ctx, 4), 0);
        box.addView(ends);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(ctx, 8);
        right.addView(box, blp);
    }

    /// Igual que pills(), pero en la columna izquierda
    private void leftPills(String title, String hint, String[] names, int cur, java.util.function.IntConsumer pick) {
        int before = right.getChildCount();
        pills(title, hint, names, cur, pick);
        View v = right.getChildAt(before);
        right.removeViewAt(before);
        left.addView(v);
    }

    private static int indexOf(int[] a, int v) {
        int best = 0;
        for (int i = 0; i < a.length; i++) if (Math.abs(a[i] - v) < Math.abs(a[best] - v)) best = i;
        return best;
    }

    /// Imagen del juego: perfil rápido, detalle opcional y «Aplicar» (lo guarda la Mac)
    private void gameQuality(JSONObject st) {
        right.addView(Ui.section(ctx, I18n.t("Imagen del juego")));
        JSONObject cfg = st == null ? null : st.optJSONObject("cfg");
        if (cfg == null) {
            right.addView(Ui.label(ctx, I18n.t("Sin conexión con la Mac"), 15, Ui.TEXT2, false));
            return;
        }
        // Para este juego o para todos: si el juego ya tiene su propia calidad, arranca en «este juego»
        JSONObject advSt = st.optJSONObject("adv"), g = advSt == null ? null : advSt.optJSONObject("game");
        JSONObject ownAll = advSt == null ? null : advSt.optJSONObject("own");
        JSONObject own = g == null || ownAll == null ? null : ownAll.optJSONObject(g.optString("name"));
        if (qAll < 0) qAll = own != null && own.has("eye") ? 0 : 1;
        if (g != null) pills(I18n.t("Para"), I18n.t("Un juego con calidad propia siempre usa la suya"),
                new String[]{I18n.t("Este juego"), I18n.t("Todos los juegos")}, qAll, i -> { qAll = i; selEye = -1; applied = false; });
        else qAll = 1;
        JSONObject q = cfg;   // lo que vale ahora para lo elegido: la general o la del juego encima
        if (qAll == 0 && own != null) {
            try { q = new JSONObject(cfg.toString()); for (String k : new String[]{"eye", "fps", "mbps", "fov"}) if (own.has(k)) q.put(k, own.get(k)); }
            catch (Exception ignored) { }
        }
        int curEye = indexOf(EYES, q.optInt("eye", 1024)), curFps = indexOf(FPSS, q.optInt("fps", 90)), curMbps = indexOf(MBPSS, q.optInt("mbps", 70));
        int curFov = MenuUi.fovLevel(q), curFovC = MenuUi.fovSize(q);
        if (selEye < 0) { selEye = curEye; selFps = curFps; selMbps = curMbps; selFov = curFov; selFovC = curFovC; }
        int prof = -1;
        for (int i = 0; i < PROFILES.length; i++)
            if (PROFILES[i][0] == selEye && PROFILES[i][1] == selFps && PROFILES[i][2] == selMbps) prof = i;
        pills(I18n.t("Perfil"), prof < 0 ? I18n.t("A tu gusto (abajo)") : I18n.t("Lo más fácil: elegí uno y tocá «Aplicar»"),
                new String[]{I18n.t("Más fluido"), I18n.t("Equilibrado"), I18n.t("Más nítido")}, prof,
                i -> { selEye = PROFILES[i][0]; selFps = PROFILES[i][1]; selMbps = PROFILES[i][2]; applied = false; });
        TextView more = Ui.label(ctx, showDetail ? I18n.t("Ocultar el detalle") : I18n.t("Ajustar a mano"), 14, Ui.ACCENT_TEXT, true);
        more.setPadding(dp(ctx, 18), dp(ctx, 8), 0, dp(ctx, 4));
        more.setOnClickListener(v -> { showDetail = !showDetail; drawnKey = ""; render(); });
        right.addView(more);
        if (showDetail || prof < 0) {
            pills(I18n.t("Nitidez"), I18n.t("Píxeles por ojo · más = la Mac trabaja más") + "\n" + I18n.t("Hay que volver a abrir el juego para que se aplique."),
                    new String[]{"1024", "1280", "1536", "1832", "2048", "2304", "2560"}, selEye, i -> { selEye = i; applied = false; });
            pills(I18n.t("Fluidez"), I18n.t("Cuadros por segundo del visor"),
                    new String[]{"72 Hz", "90 Hz", "120 Hz"}, selFps, i -> { selFps = i; applied = false; });
            pills(I18n.t("Calidad de imagen"), I18n.t("Más = menos manchas, pide más al Wi-Fi"),
                    new String[]{"40", "70", "100", "150", "200 Mbps"}, selMbps, i -> { selMbps = i; applied = false; });
        }
        // Foveated encoding: el borde de cada ojo va más chico (menos demora, más nitidez en el centro)
        pills(I18n.t("Foveated encoding"), I18n.t("Borde más liviano: menos demora y un centro más nítido"),
                new String[]{I18n.t("Apagado"), I18n.t("Suave"), I18n.t("Medio"), I18n.t("Fuerte")}, selFov, i -> { selFov = i; applied = false; });
        if (selFov > 0) pills(I18n.t("Tamaño del centro nítido"), null,
                new String[]{I18n.t("Chico"), I18n.t("Medio"), I18n.t("Grande")}, selFovC, i -> { selFovC = i; applied = false; });
        boolean changed = selEye != curEye || selFps != curFps || selMbps != curMbps || selFov != curFov || (selFov > 0 && selFovC != curFovC);
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(ctx, 4), dp(ctx, 10), 0, dp(ctx, 6));
        TextView apply = changed ? Ui.primary(ctx, I18n.t("Aplicar")) : Ui.pill(ctx, I18n.t(applied ? "Aplicado" : "Así está ahora"));
        apply.setEnabled(changed);
        if (!changed) apply.setAlpha(0.6f);
        apply.setOnClickListener(v -> {
            String vals = "\"eye\":" + EYES[selEye] + ",\"fps\":" + FPSS[selFps] + ",\"mbps\":" + MBPSS[selMbps]
                    + ",\"fov\":\"" + MenuUi.fovString(ctx, selFov, selFovC) + "\"";
            Link.I.send(qAll == 0 ? "{\"type\":\"gamecfg\",\"scope\":\"game\"," + vals + "}" : "{\"type\":\"settings\"," + vals + "}");
            applied = true; drawnKey = ""; render();
        });
        row.addView(apply);
        TextView now = Ui.label(ctx, "  " + I18n.t("Ahora: ") + cfg.optString("run", EYES[curEye] + " · " + FPSS[curFps] + " Hz · " + MBPSS[curMbps] + " Mbps"), 13, Ui.TEXT3, false);
        row.addView(now);
        right.addView(row);
        if (changed || applied) {
            TextView n = Ui.label(ctx, I18n.t("Al aplicar, la imagen se corta unos segundos. La nitidez nueva se ve del todo la próxima vez que abras el juego."), 13, Ui.TEXT2, false);
            n.setPadding(dp(ctx, 4), 0, 0, dp(ctx, 6));
            right.addView(n);
        }
        // Avanzado: escala de render, campo de visión y controles de Index de este juego
        LinearLayout adv = Ui.row(ctx, "sliders", I18n.t("Avanzado"), I18n.t("Escala de render, campo de visión, controles de Index"));
        adv.addView(Ui.label(ctx, "›", 26, Ui.TEXT2, false));
        adv.setOnClickListener(v -> { advPage = true; drawnKey = ""; render(); slide(1); });
        right.addView(adv);
    }

    /// Fila de opciones (pastillas): título, ayuda y una opción elegida
    private void pills(String title, String hint, String[] names, int cur, java.util.function.IntConsumer pick) {
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(ctx, 18), dp(ctx, 12), dp(ctx, 14), dp(ctx, 12));
        box.setBackground(Ui.hoverable(ctx, Ui.FILL, 20, false));
        box.addView(Ui.label(ctx, title, 16, Ui.TEXT, true));
        if (hint != null) box.addView(Ui.label(ctx, hint, 13, Ui.TEXT2, false));
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(ctx, 10), 0, 0);
        for (int i = 0; i < names.length; i++) {
            final int m = i;
            TextView b = i == cur ? Ui.primary(ctx, names[i]) : Ui.pill(ctx, names[i]);
            b.setTextSize(14);
            b.setGravity(Gravity.CENTER);
            b.setOnClickListener(v -> { pick.accept(m); drawnKey = ""; render(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            if (i > 0) lp.leftMargin = dp(ctx, 8);
            row.addView(b, lp);
        }
        box.addView(row);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(ctx, 8);
        right.addView(box, blp);
    }

    /// Fila de opciones cortas (la elegida resaltada); al tocar se aplica y se vuelve a dibujar
    private void pills(String[] names, int sel, java.util.function.IntConsumer pick) {
        LinearLayout r = new LinearLayout(ctx);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setPadding(0, dp(ctx, 4), 0, dp(ctx, 4));
        for (int i = 0; i < names.length; i++) {
            final int k = i;
            TextView b = i == sel ? Ui.primary(ctx, names[i]) : Ui.pill(ctx, names[i]);
            b.setTextSize(14);
            b.setPadding(dp(ctx, 14), dp(ctx, 6), dp(ctx, 14), dp(ctx, 6));
            b.setOnClickListener(v -> { pick.accept(k); drawnKey = ""; render(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = dp(ctx, 6);
            r.addView(b, lp);
        }
        left.addView(r);
    }

    private void toggle(String title, String hint, boolean on, java.util.function.Consumer<Boolean> set) {
        LinearLayout r = new LinearLayout(ctx);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(ctx, 18), dp(ctx, 12), dp(ctx, 14), dp(ctx, 12));
        r.setBackground(Ui.hoverable(ctx, Ui.FILL, 20, false));
        r.setClickable(true);
        LinearLayout txt = new LinearLayout(ctx);
        txt.setOrientation(LinearLayout.VERTICAL);
        txt.addView(Ui.label(ctx, title, 16, Ui.TEXT, true));
        if (hint != null) txt.addView(Ui.label(ctx, hint, 13, Ui.TEXT2, false));
        r.addView(txt, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
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
        FrameLayout.LayoutParams tlp = new FrameLayout.LayoutParams(dp(ctx, 26), dp(ctx, 26), Gravity.CENTER_VERTICAL | (on ? Gravity.END : Gravity.START));
        tlp.setMargins(dp(ctx, 3), 0, dp(ctx, 3), 0);
        sw.addView(thumb, tlp);
        r.addView(sw, new LinearLayout.LayoutParams(dp(ctx, 56), dp(ctx, 32)));
        r.setOnClickListener(v -> { set.accept(!on); drawnKey = ""; render(); });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 8);
        list.addView(r, lp);
    }
}
