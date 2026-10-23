package org.questvrbridge.client;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;

/**
 * Conexión por cable USB-C (como Virtual Desktop), sin modo desarrollador: Horizon OS 2.5+ deja que una app
 * pida la red USB (CDC-NCM) con un aviso que el usuario acepta una vez. La Mac la ve como un adaptador de red
 * cableado (sin instalar nada). La red es IPv6 «link-local»: la Mac se encuentra preguntando a todos
 * (ff02::1) en el puerto 9103 y contesta con su nombre.
 */
final class UsbLink {
    interface Listener { void onUsb(long handle, String ifname); }

    static final UsbLink I = new UsbLink();
    private static final String TAG = "QVBUSB";
    static final int PROBE_PORT = 9103;

    volatile Network net;
    volatile String ifname = "";
    volatile int kbps = 0;
    private ConnectivityManager cm;
    private ConnectivityManager.NetworkCallback cb;
    private Listener listener;

    synchronized void start(Context c, Listener l) {
        listener = l;
        if (cb != null) { if (net != null && l != null) l.onUsb(net.getNetworkHandle(), ifname); return; }
        try {
            cm = (ConnectivityManager) c.getApplicationContext().getSystemService(Context.CONNECTIVITY_SERVICE);
            NetworkRequest req = new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_USB)
                    .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    .removeCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED)
                    .build();
            cb = new ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(Network n) {
                    net = n;
                    LinkProperties lp = cm.getLinkProperties(n);
                    ifname = lp != null && lp.getInterfaceName() != null ? lp.getInterfaceName() : "";
                    Log.i(TAG, "Red USB disponible: " + ifname);
                    Listener li = listener;
                    if (li != null) li.onUsb(n.getNetworkHandle(), ifname);
                }
                @Override public void onLinkPropertiesChanged(Network n, LinkProperties lp) {
                    if (lp != null && lp.getInterfaceName() != null) ifname = lp.getInterfaceName();
                }
                @Override public void onCapabilitiesChanged(Network n, NetworkCapabilities nc) {
                    kbps = nc.getLinkDownstreamBandwidthKbps();
                }
                @Override public void onLost(Network n) {
                    if (n.equals(net)) {
                        net = null;
                        Log.i(TAG, "Red USB perdida");
                        Listener li = listener;
                        if (li != null) li.onUsb(0, "");
                    }
                }
                @Override public void onUnavailable() {
                    // (rechazado o sin cable): se vuelve a pedir la próxima vez que se abra la app
                    synchronized (UsbLink.this) { cb = null; }
                }
            };
            cm.requestNetwork(req, cb);
        } catch (Exception e) {
            Log.w(TAG, "La red USB no está disponible en este sistema: " + e);
            cb = null;
        }
    }

    synchronized void stop() {
        if (cm != null && cb != null) try { cm.unregisterNetworkCallback(cb); } catch (Exception ignored) { }
        cb = null; net = null; listener = null;
    }

    /** Pregunta por el cable quién está (la app de la Mac contesta). Devuelve las respuestas: «json\tip%if». */
    java.util.List<String[]> probe(int waitMs) {
        java.util.List<String[]> out = new java.util.ArrayList<>();
        Network n = net;
        if (n == null || ifname.isEmpty()) return out;
        try (DatagramSocket u = new DatagramSocket()) {
            n.bindSocket(u);
            u.setSoTimeout(150);
            byte[] q = "{\"qvb\":\"probe\"}".getBytes(StandardCharsets.UTF_8);
            NetworkInterface ni = NetworkInterface.getByName(ifname);
            InetAddress all = ni != null ? Inet6Address.getByAddress("ff02::1", new byte[]{(byte) 0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, ni)
                                         : InetAddress.getByName("ff02::1%" + ifname);
            u.send(new DatagramPacket(q, q.length, all, PROBE_PORT));
            byte[] buf = new byte[512];
            long until = System.currentTimeMillis() + waitMs;
            while (System.currentTimeMillis() < until) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try { u.receive(p); } catch (java.net.SocketTimeoutException e) { continue; }
                String ip = p.getAddress().getHostAddress();   // fe80::…%ncm0
                if (ip.indexOf('%') < 0) ip = ip + "%" + ifname;
                out.add(new String[]{new String(buf, 0, p.getLength(), StandardCharsets.UTF_8), ip});
            }
        } catch (Exception e) {
            Log.w(TAG, "No pude preguntar por el cable: " + e);
        }
        return out;
    }
}
