#!/usr/bin/env python3
"""
QuestVRBridge - servidor puente (etapa 2).

- Sirve la página WebXR por HTTPS y un WebSocket (/ws) para el Quest.
- Escucha en 127.0.0.1:9000 al renderizador (QuestRenderer, Swift+Metal).
- Quest -> renderizador: poses (JSON, una por línea).
- Renderizador -> Quest: cuadros de video H.264 (binario, con prefijo de largo).
Solo usa la biblioteca estándar de Python.
"""
import asyncio, base64, hashlib, json, os, socket, ssl, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get("PORT", "8443"))
RENDER_PORT = int(os.environ.get("RENDER_PORT", "9000"))
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FPS = int(os.environ.get("VR_FPS", "72"))
MAX_BACKLOG = 1_500_000  # bytes pendientes por cliente antes de descartar video

clients = {}            # writer -> {"need_key": bool}
renderer = {"w": None}
S = {}


def reset_stats():
    S.update(poses=0, rtts=[], last=None, vframes=0, vbytes=0, enc_ms=[], dropped=0, achunks=0,
             _diag=S.get("_diag"))


reset_stats()


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def ws_frame(writer, opcode, data):
    n = len(data)
    if n < 126:
        head = struct.pack(">BB", 0x80 | opcode, n)
    elif n < 65536:
        head = struct.pack(">BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack(">BBQ", 0x80 | opcode, 127, n)
    writer.write(head + data)


def send_bin(w, data):
    """Manda un paquete binario (video=1, sonido=2) a un visor, sea navegador o app nativa."""
    if clients.get(w, {}).get("kind") == "raw":
        w.write(struct.pack(">I", len(data)) + data)
    else:
        ws_frame(w, 0x2, data)


def send_text(w, text):
    if clients.get(w, {}).get("kind") == "raw":
        b = b"\x03" + text.encode()
        w.write(struct.pack(">I", len(b)) + b)
    else:
        ws_frame(w, 0x1, text.encode())


def ask_keyframe():
    w = renderer["w"]
    if w is not None:
        w.write(b'{"type":"key"}\n')


# ---------------- Quest (HTTPS + WebSocket) ----------------
async def handle_quest(reader, writer):
    try:
        line = (await reader.readline()).decode("latin-1").strip()
        parts = line.split()
        if len(parts) < 2:
            return
        path = parts[1].split("?")[0]
        headers = {}
        while True:
            h = (await reader.readline()).decode("latin-1")
            if h in ("\r\n", "\n", ""):
                break
            k, _, v = h.partition(":")
            headers[k.strip().lower()] = v.strip()
        if path == "/ws" and headers.get("upgrade", "").lower() == "websocket":
            await websocket(reader, writer, headers)
        else:
            serve_file(writer, path)
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError, OSError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


def serve_file(writer, path):
    if path in ("/", ""):
        path = "/index.html"
    name = os.path.basename(path)
    full = os.path.join(HERE, "web", name)
    if not os.path.isfile(full):
        body, status, ctype = b"not found", "404 Not Found", "text/plain"
    else:
        with open(full, "rb") as f:
            body = f.read()
        status = "200 OK"
        ctype = {"html": "text/html; charset=utf-8", "js": "text/javascript"}.get(
            name.rsplit(".", 1)[-1], "application/octet-stream")
    writer.write((f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\n"
                  f"Content-Length: {len(body)}\r\nCache-Control: no-store\r\n"
                  "Connection: close\r\n\r\n").encode() + body)


async def websocket(reader, writer, headers):
    key = headers.get("sec-websocket-key", "")
    accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
    writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    sock = writer.get_extra_info("socket")
    if sock is not None:
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
    peer = writer.get_extra_info("peername")
    clients[writer] = {"need_key": True, "kind": "ws"}
    send_text(writer, json.dumps({"type": "config", "fps": FPS}))
    ask_keyframe()
    print(f"[+] Quest conectado desde {peer[0] if peer else '?'}", flush=True)
    try:
        while True:
            hdr = await reader.readexactly(2)
            b1, b2 = hdr
            opcode, length = b1 & 0x0F, b2 & 0x7F
            if length == 126:
                length = struct.unpack(">H", await reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", await reader.readexactly(8))[0]
            mask = await reader.readexactly(4) if b2 & 0x80 else b""
            payload = bytearray(await reader.readexactly(length))
            if mask:
                for i in range(len(payload)):
                    payload[i] ^= mask[i % 4]
            if opcode == 0x8:
                break
            if opcode == 0x9:
                ws_frame(writer, 0xA, bytes(payload))
                continue
            if opcode != 0x1:
                continue
            on_quest_message(writer, payload.decode("utf-8", "replace"))
    finally:
        clients.pop(writer, None)
        print("[-] Quest desconectado", flush=True)


def on_quest_message(writer, text):
    try:
        data = json.loads(text)
    except ValueError:
        return
    kind = data.get("type")
    if kind == "pose":
        send_text(writer, json.dumps({"type": "echo", "t": data.get("t")}))
        S["poses"] += 1
        S["last"] = data
        if isinstance(data.get("rtt"), (int, float)):
            S["rtts"].append(data["rtt"])
        w = renderer["w"]
        if w is not None and data.get("views"):
            w.write(text.encode() + b"\n")
    elif kind == "log":
        print("  [visor]", str(data.get("msg", ""))[:300], flush=True)
    elif kind == "key":
        clients.get(writer, {})["need_key"] = True
        ask_keyframe()


# ---------------- Renderizador (Swift) ----------------
async def handle_renderer(reader, writer):
    if renderer["w"] is not None:
        writer.close()
        return
    renderer["w"] = writer
    sock = writer.get_extra_info("socket")
    if sock is not None:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print("[+] Renderizador de la Mac conectado", flush=True)
    ask_keyframe()
    try:
        while True:
            n = struct.unpack(">I", await reader.readexactly(4))[0]
            data = await reader.readexactly(n)
            if len(data) < 8:
                continue
            if data[0] == 2:
                # Sonido (PCM 16 bits): se manda tal cual; si la red está saturada, se descarta
                S["achunks"] += 1
                for w in list(clients):
                    if w.transport.get_write_buffer_size() <= MAX_BACKLOG:
                        send_bin(w, data)
                continue
            is_key = data[5] == 1
            S["vframes"] += 1
            S["vbytes"] += n
            S["enc_ms"].append(struct.unpack(">H", data[6:8])[0] / 10.0)
            for w, st in list(clients.items()):
                if st["need_key"] and not is_key:
                    continue
                if w.transport.get_write_buffer_size() > MAX_BACKLOG:
                    # Red saturada: descartamos y pedimos un cuadro completo nuevo.
                    S["dropped"] += 1
                    st["need_key"] = True
                    ask_keyframe()
                    continue
                st["need_key"] = False
                send_bin(w, data)
    except (asyncio.IncompleteReadError, ConnectionError, OSError):
        pass
    finally:
        renderer["w"] = None
        print("[-] Renderizador desconectado", flush=True)


# ---------------- App nativa del Quest (TCP simple, sin navegador) ----------------
# Paquetes con prefijo de largo (4 bytes, big endian).
#  visor -> Mac: texto JSON (mismo formato que la página: pose, key, log)
#  Mac -> visor: 1=video, 2=sonido, 3=texto JSON (config, echo)
NATIVE_PORT = int(os.environ.get("NATIVE_PORT", "9100"))
BEACON_PORT = int(os.environ.get("BEACON_PORT", "9101"))


async def handle_native(reader, writer):
    sock = writer.get_extra_info("socket")
    if sock is not None:
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
        except OSError:
            pass
    peer = writer.get_extra_info("peername")
    clients[writer] = {"need_key": True, "kind": "raw"}
    send_text(writer, json.dumps({"type": "config", "fps": FPS}))
    ask_keyframe()
    print(f"[+] Quest conectado (app nativa) desde {peer[0] if peer else '?'}", flush=True)
    try:
        while True:
            n = struct.unpack(">I", await reader.readexactly(4))[0]
            if n > 1 << 20:
                break
            payload = await reader.readexactly(n)
            on_quest_message(writer, payload.decode("utf-8", "replace"))
    except (asyncio.IncompleteReadError, ConnectionError, OSError):
        pass
    finally:
        clients.pop(writer, None)
        print("[-] Quest desconectado", flush=True)
        try:
            writer.close()
        except Exception:
            pass


async def beacon():
    """Avisa por la red local dónde está la Mac, para que la app del Quest la encuentre sola."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setblocking(False)
    msg = json.dumps({"qvb": 1, "port": NATIVE_PORT, "name": socket.gethostname()}).encode()
    while True:
        for dest in ("255.255.255.255", "<broadcast>"):
            try:
                s.sendto(msg, (dest, BEACON_PORT))
                break
            except OSError:
                continue
        await asyncio.sleep(1)


def avg(xs):
    return sum(xs) / len(xs) if xs else None


async def reporter():
    while True:
        await asyncio.sleep(1)
        snap = dict(S)
        reset_stats()
        if not clients:
            continue
        parts = [f"Quest {snap['poses']:3d} poses/s"]
        r = avg(snap["rtts"])
        if r is not None:
            parts.append(f"red {r:4.1f} ms")
        if renderer["w"] is not None:
            e = avg(snap["enc_ms"])
            parts.append(f"| Mac {snap['vframes']:3d} fps  {snap['vbytes'] * 8 / 1e6:5.1f} Mbps"
                         + (f"  render+codif {e:4.1f} ms" if e is not None else ""))
            if snap["dropped"]:
                parts.append(f"descartados {snap['dropped']}")
            if snap["achunks"]:
                parts.append("| sonido OK")
        else:
            parts.append("| renderizador NO conectado")
        last = snap["last"] or {}
        diag = last.get("diag")
        if diag and diag != S.get("_diag"):
            S["_diag"] = diag
            print("  [visor] subida:", diag.get("up"), "| cuadro:", diag.get("fmt") or "-",
                  "| pixel muestra:", diag.get("pix") or "-", "| codec:", diag.get("codec"),
                  "| errores:", (diag.get("glErr") or "") + " " + (diag.get("derr") or ""), flush=True)
        if isinstance(last.get("vfps"), (int, float)):
            parts.append(f"| Visor {last['vfps']:3.0f} fps")
        if isinstance(last.get("m2p"), (int, float)):
            parts.append(f"latencia total {last['m2p']:5.1f} ms")
        print("  ".join(parts), flush=True)


async def main():
    cdir = os.environ.get("QVB_DATA") or HERE   # la app guarda el certificado en su carpeta de datos
    cert, key = os.path.join(cdir, "cert.pem"), os.path.join(cdir, "key.pem")
    if not (os.path.isfile(cert) and os.path.isfile(key)):
        sys.exit("Falta el certificado. Ejecutá: bash start.sh")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    quest_srv = await asyncio.start_server(handle_quest, "0.0.0.0", PORT, ssl=ctx)
    rend_srv = await asyncio.start_server(handle_renderer, "127.0.0.1", RENDER_PORT)
    # (la app nativa del Quest se conecta al renderizador, que pide el emparejado; acá ya no se atiende)
    print("=" * 64)
    print(" QuestVRBridge - Etapa 2 (video desde la Mac)")
    print(f" En el navegador del Quest 3 abrí:  https://{local_ip()}:{PORT}")
    print(" Ctrl+C para cortar")
    print("=" * 64, flush=True)
    asyncio.ensure_future(reporter())
    # (el aviso en la red lo hace QuestRenderer con el nombre de la Mac; uno acá la mostraba dos veces, con ".local")
    async with quest_srv, rend_srv:
        await asyncio.gather(quest_srv.serve_forever(), rend_srv.serve_forever())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nListo.")
