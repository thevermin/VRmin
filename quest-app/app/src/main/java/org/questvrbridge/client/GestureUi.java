package org.questvrbridge.client;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import static org.questvrbridge.client.Ui.dp;

/**
 * Gestos de las manos (sin controles), con dibujos:
 *  · Acciones: cada acción de un control (gatillo, agarre, A/X, B/Y, clic del joystick, menú, y el joystick para
 *    moverte o girar) muestra el gesto que la hace, y se puede elegir otro.
 *  · Macros (al final): un gesto aprieta hasta 3 botones juntos, de cualquiera de los dos controles. Hasta 10.
 * Un perfil para la sala VR (Studio) y otro para los juegos; por mano.
 * A la parte nativa le llega, por mano y perfil, qué botones activa cada gesto (los suyos en los bits 0–5 y los
 * del otro control en los bits 8–13) y el modo del joystick.
 */
final class GestureUi {
    static final int G_PI = 0, G_PM = 1, G_PR = 2, G_PL = 3, G_FIST = 4, G_GRAB = 5, G_POINT = 6, G_THUMBPALM = 7, G_THUMBUP = 8, G_N = 9;
    static final int A_TRIG = 1, A_GRIP = 2, A_A = 4, A_B = 8, A_CLICK = 16, A_MENU = 32;
    static final String[] GESTURE_NAMES = {"Pellizco con el índice", "Pellizco con el mayor", "Pellizco con el anular", "Pellizco con el meñique",
            "Puño", "Agarrar (sin el índice)", "Señalar", "Pulgar sobre la palma", "Pulgar arriba"};
    static final String[] JOY_NAMES = {"Sin joystick", "Puño + pulgar", "Pellizcar y arrastrar", "Puño y mover la mano"};
    static final String[] JOY_HINTS = {"Esta mano no tiene joystick",
            "Cerrá el puño con el pulgar apoyado encima y movelo hacia los costados, adelante o atrás",
            "Como VRChat: pellizcá el mayor con el pulgar y arrastrá la mano desde ese punto",
            "Cerrá el puño con el pulgar encima y mové toda la mano desde donde empezaste"};

