package org.questvrbridge.client;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Idiomas: inglés (por defecto) y español.
 * El código está escrito en español; {@link #t} devuelve la traducción al inglés cuando corresponde
 * (si falta una traducción, se muestra el texto original). Las traducciones están en {@link I18nEn}.
 */
final class I18n {
    private I18n() { }

    private static SharedPreferences prefs;

    static void init(Context c) {
        if (prefs == null) prefs = c.getApplicationContext().getSharedPreferences("qvb", Context.MODE_PRIVATE);
    }

    /** "en" (por defecto) o "es". */
    static String lang() { return prefs == null ? "en" : prefs.getString("lang", "en"); }

    static boolean english() { return !"es".equals(lang()); }

    static void setLang(String l) { if (prefs != null) prefs.edit().putString("lang", "es".equals(l) ? "es" : "en").apply(); }

    private static final Map<String, String> EN = new HashMap<>();
    static { I18nEn.fill(EN); }

    /** Texto en el idioma elegido. */
    static String t(String es) {
        if (es == null || !english()) return es;
        String en = EN.get(es);
        return en != null ? en : es;
    }

    /** Texto con valores: t(«Conectado a %s», nombre). */
    static String f(String es, Object... args) { return String.format(Locale.US, t(es), args); }
}
