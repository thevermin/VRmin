package org.questvrbridge.client;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Una sola conexión con la Mac para todas las ventanas del Quest (menú, escritorio y ventanas de apps).
 * Se mantiene mientras haya alguna ventana visible; al entrar en VR se corta para que la use la parte inmersiva.
 */
final class Link {
    interface Listener {
        void onState(JSONObject o);
        void onConnection(boolean connected, String name);
        default void onMacs(java.util.List<Mac> macs) { }
        /** La Mac pide el código de emparejado (wrong: el que se escribió no era). */
        default void onPairing(String mac, boolean wrong) { }
    }

    static final Link I = new Link();
    private static final String TAG = "QVB2D";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService sender = Executors.newSingleThreadExecutor();
    private final List<Listener> listeners = new CopyOnWriteArrayList<>();
    private final Map<Integer, StreamDecoder> windows = new ConcurrentHashMap<>();
    final StreamDecoder desk = new StreamDecoder();

    private Context app;
    private int users = 0;
    private volatile boolean running = false;
    private volatile int gen = 0;   // cada arranque tiene su número: un hilo viejo se da cuenta y termina
    private volatile Socket sock;
    private WifiManager.MulticastLock mlock;
    private volatile JSONObject lastState;
    private volatile boolean connected = false;
    private volatile String macName = "";

    /** Para leer y guardar el emparejado (la sala VR, que no usa acquire). */
    void setContext(Context c) { if (app == null) app = c.getApplicationContext(); }

    synchronized void acquire(Context c) {
        app = c.getApplicationContext();
        users++;
        if (!running && bridge == null) start();
    }

    synchronized void release() {
        users = Math.max(0, users - 1);
        if (users == 0) stop();
    }

    /** La parte VR necesita la conexión: cortar la de las ventanas. */
    synchronized void pauseForVR() { stop(); }

    private void start() {
        WifiManager wm = (WifiManager) app.getSystemService(Context.WIFI_SERVICE);
        if (wm != null && mlock == null) { mlock = wm.createMulticastLock("questvrbridge-2d"); mlock.setReferenceCounted(false); }
        if (mlock != null) mlock.acquire();
        running = true;
        UsbLink.I.start(app, null);   // cable USB-C (si el sistema lo permite y el usuario acepta)
        final int g = ++gen;
        new Thread(() -> loop(g), "qvb-red").start();
    }

    private void stop() {
        running = false;
        Socket s = sock;
        if (s != null) try { s.close(); } catch (Exception ignored) { }
        if (mlock != null) mlock.release();
        desk.reset();
        for (StreamDecoder d : windows.values()) d.reset();
    }

    void addListener(Listener l) {
        listeners.add(l);
        final JSONObject st = lastState;
        final boolean c = connected;
        final String n = macName;
        final java.util.List<Mac> ms = macs();
        ui.post(() -> { l.onMacs(ms); l.onConnection(c, n); if (st != null && c) l.onState(st); });
    }

    void removeListener(Listener l) { listeners.remove(l); }

    JSONObject state() { return lastState; }

    void send(String json) {
        Bridge br = bridge;
        if (br != null) { br.send(json); return; }
        final Socket s = sock;
        if (s == null) return;
        sender.execute(() -> {
            byte[] b = json.getBytes(StandardCharsets.UTF_8);
            ByteBuffer bb = ByteBuffer.allocate(4 + b.length);
            bb.putInt(b.length).put(b);
            try { OutputStream o = s.getOutputStream(); o.write(bb.array()); o.flush(); } catch (Exception ignored) { }
        });
    }

    void openWindow(int id, StreamDecoder d) {
        windows.put(id, d);
        send("{\"type\":\"win\",\"id\":" + id + ",\"on\":true}");
    }

    void closeWindow(int id) {
        windows.remove(id);
        send("{\"type\":\"win\",\"id\":" + id + ",\"on\":false}");
    }