    // ---------------------------------------------------------------- datos
    static int[] defaults(int profile, int hand) {
        if (profile == 0) return hand == 0 ? new int[]{0, 0, 0, A_MENU, 0, 0, 0, 0, 0, 1} : new int[]{0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return hand == 0 ? new int[]{A_TRIG, A_B, A_A, A_MENU, 0, A_GRIP, 0, A_CLICK, 0, 1}
                         : new int[]{A_TRIG, A_B, A_A, 0, 0, A_GRIP, 0, A_CLICK, 0, 1};
    }
    static String key(int profile, int hand) { return "gm_p" + profile + "_h" + hand; }
    /// Lo elegido en «Acciones» (sin las macros)
    static int[] base(SharedPreferences pr, int profile, int hand) {
        int[] d = defaults(profile, hand);
        String s = pr.getString(key(profile, hand), null);
        if (s == null) return d;
        String[] p = s.split(",");
        for (int i = 0; i < Math.min(p.length, d.length); i++) try { d[i] = Integer.parseInt(p[i].trim()); } catch (Exception ignored) { }
        return d;
    }
    static void saveBase(SharedPreferences pr, int profile, int hand, int[] v) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < v.length; i++) { if (i > 0) b.append(','); b.append(v[i] & 0xFF); }
        pr.edit().putString(key(profile, hand), b.toString()).apply();
    }
    /// Lo que va a la parte nativa: acciones + macros
    static int[] load(SharedPreferences pr, int profile, int hand) {
        int[] v = base(pr, profile, hand);
        for (int[] m : macros(pr, profile)) if (m[0] == hand && m[1] >= 0 && m[1] < G_N) v[m[1]] |= macroMask(m);
        return v;
    }

    /// Botones que puede apretar una macro: {mano del control, bit}
    static final int[][] BUTTONS = {{0, A_TRIG}, {0, A_GRIP}, {0, A_A}, {0, A_B}, {0, A_CLICK}, {0, A_MENU},
                                    {1, A_TRIG}, {1, A_GRIP}, {1, A_A}, {1, A_B}, {1, A_CLICK}};
    static String buttonName(int i) {
        int h = BUTTONS[i][0], a = BUTTONS[i][1];
        if (a == A_A) return h == 0 ? "X" : "A";
        if (a == A_B) return h == 0 ? "Y" : "B";
        if (a == A_MENU) return I18n.t("Menú");
        return actionName(a, h) + (h == 0 ? I18n.t(" izq.") : I18n.t(" der."));
    }
    /// Macro: {mano del gesto, gesto, botón 1, botón 2, botón 3} (botones = índices en BUTTONS, -1 = ninguno)
    static java.util.List<int[]> macros(SharedPreferences pr, int profile) {
        java.util.List<int[]> out = new java.util.ArrayList<>();
        String s = pr.getString("mac_p" + profile, "");
        for (String e : s.split(";")) {
            if (e.trim().isEmpty()) continue;
            String[] p = e.split(",");
            int[] m = {0, G_FIST, -1, -1, -1};
            for (int i = 0; i < Math.min(p.length, 5); i++) try { m[i] = Integer.parseInt(p[i].trim()); } catch (Exception ignored) { }
            out.add(m);
        }
        return out;
    }
    static void saveMacros(SharedPreferences pr, int profile, java.util.List<int[]> l) {
        StringBuilder b = new StringBuilder();
        for (int[] m : l) { if (b.length() > 0) b.append(';'); b.append(m[0]).append(',').append(m[1]).append(',').append(m[2]).append(',').append(m[3]).append(',').append(m[4]); }
        pr.edit().putString("mac_p" + profile, b.toString()).apply();
    }
    static int macroMask(int[] m) {
        int mask = 0;
        for (int k = 2; k < 5; k++) {
            int i = m[k];
            if (i < 0 || i >= BUTTONS.length) continue;
            mask |= BUTTONS[i][0] == m[0] ? BUTTONS[i][1] : BUTTONS[i][1] << 8;
        }
        return mask;
    }
    static String actionName(int a, int hand) {
        switch (a) {
            case A_TRIG: return I18n.t("Gatillo");
            case A_GRIP: return I18n.t("Agarre");
            case A_A: return hand == 0 ? "X" : "A";
            case A_B: return hand == 0 ? "Y" : "B";
            case A_CLICK: return I18n.t("Clic del joystick");
            default: return I18n.t("Menú");
        }
    }

    // ---------------------------------------------------------------- vista
    interface Changed { void run(); }
    private final Context ctx;
    private final SharedPreferences prefs;
    private final Changed changed;
    private final Runnable redraw;
    /// Qué fila tiene abierta la elección de gesto (sobrevive al redibujar)
    private static String openKey = null;
    private static int[] editing = null;   // macro que se está armando: {mano, gesto, b1, b2, b3, índice o -1}

    GestureUi(Context c, SharedPreferences p, Changed onChange, Runnable redraw) { ctx = c; prefs = p; changed = onChange; this.redraw = redraw; }

    private void pills(LinearLayout into, String[] names, boolean[] on, int per, java.util.function.IntConsumer pick) {
        LinearLayout row = null;
        for (int i = 0; i < names.length; i++) {
            if (i % per == 0) {
                row = new LinearLayout(ctx);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setPadding(0, dp(ctx, 3), 0, dp(ctx, 3));
                into.addView(row);
            }
            final int k = i;
            TextView b = on[i] ? Ui.primary(ctx, names[i]) : Ui.pill(ctx, names[i]);
            b.setTextSize(13);
            b.setPadding(dp(ctx, 12), dp(ctx, 5), dp(ctx, 12), dp(ctx, 5));
            b.setOnClickListener(v -> pick.accept(k));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = dp(ctx, 6);
            row.addView(b, lp);
        }
    }

    void profilePicker(LinearLayout into, int profile, java.util.function.IntConsumer pick) {
        pills(into, new String[]{I18n.t("Studio"), I18n.t("Juegos")}, new boolean[]{profile == 0, profile == 1}, 2, pick);
    }

    static void note(Context ctx, LinearLayout into) {
        TextView t = Ui.label(ctx, I18n.t("Tocá una acción para ver o cambiar el gesto que la hace. Mientras usás el joystick de una mano, los otros gestos de esa mano no cuentan."), 13, Ui.TEXT2, false);
        t.setPadding(dp(ctx, 4), dp(ctx, 4), 0, dp(ctx, 4));
        into.addView(t);
    }

    /// Una fila: dibujo + título + lo que hace; al tocarla se abre (o cierra) lo de abajo
    private LinearLayout row(LinearLayout into, Drawable icon, String title, String sub, boolean open, Runnable tap) {
        LinearLayout r = new LinearLayout(ctx);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(ctx, 8), dp(ctx, 6), dp(ctx, 12), dp(ctx, 6));
        r.setBackground(Ui.hoverable(ctx, open ? Color.argb(34, 255, 255, 255) : Ui.FILL, 14, false));
        r.setClickable(true);
        r.setOnClickListener(v -> tap.run());
        View iv = new View(ctx);
        iv.setBackground(icon);
        r.addView(iv, new LinearLayout.LayoutParams(dp(ctx, 58), dp(ctx, 58)));
        LinearLayout col = new LinearLayout(ctx);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(ctx, 12), 0, 0, 0);
        col.addView(Ui.label(ctx, title, 15, Ui.TEXT, true));
        if (sub != null) col.addView(Ui.label(ctx, sub, 13, Ui.TEXT2, false));
        r.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        r.addView(Ui.label(ctx, open ? "⌃" : "›", 22, Ui.TEXT2, false));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 5);
        into.addView(r, lp);
        return r;
    }

    /// Cuadrícula de opciones con dibujo (para elegir un gesto o un modo de joystick)
    private void iconGrid(LinearLayout into, Drawable[] icons, String[] names, int sel, int per, java.util.function.IntConsumer pick) {
        LinearLayout row = null;
        for (int i = 0; i < icons.length; i++) {
            if (i % per == 0) {
                row = new LinearLayout(ctx);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setPadding(dp(ctx, 6), dp(ctx, 4), dp(ctx, 6), 0);
                into.addView(row);
            }
            final int k = i;
            LinearLayout cell = new LinearLayout(ctx);
            cell.setOrientation(LinearLayout.VERTICAL);
            cell.setGravity(Gravity.CENTER_HORIZONTAL);
            cell.setPadding(dp(ctx, 4), dp(ctx, 6), dp(ctx, 4), dp(ctx, 6));
            cell.setBackground(Ui.hoverable(ctx, i == sel ? Color.argb(70, 230, 168, 74) : Color.argb(14, 255, 255, 255), 12, false));
            cell.setClickable(true);
            cell.setOnClickListener(v -> pick.accept(k));
            View iv = new View(ctx);
            iv.setBackground(icons[i]);
            cell.addView(iv, new LinearLayout.LayoutParams(dp(ctx, 52), dp(ctx, 52)));
            TextView t = Ui.label(ctx, names[i], 11, i == sel ? Ui.ACCENT_TEXT : Ui.TEXT2, i == sel);
            t.setGravity(Gravity.CENTER);
            t.setMaxLines(2);
            cell.addView(t);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            lp.rightMargin = dp(ctx, 5);
            row.addView(cell, lp);
        }
        int rest = icons.length % per;
        if (rest != 0 && row != null) for (int i = rest; i < per; i++) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, 1, 1f); lp.rightMargin = dp(ctx, 5);
            row.addView(new View(ctx), lp);
        }
    }

    private Drawable[] gestureIcons(boolean left, boolean withNone) {
        Drawable[] d = new Drawable[G_N + (withNone ? 1 : 0)];
        for (int g = 0; g < G_N; g++) d[g] = new GestureIcon(g, left, 0);
        if (withNone) d[G_N] = new GestureIcon(-1, left, 0);
        return d;
    }
    private String[] gestureNames(boolean withNone) {
        String[] n = new String[G_N + (withNone ? 1 : 0)];
        for (int g = 0; g < G_N; g++) n[g] = I18n.t(GESTURE_NAMES[g]);
        if (withNone) n[G_N] = I18n.t("Ninguno");
        return n;
    }

    /// Acciones de una mano en un perfil
    void actions(LinearLayout into, int profile, int hand) {
        final boolean left = hand == 0;
        TextView hs = Ui.label(ctx, I18n.t(left ? "Mano izquierda" : "Mano derecha"), 16, Ui.ACCENT_TEXT, true);
        hs.setPadding(dp(ctx, 4), dp(ctx, 12), 0, dp(ctx, 2));
        into.addView(hs);
        int[] v = base(prefs, profile, hand);
        // Joystick (moverte o girar)
        {
            String k = profile + ":" + hand + ":joy";
            boolean open = k.equals(openKey);
            String title = profile == 0 ? I18n.t(left ? "Caminar" : "Girar y teletransportarte")
                                        : I18n.t(left ? "Moverte (joystick izquierdo)" : "Girar (joystick derecho)");
            row(into, new GestureIcon(G_N, left, v[G_N]), title, I18n.t(JOY_NAMES[v[G_N]]) + " · " + I18n.t(JOY_HINTS[v[G_N]]), open,
                    () -> { openKey = open ? null : k; redraw.run(); });
            if (open) {
                Drawable[] ic = new Drawable[JOY_NAMES.length];
                String[] nm = new String[JOY_NAMES.length];
                for (int i = 0; i < ic.length; i++) { ic[i] = new GestureIcon(G_N, left, i); nm[i] = I18n.t(JOY_NAMES[i]); }
                iconGrid(into, ic, nm, v[G_N], 4, i -> { remember(profile, hand); int[] w = base(prefs, profile, hand); w[G_N] = i; saveBase(prefs, profile, hand, w); openKey = null; changed.run(); redraw.run(); });
            }
        }
        // Botones
        int[] acts = {A_TRIG, A_GRIP, A_A, A_B, A_CLICK, A_MENU};   // (en Studio también: vienen sin gesto, cada uno elige)
        for (int a : acts) {
            int g = -1;
            for (int i = 0; i < G_N; i++) if ((v[i] & a) != 0) { g = i; break; }
            String k = profile + ":" + hand + ":" + a;
            boolean open = k.equals(openKey);
            String title = a == A_MENU ? I18n.t(profile == 0 ? "Abrir el menú de VRmin" : "Menú del juego") : actionName(a, hand);
            final int cur = g;
            row(into, new GestureIcon(g, left, 0), title, g < 0 ? I18n.t("Sin gesto") : I18n.t(GESTURE_NAMES[g]), open,
                    () -> { openKey = open ? null : k; redraw.run(); });
            if (open) {
                iconGrid(into, gestureIcons(left, true), gestureNames(true), cur < 0 ? G_N : cur, 5, i -> {
                    remember(profile, hand);
                    int[] w = base(prefs, profile, hand);
                    for (int q = 0; q < G_N; q++) w[q] &= ~a;
                    if (i < G_N) w[i] |= a;
                    saveBase(prefs, profile, hand, w);
                    openKey = null; changed.run(); redraw.run();
                });
                // ¿ese gesto ya hace otra cosa? (se puede: hace las dos)
                if (cur >= 0) {
                    StringBuilder also = new StringBuilder();
                    for (int b : new int[]{A_TRIG, A_GRIP, A_A, A_B, A_CLICK, A_MENU}) if (b != a && (v[cur] & b) != 0) { if (also.length() > 0) also.append(", "); also.append(actionName(b, hand)); }
                    if (also.length() > 0) {
                        TextView t = Ui.label(ctx, I18n.t("Ese gesto también hace: ") + also, 12, Ui.TEXT2, false);
                        t.setPadding(dp(ctx, 10), dp(ctx, 2), 0, dp(ctx, 2));
                        into.addView(t);
                    }
                }
            }
        }
        // Volver a lo recomendado (pregunta antes), y deshacer / rehacer los cambios de esta mano
        LinearLayout bar = new LinearLayout(ctx);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        TextView reset = Ui.pill(ctx, I18n.t("Volver a lo recomendado"));
        reset.setTextSize(13);
        reset.setOnClickListener(x -> {
            android.view.View rv = x.getRootView();
            Runnable doIt = () -> { remember(profile, hand); prefs.edit().remove(key(profile, hand)).apply(); openKey = null; changed.run(); redraw.run(); };
            if (rv instanceof android.widget.FrameLayout)
                Tutorials.confirm((android.widget.FrameLayout) rv, I18n.t("¿Volver a lo recomendado?"),
                        I18n.t(left ? "Los gestos que elegiste para la mano izquierda se reemplazan por los recomendados. Después podés deshacerlo." : "Los gestos que elegiste para la mano derecha se reemplazan por los recomendados. Después podés deshacerlo."),
                        I18n.t("Sí, volver"), doIt);
            else doIt.run();
        });
        bar.addView(reset);
        String hk = key(profile, hand);
        java.util.ArrayDeque<String> un = UNDO.get(hk), re = REDO.get(hk);
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        bl.leftMargin = dp(ctx, 8);
        if (un != null && !un.isEmpty()) {
            TextView u = Ui.pill(ctx, I18n.t("Deshacer"));
            u.setTextSize(13);
            Ui.leadingIcon(u, "back", 16, Ui.TEXT);
            u.setOnClickListener(x -> { step(profile, hand, true); openKey = null; changed.run(); redraw.run(); });
            bar.addView(u, bl);
        }
        if (re != null && !re.isEmpty()) {
            TextView r = Ui.pill(ctx, I18n.t("Rehacer"));
            r.setTextSize(13);
            Ui.leadingIcon(r, "chevron", 16, Ui.TEXT);
            r.setOnClickListener(x -> { step(profile, hand, false); openKey = null; changed.run(); redraw.run(); });
            bar.addView(r, bl);
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 8);
        into.addView(bar, lp);
    }

    // Deshacer y rehacer (por perfil y mano; mientras la app esté abierta). NONE = lo recomendado (nada guardado).
    private static final String NONE = "-";
    private static final java.util.Map<String, java.util.ArrayDeque<String>> UNDO = new java.util.HashMap<>(), REDO = new java.util.HashMap<>();
    private void remember(int profile, int hand) {
        String k = key(profile, hand);
        UNDO.computeIfAbsent(k, q -> new java.util.ArrayDeque<>()).push(prefs.getString(k, NONE));
        java.util.ArrayDeque<String> r = REDO.get(k);
        if (r != null) r.clear();
    }
    private void step(int profile, int hand, boolean undo) {
        String k = key(profile, hand);
        java.util.ArrayDeque<String> from = (undo ? UNDO : REDO).get(k);
        if (from == null || from.isEmpty()) return;
        (undo ? REDO : UNDO).computeIfAbsent(k, q -> new java.util.ArrayDeque<>()).push(prefs.getString(k, NONE));
        String v = from.pop();
        if (NONE.equals(v)) prefs.edit().remove(k).apply(); else prefs.edit().putString(k, v).apply();
    }

    /// Macros (al final del menú de gestos): un gesto aprieta hasta 3 botones juntos. Hasta 10 por perfil.
    void macrosSection(LinearLayout into, int profile) {
        into.addView(Ui.section(ctx, I18n.t("Macros")));
        TextView h = Ui.label(ctx, I18n.t("Un gesto aprieta hasta 3 botones juntos, de cualquiera de los dos controles. Hasta 10 macros."), 13, Ui.TEXT2, false);
        h.setPadding(dp(ctx, 4), 0, 0, dp(ctx, 4));
        into.addView(h);
        java.util.List<int[]> l = macros(prefs, profile);
        for (int i = 0; i < l.size(); i++) {
            final int idx = i;
            int[] m = l.get(i);
            if (editing != null && editing[5] == i && editing[6] == profile) { macroEditor(into, profile); continue; }
            StringBuilder sb = new StringBuilder();
            for (int k = 2; k < 5; k++) if (m[k] >= 0 && m[k] < BUTTONS.length) { if (sb.length() > 0) sb.append(" + "); sb.append(buttonName(m[k])); }
            String g = m[1] >= 0 && m[1] < G_N ? I18n.t(GESTURE_NAMES[m[1]]) : "?";
            row(into, new GestureIcon(m[1], m[0] == 0, 0), I18n.t(m[0] == 0 ? "Izquierda · " : "Derecha · ") + g,
                    sb.length() == 0 ? I18n.t("Sin botones") : "→ " + sb, false,
                    () -> { editing = new int[]{m[0], m[1], m[2], m[3], m[4], idx, profile}; redraw.run(); });
        }
        if (editing != null && editing[5] < 0 && editing[6] == profile) macroEditor(into, profile);
        else if (l.size() < 10) {
            TextView add = Ui.pill(ctx, I18n.t("+ Agregar macro"));
            add.setTextSize(13);
            add.setOnClickListener(x -> { editing = new int[]{0, G_THUMBUP, -1, -1, -1, -1, profile}; redraw.run(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(ctx, 8);
            into.addView(add, lp);
        }
    }

    private void macroEditor(LinearLayout into, int profile) {
        final int[] e = editing;
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(ctx, 10), dp(ctx, 8), dp(ctx, 10), dp(ctx, 10));
        box.setBackground(Ui.hoverable(ctx, Color.argb(34, 255, 255, 255), 14, false));
        box.addView(Ui.label(ctx, I18n.t(e[5] < 0 ? "Nueva macro" : "Editar macro"), 15, Ui.TEXT, true));
        box.addView(Ui.label(ctx, I18n.t("Mano que hace el gesto"), 13, Ui.TEXT2, false));
        pills(box, new String[]{I18n.t("Izquierda"), I18n.t("Derecha")}, new boolean[]{e[0] == 0, e[0] == 1}, 2, i -> { e[0] = i; redraw.run(); });
        box.addView(Ui.label(ctx, I18n.t("Gesto"), 13, Ui.TEXT2, false));
        iconGrid(box, gestureIcons(e[0] == 0, false), gestureNames(false), e[1], 5, i -> { e[1] = i; redraw.run(); });
        int n = 0; for (int k = 2; k < 5; k++) if (e[k] >= 0) n++;
        box.addView(Ui.label(ctx, I18n.t("Botones (hasta 3): ") + n + "/3", 13, Ui.TEXT2, false));
        String[] bn = new String[BUTTONS.length];
        boolean[] bon = new boolean[BUTTONS.length];
        for (int i = 0; i < BUTTONS.length; i++) { bn[i] = buttonName(i); bon[i] = e[2] == i || e[3] == i || e[4] == i; }
        pills(box, bn, bon, 6, i -> {
            for (int k = 2; k < 5; k++) if (e[k] == i) { e[k] = -1; redraw.run(); return; }   // (tocar uno elegido lo saca)
            for (int k = 2; k < 5; k++) if (e[k] < 0) { e[k] = i; redraw.run(); return; }
        });
        LinearLayout acts = new LinearLayout(ctx);
        acts.setOrientation(LinearLayout.HORIZONTAL);
        acts.setPadding(0, dp(ctx, 8), 0, 0);
        TextView ok = Ui.primary(ctx, I18n.t("Guardar"));
        ok.setTextSize(13);
        ok.setOnClickListener(x -> {
            java.util.List<int[]> l = macros(prefs, profile);
            int[] m = {e[0], e[1], e[2], e[3], e[4]};
            if (e[5] >= 0 && e[5] < l.size()) l.set(e[5], m); else if (l.size() < 10) l.add(m);
            saveMacros(prefs, profile, l);
            editing = null; changed.run(); redraw.run();
        });
        acts.addView(ok);
        TextView cancel = Ui.pill(ctx, I18n.t("Cancelar"));
        cancel.setTextSize(13);
        cancel.setOnClickListener(x -> { editing = null; redraw.run(); });
        LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cl.leftMargin = dp(ctx, 8);
        acts.addView(cancel, cl);
        if (e[5] >= 0) {
            TextView del = Ui.pill(ctx, I18n.t("Borrar"));
            del.setTextSize(13);
            del.setTextColor(Ui.ACCENT_TEXT);
            del.setOnClickListener(x -> {
                java.util.List<int[]> l = macros(prefs, profile);
                if (e[5] < l.size()) l.remove(e[5]);
                saveMacros(prefs, profile, l);
                editing = null; changed.run(); redraw.run();
            });
            acts.addView(del, cl);
        }
        box.addView(acts);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(ctx, 6);
        into.addView(box, lp);
    }

    /**
     * Dibujo animado de una mano (palma hacia vos, como la ves en el visor): empieza abierta, hace el gesto, lo
     * mantiene y vuelve. Lo que hace el gesto va en ámbar claro (el dedo que pellizca y el pulgar, el índice que
     * señala…) y un punto ámbar marca dónde se tocan. g = G_N: joystick (joy = modo: el gesto y después cómo se
     * mueve, con flechas); g = -1: ningún gesto (quieta, en gris).
     */
    static final class GestureIcon extends Drawable {
        private final int g, joy; private final boolean left;
        private final int SK, FOLD, HI = Color.rgb(250, 204, 133), HIF = Color.rgb(214, 160, 86), EDGE = Color.rgb(28, 28, 32);
        private final Paint st = new Paint(Paint.ANTI_ALIAS_FLAG), fl = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private static final float[] FX = {41, 52, 63, 73}, BY = {46, 44, 46, 50}, LEN = {34, 38, 34, 27}, W = {10, 10.5f, 10, 8.5f};
        private static final long PERIOD = 2600;
        private final Runnable tick = this::invalidateSelf;
        GestureIcon(int gesture, boolean leftHand, int joyMode) {
            g = gesture; left = leftHand; joy = joyMode;
            SK = g < 0 ? Color.rgb(120, 120, 126) : Color.rgb(230, 230, 235);
            FOLD = g < 0 ? Color.rgb(92, 92, 98) : Color.rgb(168, 168, 178);
            st.setStyle(Paint.Style.STROKE); st.setStrokeCap(Paint.Cap.ROUND); st.setStrokeJoin(Paint.Join.ROUND);
            fl.setStyle(Paint.Style.FILL);
        }
        private static float mix(float a, float b, float k) { return a + (b - a) * k; }
        private static int mixC(int a, int b, float k) {
            return Color.rgb(Math.round(mix(Color.red(a), Color.red(b), k)), Math.round(mix(Color.green(a), Color.green(b), k)), Math.round(mix(Color.blue(a), Color.blue(b), k)));
        }
        private void seg(Canvas c, float w, int col, float x0, float y0, float x1, float y1, float x2, float y2) {
            path.reset(); path.moveTo(x0, y0); path.lineTo(x1, y1); if (x2 > -900) path.lineTo(x2, y2);
            st.setColor(EDGE); st.setStrokeWidth(w + 3); c.drawPath(path, st);
            st.setColor(col); st.setStrokeWidth(w); c.drawPath(path, st);
        }
        private void cub(Canvas c, float w, int col, float[] q) {
            path.reset(); path.moveTo(q[0], q[1]); path.cubicTo(q[2], q[3], q[4], q[5], q[6], q[7]);
            st.setColor(EDGE); st.setStrokeWidth(w + 3); c.drawPath(path, st);
            st.setColor(col); st.setStrokeWidth(w); c.drawPath(path, st);
        }
        private void dot(Canvas c, float x, float y, float r, float a) {
            if (a <= 0.02f) return;
            fl.setColor(Color.argb(Math.round(90 * a), 230, 168, 74)); c.drawCircle(x, y, r * 1.9f, fl);
            fl.setColor(Color.argb(Math.round(255 * a), 230, 168, 74)); c.drawCircle(x, y, r, fl);
        }
        private void arrow(Canvas c, float x0, float y0, float x1, float y1, float a) {
            if (a <= 0.02f) return;
            st.setColor(Color.argb(Math.round(255 * a), 230, 168, 74)); st.setStrokeWidth(4);
            c.drawLine(x0, y0, x1, y1, st);
            float dx = x1 - x0, dy = y1 - y0, l = (float) Math.hypot(dx, dy); dx /= l; dy /= l;
            path.reset(); path.moveTo(x1 - dx * 8 - dy * 6, y1 - dy * 8 + dx * 6); path.lineTo(x1, y1); path.lineTo(x1 - dx * 8 + dy * 6, y1 - dy * 8 - dx * 6);
            c.drawPath(path, st);
        }
        private static float ease(float x) { x = Math.max(0, Math.min(1, x)); return x * x * (3 - 2 * x); }

        @Override public void draw(Canvas c) {
            float S = Math.min(getBounds().width(), getBounds().height());
            c.save();
            c.translate(getBounds().left + (getBounds().width() - S) / 2f, getBounds().top + (getBounds().height() - S) / 2f);
            c.scale(S / 100f, S / 100f);
            if (left) c.scale(-1, 1, 50, 50);   // la izquierda, en espejo (pulgar a la derecha)
            // Tiempo: abierta · hace el gesto · lo mantiene (el joystick se mueve) · vuelve
            float u = 0, k = 1, hold = 0;
            final boolean anim = g >= 0 && !(g == G_N && joy == 0);
            if (anim) {
                long now = android.os.SystemClock.uptimeMillis();
                u = (now % PERIOD) / (float) PERIOD;
                k = u < 0.2f ? 0 : u < 0.42f ? ease((u - 0.2f) / 0.22f) : u < 0.86f ? 1 : 1 - ease((u - 0.86f) / 0.14f);
                hold = u >= 0.42f && u < 0.86f ? (u - 0.42f) / 0.44f : 0;   // 0..1 mientras se mantiene
                unscheduleSelf(tick);
                scheduleSelf(tick, now + 50);   // ≈ 20 cuadros por segundo
            }
            // El gesto: qué dedos se cierran, cuál pellizca, dónde va el pulgar, qué se resalta
            int[] fs = {0, 0, 0, 0};          // 0 estirado, 1 cerrado
            boolean[] fh = new boolean[4];
            int pin = -1;
            String th = "open";
            boolean thHi = false;
            if (g == G_N) {
                if (joy == 1 || joy == 3) { fs = new int[]{1, 1, 1, 1}; th = "across"; thHi = joy == 1; }
                else if (joy == 2) pin = 1;
            } else switch (g) {
                case G_PI: pin = 0; break;
                case G_PM: pin = 1; break;
                case G_PR: pin = 2; break;
                case G_PL: pin = 3; break;
                case G_FIST: fs = new int[]{1, 1, 1, 1}; th = "across"; fh = new boolean[]{true, true, true, true}; break;
                case G_THUMBUP: fs = new int[]{1, 1, 1, 1}; th = "up"; thHi = true; break;
                case G_GRAB: fs = new int[]{0, 1, 1, 1}; fh = new boolean[]{false, true, true, true}; break;
                case G_POINT: fs = new int[]{0, 1, 1, 1}; fh[0] = true; th = "across"; break;
                case G_THUMBPALM: th = "palm"; thHi = true; break;
                default: break;
            }
            // Joystick: mientras se mantiene, la mano (o el pulgar) se mueve
            float wob = (float) Math.sin(hold * Math.PI * 2);
            float hx = 0, hy = 0, tdx = 0, tdy = 0;
            if (g == G_N && joy == 3) { hx = 7 * wob; hy = -4 * (float) Math.sin(hold * Math.PI * 4); }
            else if (g == G_N && joy == 2) { hx = 6 * wob; hy = -5 * (float) Math.sin(hold * Math.PI * 4); }
            else if (g == G_N && joy == 1) { tdx = 5 * wob; tdy = -4 * (float) Math.sin(hold * Math.PI * 4); }
            c.translate(hx * k, hy * k);
            // palma
            path.reset();
            path.moveTo(34, 50); path.cubicTo(34, 44, 80, 44, 80, 52); path.lineTo(79, 80); path.cubicTo(78, 94, 40, 96, 36, 84); path.close();
            st.setColor(EDGE); st.setStrokeWidth(3); c.drawPath(path, st);
            fl.setColor(SK); c.drawPath(path, fl);
            float tx = 0, ty = 0;
            if (pin >= 0) { tx = FX[pin] - 9; ty = BY[pin] - LEN[pin] * 0.55f + 9; }
            // dedos (de atrás para adelante); el que pellizca va después del pulgar (adelante)
            for (int i = 3; i >= 0; i--) {
                if (i == pin) continue;
                float x = FX[i], b = BY[i], L = LEN[i], w = W[i], sp = (i - 1.5f) * 2.5f;
                if (fs[i] == 0) {   // estirado (y, si se resalta, se ilumina al hacer el gesto)
                    seg(c, w, fh[i] ? mixC(SK, HI, k) : SK, x, b, x + sp / 2, b - L / 2, x + sp, b - L);
                } else {            // se cierra sobre la palma
                    float kk = k;
                    float mx = mix(x + sp / 2, x, kk), my = mix(b - L / 2, b - 7, kk);
                    float ex = mix(x + sp, x, kk), ey = mix(b - L, b + 10, kk);
                    int top = fh[i] ? mixC(SK, HI, kk) : SK, bot = fh[i] ? mixC(SK, HIF, kk) : mixC(SK, FOLD, kk);
                    seg(c, w + 0.5f * kk, top, x, b + 2 * kk, mx, my, -1000, 0);
                    seg(c, w - kk, bot, mx, my, ex, ey, -1000, 0);
                }
            }
            // pulgar
            final float[] open = {38, 82, 26, 72, 20, 62, 16, 50};
            float[] tgt;
            switch (pin >= 0 ? "pinch" : th) {
                case "across": tgt = new float[]{38, 82, 32, 66, 44, 58, 60, 58}; break;
                case "up": tgt = new float[]{38, 76, 30, 68, 28, 52, 28, 30}; break;
                case "palm": tgt = new float[]{38, 82, 40, 76, 50, 72, 58, 70}; break;
                case "pinch": tgt = new float[]{38, 82, 22, 68, tx - 10, ty + 16, tx, ty}; break;
                default: tgt = open;
            }
            float[] tq = new float[8];
            for (int q = 0; q < 8; q++) tq[q] = mix(open[q], tgt[q], k);
            tq[6] += tdx * k; tq[7] += tdy * k; tq[4] += tdx * k * 0.5f; tq[5] += tdy * k * 0.5f;
            cub(c, 11, (thHi || pin >= 0) ? mixC(SK, HI, k) : SK, tq);
            if (pin >= 0) {   // el dedo que pellizca: se dobla hasta la punta del pulgar
                float x = FX[pin], b = BY[pin], L = LEN[pin], sp = (pin - 1.5f) * 2.5f;
                seg(c, W[pin], mixC(SK, HI, k), x, b, mix(x + sp / 2, x, k), mix(b - L / 2, b - L * 0.55f, k), mix(x + sp, tx, k), mix(b - L, ty, k));
            }
            // dónde se tocan (aparece al hacer el gesto)
            float da = k * k;
            if (pin >= 0) dot(c, tx, ty, 5.5f, da);
            if (g == G_FIST) dot(c, 58, 58, 5, da);
            if (g == G_THUMBUP) dot(c, 28, 28, 5, da);
            if (g == G_POINT) dot(c, FX[0] - 3.75f, BY[0] - LEN[0], 5, da);
            if (g == G_GRAB) dot(c, 64, 56, 5, da);
            if (g == G_THUMBPALM) dot(c, 58, 70, 5, da);
            // joystick: flechas de hacia dónde se mueve
            if (g == G_N && joy == 1) { arrow(c, 60, 58, 60, 34, k); arrow(c, 60, 58, 86, 58, k); }
            else if (g == G_N && joy == 2) { arrow(c, tx, ty, tx, ty - 20, k); arrow(c, tx, ty, tx + 22, ty, k); }
            else if (g == G_N && joy == 3) { arrow(c, 84, 72, 98, 72, k); arrow(c, 84, 72, 84, 52, k); }
            else if (g == G_N && joy == 0) {
                st.setColor(Color.rgb(160, 160, 166)); st.setStrokeWidth(5);
                c.drawLine(70, 66, 92, 88, st); c.drawLine(92, 66, 70, 88, st);
            }
            c.restore();
        }
        @Override public void setAlpha(int a) { }
        @Override public void setColorFilter(ColorFilter cf) { }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }
}
