package org.questvrbridge.client;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;

import com.meta.horizon.platform.ovr.Core;
import com.meta.horizon.platform.ovr.requests.Entitlements;
import com.meta.horizon.platform.ovr.requests.Request;

/**
 * Verificación de compra en la tienda de Meta («entitlement check»), como pide Meta en los primeros 10 segundos.
 * Si la app se compiló sin App ID (versiones de prueba e instalaciones por cable) no se verifica nada.
 * Funciona sin internet (la consulta es local al visor).
 */
final class Licencia {
    private Licencia() { }

    interface Resultado { void listo(boolean comprada, String detalle); }

    private static Boolean cache;   // una vez por proceso

    @SuppressWarnings({"rawtypes", "unchecked"})
    static void comprobar(Activity a, Resultado r) {
        String appId = a.getString(R.string.meta_app_id).trim();
        if (appId.isEmpty()) { r.listo(true, "sin App ID: versión de prueba"); return; }
        if (cache != null) { r.listo(cache, null); return; }
        final Handler h = new Handler(Looper.getMainLooper());
        final boolean[] fin = {false};
        final Runnable[] bomba = new Runnable[1];
        bomba[0] = () -> {
            if (fin[0]) return;
            try { Request.runCallbacks(); } catch (Throwable ignored) { }
            h.postDelayed(bomba[0], 100);
        };
        java.util.function.BiConsumer<Boolean, String> terminar = (ok, det) -> {
            if (fin[0]) return;
            fin[0] = true;
            if (ok) cache = true;
            r.listo(ok, det);
        };
        try {
            Core.asyncInitialize(appId, a.getApplicationContext());
            Request q = Entitlements.getIsViewerEntitled();
            q.onSuccess(x -> h.post(() -> terminar.accept(true, null)));
            q.onError(e -> {
                String m = e instanceof com.meta.horizon.platform.ovr.Error ? ((com.meta.horizon.platform.ovr.Error) e).getMessage() : String.valueOf(e);
                h.post(() -> terminar.accept(false, m));
            });
        } catch (Throwable t) {
            terminar.accept(false, t.getMessage());
            return;
        }
        h.post(bomba[0]);
        h.postDelayed(() -> terminar.accept(false, "sin respuesta de la plataforma de Meta"), 15000);
    }
}