    private void setConnected(boolean c, String name) {
        connected = c;
        macName = name;
        if (!c) lastState = null;
        ui.post(() -> { for (Listener l : listeners) l.onConnection(c, name); });
    }

    // Macs encontradas en la red (nombre -> ip, puerto, última vez vista)
    static final class Mac {
        final String name, ip; final int port; long seen;
        boolean idle;   // la app de la Mac está abierta pero el servidor apagado: se enciende al conectar
        boolean usb;    // encontrada por el cable USB-C (se conecta por ahí)
        Mac(String n, String i, int p) { name = n; ip = i; port = p; }
    }
    private final Map<String, Mac> macs = new ConcurrentHashMap<>();
    private volatile Mac target;   // la Mac elegida con "Conectar" (se recuerda mientras la app esté abierta)

    java.util.List<Mac> macs() { return new java.util.ArrayList<>(macs.values()); }
    Mac target() { return target; }
    boolean isConnected() { return connected; }

    /** El usuario eligió una Mac: conectarse (y reconectarse sola si se corta). */
    void connectTo(Mac m) { target = m; Bridge b = bridge; if (b != null) b.connect(m.name); }

    /** Cortar y no volver a conectar hasta que elija una Mac de nuevo. */
    void disconnect() {
        target = null;
        Bridge br = bridge;
        if (br != null) { br.disconnect(); return; }
        Socket s = sock;
        if (s != null) try { s.close(); } catch (Exception ignored) { }
    }

