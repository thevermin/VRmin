package org.questvrbridge.client;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.Gravity;
import android.view.MotionEvent;
import android.widget.TextView;

/**
 * Botón que flota sobre la imagen de la Mac sin estorbar: casi invisible, y solo se puede tocar
 * después de apuntarle un segundo entero (un anillo se va llenando). Antes de eso, el clic pasa a la Mac.
 */
final class DwellButton extends TextView {
    private static final long DWELL_MS = 1000;
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF box = new RectF();
    private float progress = 0f;      // 0..1 mientras se apunta
    private boolean armed = false;
    private boolean instant = false;  // sin espera (por ejemplo, con el menú abierto)
    private ValueAnimator anim;
    // Al apretar el gatillo, el Quest avisa primero que el puntero "salió": esperar un poco antes de desarmar
    private final Runnable disarm = this::idle;

    DwellButton(Context c, String text) {
        super(c);
        setText(text);
        setTextSize(20);
        setTextColor(Color.WHITE);
        setTypeface(Ui.MEDIUM);
        setGravity(Gravity.CENTER);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(Ui.dp(c, 3));
        ring.setColor(Color.WHITE);
        ring.setStrokeCap(Paint.Cap.ROUND);
        setBackground(Ui.glass(c, 999, 150));
        idle();
    }

    void setInstant(boolean i) { instant = i; if (i) arm(); else idle(); }

    private void idle() {
        armed = instant;
        progress = 0;
        if (anim != null) anim.cancel();
        setAlpha(instant ? 1f : 0.28f);
        invalidate();
    }

    private void arm() { armed = true; progress = 1; setAlpha(1f); invalidate(); }

    @Override
    public boolean onHoverEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
                removeCallbacks(disarm);
                if (instant || armed || (anim != null && anim.isRunning())) break;
                setAlpha(0.75f);
                anim = ValueAnimator.ofFloat(0f, 1f).setDuration(DWELL_MS);
                anim.addUpdateListener(a -> { progress = (float) a.getAnimatedValue(); invalidate(); });
                anim.addListener(new android.animation.AnimatorListenerAdapter() {
                    @Override public void onAnimationEnd(android.animation.Animator a) { if (progress >= 1f) arm(); }
                });
                anim.start();
                break;
            case MotionEvent.ACTION_HOVER_EXIT:
                removeCallbacks(disarm);
                postDelayed(disarm, 600);
                break;
        }
        return true;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!armed) return false;   // todavía no: el toque le llega a la Mac
        removeCallbacks(disarm);
        if (e.getActionMasked() == MotionEvent.ACTION_UP) {
            performClick();
            if (!instant) idle();
        }
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (progress <= 0 || instant) return;
        float in = ring.getStrokeWidth();
        box.set(in, in, getWidth() - in, getHeight() - in);
        ring.setAlpha(armed ? 255 : 200);
        c.drawArc(box, -90, 360 * progress, false, ring);
    }
}
