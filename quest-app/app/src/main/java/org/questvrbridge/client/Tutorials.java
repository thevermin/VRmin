package org.questvrbridge.client;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import static org.questvrbridge.client.Ui.dp;

/**
 * Tutoriales cortos, con un ícono por paso. Aparecen solos la primera vez que llegás a cada lugar (debajo: «No mostrar
 * este otra vez» y «No mostrar más tutoriales») y están todos en la Ayuda (botón «?»), ordenados por tema.
 * Los textos van en los dos idiomas acá mismo (es, en).
 */
final class Tutorials {
    private Tutorials() { }

    /** Un tutorial: ícono, tema, título y pasos ({ícono, español, inglés}). */
    static final class Tut {
        final String id, icon, group, es, en;
        final String[][] steps;
        Tut(String id, String icon, String group, String es, String en, String[][] steps) {
            this.id = id; this.icon = icon; this.group = group; this.es = es; this.en = en; this.steps = steps;
        }
        String title() { return I18n.english() ? en : es; }
    }

    private static String[] s(String icon, String es, String en) { return new String[]{icon, es, en}; }

    /** Temas, en el orden de la Ayuda: {id, español, inglés} */
    static final String[][] GROUPS = {
            {"start", "Primeros pasos", "Getting started"},
            {"screens", "Pantallas y ventanas", "Screens and windows"},
            {"studio", "Studio", "Studio"},
            {"games", "Juegos", "Games"},
            {"sentinel", "Modo Centinela", "Sentinel Mode"},
            {"rec", "Grabar", "Recording"},
    };

