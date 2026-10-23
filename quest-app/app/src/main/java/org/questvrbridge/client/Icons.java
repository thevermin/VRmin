package org.questvrbridge.client;

import android.content.Context;
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

/**
 * Íconos de línea, todos del mismo estilo (trazo redondeado y parejo, como los símbolos de visionOS).
 * Se dibujan en una grilla de 24×24 y se escalan al tamaño pedido, así se ven nítidos en cualquier pantalla.
 */
final class Icons {
    private Icons() { }

    /** Pone un ícono centrado sobre una vista (botón), en lugar de texto. */
    static void on(View v, String name, float sizeDp) {
        if (v instanceof android.widget.TextView) ((android.widget.TextView) v).setText("");
        v.setForeground(new Icon(v.getContext(), name, sizeDp, Color.WHITE));
        v.setForegroundGravity(Gravity.CENTER);
    }

    static Icon of(Context c, String name, float sizeDp, int color) { return new Icon(c, name, sizeDp, color); }

    static final class Icon extends Drawable {
        private final String name;
        private final int size;
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path p = new Path();
        private final RectF r = new RectF();

        Icon(Context c, String name, float sizeDp, int color) {
            this.name = name;
            size = Ui.dp(c, sizeDp);
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeCap(Paint.Cap.ROUND);
            stroke.setStrokeJoin(Paint.Join.ROUND);
            stroke.setColor(color);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(color);
        }

        @Override public int getIntrinsicWidth() { return size; }
        @Override public int getIntrinsicHeight() { return size; }
        @Override public void setAlpha(int a) { stroke.setAlpha(a); fill.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) { stroke.setColorFilter(f); fill.setColorFilter(f); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }

        private void line(Canvas c, float x0, float y0, float x1, float y1) { c.drawLine(x0, y0, x1, y1, stroke); }
        private void rrect(Canvas c, float l, float t, float rr, float b, float rad) { r.set(l, t, rr, b); c.drawRoundRect(r, rad, rad, stroke); }
        private void dot(Canvas c, float x, float y, float rad) { c.drawCircle(x, y, rad, fill); }

