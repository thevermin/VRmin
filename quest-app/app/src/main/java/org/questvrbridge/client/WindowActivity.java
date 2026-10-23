package org.questvrbridge.client;

import android.app.Activity;
import android.os.Bundle;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;

/** Una ventana de la Mac en su propia ventana del Quest (modo ventanas). */
public class WindowActivity extends Activity implements WindowUi.Host {
    static final Set<WindowActivity> OPEN = new HashSet<>();
    static void closeAll() { for (WindowActivity a : new ArrayList<>(OPEN)) a.finishAndRemoveTask(); }

    private WindowUi ui;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        I18n.init(this);
        String app = getIntent().getStringExtra("app");
        setTitle(app);
        setTaskDescription(new android.app.ActivityManager.TaskDescription(app));
        ui = new WindowUi(this, this, getIntent().getIntExtra("id", 0), app, getIntent().getStringExtra("title"),
                getIntent().getIntExtra("w", 16), getIntent().getIntExtra("h", 10));
        setContentView(ui.view());
        OPEN.add(this);
    }

    @Override protected void onStart() { super.onStart(); Link.I.acquire(this); ui.start(); }
    @Override protected void onStop() { super.onStop(); ui.stop(); Link.I.release(); }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        OPEN.remove(this);
        if (isFinishing()) ui.destroyed();
    }

    int winId() { return ui.id; }

    /// Antes de pasar a VR, la ventana se desvanece suave (y se achica un poco)
    void fadeOut(long ms) {
        android.view.View v = ui.view();
        v.animate().alpha(0f).scaleX(0.94f).scaleY(0.94f).setDuration(ms)
                .setInterpolator(new android.view.animation.DecelerateInterpolator()).start();
    }

    @Override public boolean inVR() { return false; }
    @Override public void close() { finishAndRemoveTask(); }
}