    static final Tut[] ALL = {
            // ---------------------------------------------------------------- primeros pasos
            new Tut("modos", "studio", "start", "Los modos de VRmin", "VRmin's modes", new String[][]{
                    s("panel", "Modo Panel: tu Mac en ventanas del Quest, como cualquier ventana de Meta. La usás junto a otras apps y juegos (por ejemplo, con un video o el navegador al lado).", "Panel Mode: your Mac in Quest windows, like any other Meta window. Use it alongside other apps and games (say, next to a video or the browser)."),
                    s("studio", "Modo Studio: una sala inmersiva (o tu cuarto real) con las pantallas de la Mac del tamaño que quieras.", "Studio Mode: an immersive room (or your real room) with your Mac's screens at any size."),
                    s("gamepad", "Modo Juego: los juegos VR de Steam corren en la Mac y los jugás en el visor.", "Game Mode: Steam VR games run on the Mac and you play them in the headset."),
                    s("eye", "Modo Centinela: jugás sin el visor puesto. El visor queda apoyado mirándote y el juego se ve en la Mac.", "Sentinel Mode: play without wearing the headset. It sits facing you and the game shows on the Mac."),
                    s("help", "Todos los tutoriales están en «Ayuda», cuando los quieras ver de nuevo.", "Every tutorial is in “Help” whenever you want to see it again."),
            }),
            new Tut("conectar", "laptop", "start", "Conectar tu Mac", "Connecting your Mac", new String[][]{
                    s("laptop", "Abrí VRmin Host en la Mac.", "Open VRmin Host on the Mac."),
                    s("cable", "El Quest y la Mac en la misma red Wi-Fi, o unidos con un cable USB-C (más estable y con menos demora).", "The Quest and the Mac on the same Wi-Fi, or joined by a USB-C cable (steadier, with less delay)."),
                    s("check", "Elegí tu Mac en «Computadoras». La primera vez, escribí el código de 6 números que aparece en la Mac.", "Pick your Mac in “Computers”. The first time, type the 6-digit code shown on the Mac."),
            }),
            // ---------------------------------------------------------------- pantallas
            new Tut("escritorio", "display", "screens", "Ver tu Mac", "Seeing your Mac", new String[][]{
                    s("display", "En «Escritorio» activás la pantalla principal de la Mac.", "In “Desktop” you turn on the Mac's main screen."),
                    s("window", "También podés abrir ventanas sueltas de una app, cada una en su propia pantalla.", "You can also open single app windows, each on its own screen."),
                    s("addDisplay", "«Agregar monitor» suma primero tus otros monitores físicos y, cuando ya están todos, uno virtual que la Mac usa como una pantalla más.", "“Add monitor” first adds your other physical monitors and, once they're all open, a virtual one the Mac uses as one more screen."),
            }),
            new Tut("barra", "menu", "screens", "Los botones de cada pantalla", "Each screen's buttons", new String[][]{
                    s("bulb", "Arriba a la izquierda, lo de la ventana:", "Top left, the window itself:"),
                    s("menu", "Abre o cierra el menú de VRmin.", "Opens or closes the VRmin menu."),
                    s("keyboard", "Teclado virtual.", "Virtual keyboard."),
                    s("laser", "El láser toca esta pantalla. Apagado, el láser la atraviesa sin hacer clic (sus botones se siguen usando): útil para usar el mouse de la Mac.", "The laser touches this screen. When off, the laser passes through without clicking (its buttons still work): handy for using the Mac's mouse."),
                    s("lock", "Fijar a tu vista: la pantalla te sigue cuando girás la cabeza.", "Pin to your view: the screen follows you when you turn your head."),
                    s("recenter", "La vuelve a poner delante tuyo.", "Puts it back in front of you."),
                    s("close", "Cerrar (la pantalla principal no se cierra).", "Close (the main screen can't be closed)."),
                    s("bulb", "Abajo a la derecha, la imagen:", "Bottom right, the picture:"),
                    s("cube", "3D: convierte la imagen en 3D con IA, o muestra videos lado a lado y arriba/abajo. Mantenelo para la intensidad.", "3D: turns the image 3D with AI, or shows side-by-side and top/bottom videos. Hold it for the strength."),
                    s("size", "Tamaño: mantenelo apretado y mové el control a los costados.", "Size: hold it and move the controller sideways."),
                    s("curve", "Curva: igual que el tamaño.", "Curve: same as size."),
                    s("filter", "Filtro de imagen: cada toque pasa al siguiente.", "Image filter: each tap goes to the next one."),
                    s("swapDisplay", "Cambiar de monitor: muestra otro monitor de la Mac en esta pantalla.", "Switch monitor: shows another Mac monitor on this screen."),
                    s("addDisplay", "Agregar monitor: primero los físicos, después uno virtual.", "Add monitor: physical ones first, then a virtual one."),
                    s("stats", "Rendimiento: cuadros por segundo, Mbps y demora.", "Performance: frames per second, Mbps and delay."),
                    s("sliders", "Ajustes de las pantallas (calidad, cuadros por segundo y sonido).", "Screen settings (quality, frame rate and sound)."),
                    s("help", "Apuntale un segundo a un botón y aparece su nombre. Los grises no se usan en esa pantalla.", "Point at a button for a second to see its name. Grey ones don't apply to that screen."),
            }),
            new Tut("mover_ventanas", "move", "screens", "Mover y acomodar pantallas", "Moving and arranging screens", new String[][]{
                    s("pinch", "Apuntá a la barrita de abajo de la pantalla, apretá el gatillo (o pellizcá) y, sin soltar, llevala adonde quieras.", "Point at the small bar under the screen, press the trigger (or pinch) and, holding it, move it anywhere."),
                    s("recenter", "El botón de la mira la trae de nuevo delante tuyo.", "The crosshair button brings it back in front of you."),
                    s("lock", "Con el candado, te sigue cuando girás.", "With the lock, it follows you when you turn."),
                    s("bulb", "En Living y Cine la pantalla principal es fija; en Espacio y Passthrough todo flota.", "In Living room and Cinema the main screen is fixed; in Space and Passthrough everything floats."),
            }),
            // ---------------------------------------------------------------- studio
            new Tut("studio_menu", "menu", "studio", "Abrir el menú en Studio", "Opening the menu in Studio", new String[][]{
                    s("menu", "Tocá el botón de menú en la barra de la pantalla principal.", "Tap the menu button on the main screen's bar."),
                    s("watch", "O mirá un momento el borde de tu mano, del lado del meñique: aparece el menú en la mano.", "Or look at the edge of your hand (little-finger side) for a moment: the menu appears on your hand."),
                    s("trigger", "El láser sale de una sola mano. Para pasarlo a la otra, apretá el gatillo de esa mano.", "The laser comes from one hand. To switch, press the other hand's trigger."),
            }),
            new Tut("moverse", "move", "studio", "Moverte por la sala", "Moving around the room", new String[][]{
                    s("joystick", "Teletransporte: joystick derecho hacia atrás, apuntá al piso y soltá.", "Teleport: right joystick back, aim at the floor and let go."),
                    s("joystick", "Girar: joystick derecho a los costados.", "Turn: right joystick left or right."),
                    s("move", "Caminar con el joystick izquierdo: se activa en Ajustes › Moverte por la sala.", "Walking with the left joystick: turn it on in Settings › Moving around the room."),
                    s("bulb", "En la sala Passthrough no te movés: es tu cuarto real.", "In the Passthrough room you don't move: it's your real room."),
            }),
            new Tut("manos", "hand", "studio", "Usar las manos sin controles", "Using your hands, no controllers", new String[][]{
                    s("pinch", "Apuntá con la mano y juntá índice y pulgar para hacer clic.", "Point with your hand and touch index finger to thumb to click."),
                    s("hand", "Para teletransportarte: mano derecha en forma de pistola, apuntá al piso y bajá el pulgar.", "To teleport: right hand like a pistol, aim at the floor and drop your thumb."),
                    s("sliders", "En Ajustes › Gestos de las manos elegís qué hace cada gesto, en Studio y en los juegos (y armás macros).", "In Settings › Hand gestures you choose what each gesture does, in Studio and in games (and build macros)."),
            }),
            new Tut("salas", "room", "studio", "Las salas", "The rooms", new String[][]{
                    s("room", "Living: un living con una tele grande fija.", "Living room: a living room with a big fixed TV."),
                    s("studio", "Espacio: todo flota; ponés las pantallas donde quieras.", "Space: everything floats; put screens wherever you want."),
                    s("display", "Cine: una pantalla gigante en una sala oscura.", "Cinema: a giant screen in a dark theater."),
                    s("camera", "Passthrough: tu cuarto real. Todo flota y no te movés; tus manos se ven delante de las pantallas.", "Passthrough: your real room. Everything floats and you stay put; your hands show in front of the screens."),
            }),
            new Tut("ajustes_rapidos", "sliders", "studio", "Ajustes rápidos", "Quick settings", new String[][]{
                    s("sliders", "Arriba de todo en «Ajustes»: cada ícono prende o apaga algo al instante.", "At the top of “Settings”: each icon turns something on or off right away."),
                    s("plus", "Con «Editar» lo armás a tu gusto: dejá solo lo que más usás (las manos, el portal, el teclado, moverte con las manos, grabar…) y sacá lo que no.", "With “Edit” you make it yours: keep only what you use most (hands, portal, keyboard, moving with your hands, recording…) and drop the rest."),
            }),
            new Tut("portal", "portal", "studio", "El portal", "The portal", new String[][]{
                    s("portal", "Un hueco hacia tu cuarto real: para ver el teclado, la taza o la puerta sin salir de la sala.", "A window into your real room: see your keyboard, mug or door without leaving the room."),
                    s("pinch", "Fuera de las pantallas, pellizcá (o apretá el gatillo) y recorré el contorno con la punta del índice o del control. Al soltar, queda listo.", "Away from the screens, pinch (or press the trigger) and trace the outline with your fingertip or controller. Let go and it's ready."),
                    s("bulb", "Para hacer otro, apagalo y volvé a prenderlo desde los ajustes rápidos.", "To draw another, turn it off and on again from quick settings."),
            }),
            // ---------------------------------------------------------------- juegos
            new Tut("juego_abrir", "gamepad", "games", "Abrir un juego", "Opening a game", new String[][]{
                    s("gamepad", "En «Juegos» tocá la portada. VRmin pasa solo al Modo Juego.", "In “Games”, tap the cover. VRmin switches to Game Mode by itself."),
                    s("bulb", "La primera vez un juego puede tardar en abrir: la Mac lo está preparando.", "The first launch can take a while: the Mac is getting the game ready."),
                    s("sliders", "La calidad (resolución, Hz y Mbps) está en Ajustes › Juegos VR y en el menú del juego.", "Quality (resolution, Hz and Mbps) is in Settings › VR games and in the game menu."),
            }),
            new Tut("juego_menu", "menu", "games", "El menú dentro del juego", "The in-game menu", new String[][]{
                    s("menu", "Mantené el botón de menú del control izquierdo (o el pellizco del meñique izquierdo).", "Hold the left controller's menu button (or a left little-finger pinch)."),
                    s("gamepad", "Mientras está abierto, el juego no recibe tus botones.", "While it's open, the game doesn't get your buttons."),
                    s("window", "Desde ahí abrís pantallas de la Mac que flotan mientras jugás, y cambiás tu altura, la calidad y los gestos.", "From there you open Mac screens that float while you play, and change your height, quality and gestures."),
                    s("close", "«Cerrar el juego» lo cierra en la Mac (tocá dos veces).", "“Close game” closes it on the Mac (tap twice)."),
            }),
            new Tut("altura", "visor", "games", "Tu altura", "Your height", new String[][]{
                    s("visor", "Sentado: el juego te pone a la altura que elijas, como si estuvieras parado.", "Seated: the game puts you at the height you choose, as if you were standing."),
                    s("move", "De pie: «Real» usa tu altura de verdad, o elegí una.", "Standing: “Real” uses your actual height, or pick one."),
                    s("sliders", "Se cambia en Ajustes › Tu altura o en el menú del juego, al instante.", "Change it in Settings › Your height or in the game menu, instantly."),
            }),
            // ---------------------------------------------------------------- centinela
            new Tut("centinela", "eye", "sentinel", "El Modo Centinela", "Sentinel Mode", new String[][]{
                    s("eye", "Jugá juegos VR sin ponerte el visor: queda apoyado mirándote, sigue tus controles con sus cámaras y el juego se ve en la pantalla de la Mac. Ideal para jugar sentado, para partidas largas o para mostrarle el juego a otros.", "Play VR games without wearing the headset: it sits facing you, tracks your controllers with its cameras, and the game shows on the Mac's screen. Great for playing seated, long sessions, or showing the game to others."),
                    s("cable", "Conectá el visor a la Mac con un cable USB-C.", "Connect the headset to the Mac with a USB-C cable."),
                    s("laptop", "La primera vez: en VRmin Host, «Modo Centinela» › «Instalar» (necesita el modo desarrollador; el «?» de ahí explica cómo). Así la pantalla del visor no se apaga.", "The first time: in VRmin Host, “Sentinel Mode” › “Install” (needs developer mode; the “?” there explains how). That keeps the headset's display on."),
                    s("display", "Dónde ponerlo: encima del monitor, apuntando un poco hacia abajo, es lo mejor para jugar sentado. Para jugar parado, en un estante o trípode a la altura de la cabeza, a 1–2 m.", "Where to put it: on top of the monitor, tilted slightly down, is best for playing seated. For standing, on a shelf or tripod at head height, 1–2 m away."),
                    s("bulb", "Que te vea las manos todo el tiempo: buena luz y nada delante de los controles.", "Make sure it always sees your hands: good light and nothing in front of the controllers."),
                    s("recenter", "Centrar el juego: mirá al visor con los controles delante del pecho y mantené el botón de menú izquierdo.", "Center the game: face the headset with the controllers in front of your chest and hold the left menu button."),
                    s("joystick", "Mirar alrededor: apoyá el pulgar en el joystick derecho (sin moverlo) y girá el control. La vista se queda donde la dejes.", "Look around: rest your thumb on the right joystick (without moving it) and turn the controller. The view stays where you leave it."),
                    s("camera", "Con la cámara de la Mac, el juego también sigue tu cabeza (se prende en la ventana del Modo Centinela). Tu altura sale de Ajustes › Tu altura.", "With the Mac's camera, the game also follows your head (turn it on in the Sentinel Mode window). Your height comes from Settings › Your height."),
                    s("close", "Para salir: cerrá la ventana del Modo Centinela en la Mac o apretá Esc.", "To exit: close the Sentinel Mode window on the Mac or press Esc."),
            }),
            // ---------------------------------------------------------------- grabar
            new Tut("grabar", "record", "rec", "Grabar un video", "Recording a video", new String[][]{
                    s("record", "Tocá «Grabar» en los ajustes rápidos, o en la tarjeta «Grabación» de VRmin Host. Otro toque la termina.", "Tap “Record” in quick settings, or on the “Recording” card in VRmin Host. Tap again to stop."),
                    s("cable", "Con el visor conectado por cable se graba también lo que ves en el visor; si no, solo la Mac.", "With the headset on the cable, what you see in the headset is recorded too; otherwise only the Mac."),
                    s("laptop", "Los videos quedan en Películas › VRmin, en la Mac. Van sin sonido.", "Videos go to Movies › VRmin on the Mac. They have no sound."),
                    s("bulb", "La grabación del visor sale con los dos ojos lado a lado: para un video, recortá uno.", "The headset recording has both eyes side by side: crop one for a video."),
            }),
    };

