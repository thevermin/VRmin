package org.questvrbridge.client;

import android.content.Context;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import static org.questvrbridge.client.Ui.dp;

/**
 * Teclado virtual para escribir en la Mac (en español: con ñ y tildes).
 * Lo que se toca se manda a la Mac como texto (no depende de la distribución de su teclado);
 * las teclas especiales (borrar, enter, flechas…) y los atajos (⌘C, ⌘V…) van como teclas.
 */
final class KeyboardUi {
    interface Host { void closeKeyboard(); }

    private final Context ctx;
    private final Host host;
    private final FrameLayout root;
    private final LinearLayout rows;
    private boolean shift = false, caps = false, symbols = false;
    private String accent = null;   // tilde pendiente: la próxima vocal sale con tilde
    private long lastShift = 0;

    KeyboardUi(Context c, Host h) {
        ctx = c; host = h;
        root = new FrameLayout(c);
        root.setBackgroundColor(Color.TRANSPARENT);
        rows = new LinearLayout(c);
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setBackground(Ui.glass(c, 26, 240));
        rows.setPadding(dp(c, 14), dp(c, 12), dp(c, 14), dp(c, 14));
        root.addView(rows, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        render();
    }

    View view() { return root; }

    private static final String[][] LETTERS = {
            {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
            {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
            {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ñ"},
            {"z", "x", "c", "v", "b", "n", "m", ",", ".", "-"}};
    private static final String[][] SYMBOLS = {
            {"!", "@", "#", "$", "%", "&", "/", "(", ")", "="},
            {"?", "¿", "¡", "'", "\"", ":", ";", "_", "+", "*"},
            {"<", ">", "[", "]", "{", "}", "\\", "|", "^", "~"},
            {"€", "°", "`", "´", "¨", "·", "ç", "…", "«", "»"}};

    private void send(String json) { Link.I.send(json); }
    private void text(String t) {
        try { send(new JSONObject().put("type", "kb").put("text", t).toString()); } catch (Exception ignored) { }
    }
    private void key(String k, String... mods) {
        try {
            JSONObject o = new JSONObject().put("type", "kb").put("k", k);
            if (mods.length > 0) { JSONArray a = new JSONArray(); for (String m : mods) a.put(m); o.put("mods", a); }
            send(o.toString());
        } catch (Exception ignored) { }
    }

    /// Letra con tilde si había una pendiente (á é í ó ú, ü con diéresis)
    private String withAccent(String ch) {
        if (accent == null) return ch;
        String a = accent; accent = null;
        String base = "aeiouAEIOU", acute = "áéíóúÁÉÍÓÚ", diae = "äëïöüÄËÏÖÜ";
        int i = base.indexOf(ch);
        if (i < 0) return a + ch;
        return String.valueOf((a.equals("¨") ? diae : acute).charAt(i));
    }

    private void tap(String ch) {
        if (ch.equals("´") || ch.equals("¨")) { accent = ch; render(); return; }
        boolean had = accent != null;
        String out = (shift || caps) && !symbols ? ch.toUpperCase() : ch;
        text(withAccent(out));
        if (shift && !caps) { shift = false; render(); }
        else if (had) render();
    }

    private TextView keyView(String label, float weight, boolean dark, Runnable act) {
        TextView k = Ui.label(ctx, label, label.length() > 2 ? 15 : 21, Ui.TEXT, true);
        k.setGravity(Gravity.CENTER);
        k.setBackground(Ui.hoverable(ctx, dark ? Color.argb(70, 255, 255, 255) : Color.argb(34, 255, 255, 255), 10, false));
        k.setClickable(true);
        k.setOnClickListener(v -> act.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(ctx, 54), weight);
        lp.setMargins(dp(ctx, 3), dp(ctx, 3), dp(ctx, 3), dp(ctx, 3));
        k.setLayoutParams(lp);
        return k;
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(ctx);
        r.setOrientation(LinearLayout.HORIZONTAL);
        rows.addView(r, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return r;
    }

    private void render() {
        rows.removeAllViews();
        // Fila de arriba: Esc, Tab, atajos y cerrar
        LinearLayout top = row();
        top.addView(keyView("Esc", 1, true, () -> key("escape")));
        top.addView(keyView("Tab", 1, true, () -> key("tab")));
        top.addView(keyView("⌘A", 1, true, () -> key("a", "cmd")));
        top.addView(keyView("⌘C", 1, true, () -> key("c", "cmd")));
        top.addView(keyView("⌘V", 1, true, () -> key("v", "cmd")));
        top.addView(keyView("⌘X", 1, true, () -> key("x", "cmd")));
        top.addView(keyView("⌘Z", 1, true, () -> key("z", "cmd")));
        TextView close = keyView("✕", 1f, true, host::closeKeyboard);
        close.setTextColor(Ui.ACCENT_TEXT);
        close.setTextSize(24);
        top.addView(close);

        String[][] set = symbols ? SYMBOLS : LETTERS;
        for (int r = 0; r < set.length; r++) {
            LinearLayout row = row();
            if (r == 3 && !symbols) {
                String sl = caps ? "⇪" : "⇧";
                TextView sh = keyView(sl, 1.4f, true, () -> {
                    long now = System.currentTimeMillis();
                    if (caps) { caps = false; shift = false; }
                    else if (shift && now - lastShift < 400) caps = true;
                    else shift = !shift;
                    lastShift = now;
                    render();
                });
                if (shift || caps) { sh.setBackground(Ui.hoverable(ctx, Ui.ACCENT, 10, false)); sh.setTextColor(Ui.ACCENT_DARK); }
                row.addView(sh);
            }
            for (String ch : set[r]) {
                String shown = (shift || caps) && !symbols ? ch.toUpperCase() : ch;
                row.addView(keyView(shown, 1, false, () -> tap(ch)));
            }
            if (r == 0) row.addView(keyView("⌫", 1.4f, true, () -> { accent = null; key("delete"); }));
            if (r == 2) row.addView(keyView("´", 1, accent != null, () -> { accent = accent == null ? "´" : null; render(); }));
            if (r == 1) row.addView(keyView("⏎", 1.4f, true, () -> key("return")));
        }
        LinearLayout bottom = row();
        bottom.addView(keyView(symbols ? "abc" : "#+=", 1.4f, true, () -> { symbols = !symbols; render(); }));
        bottom.addView(keyView("@", 1, false, () -> text("@")));
        bottom.addView(keyView(I18n.t("espacio"), 5, false, () -> { accent = null; text(" "); }));
        bottom.addView(keyView("←", 1, true, () -> key("left")));
        bottom.addView(keyView("↑", 1, true, () -> key("up")));
        bottom.addView(keyView("↓", 1, true, () -> key("down")));
        bottom.addView(keyView("→", 1, true, () -> key("right")));
    }
}
