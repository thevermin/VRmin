package org.questvrbridge.client;

import android.content.Context;
import android.graphics.Color;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Estilo inspirado en visionOS: vidrio translúcido con un brillo suave arriba, esquinas muy redondeadas,
 * botones tipo píldora e íconos de línea (ver Icons). Todo lo que tiene esquinas usa los mismos radios.
 */
final class Ui {
    // Paleta «Grafito + ámbar»: grises neutros, algo de transparencia, y el ámbar del logo para lo importante
    static final int TEXT = Color.rgb(243, 243, 244);
    static final int TEXT2 = Color.argb(163, 243, 243, 244);
    static final int TEXT3 = Color.argb(110, 243, 243, 244);
    static final int BG = Color.rgb(20, 20, 22);
    static final int SEL = Color.argb(34, 255, 255, 255);    // elegido sin énfasis
    static final int FILL = Color.argb(16, 255, 255, 255);   // filas y tarjetas
    static final int ACCENT = Color.rgb(230, 168, 74);       // ámbar
    static final int ACCENT_TEXT = Color.rgb(242, 194, 122); // ámbar claro (texto sobre gris)
    static final int ACCENT_DARK = Color.rgb(26, 19, 8);     // texto sobre ámbar
    static final int ACCENT_SEL = Color.argb(41, 230, 168, 74);   // fondo de lo elegido (pestaña)
    static final int ACCENT_SEG = Color.argb(56, 230, 168, 74);   // opción elegida
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    static int dp(Context c, float v) { return (int) (v * c.getResources().getDisplayMetrics().density + 0.5f); }

    /**
     * Radio de las esquinas: casi rectas, con un toque de curva (lo muy redondeado se ve genérico).
     * Píldoras (999) → 7 dp · tarjetas (26) → 10 · filas (20) → 8 · cuadraditos (12) → 5.
     */
    static float rad(Context c, float radiusDp) {
        float r = radiusDp >= 100 ? 7f : Math.min(10f, radiusDp * 0.4f);
        return dp(c, r);
    }