    static Tut find(String id) { for (Tut t : ALL) if (t.id.equals(id)) return t; return null; }

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("qvb", Context.MODE_PRIVATE); }
    /** Los que ya aparecieron en esta sesión de la app (vuelven la próxima vez, salvo que elijas no verlos más). */
    private static final java.util.Set<String> shownNow = new java.util.HashSet<>();

    static boolean wanted(Context c, String id) { return enabled(c, id) && !shownNow.contains(id); }
    /** No lo apagaste (ni a todos los tutoriales). */
    static boolean enabled(Context c, String id) {
        SharedPreferences p = prefs(c);
        return !p.getBoolean("tutOff", false) && !p.getBoolean("tutNo_" + id, false);
    }

    /** Muestra el tutorial solo si corresponde (no lo apagaste y no apareció todavía en esta sesión). after: al cerrarlo. */
    static boolean auto(FrameLayout root, String id) { return auto(root, id, null); }
    static boolean auto(FrameLayout root, String id, Runnable after) {
        if (root == null || id == null || !wanted(root.getContext(), id) || root.findViewWithTag(TAG) != null || root.findViewWithTag(TAG_HELP) != null) return false;
        Tut t = find(id);
        if (t == null) return false;
        shownNow.add(id);
        show(root, t, true, after, null);
        return true;
    }

    /**
     * Un tutorial con un botón para empezar (el Modo Centinela): si lo apagaste, empieza directo sin mostrarlo.
     */
    static void orStart(FrameLayout root, String id, String startLabel, Runnable start) {
        Tut t = find(id);
        if (t == null || !enabled(root.getContext(), id)) { start.run(); return; }
        show(root, t, true, null, new Object[]{startLabel, start});
    }

    private static final String TAG = "vrmin-tutorial", TAG_HELP = "vrmin-help";

    private static void close(FrameLayout root, String tag, Runnable after) {
        View v = root.findViewWithTag(tag);
        if (v == null) { if (after != null) after.run(); return; }
        v.setTag(null);
        v.animate().alpha(0f).setDuration(140).withEndAction(() -> { root.removeView(v); if (after != null) after.run(); }).start();
    }

    /** Fondo oscuro a pantalla completa con una tarjeta en el medio; devuelve la columna de la tarjeta. */
    private static LinearLayout sheet(FrameLayout root, String tag) {
        Context c = root.getContext();
        View old = root.findViewWithTag(tag);
        if (old != null) root.removeView(old);
        FrameLayout dim = new FrameLayout(c);
        dim.setTag(tag);
        dim.setBackgroundColor(Color.argb(tag.equals(TAG_HELP) || root.findViewWithTag(TAG_HELP) == null ? 150 : 60, 0, 0, 0));
        dim.setClickable(true);   // (no deja tocar lo de abajo)
        dim.setElevation(dp(c, tag.equals(TAG_HELP) ? 60 : 70));
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = Ui.glass(c, 32, 246);
        bg.setStroke(dp(c, 1), Color.argb(60, 255, 255, 255));
        card.setBackground(bg);
        card.setPadding(dp(c, 30), dp(c, 26), dp(c, 30), dp(c, 22));
        card.setClickable(true);
        int rootW = root.getWidth() > 0 ? root.getWidth() : dp(c, 1000);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(dp(c, 700), rootW - dp(c, 40)), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        dim.addView(card, lp);
        root.addView(dim, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        dim.setAlpha(0f);
        card.setScaleX(0.96f); card.setScaleY(0.96f);
        dim.animate().alpha(1f).setDuration(180).start();
        card.animate().scaleX(1f).scaleY(1f).setDuration(220).setInterpolator(new android.view.animation.DecelerateInterpolator(1.6f)).start();
        return card;
    }
    private static int maxH(FrameLayout root) {
        Context c = root.getContext();
        int rootH = root.getHeight() > 0 ? root.getHeight() : dp(c, 700);
        return Math.max(dp(c, 240), rootH - dp(c, 260));
    }

    /** Lista con desplazamiento que no pasa de cierta altura (así la tarjeta entra en la pantalla). */
    private static final class MaxScroll extends ScrollView {
        final int maxH;
        MaxScroll(Context c, int maxH) { super(c); this.maxH = maxH; setVerticalScrollBarEnabled(false); }
        @Override protected void onMeasure(int w, int h) {
            int size = MeasureSpec.getSize(h);
            int lim = MeasureSpec.getMode(h) == MeasureSpec.UNSPECIFIED ? maxH : Math.min(size, maxH);
            super.onMeasure(w, MeasureSpec.makeMeasureSpec(lim, MeasureSpec.AT_MOST));
        }
    }

    private static void titleRow(LinearLayout card, String icon, String title, String sub) {
        Context c = card.getContext();
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        View tile = new View(c);
        GradientDrawable g = new GradientDrawable();
        g.setColor(Ui.ACCENT);
        g.setCornerRadius(Ui.rad(c, 14));
        tile.setBackground(g);
        tile.setForeground(Icons.of(c, icon, 28, Ui.ACCENT_DARK));
        tile.setForegroundGravity(Gravity.CENTER);
        r.addView(tile, new LinearLayout.LayoutParams(dp(c, 52), dp(c, 52)));
        LinearLayout col = new LinearLayout(c);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(c, 16), 0, 0, 0);
        col.addView(Ui.label(c, title, 26, Ui.TEXT, true));
        if (sub != null) col.addView(Ui.label(c, sub, 14, Ui.TEXT2, false));
        r.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(r);
    }

    private static TextView smallButton(Context c, String text, Runnable act) {
        TextView b = Ui.label(c, text, 14, Ui.TEXT2, true);
        b.setPadding(dp(c, 12), dp(c, 8), dp(c, 12), dp(c, 8));
        b.setBackground(Ui.hoverable(c, 0, 999, false));
        b.setClickable(true);
        b.setOnClickListener(v -> act.run());
        return b;
    }

    /**
     * Muestra un tutorial. auto: apareció solo (abajo, «No mostrar este otra vez» y «No mostrar más tutoriales");
     * si no, se abrió desde la Ayuda (que queda abierta detrás). start: {texto, acción} para un botón «Empezar».
     */
    static void show(FrameLayout root, Tut t, boolean auto, Runnable after, Object[] start) {
        Context c = root.getContext();
        boolean en = I18n.english();
        LinearLayout card = sheet(root, TAG);
        String groupName = "";
        for (String[] g : GROUPS) if (g[0].equals(t.group)) groupName = en ? g[2] : g[1];
        titleRow(card, t.icon, t.title(), groupName);
        LinearLayout list = new LinearLayout(c);
        list.setOrientation(LinearLayout.VERTICAL);
        for (String[] st : t.steps) {
            LinearLayout r = new LinearLayout(c);
            r.setOrientation(LinearLayout.HORIZONTAL);
            r.setGravity(Gravity.CENTER_VERTICAL);
            r.setPadding(0, dp(c, 7), 0, dp(c, 7));
            r.addView(Ui.iconTile(c, st[0]), new LinearLayout.LayoutParams(dp(c, 42), dp(c, 42)));
            TextView tx = Ui.label(c, en ? st[2] : st[1], 16, Ui.TEXT, false);
            tx.setPadding(dp(c, 14), 0, 0, 0);
            tx.setLineSpacing(0, 1.1f);
            r.addView(tx, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            list.addView(r);
        }
        MaxScroll sc = new MaxScroll(c, maxH(root));
        sc.addView(list);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.topMargin = dp(c, 14);
        card.addView(sc, slp);

        // Abajo: lo principal a la derecha; las opciones de no verlo más, chiquitas a la izquierda
        LinearLayout bar = new LinearLayout(c);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(c, 16);
        card.addView(bar, blp);
        if (auto) {
            LinearLayout opts = new LinearLayout(c);
            opts.setOrientation(LinearLayout.VERTICAL);
            opts.addView(smallButton(c, en ? "Don't show this one again" : "No mostrar este otra vez",
                    () -> { prefs(c).edit().putBoolean("tutNo_" + t.id, true).apply(); close(root, TAG, start == null ? after : null); }));
            opts.addView(smallButton(c, en ? "Don't show any more tutorials" : "No mostrar más tutoriales",
                    () -> { prefs(c).edit().putBoolean("tutOff", true).apply(); close(root, TAG, null); }));
            bar.addView(opts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        } else {
            bar.addView(new View(c), new LinearLayout.LayoutParams(0, 1, 1f));
        }
        if (start != null) {
            TextView later = Ui.pill(c, en ? "Not now" : "Ahora no");
            later.setOnClickListener(v -> close(root, TAG, null));
            LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            llp.rightMargin = dp(c, 10);
            bar.addView(later, llp);
            TextView go = Ui.primary(c, (String) start[0]);
            go.setOnClickListener(v -> close(root, TAG, (Runnable) start[1]));
            bar.addView(go);
        } else {
            TextView ok = Ui.primary(c, auto ? (en ? "Got it" : "Entendido") : (en ? "Back to Help" : "Volver a la Ayuda"));
            if (!auto) Ui.leadingIcon(ok, "back", 18, Ui.ACCENT_DARK);
            ok.setOnClickListener(v -> close(root, TAG, after));
            bar.addView(ok);
        }
    }

    /** Una pregunta antes de algo que borra lo tuyo (como el aviso del portal): «Cancelar» o la acción. */
    static void confirm(FrameLayout root, String title, String text, String yes, Runnable act) {
        Context c = root.getContext();
        LinearLayout card = sheet(root, TAG);
        titleRow(card, "help", title, null);
        TextView t = Ui.label(c, text, 16, Ui.TEXT2, false);
        t.setPadding(0, dp(c, 12), 0, 0);
        card.addView(t);
        LinearLayout bar = new LinearLayout(c);
        bar.setGravity(Gravity.END);
        TextView no = Ui.pill(c, I18n.english() ? "Cancel" : "Cancelar");
        no.setOnClickListener(v -> close(root, TAG, null));
        LinearLayout.LayoutParams nlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        nlp.rightMargin = dp(c, 10);
        bar.addView(no, nlp);
        TextView ok = Ui.primary(c, yes);
        ok.setOnClickListener(v -> close(root, TAG, act));
        bar.addView(ok);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(c, 18);
        card.addView(bar, blp);
    }

    /** Ayuda: todos los tutoriales por tema, y si aparecen solos o no. */
    static void showHelp(FrameLayout root) {
        Context c = root.getContext();
        boolean en = I18n.english();
        LinearLayout card = sheet(root, TAG_HELP);
        titleRow(card, "help", en ? "Help" : "Ayuda", en ? "Short guides for everything in VRmin" : "Guías cortas de todo VRmin");
        LinearLayout list = new LinearLayout(c);
        list.setOrientation(LinearLayout.VERTICAL);
        for (String[] g : GROUPS) {
            list.addView(Ui.section(c, en ? g[2] : g[1]));
            for (Tut t : ALL) {
                if (!t.group.equals(g[0])) continue;
                LinearLayout r = Ui.row(c, t.icon, t.title(), null);
                r.addView(Ui.label(c, "›", 26, Ui.TEXT2, false));
                r.setOnClickListener(v -> show(root, t, false, null, null));   // (la Ayuda queda abierta detrás)
                list.addView(r);
            }
        }
        // Tutoriales automáticos
        list.addView(Ui.section(c, en ? "Tutorials" : "Tutoriales"));
        SharedPreferences p = prefs(c);
        boolean off = p.getBoolean("tutOff", false);
        LinearLayout tr = Ui.row(c, "bulb", en ? "Show tutorials on their own" : "Mostrar tutoriales solos",
                off ? (en ? "Off: only here in Help" : "Apagado: solo acá en la Ayuda") : (en ? "On: the first time you reach each place" : "Encendido: la primera vez que llegás a cada lugar"));
        TextView sw = Ui.pill(c, off ? (en ? "Turn on" : "Encender") : (en ? "Turn off" : "Apagar"));
        tr.addView(sw);
        View.OnClickListener tog = v -> {
            SharedPreferences.Editor e = p.edit().putBoolean("tutOff", !off);
            if (off) for (Tut t : ALL) e.remove("tutNo_" + t.id);   // al encenderlos de nuevo, vuelven todos
            e.apply();
            showHelp(root);
        };
        tr.setOnClickListener(tog);
        sw.setOnClickListener(tog);
        list.addView(tr);
        MaxScroll sc = new MaxScroll(c, maxH(root));
        sc.addView(list);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.topMargin = dp(c, 8);
        card.addView(sc, slp);
        LinearLayout bar = new LinearLayout(c);
        bar.setGravity(Gravity.END);
        TextView ok = Ui.primary(c, en ? "Close" : "Cerrar");
        ok.setOnClickListener(v -> close(root, TAG_HELP, null));
        bar.addView(ok);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(c, 14);
        card.addView(bar, blp);
    }
}
