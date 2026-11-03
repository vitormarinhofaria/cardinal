#!/usr/bin/env python3
"""A pretend Trello for developing and benchmarking Cardinal without real credentials.

    tools/mock_trello.py --port 8099 --cards 2000
    build/ui-gtk/cardinal_gtk --api-base http://127.0.0.1:8099/1 --api-key K --token T0123456789abcdefghij

It implements only the endpoints Cardinal calls, keeps everything in memory, accepts any key
and token, and applies writes (card edits, moves, labels, members, comments).

It also speaks a minimal version of Trello's websocket (ws://127.0.0.1:8099/1/Session/socket): it
answers subscribe/ping and, after every write, notifies the subscribers of the board. Pass
--socket-url ws://127.0.0.1:8099/1/Session/socket to the app to use it.

Webhooks work too (the app uses them by default; with --api-base it skips the tunnel): created,
updated and deleted through /1/webhooks, checked with a HEAD like Trello does, and called with a
POST after every write.
"""

import argparse
import base64
import hashlib
import json
import random
import struct
import threading
import time
import urllib.parse
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
STATE = {}
SOCKETS = {}  # Handler -> subscribed board id (or None); guarded by SOCKETS_LOCK
SOCKETS_LOCK = threading.Lock()
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC11B65"


def ws_frame(text):
    data = text.encode()
    head = bytes([0x81])
    if len(data) < 126:
        head += bytes([len(data)])
    elif len(data) < 65536:
        head += bytes([126]) + struct.pack(">H", len(data))
    else:
        head += bytes([127]) + struct.pack(">Q", len(data))
    return head + data


HOOKS = {}  # id -> webhook dict; guarded by HOOKS_LOCK
HOOKS_LOCK = threading.Lock()


def reachable(url):
    try:
        urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=3)
        return True
    except Exception:
        return False


def hooks_broadcast():
    with HOOKS_LOCK:
        targets = [dict(h) for h in HOOKS.values() if h["active"]]

    def deliver(h):
        body = json.dumps({"action": {"type": "updateCard"}, "model": {"id": h["idModel"]}}).encode()
        try:
            urllib.request.urlopen(urllib.request.Request(h["callbackURL"], data=body, method="POST",
                                                          headers={"Content-Type": "application/json"}), timeout=3)
            print("webhook: delivered to %s" % h["id"], flush=True)
        except Exception as e:
            print("webhook: delivery to %s failed: %s" % (h["id"], e), flush=True)
    for h in targets:
        threading.Thread(target=deliver, args=(h,), daemon=True).start()


def ws_broadcast():
    """Something changed: tell every subscriber of a board (content is not what the app reads)."""
    with SOCKETS_LOCK:
        targets = [(h, b) for h, b in SOCKETS.items() if b]
    for h, board in targets:
        h.ws_send(json.dumps({"idModelChannel": board,
                              "notify": {"event": "updateModels", "typeName": "Card", "deltas": []}}))


def iso(t=None):
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(t or time.time()))