    private void discover(int g) {
        try (DatagramSocket u = new DatagramSocket(null)) {
            u.setReuseAddress(true);
            u.setBroadcast(true);
            u.bind(new InetSocketAddress(9101));
            u.setSoTimeout(400);
            byte[] buf = new byte[512];
            long until = System.currentTimeMillis() + 1200;
            while (System.currentTimeMillis() < until && running && g == gen && !(target != null && macs.containsKey(target.name))) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try { u.receive(p); } catch (java.net.SocketTimeoutException e) { continue; }
                JSONObject o = new JSONObject(new String(buf, 0, p.getLength(), StandardCharsets.UTF_8));
                if (!o.has("qvb")) continue;
                Mac m = new Mac(o.optString("name", "Mac"), p.getAddress().getHostAddress(), o.optInt("port", 9100));
                m.seen = System.currentTimeMillis();
                m.idle = o.optBoolean("idle");
                Mac old = macs.get(m.name);
                // Si hace poco se vio el servidor encendido, no pisarlo con el aviso de "apagado" de la app
                if (m.idle && old != null && !old.idle && m.seen - old.seen < 2500) old.seen = m.seen;
                else if (old != null && old.usb && m.seen - old.seen < 4000) old.seen = m.seen;   // por cable gana al Wi-Fi
                else macs.put(m.name, m);
            }
            // Por el cable USB-C: preguntar quién está (la Mac contesta con su nombre)
            for (String[] r : UsbLink.I.probe(350)) {
                JSONObject o = new JSONObject(r[0]);
                if (!o.has("qvb")) continue;
                Mac m = new Mac(o.optString("name", "Mac"), r[1], o.optInt("port", 9100));
                m.seen = System.currentTimeMillis();
                m.idle = o.optBoolean("idle");
                m.usb = true;
                macs.put(m.name, m);
            }
        } catch (Exception ignored) { }
        long now = System.currentTimeMillis();
        macs.values().removeIf(m -> now - m.seen > 6000);
        final java.util.List<Mac> list = macs();
        ui.post(() -> { for (Listener l : listeners) l.onMacs(list); });
    }

    private void askStart(Mac m) {
        try (DatagramSocket u = new DatagramSocket()) {
            if (m.usb && UsbLink.I.net != null) UsbLink.I.net.bindSocket(u);
            byte[] b = "{\"qvb\":\"start\"}".getBytes(StandardCharsets.UTF_8);
            u.send(new DatagramPacket(b, b.length, java.net.InetAddress.getByName(m.ip), 9102));
        } catch (Exception ignored) { }
    }

    private void loop(int g) {
        while (running && g == gen) {
            Mac t = target;
            if (t == null || !macs.containsKey(t.name)) { discover(g); t = target; }
            if (t == null || !running || g != gen) continue;
            Mac seen = macs.get(t.name);
            if (seen != null) t = seen;   // la IP puede haber cambiado
            if (t.idle) {
                // Pedirle a la app de la Mac que encienda el servidor y darle unos segundos
                askStart(t);
                ui.post(() -> { for (Listener l : listeners) l.onMacs(macs()); });
                for (int i = 0; i < 20 && running && g == gen && target != null; i++) {
                    try { Thread.sleep(300); } catch (InterruptedException ignored) { }
                    macs.remove(t.name);   // esperar un aviso nuevo de esa Mac
                    discover(g);
                    Mac now = macs.get(t.name);
                    if (now != null && !now.idle) break;
                    if (i % 4 == 3) askStart(t);
                }
                continue;
            }
            String auth = authJson(t.name);
            if (auth == null) {   // esperando que el usuario escriba el código
                try { Thread.sleep(400); } catch (InterruptedException ignored) { }
                continue;
            }
            try (Socket s = new Socket()) {
                if (t.usb && UsbLink.I.net != null) UsbLink.I.net.bindSocket(s);
                s.connect(new InetSocketAddress(t.ip, t.port), 3000);
                s.setTcpNoDelay(true);
                s.setReceiveBufferSize(4 << 20);
                {   // lo primero: la clave (o el código)
                    byte[] b = auth.getBytes(StandardCharsets.UTF_8);
                    ByteBuffer bb = ByteBuffer.allocate(4 + b.length);
                    bb.putInt(b.length).put(b);
                    OutputStream o = s.getOutputStream(); o.write(bb.array()); o.flush();
                }
                sock = s;
                send("{\"type\":\"hello\",\"mode\":\"2d\"}");
                for (Integer id : windows.keySet()) send("{\"type\":\"win\",\"id\":" + id + ",\"on\":true}");
                DataInputStream in = new DataInputStream(s.getInputStream());
                byte[] buf = new byte[1 << 20];
                while (running && g == gen && target != null) {
                    int n = in.readInt();
                    if (n <= 0 || n > (64 << 20)) break;
                    if (n > buf.length) buf = new byte[n];
                    in.readFully(buf, 0, n);
                    handlePacket(buf, n);
                }
            } catch (Exception e) {
                Log.i(TAG, "Conexión cortada: " + e);
                macs.remove(t.name);   // volver a buscarla antes de reintentar
            }
            if (g != gen) return;
            stopAudio();
            sock = null;
            desk.reset();
            for (StreamDecoder d : windows.values()) d.reset();
            setConnected(false, "");
        }
    }

    /** Un paquete de la Mac: estado (3), escritorio (7) o una ventana (8). */
    void handlePacket(byte[] buf, int n) {
        int type = buf[0] & 0xff;
        if (type == 2 && n > 8) playAudio(buf, n);
        else if (type == 3) onText(new String(buf, 1, n - 1, StandardCharsets.UTF_8));
        else if (type == 7 && n > 6) {
            // [7][key][w u16][h u16] + H.264
            desk.feed(buf, 6, n - 6, buf[1] == 1, (buf[2] & 0xff) << 8 | (buf[3] & 0xff), (buf[4] & 0xff) << 8 | (buf[5] & 0xff));
        } else if (type == 8 && n > 10) {
            // [8][id u32][key][w u16][h u16] + H.264
            int id = (buf[1] & 0xff) << 24 | (buf[2] & 0xff) << 16 | (buf[3] & 0xff) << 8 | (buf[4] & 0xff);
            StreamDecoder d = windows.get(id);
            if (d != null) d.feed(buf, 10, n - 10, buf[5] == 1, (buf[6] & 0xff) << 8 | (buf[7] & 0xff), (buf[8] & 0xff) << 8 | (buf[9] & 0xff));
        }
    }

    // ---- Sonido de la Mac en la ventana del Quest: [2][0][0][0][frecuencia u32] + PCM 16 bits estéreo
    // Se junta un colchón antes de empezar a sonar (la red no llega pareja) y, si se acumula demasiado, se tira lo viejo.
    private android.media.AudioTrack track;
    private int trackRate = 0;
    private long trackWritten = 0;      // cuadros escritos desde que empezó
    private boolean trackPlaying = false;
    private int lastUnderruns = 0;
    private int primeMs = 80;           // crece si se queda sin sonido seguido

    private void playAudio(byte[] b, int n) {
        int rate = (b[4] & 0xff) << 24 | (b[5] & 0xff) << 16 | (b[6] & 0xff) << 8 | (b[7] & 0xff);
        if (rate < 8000 || rate > 192000) return;
        if (track == null || rate != trackRate) {
            if (track != null) { track.release(); track = null; }
            int min = android.media.AudioTrack.getMinBufferSize(rate, android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT);
            track = new android.media.AudioTrack.Builder()
                    .setAudioAttributes(new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_MEDIA)
                            .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new android.media.AudioFormat.Builder().setSampleRate(rate)
                            .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT).build())
                    .setBufferSizeInBytes(Math.max(min * 2, rate * 4 * 4 / 10))   // 400 ms de lugar
                    .build();
            trackRate = rate;
            trackWritten = 0;
            trackPlaying = false;
            lastUnderruns = 0;
        }
        int bytes = (n - 8) & ~3;
        // Lo que todavía no sonó (en cuadros)
        long queued = trackWritten - (trackPlaying ? (track.getPlaybackHeadPosition() & 0xffffffffL) : 0);
        if (trackPlaying && queued > rate * 3L / 10) return;   // más de 300 ms atrasado: tirar este pedazo para alcanzar
        int wrote = track.write(b, 8, bytes, android.media.AudioTrack.WRITE_NON_BLOCKING);
        if (wrote > 0) trackWritten += wrote / 4;
        if (!trackPlaying && trackWritten >= (long) rate * primeMs / 1000) { track.play(); trackPlaying = true; }
        if (trackPlaying) {
            int u = track.getUnderrunCount();
            if (u > lastUnderruns) {   // se quedó sin sonido: la próxima vez, más colchón
                lastUnderruns = u;
                primeMs = Math.min(200, primeMs + 20);
                track.pause(); track.flush();
                trackWritten = 0; trackPlaying = false;
            }
        }
    }

    private void stopAudio() { if (track != null) { track.release(); track = null; trackRate = 0; trackPlaying = false; trackWritten = 0; } }

    // ---- Emparejado con la Mac: la primera vez la Mac muestra un código; después el Quest usa una clave guardada.
    // Se guarda en un archivo (no en preferencias) porque lo comparten la ventana y la sala VR (otro proceso).
    private volatile String awaitMac;    // Mac que espera el código
    private volatile String typedCode;   // código escrito por el usuario (se usa una vez)
    private volatile boolean pairWrong;

    private synchronized JSONObject pairFile() {
        try {
            java.io.File f = new java.io.File(app.getFilesDir(), "emparejado.json");
            JSONObject o = f.exists() ? new JSONObject(new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8)) : new JSONObject();
            if (!o.has("id")) { o.put("id", java.util.UUID.randomUUID().toString()); savePairFile(o); }
            return o;
        } catch (Exception e) { return new JSONObject(); }
    }

    private synchronized void savePairFile(JSONObject o) {
        try {
            java.io.File f = new java.io.File(app.getFilesDir(), "emparejado.json"), tmp = new java.io.File(app.getFilesDir(), "emparejado.tmp");
            java.nio.file.Files.write(tmp.toPath(), o.toString().getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(f)) java.nio.file.Files.write(f.toPath(), o.toString().getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
    }

    /** Primer mensaje al conectarse a esa Mac, o null si hay que esperar a que el usuario escriba el código. */
    String authJson(String mac) {
        if (app == null) return null;
        if (mac == null) mac = "";
        try {
            JSONObject f = pairFile();
            JSONObject a = new JSONObject().put("type", "auth").put("id", f.optString("id")).put("device", android.os.Build.MODEL);
            JSONObject toks = f.optJSONObject("macs");
            String tok = toks != null ? toks.optString(mac, "") : "";
            if (!tok.isEmpty()) return a.put("token", tok).toString();
            if (mac.equals(awaitMac)) {
                String c = typedCode;
                if (c == null) return null;   // esperando el código
                typedCode = null;
                return a.put("code", c).toString();
            }
            return a.toString();   // primera vez: la Mac va a mostrar un código
        } catch (Exception e) { return null; }
    }

    String pairingMac() { return awaitMac; }
    boolean pairWrong() { return pairWrong; }

    /** El usuario escribió el código: el próximo intento de conexión lo manda. */
    void submitCode(String code) { pairWrong = false; typedCode = code; }

    /** Cancelar el emparejado (y la conexión a esa Mac). */
    void cancelPairing() { awaitMac = null; typedCode = null; pairWrong = false; disconnect(); }

    private void onAuth(JSONObject o) {
        Mac t = target;
        String mac = t != null ? t.name : "";
        if (o.optBoolean("ok")) {
            String tok = o.optString("token", "");
            if (!tok.isEmpty()) {
                try {
                    JSONObject f = pairFile();
                    JSONObject toks = f.optJSONObject("macs");
                    if (toks == null) { toks = new JSONObject(); f.put("macs", toks); }
                    toks.put(mac, tok);
                    savePairFile(f);
                } catch (Exception ignored) { }
            }
            awaitMac = null; pairWrong = false;
            if (bridge == null) setConnected(true, mac);
        } else {
            // la Mac no nos conoce (o se olvidó): borrar la clave vieja y pedir el código
            try {
                JSONObject f = pairFile();
                JSONObject toks = f.optJSONObject("macs");
                if (toks != null && toks.has(mac)) { toks.remove(mac); savePairFile(f); }
            } catch (Exception ignored) { }
            awaitMac = mac;
            pairWrong = o.optBoolean("wrong");
        }
        final String m = awaitMac; final boolean w = pairWrong;
        ui.post(() -> { for (Listener l : listeners) l.onPairing(m, w); });
    }

    // ---- Sala VR: la conexión la tiene la parte nativa; esta clase solo recibe y manda a través de ella
    interface Bridge {
        void send(String json);
        default void connect(String name) { }
        default void disconnect() { }
    }
    private volatile Bridge bridge;

    void useBridge(Bridge b) { bridge = b; }

    /** Sala VR: la lista de Macs la arma la parte nativa (escucha los avisos de la red). */
    void setBridgeMacs(java.util.List<Mac> list) {
        macs.clear();
        for (Mac m : list) { m.seen = System.currentTimeMillis(); macs.put(m.name, m); }
        final java.util.List<Mac> l = macs();
        ui.post(() -> { for (Listener x : listeners) x.onMacs(l); });
    }

    void bridgeConnection(boolean c, String name) {
        if (!c) { desk.reset(); for (StreamDecoder d : windows.values()) d.reset(); }
        setConnected(c, name);
    }

    private void onText(String t) {
        try {
            JSONObject o = new JSONObject(t);
            if ("auth".equals(o.optString("type"))) { onAuth(o); return; }
            if (!"lobby2d".equals(o.optString("type"))) return;
            lastState = o;
            ui.post(() -> { for (Listener l : listeners) l.onState(o); });
        } catch (Exception e) {
            Log.w(TAG, "JSON: " + e);
        }
    }
}