    /** Vidrio: un poco más claro arriba (como si le diera la luz) y un borde fino que brilla apenas. */
    static GradientDrawable glass(Context c, float radiusDp, int alpha) {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[]{Color.argb(alpha, 56, 56, 60), Color.argb(alpha, 40, 40, 44)});
        g.setCornerRadius(rad(c, radiusDp));
        g.setStroke(dp(c, 1), Color.argb(40, 255, 255, 255));
        return g;
    }

    /** Ícono en un cuadradito redondeado (las filas de las listas). */
    static android.view.View iconTile(Context c, String icon) {
        android.view.View v = new android.view.View(c);
        GradientDrawable g = new GradientDrawable();
        g.setColor(Color.argb(34, 255, 255, 255));
        g.setCornerRadius(rad(c, 12));
        v.setBackground(g);
        v.setForeground(Icons.of(c, icon, 22, TEXT));
        v.setForegroundGravity(Gravity.CENTER);
        return v;
    }

    /** Los íconos se piden por nombre; por las dudas también se aceptan los emoji de antes. */
    static String iconName(String s) {
        switch (s) {
            case "💻": return "laptop";
            case "🖥": return "display";
            case "➕": return "addDisplay";
            case "❐": return "window";
            default: return s;
        }
    }

    private static GradientDrawable fill(Context c, int color, float radiusDp, boolean oval) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        if (oval) g.setShape(GradientDrawable.OVAL); else g.setCornerRadius(rad(c, radiusDp));
        return g;
    }

    /** Fondo que se ilumina al apuntar con el control (como el "hover" de visionOS). */
    static Drawable hoverable(Context c, int base, float radiusDp, boolean oval) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_pressed}, fill(c, Color.argb(110, 255, 255, 255), radiusDp, oval));
        s.addState(new int[]{android.R.attr.state_hovered}, fill(c, Color.argb(60, 255, 255, 255), radiusDp, oval));
        s.addState(new int[]{}, fill(c, base, radiusDp, oval));
        return s;
    }

    static TextView label(Context c, String text, float sp, int color, boolean medium) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (medium) t.setTypeface(MEDIUM);
        return t;
    }

    /** Fila de lista: título y subtítulo opcional. */
    static LinearLayout row(Context c, String icon, String title, String subtitle) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(c, 12), dp(c, 10), dp(c, 16), dp(c, 10));
        r.setBackground(hoverable(c, FILL, 20, false));
        r.setClickable(true);
        r.setFocusable(true);
        r.addView(iconTile(c, iconName(icon)), new LinearLayout.LayoutParams(dp(c, 44), dp(c, 44)));
        LinearLayout col = new LinearLayout(c);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(c, 14), 0, 0, 0);
        TextView t = label(c, title, 18, TEXT, true);
        t.setSingleLine(true);
        t.setEllipsize(android.text.TextUtils.TruncateAt.END);
        col.addView(t);
        if (subtitle != null && !subtitle.isEmpty()) {
            TextView s = label(c, subtitle, 14, TEXT2, false);
            s.setSingleLine(true);
            s.setEllipsize(android.text.TextUtils.TruncateAt.END);
            col.addView(s);
        }
        r.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(c, 8);
        r.setLayoutParams(lp);
        return r;
    }

    /** Botón principal: ámbar con texto oscuro (la acción más importante de la pantalla). */
    static TextView primary(Context c, String text) {
        TextView b = pill(c, text);
        b.setTextColor(ACCENT_DARK);
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_pressed}, fill(c, Color.rgb(200, 144, 60), 999, false));
        s.addState(new int[]{android.R.attr.state_hovered}, fill(c, Color.rgb(242, 186, 98), 999, false));
        s.addState(new int[]{}, fill(c, ACCENT, 999, false));
        b.setBackground(s);
        return b;
    }

    /** Ícono a la izquierda del texto de un botón o pestaña. */
    static void leadingIcon(TextView t, String icon, float sizeDp, int color) {
        t.setCompoundDrawablesRelativeWithIntrinsicBounds(Icons.of(t.getContext(), icon, sizeDp, color), null, null, null);
        t.setCompoundDrawablePadding(dp(t.getContext(), 12));
    }

    /** Botón píldora. */
    static TextView pill(Context c, String text) {
        TextView b = label(c, text, 16, TEXT, true);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(c, 20), dp(c, 11), dp(c, 20), dp(c, 11));
        b.setBackground(hoverable(c, Color.argb(34, 255, 255, 255), 999, false));
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    /** Botón redondo (para la esquina). */
    static TextView circle(Context c, String text) {
        TextView b = label(c, text, 22, TEXT, true);
        b.setGravity(Gravity.CENTER);
        b.setBackground(hoverable(c, Color.argb(34, 255, 255, 255), 12, false));   // (cuadradito, no círculo)
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    /** Título de sección: en mayúsculas chicas y espaciadas, con una rayita ámbar adelante (la marca de VRmin). */
    static TextView section(Context c, String text) {
        TextView t = label(c, text.toUpperCase(java.util.Locale.ROOT), 12.5f, TEXT2, true);
        t.setLetterSpacing(0.12f);
        t.setPadding(dp(c, 4), dp(c, 26), 0, dp(c, 4));
        GradientDrawable tick = new GradientDrawable();
        tick.setColor(ACCENT);
        tick.setSize(dp(c, 10), dp(c, 2));
        t.setCompoundDrawablesRelativeWithIntrinsicBounds(tick, null, null, null);
        t.setCompoundDrawablePadding(dp(c, 8));
        return t;
    }

    /** Control deslizante con el ámbar de la paleta. */
    static void tint(android.widget.SeekBar sb) {
        sb.setProgressTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        sb.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(Color.argb(70, 255, 255, 255)));
        sb.setThumbTintList(android.content.res.ColorStateList.valueOf(TEXT));
    }

    /**
     * Logo de VRmin: «VR» en negrita y «min» fino, y el punto de la «i» es un visor ámbar
     * (la misma forma que el ícono de la app).
     */
    static final class Logo extends android.view.View {
        private final android.graphics.Paint bold = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG),
                thin = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG),
                visor = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Path path = new android.graphics.Path();
        private final float size;

        Logo(Context c, float sizeSp) {
            super(c);
            size = sizeSp * c.getResources().getDisplayMetrics().scaledDensity;
            bold.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
            thin.setTypeface(Typeface.create("sans-serif-thin", Typeface.NORMAL));
            for (android.graphics.Paint p : new android.graphics.Paint[]{bold, thin}) {
                p.setTextSize(size);
                p.setColor(TEXT);
                p.setLetterSpacing(-0.03f);
            }
            visor.setColor(ACCENT);
            // visor en una grilla de 26×14: píldora con el arco de la nariz abajo al medio
            path.moveTo(7, 0); path.lineTo(19, 0);
            path.arcTo(new RectF(12, 0, 26, 14), -90, 180);
            path.lineTo(16.6f, 14); path.quadTo(13, 7.2f, 9.4f, 14);
            path.lineTo(7, 14);
            path.arcTo(new RectF(0, 0, 14, 14), 90, 180);
            path.close();
            setContentDescription("VRmin");
        }

        private float width() { return bold.measureText("VR") + thin.measureText("m\u0131n"); }

        @Override protected void onMeasure(int w, int h) {
            setMeasuredDimension(resolveSize((int) Math.ceil(width() + size * 0.1f), w), resolveSize((int) Math.ceil(size * 1.35f), h));
        }

        @Override protected void onDraw(android.graphics.Canvas c) {
            float x = (getWidth() - width()) / 2f, base = getHeight() - size * 0.28f;
            c.drawText("VR", x, base, bold);
            x += bold.measureText("VR");
            c.drawText("m", x, base, thin);
            x += thin.measureText("m");
            float iw = thin.measureText("\u0131");
            c.drawText("\u0131", x, base, thin);
            // el visor va donde iría el punto de la «i»
            float vw = size * 0.53f, k = vw / 26f;
            c.save();
            c.translate(x + iw / 2f - vw / 2f, base - size * 0.66f - 14 * k);
            c.scale(k, k);
            c.drawPath(path, visor);
            c.restore();
            x += iw;
            c.drawText("n", x, base, thin);
        }
    }

    /** Contenedor que mantiene la forma de la imagen de la Mac (sin estirarla). */
    static class AspectFrame extends FrameLayout {
        private float aspect = 16f / 10f;
        AspectFrame(Context c) { super(c); }
        void setAspect(float a) { if (a > 0 && Math.abs(a - aspect) > 0.001f) { aspect = a; requestLayout(); } }
        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            if (h > 0 && w / (float) h > aspect) w = (int) (h * aspect); else h = (int) (w / aspect);
            super.onMeasure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
        }
    }

}