def build(cards_total, seed):
    rnd = random.Random(seed)
    labels = [
        ("lb%d" % i, name, color)
        for i, (name, color) in enumerate(
            [("Bug", "red"), ("Feature", "green"), ("Urgente", "orange"), ("Design", "purple"),
             ("Docs", "blue"), ("Infra", "sky"), ("Pesquisa", "yellow"), ("", "black")]
        )
    ]
    members = [("m%d" % i, n, "".join(p[0] for p in n.split())) for i, n in
               enumerate(["Vitor Marinho", "Ana Barbosa", "Carlos Lima", "Dora Nunes", "Edu Prado", "Fabi Costa"])]
    names = ["A fazer", "Fazendo", "Em revisão", "Pronto", "Backlog", "Ideias", "Arquivo 2023", "Arquivo 2024",
             "Clientes", "Suporte", "Bugs antigos", "Roadmap"]
    sizes = [14, 9, 6, 20] + [0] * (len(names) - 4)
    rest = cards_total - sum(sizes)
    cold = len(names) - 4
    for i in range(cold):
        sizes[4 + i] = rest // cold + (1 if i < rest % cold else 0)

    lists, cards, comments = {}, {}, {}
    now = time.time()
    n = 0
    verbs = ["Corrigir", "Revisar", "Migrar", "Documentar", "Investigar", "Refatorar", "Publicar", "Medir"]
    objs = ["login SSO", "banco de dados", "fila de e-mails", "relatório mensal", "painel de métricas",
            "integração de pagamentos", "tela de perfil", "cache de imagens", "script de deploy"]
    for li, (name, size) in enumerate(zip(names, sizes)):
        lid = "l%d" % li
        lists[lid] = {"id": lid, "name": name, "pos": (li + 1) * 65536.0, "cards": [],
                      "color": LIST_COLORS[li] if li < len(LIST_COLORS) else None}
        for ci in range(size):
            n += 1
            cid = "c%d" % n
            card = {
                "id": cid, "idList": lid,
                "name": "%s %s #%d" % (rnd.choice(verbs), rnd.choice(objs), n),
                "desc": "Descrição do card %d.\n\nCom **markdown** e acentuação: ação, coração, 日本語 🚀." % n,
                "pos": (ci + 1) * 65536.0,
                "idLabels": rnd.sample([l[0] for l in labels], rnd.choice([0, 0, 1, 1, 2])),
                "idMembers": rnd.sample([m[0] for m in members], rnd.choice([0, 1, 1, 2])),
                "dateLastActivity": iso(now - rnd.randint(0, 90 * 86400)),
                "closed": False,
                "shortLink": "sl%06d" % n,
            }
            cs = []
            for k in range(rnd.choice([0, 0, 1, 2, 4])):
                cs.append({"id": "a%d_%d" % (n, k), "idMemberCreator": rnd.choice(members)[0],
                           "date": iso(now - rnd.randint(0, 30 * 86400)),
                           "data": {"text": "Comentário %d do card %d. Vamos olhar isso na sprint." % (k + 1, n)}})
            if n <= 3:  # pictures pasted in the description and in a comment, as Trello writes them
                card["desc"] += ("\n\nTela atual:\n\n![captura.png](@BASE@/1/cards/%s/attachments/at%s_0/download/captura.png)"
                                 % (cid, cid))
                cs.append({"id": "a%d_img" % n, "idMemberCreator": members[1][0], "date": iso(now - 3600),
                           "data": {"text": "Olha o erro:\n\n![erro.png](@BASE@/1/img/erro%d.png)\n\nacontece sempre." % n}})
            if n == 1:  # the markdown Trello users write, with links to other cards
                card["desc"] += MARKDOWN_SAMPLE % {"last": cards_total}
                cs.append({"id": "a1_md", "idMemberCreator": members[2][0], "date": iso(now - 600),
                           "data": {"text": "Relacionado: https://trello.com/c/sl000002/2-card\n\n"
                                            "Ver também [o card antigo](https://trello.com/c/sl%06d) e `make test`." % cards_total}})
            cards[cid] = card
            comments[cid] = cs
            lists[lid]["cards"].append(cid)
    STATE.update(
        boards={"b1": {"id": "b1", "name": "Produto", "closed": False, "prefs": dict(BACKGROUNDS["gradient"])}},
        labels=labels, members=members, lists=lists, cards=cards, comments=comments, seq=n,
    )


# Trello list colors for the first lists (the rest have none), like a board recolored on Trello web.
LIST_COLORS = ["green", None, "yellow", "purple", "teal", "red"]

# Board backgrounds as Trello reports them in `prefs` (choose with --background).
BACKGROUNDS = {
    "gradient": {"background": "gradient-ocean", "backgroundImage": "https://trello.example/gradients/ocean.svg",
                 "backgroundTopColor": "#0c66e4", "backgroundBottomColor": "#37b4c3",
                 "backgroundBrightness": "dark"},
    "color": {"background": "green", "backgroundColor": "#4bbf6b", "backgroundBrightness": "dark"},
    "light": {"background": "grey", "backgroundColor": "#dfe1e6", "backgroundBrightness": "light"},
    # backgroundImageScaled is filled in with the mock's own URL in main()
    "picture": {"backgroundColor": "#6b5a4a", "backgroundTopColor": "#7a6a5a",
                "backgroundBottomColor": "#4a3a2a", "backgroundBrightness": "dark"},
    "none": {},
}


MARKDOWN_SAMPLE = """

## Contexto

O login **SSO** falha com _alguns_ provedores. Passos:

1. Abrir a tela de login
2. Escolher ~~Google~~ Microsoft
   - com conta corporativa
   - sem MFA
3. Ver o erro

- [x] Reproduzido
- [ ] Corrigido

> O erro aparece só em produção.
> > Confirmado pelo suporte.

```
GET /oauth/callback?code=...
HTTP/1.1 500
```

| Provedor | Status |
|---|---|
| Google | ok |
| Microsoft | **falha** |

Docs: https://example.com/sso e [guia interno](https://example.com/guia).

https://trello.com/c/sl000002/2-card-relacionado

Depende também de https://trello.com/c/sl%(last)06d e de https://trello.com/c/zzzz9999 (apagado).

---

<b>HTML</b> não é interpretado."""


