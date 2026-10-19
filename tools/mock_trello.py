#!/usr/bin/env python3
"""A pretend Trello for developing and benchmarking Cardinal without real credentials.

    tools/mock_trello.py --port 8099 --cards 2000
    build/ui-gtk/cardinal_gtk --api-base http://127.0.0.1:8099/1 --api-key K --token T0123456789abcdefghij

It implements only the endpoints Cardinal calls, keeps everything in memory, accepts any key
and token, and applies writes (card edits, moves, labels, members, comments).
"""

import argparse
import json
import random
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
STATE = {}


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
        lists[lid] = {"id": lid, "name": name, "pos": (li + 1) * 65536.0, "cards": []}
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
            }
            cs = []
            for k in range(rnd.choice([0, 0, 1, 2, 4])):
                cs.append({"id": "a%d_%d" % (n, k), "idMemberCreator": rnd.choice(members)[0],
                           "date": iso(now - rnd.randint(0, 30 * 86400)),
                           "data": {"text": "Comentário %d do card %d. Vamos olhar isso na sprint." % (k + 1, n)}})
            card["badges"] = {"comments": len(cs)}
            cards[cid] = card
            comments[cid] = cs
            lists[lid]["cards"].append(cid)
    STATE.update(
        boards={"b1": {"id": "b1", "name": "Produto", "closed": False}},
        labels=labels, members=members, lists=lists, cards=cards, comments=comments, seq=n,
    )


def pick(obj, fields):
    return {k: obj[k] for k in fields if k in obj}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if STATE.get("verbose"):
            print("%s %s" % (self.command, self.path))

    def reply(self, status, body):
        data = json.dumps(body).encode()
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
        with LOCK:
            try:
                return self.dispatch(method, parts, q, form)
            except KeyError:
                return self.reply(404, "not found")

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
                    out.append({"id": l["id"], "name": l["name"], "pos": l["pos"],
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
                    "idLabels": [], "idMembers": [], "dateLastActivity": iso(), "closed": False,
                    "badges": {"comments": 0}}
            S["cards"][cid] = card
            S["comments"][cid] = []
            lst["cards"].append(cid)
            return self.reply(200, card)
        cid = parts[2]
        card = S["cards"][cid]
        sub = parts[3:]
        if not sub and method == "GET":
            if "checklists" in q:
                return self.reply(200, {"id": cid, "checklists": [
                    {"id": "ck" + cid, "name": "Passos", "checkItems": [
                        {"id": "i1", "name": "Reproduzir", "state": "complete"},
                        {"id": "i2", "name": "Corrigir", "state": "incomplete"}]}],
                    "attachments": [{"id": "at" + cid, "name": "captura.png", "url": "https://example.com/captura.png"}],
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
    a = ap.parse_args()
    build(a.cards, a.seed)
    STATE["delay"] = a.delay
    STATE["verbose"] = a.verbose
    print("mock trello on http://127.0.0.1:%d/1 with %d cards" % (a.port, len(STATE["cards"])), flush=True)
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