        @Override
        public void draw(Canvas c) {
            android.graphics.Rect b = getBounds();
            float s = Math.min(b.width(), b.height()) / 24f;
            c.save();
            c.translate(b.centerX() - 12 * s, b.centerY() - 12 * s);
            c.scale(s, s);
            stroke.setStrokeWidth(1.7f);
            p.reset();
            switch (name) {
                case "menu":
                    line(c, 5, 7, 19, 7); line(c, 5, 12, 19, 12); line(c, 5, 17, 19, 17);
                    break;
                case "cube":   // 3D
                    p.moveTo(12, 3); p.lineTo(20, 7.5f); p.lineTo(20, 16.5f); p.lineTo(12, 21); p.lineTo(4, 16.5f); p.lineTo(4, 7.5f); p.close();
                    p.moveTo(4, 7.5f); p.lineTo(12, 12); p.lineTo(20, 7.5f); p.moveTo(12, 12); p.lineTo(12, 21);
                    c.drawPath(p, stroke);
                    break;
                case "size":
                    line(c, 13.5f, 10.5f, 19, 5); line(c, 14.5f, 5, 19, 5); line(c, 19, 5, 19, 9.5f);
                    line(c, 10.5f, 13.5f, 5, 19); line(c, 5, 14.5f, 5, 19); line(c, 5, 19, 9.5f, 19);
                    break;
                case "curve":   // pantalla curva
                    p.moveTo(4, 7); p.quadTo(12, 3, 20, 7); p.lineTo(20, 17); p.quadTo(12, 13, 4, 17); p.close();
                    c.drawPath(p, stroke);
                    break;
                case "filter":   // círculo mitad lleno (como «ajustes de imagen»)
                    c.drawCircle(12, 12, 8, stroke);
                    r.set(4, 4, 20, 20);
                    c.drawArc(r, 90, 180, true, fill);
                    break;
                case "addDisplay":
                    rrect(c, 2.5f, 6, 15.5f, 15.5f, 2);
                    line(c, 9, 15.5f, 9, 19); line(c, 6, 19, 12, 19);
                    line(c, 19.5f, 4, 19.5f, 10); line(c, 16.5f, 7, 22.5f, 7);
                    break;
                case "stats":
                    line(c, 6, 19, 6, 13); line(c, 12, 19, 12, 8); line(c, 18, 19, 18, 4);
                    break;
                case "recenter":
                    c.drawCircle(12, 12, 6.5f, stroke); dot(c, 12, 12, 2);
                    line(c, 12, 2.5f, 12, 5); line(c, 12, 19, 12, 21.5f); line(c, 2.5f, 12, 5, 12); line(c, 19, 12, 21.5f, 12);
                    break;
                case "keyboard":
                    rrect(c, 2.5f, 6.5f, 21.5f, 17.5f, 2.5f);
                    dot(c, 6.5f, 10, 0.9f); dot(c, 10, 10, 0.9f); dot(c, 13.5f, 10, 0.9f); dot(c, 17.5f, 10, 0.9f);
                    line(c, 8, 14, 16, 14);
                    break;
                case "close":
                    line(c, 7, 7, 17, 17); line(c, 17, 7, 7, 17);
                    break;
                case "pause":
                    line(c, 9, 6, 9, 18); line(c, 15, 6, 15, 18);
                    break;
                case "minus":
                    line(c, 6, 12, 18, 12);
                    break;
                case "plus":
                    line(c, 12, 6, 12, 18); line(c, 6, 12, 18, 12);
                    break;
                case "laptop":
                    rrect(c, 5, 5.5f, 19, 15, 1.5f); line(c, 3, 18.5f, 21, 18.5f);
                    break;
                case "swapDisplay":   // cambiar de monitor: dos pantallas y una flecha entre ellas
                    rrect(c, 2.5f, 4, 12.5f, 11.5f, 1.5f); rrect(c, 11.5f, 12.5f, 21.5f, 20, 1.5f);
                    p.moveTo(15, 4.5f); p.quadTo(19.5f, 4.5f, 19.5f, 9); c.drawPath(p, stroke);
                    line(c, 19.5f, 9, 17.5f, 7.2f); line(c, 19.5f, 9, 21.3f, 7.2f);
                    break;
                case "display":
                    rrect(c, 3, 4, 21, 15.5f, 2); line(c, 12, 15.5f, 12, 19.5f); line(c, 8, 19.5f, 16, 19.5f);
                    break;
                case "window":
                    rrect(c, 3, 5, 21, 19, 2.5f); line(c, 3, 9, 21, 9);
                    dot(c, 6, 7, 0.9f); dot(c, 8.6f, 7, 0.9f);
                    break;
                case "gamepad":
                    rrect(c, 2.5f, 7.5f, 21.5f, 17.5f, 5);
                    line(c, 6, 12.5f, 10, 12.5f); line(c, 8, 10.5f, 8, 14.5f);
                    dot(c, 15.5f, 11.3f, 1.1f); dot(c, 18, 13.7f, 1.1f);
                    break;
                case "room":   // sillón
                    rrect(c, 6, 6, 18, 12, 2);
                    rrect(c, 3, 10.5f, 21, 17, 2.5f);
                    line(c, 6, 17, 6, 19.5f); line(c, 18, 17, 18, 19.5f);
                    break;
                case "sliders":   // ajustes
                    line(c, 4, 7, 20, 7); line(c, 4, 12, 20, 12); line(c, 4, 17, 20, 17);
                    dot(c, 9, 7, 2.2f); dot(c, 15, 12, 2.2f); dot(c, 7, 17, 2.2f);
                    break;
                case "visor":   // entrar en VR
                    rrect(c, 2.5f, 7.5f, 21.5f, 17, 4);
                    p.moveTo(9.5f, 17); p.quadTo(12, 13, 14.5f, 17);
                    c.drawPath(p, stroke);
                    break;
                case "exit":   // salir
                    line(c, 15, 12, 4, 12); line(c, 8, 8, 4, 12); line(c, 4, 12, 8, 16);
                    p.moveTo(13, 4.5f); p.lineTo(19.5f, 4.5f); p.lineTo(19.5f, 19.5f); p.lineTo(13, 19.5f);
                    c.drawPath(p, stroke);
                    break;
                case "lock":
                case "unlock":
                    rrect(c, 5, 11, 19, 20, 2.5f);
                    p.moveTo(8, 11); p.lineTo(8, 8);
                    p.cubicTo(8, 3, 16, 3, 16, 8);
                    if (name.equals("lock")) p.lineTo(16, 11);
                    c.drawPath(p, stroke);
                    break;
                case "back":
                    line(c, 14.5f, 6, 8.5f, 12); line(c, 8.5f, 12, 14.5f, 18);
                    break;
                case "down":
                    line(c, 6, 9.5f, 12, 15.5f); line(c, 12, 15.5f, 18, 9.5f);
                    break;
                case "globe":   // idioma
                    c.drawCircle(12, 12, 8.5f, stroke);
                    r.set(8, 3.5f, 16, 20.5f); c.drawOval(r, stroke);
                    line(c, 3.5f, 12, 20.5f, 12);
                    break;
                case "hand":   // controles: una mano abierta
                    p.moveTo(7.5f, 13); p.lineTo(7.5f, 6.5f); p.quadTo(7.5f, 5, 9, 5); p.quadTo(10.5f, 5, 10.5f, 6.5f); p.lineTo(10.5f, 11);
                    p.moveTo(10.5f, 6.5f); p.lineTo(10.5f, 4.5f); p.quadTo(10.5f, 3, 12, 3); p.quadTo(13.5f, 3, 13.5f, 4.5f); p.lineTo(13.5f, 11);
                    p.moveTo(13.5f, 5.5f); p.quadTo(13.5f, 4, 15, 4); p.quadTo(16.5f, 4, 16.5f, 5.5f); p.lineTo(16.5f, 12);
                    p.moveTo(16.5f, 8.5f); p.quadTo(16.5f, 7, 18, 7); p.quadTo(19.5f, 7, 19.5f, 8.5f); p.lineTo(19.5f, 14.5f);
                    p.quadTo(19.5f, 21, 13.5f, 21); p.quadTo(10, 21, 8, 18); p.lineTo(4.8f, 13.6f); p.quadTo(4, 12.2f, 5.3f, 11.6f); p.quadTo(6.5f, 11, 7.5f, 13);
                    c.drawPath(p, stroke);
                    break;
                case "chevron":
                    line(c, 9.5f, 6, 15.5f, 12); line(c, 15.5f, 12, 9.5f, 18);
                    break;
                case "check":
                    line(c, 5.5f, 12.5f, 10, 17); line(c, 10, 17, 18.5f, 7.5f);
                    break;
                case "help":   // signo de pregunta en un círculo
                    c.drawCircle(12, 12, 9, stroke);
                    p.moveTo(9.3f, 9.6f); p.cubicTo(9.3f, 6.2f, 14.8f, 6.2f, 14.8f, 9.6f); p.cubicTo(14.8f, 11.6f, 12, 11.6f, 12, 13.8f);
                    c.drawPath(p, stroke);
                    dot(c, 12, 16.8f, 1.1f);
                    break;
                case "eye":   // Modo Centinela: un ojo que te mira
                    p.moveTo(2.5f, 12); p.quadTo(12, 3, 21.5f, 12); p.quadTo(12, 21, 2.5f, 12); p.close();
                    c.drawPath(p, stroke);
                    c.drawCircle(12, 12, 3.2f, stroke); dot(c, 12, 12, 1.2f);
                    break;
                case "record":   // grabar: punto lleno dentro de un círculo
                    c.drawCircle(12, 12, 8.5f, stroke); dot(c, 12, 12, 4.5f);
                    break;
                case "portal":   // un óvalo punteado (un hueco hacia el cuarto real)
                    stroke.setPathEffect(new android.graphics.DashPathEffect(new float[]{2.4f, 2.2f}, 0));
                    r.set(4, 3.5f, 20, 20.5f); c.drawOval(r, stroke);
                    stroke.setPathEffect(null);
                    dot(c, 12, 12, 1.6f);
                    break;
                case "laser":   // un rayo que sale de un punto y toca algo
                    dot(c, 5, 19, 2.2f); line(c, 6.5f, 17.5f, 17, 7);
                    c.drawCircle(18.5f, 5.5f, 2.6f, stroke);
                    break;
                case "laserOff":   // el láser no toca: el rayo tachado
                    dot(c, 5, 19, 2.2f); line(c, 6.5f, 17.5f, 17, 7);
                    c.drawCircle(18.5f, 5.5f, 2.6f, stroke);
                    line(c, 4, 4, 20, 20);
                    break;
                case "watch":   // menú en la mano: reloj
                    rrect(c, 7, 7, 17, 17, 3);
                    line(c, 9, 7, 9.8f, 2.8f); line(c, 15, 7, 14.2f, 2.8f); line(c, 9, 17, 9.8f, 21.2f); line(c, 15, 17, 14.2f, 21.2f);
                    line(c, 12, 12, 12, 9.5f); line(c, 12, 12, 13.8f, 13);
                    break;
                case "cable":   // enchufe USB-C
                    rrect(c, 7, 3, 17, 12, 3);
                    line(c, 10, 6.5f, 14, 6.5f);
                    p.moveTo(12, 12); p.lineTo(12, 16); p.quadTo(12, 20.5f, 16.5f, 20.5f); p.lineTo(20, 20.5f);
                    c.drawPath(p, stroke);
                    break;
                case "camera":
                    rrect(c, 3, 7, 21, 19, 2.5f);
                    p.moveTo(8, 7); p.lineTo(9.5f, 4.5f); p.lineTo(14.5f, 4.5f); p.lineTo(16, 7); c.drawPath(p, stroke);
                    c.drawCircle(12, 13, 3.5f, stroke);
                    break;
                case "bulb":   // un consejo
                    p.moveTo(9, 16); p.cubicTo(9, 13.5f, 6, 12.5f, 6, 9); p.cubicTo(6, 2, 18, 2, 18, 9); p.cubicTo(18, 12.5f, 15, 13.5f, 15, 16); p.close();
                    c.drawPath(p, stroke);
                    line(c, 9.5f, 19, 14.5f, 19); line(c, 10.5f, 21.5f, 13.5f, 21.5f);
                    break;
                case "move":   // flechas en cruz (moverte)
                    line(c, 12, 3, 12, 21); line(c, 3, 12, 21, 12);
                    line(c, 12, 3, 9.5f, 5.5f); line(c, 12, 3, 14.5f, 5.5f); line(c, 12, 21, 9.5f, 18.5f); line(c, 12, 21, 14.5f, 18.5f);
                    line(c, 3, 12, 5.5f, 9.5f); line(c, 3, 12, 5.5f, 14.5f); line(c, 21, 12, 18.5f, 9.5f); line(c, 21, 12, 18.5f, 14.5f);
                    break;
                case "pinch":   // índice y pulgar juntándose
                    p.moveTo(6, 20); p.quadTo(5, 12, 9, 8); p.lineTo(14.5f, 4.5f);
                    p.moveTo(9, 20); p.quadTo(11, 15, 15.5f, 13.5f); p.lineTo(18, 12.8f);
                    c.drawPath(p, stroke);
                    dot(c, 17.2f, 8.4f, 1.6f);
                    break;
                case "trigger":   // el gatillo del control
                    p.moveTo(8, 4); p.lineTo(16, 4); p.quadTo(19, 4, 19, 7); p.lineTo(19, 11); p.lineTo(8, 11); p.close();
                    c.drawPath(p, stroke);
                    p.reset(); p.moveTo(11, 11); p.quadTo(10, 16, 13, 19.5f); c.drawPath(p, stroke);
                    break;
                case "joystick":
                    r.set(4, 15, 20, 21); c.drawOval(r, stroke);
                    line(c, 12, 18, 12, 9); c.drawCircle(12, 6.5f, 3, stroke);
                    break;
                case "studio":   // Studio: visor con destellos
                    rrect(c, 2.5f, 9.5f, 18.5f, 18, 3.5f);
                    p.moveTo(8, 18); p.quadTo(10.5f, 14.5f, 13, 18); c.drawPath(p, stroke);
                    line(c, 19.5f, 2.5f, 19.5f, 7.5f); line(c, 17, 5, 22, 5);
                    break;
                case "panel":   // Modo Panel: ventanas planas
                    rrect(c, 2.5f, 5, 15.5f, 15, 2); rrect(c, 8.5f, 10, 21.5f, 20, 2);
                    break;
                default:
                    c.drawCircle(12, 12, 3, fill);
            }
            c.restore();
        }
    }
}