PNG_CACHE = {}


def png(w, h, hue):
    """A w×h gradient PNG, so attachments and their previews are real pictures."""
    key = (w, h, hue)
    if key in PNG_CACHE:
        return PNG_CACHE[key]
    r0, g0, b0 = [(hue * k) % 256 for k in (97, 57, 151)]
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        v = y * 255 // max(1, h - 1)
        for x in range(w):
            u = x * 255 // max(1, w - 1)
            rows += bytes(((r0 + u) % 256, (g0 + v) % 256, (b0 + (u ^ v) // 2) % 256))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    out = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))
    PNG_CACHE[key] = out
    return out


# Uploaded attachments: id -> (name, width, height). Their downloads need the OAuth header, as on Trello.
IMAGE_SIZES = {"full": (1600, 1200), "p150": (150, 113), "p600": (600, 450)}


def attachments_of(cid, base):
    n = int(cid[1:]) if cid[1:].isdigit() else 0
    out = []
    for k, name in enumerate(["captura.png", "diagrama.png"][: 1 + n % 2]):
        aid = "at%s_%d" % (cid, k)
        url = "%s/1/cards/%s/attachments/%s/download/%s" % (base, cid, aid, name)
        out.append({"id": aid, "name": name, "url": url, "mimeType": "image/png", "isUpload": True,
                    "previews": [{"id": p, "url": "%s/1/cards/%s/attachments/%s/previews/%s/download/%s" %
                                  (base, cid, aid, p, name), "width": w, "height": h, "scaled": p != "full"}
                                 for p, (w, h) in IMAGE_SIZES.items()]})
    out.append({"id": "pdf" + cid, "name": "especificacao.pdf", "url": "https://example.com/especificacao.pdf",
                "mimeType": "application/pdf", "isUpload": False, "previews": []})
    return out


def pick(obj, fields):
    return {k: obj[k] for k in fields if k in obj}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if STATE.get("verbose"):
            print("%s %s" % (self.command, self.path))

    def base(self):
        return "http://" + self.headers.get("Host", "127.0.0.1")

    def reply_png(self, data):
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def reply(self, status, body):
        data = json.dumps(body).replace("@BASE@", self.base()).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        if STATE.get("delay"):
            pass

    def route(self, method):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        length = int(self.headers.get("Content-Length") or 0)
        form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(length).decode(), keep_blank_values=True).items()} if length else {}
        if STATE.get("delay"):
            time.sleep(STATE["delay"])
        parts = [p for p in u.path.split("/") if p]  # ["1", "cards", "c1", ...]
        if parts == ["1", "Session", "socket"] and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.websocket()
        if self.images(method, parts):
            return
        if self.webhooks(method, parts, form):
            return
        with LOCK:
            try:
                self.dispatch(method, parts, q, form)
            except KeyError:
                return self.reply(404, "not found")
        if method != "GET":
            ws_broadcast()
            hooks_broadcast()

    def ws_send(self, text):
        try:
            with self.ws_lock:
                self.wfile.write(ws_frame(text))
                self.wfile.flush()
        except OSError:
            pass

    def ws_read(self):
        """One frame: (opcode, payload bytes), or None when the peer went away."""
        head = self.rfile.read(2)
        if len(head) < 2:
            return None
        opcode, n = head[0] & 0x0F, head[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.rfile.read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self.rfile.read(8))[0]
        mask = self.rfile.read(4) if head[1] & 0x80 else b"\0\0\0\0"
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(n)))
        return opcode, data

    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        self.ws_lock = threading.Lock()
        with SOCKETS_LOCK:
            SOCKETS[self] = None
        print("socket: connected", flush=True)
        try:
            while True:
                frame = self.ws_read()
                if frame is None or frame[0] == 0x8:
                    break
                opcode, data = frame
                if opcode == 0x9:  # ping
                    with self.ws_lock:
                        self.wfile.write(bytes([0x8A, len(data)]) + data)
                        self.wfile.flush()
                    continue
                if opcode != 0x1:
                    continue
                msg = json.loads(data.decode() or "{}")
                kind = msg.get("type")
                if kind == "subscribe":
                    with SOCKETS_LOCK:
                        SOCKETS[self] = msg.get("idModel")
                    print("socket: subscribe %s" % msg.get("idModel"), flush=True)
                elif kind == "unsubscribe":
                    with SOCKETS_LOCK:
                        if SOCKETS.get(self) == msg.get("idModel"):
                            SOCKETS[self] = None
                self.ws_send(json.dumps({"reqid": msg.get("reqid"), "result": 1}))
        except (OSError, ValueError):
            pass
        finally:
            with SOCKETS_LOCK:
                SOCKETS.pop(self, None)
            print("socket: disconnected", flush=True)

    def webhooks(self, method, parts, form):
        """/1/webhooks and /1/tokens/{t}/webhooks, outside the lock: the HEAD check calls the app."""
        if parts[:2] == ["1", "tokens"] and parts[3:] == ["webhooks"] and method == "GET":
            with HOOKS_LOCK:
                self.reply(200, list(HOOKS.values()))
            return True
        if parts[:2] != ["1", "webhooks"]:
            return None
        if len(parts) == 2 and method == "POST":
            if not reachable(form.get("callbackURL", "")):
                self.reply(400, "URL (callbackURL) did not return 200 status code")
                return True
            with HOOKS_LOCK:
                STATE["seq"] += 1
                hid = "wh%d" % STATE["seq"]
                HOOKS[hid] = {"id": hid, "description": form.get("description", ""),
                              "callbackURL": form["callbackURL"], "idModel": form.get("idModel", ""),
                              "active": True}
                print("webhook: created %s -> %s" % (hid, form["callbackURL"]), flush=True)
                self.reply(200, HOOKS[hid])
            return True
        hid = parts[2] if len(parts) > 2 else ""
        with HOOKS_LOCK:
            exists = hid in HOOKS
        if not exists:
            self.reply(404, "not found")
            return True
        if method == "DELETE":
            with HOOKS_LOCK:
                HOOKS.pop(hid, None)
            print("webhook: deleted %s" % hid, flush=True)
            self.reply(200, {"_value": None})
            return True
        if method == "PUT":
            if "callbackURL" in form and not reachable(form["callbackURL"]):
                self.reply(400, "URL (callbackURL) did not return 200 status code")
                return True
            with HOOKS_LOCK:
                h = HOOKS[hid]
                for k in ("callbackURL", "idModel", "description"):
                    if k in form:
                        h[k] = form[k]
                h["active"] = form.get("active", "true") == "true"
                print("webhook: updated %s -> %s %s" % (hid, h["callbackURL"], h["idModel"]), flush=True)
                self.reply(200, h)
            return True
        self.reply(200, HOOKS.get(hid))
        return True

    def images(self, method, parts):
        """Picture downloads, served outside the lock: generating a PNG is slow in Python."""
        S = STATE
        if parts[:2] == ["1", "img"] and method == "GET":  # an outside picture: no credentials
            with LOCK:
                S["image_downloads"] = S.get("image_downloads", 0) + 1
            print("image download #%d %s" % (S["image_downloads"], self.path), flush=True)
            self.reply_png(png(800, 500, len(parts[-1])))
            return True
        if parts[:2] == ["1", "cards"] and "attachments" in parts and "download" in parts and method == "GET":
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("OAuth oauth_consumer_key="):
                self.reply(401, "unauthorized permission requested")
                return True
            size = parts[parts.index("previews") + 1] if "previews" in parts else "full"
            w, h = IMAGE_SIZES.get(size, IMAGE_SIZES["full"])
            with LOCK:
                S["image_downloads"] = S.get("image_downloads", 0) + 1
            print("image download #%d %s" % (S["image_downloads"], self.path), flush=True)
            self.reply_png(png(w, h, len(parts[4])))
            return True
        return None

    def dispatch(self, method, parts, q, form):
        S = STATE
        if parts[:3] == ["1", "members", "me"] and parts[3:] == ["boards"]:
            return self.reply(200, list(S["boards"].values()))
        if parts[:2] == ["1", "boards"]:
            bid = parts[2]
            board = S["boards"][bid]
            rest = parts[3:]
            if not rest:
                return self.reply(200, dict(board, labels=[{"id": i, "name": n, "color": c} for i, n, c in S["labels"]],
                                            members=[{"id": i, "fullName": n, "initials": s} for i, n, s in S["members"]]))
            if rest == ["lists"]:
                out = []
                for l in sorted(S["lists"].values(), key=lambda l: l["pos"]):
                    out.append({"id": l["id"], "name": l["name"], "pos": l["pos"], "color": l.get("color"),
                                "cards": [{"id": c} for c in l["cards"] if not S["cards"][c]["closed"]]})
                return self.reply(200, out)
            if rest == ["actions"]:
                return self.reply(200, [])
        if parts[:2] == ["1", "lists"] and parts[3:] == ["cards"]:
            lst = S["lists"][parts[2]]
            fields = q.get("fields", "").split(",")
            cs = [S["cards"][c] for c in lst["cards"] if not S["cards"][c]["closed"]]
            return self.reply(200, [pick(c, fields + ["id"]) for c in sorted(cs, key=lambda c: c["pos"])])
        if parts[:2] == ["1", "cards"]:
            return self.cards(method, parts, q, form)
        return self.reply(404, "not found")

    def cards(self, method, parts, q, form):
        S = STATE
        if len(parts) == 2 and method == "POST":
            S["seq"] += 1
            cid = "c%d" % S["seq"]
            lst = S["lists"][form["idList"]]
            last = max([S["cards"][c]["pos"] for c in lst["cards"]] or [0])
            card = {"id": cid, "idList": lst["id"], "name": form["name"], "desc": "", "pos": last + 65536.0,
                    "shortLink": "sl%06d" % S["seq"],
                    "idLabels": [], "idMembers": [], "dateLastActivity": iso(), "closed": False,
                    "badges": {"comments": 0}}
            S["cards"][cid] = card
            S["comments"][cid] = []
            lst["cards"].append(cid)
            return self.reply(200, card)
        cid = parts[2]
        if cid not in S["cards"]:  # Trello takes a card's shortLink wherever it takes its id
            cid = next((c["id"] for c in S["cards"].values() if c.get("shortLink") == cid), cid)
        card = S["cards"][cid]
        sub = parts[3:]
        if not sub and method == "GET":
            if "checklists" in q:
                return self.reply(200, {"id": cid, "checklists": [
                    {"id": "ck" + cid, "name": "Passos", "checkItems": [
                        {"id": "i1", "name": "Reproduzir", "state": "complete"},
                        {"id": "i2", "name": "Corrigir", "state": "incomplete"}]}],
                    "attachments": attachments_of(cid, self.base()),
                    "members": [{"id": m[0], "fullName": m[1], "initials": m[2]} for m in S["members"] if m[0] in card["idMembers"]]})
            out = pick(card, q.get("fields", "").split(",") + ["id"])
            if "actions" in q:
                out["actions"] = list(reversed(S["comments"][cid]))
            return self.reply(200, out)
        if not sub and method == "PUT":
            if "idList" in form:
                old = S["lists"][card["idList"]]
                old["cards"].remove(cid)
                S["lists"][form["idList"]]["cards"].append(cid)
                card["idList"] = form["idList"]
            if "pos" in form:
                card["pos"] = float(form["pos"])
            if "name" in form:
                card["name"] = form["name"]
            if "desc" in form:
                card["desc"] = form["desc"]
            card["dateLastActivity"] = iso()
            return self.reply(200, card)
        if sub == ["idLabels"] and method == "POST":
            if form["value"] not in card["idLabels"]:
                card["idLabels"].append(form["value"])
            return self.reply(200, card["idLabels"])
        if sub == ["idMembers"] and method == "POST":
            if form["value"] not in card["idMembers"]:
                card["idMembers"].append(form["value"])
            return self.reply(200, card["idMembers"])
        if sub == ["actions", "comments"] and method == "POST":
            S["seq"] += 1
            c = {"id": "a%d" % S["seq"], "idMemberCreator": S["members"][0][0], "date": iso(),
                 "data": {"text": form["text"]}}
            S["comments"][cid].append(c)
            card["badges"]["comments"] = len(S["comments"][cid])
            return self.reply(200, c)
        return self.reply(404, "not found")

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PUT(self):
        self.route("PUT")

    def do_DELETE(self):
        self.route("DELETE")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--cards", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--delay", type=float, default=0.0, help="seconds of artificial latency per request")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--background", choices=sorted(BACKGROUNDS), default="gradient",
                    help="the board background reported in prefs")
    a = ap.parse_args()
    build(a.cards, a.seed)
    prefs = dict(BACKGROUNDS[a.background])
    if a.background == "picture":
        base = "http://127.0.0.1:%d/1/img/" % a.port
        prefs["backgroundImage"] = base + "background.png"
        prefs["backgroundImageScaled"] = [{"width": 480, "height": 300, "url": base + "bg-480.png"},
                                          {"width": 1920, "height": 1200, "url": base + "bg-1920.png"}]
    STATE["boards"]["b1"]["prefs"] = prefs
    STATE["delay"] = a.delay
    STATE["verbose"] = a.verbose
    print("mock trello on http://127.0.0.1:%d/1 with %d cards" % (a.port, len(STATE["cards"])), flush=True)
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
