#!/usr/bin/env python3
#
# FILE            check.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corMcp's tests: seeds a broker (its own entities, ids with a per-run suffix), starts corMcp over stdio,
# and checks every tool's answer and the protocol errors. Any NGSI-LD broker; a check a broker cannot
# pass (no temporal API, no 'omit') is reported as skipped with the broker's error.
#
#   check.py <corMcp binary> <broker URL>
#
import json, math, subprocess, sys, time, urllib.request, urllib.error

CORMCP, BROKER = sys.argv[1], sys.argv[2].rstrip("/")
RUN    = str(int(time.time() * 1000))[-8:]       # every run's own entities: no leftovers make a check pass
TYPE   = "McpVehicle" + RUN
DEPOT  = "McpDepot" + RUN
failed = 0
passed = 0
skipped = 0



def http(method, path, body=None):
    req = urllib.request.Request(BROKER + "/ngsi-ld/v1" + path, method=method, data=json.dumps(body).encode() if body is not None else None)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code



def seed():
    ents = []
    for i, (sp, st, lon, lat) in enumerate([(95, "moving", -3.70, 40.41), (40, "moving", -3.69, 40.42), (0, "parked", -3.60, 40.30), (120, "moving", 2.17, 41.38)]):
        ents.append({ "id": f"urn:ngsi-ld:{TYPE}:V{i + 1}", "type": TYPE,
                      "speed":    { "type": "Property", "value": sp, "unitCode": "KMH", "observedAt": "2026-10-15T10:00:00Z" },
                      "status":   { "type": "Property", "value": st },
                      "location": { "type": "GeoProperty", "value": { "type": "Point", "coordinates": [lon, lat] } },
                      "depot":    { "type": "Relationship", "object": f"urn:ngsi-ld:{DEPOT}:D1" } })
    ring = [[-3.7 + 0.01 * math.cos(a * 2 * math.pi / 150), 40.4 + 0.01 * math.sin(a * 2 * math.pi / 150)] for a in range(150)]
    ring.append(ring[0])
    ents.append({ "id": f"urn:ngsi-ld:{DEPOT}:D1", "type": DEPOT,
                  "name": { "type": "Property", "value": "Madrid Norte " + "x" * 1500 },
                  "area": { "type": "GeoProperty", "value": { "type": "Polygon", "coordinates": [ring] } } })
    status = http("POST", "/entityOperations/create", ents)
    if status not in (200, 201):
        print(f"seed: POST /entityOperations/create answered {status}")
        sys.exit(1)



def cleanup():
    for i in range(4):
        http("DELETE", f"/entities/urn:ngsi-ld:{TYPE}:V{i + 1}")
    http("DELETE", f"/entities/urn:ngsi-ld:{DEPOT}:D1")



class Mcp:
    def __init__(self, *args):
        self.p  = subprocess.Popen([CORMCP, "--broker", BROKER, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.id = 0

    def raw(self, text):
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def notify(self, method):
        self.p.stdin.write(json.dumps({ "jsonrpc": "2.0", "method": method }) + "\n")
        self.p.stdin.flush()

    def call(self, method, params=None):
        self.id += 1
        msg = { "jsonrpc": "2.0", "id": self.id, "method": method }
        if params is not None:
            msg["params"] = params
        answer = self.raw(json.dumps(msg))
        assert answer["id"] == self.id, f"id {answer['id']} for request {self.id}"
        return answer

    def tool(self, name, **args):
        return self.call("tools/call", { "name": name, "arguments": args })["result"]

    def close(self):
        self.p.stdin.close()
        self.p.wait()



def check(name, fn):
    global failed, passed, skipped
    try:
        fn()
        passed += 1
        print(f"  ok    {name}")
    except Skip as e:
        skipped += 1
        print(f"  skip  {name} - {e}")
    except Exception as e:
        failed += 1
        print(f"  FAIL  {name} - {type(e).__name__}: {e}")



class Skip(Exception):
    pass



def ok(result):
    assert not result.get("isError"), json.dumps(result["structuredContent"])[:300]
    text = json.loads(result["content"][0]["text"])
    assert text == result["structuredContent"], "the text is not the structuredContent"
    return result["structuredContent"]



def okOrSkip(result):
    if result.get("isError"):
        sc = result["structuredContent"]
        raise Skip(f"the broker answered {sc.get('status')} {sc.get('title')}")
    return ok(result)



def main():
    seed()
    m = Mcp()

    try:
        def initialize():
            r = m.call("initialize", { "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": { "name": "check", "version": "0" } })["result"]
            assert r["protocolVersion"] == "2025-06-18"
            assert r["capabilities"] == { "tools": { "listChanged": False } }
            assert r["serverInfo"]["name"] == "corMcp"
        check("initialize: the client's version, tools capability", initialize)

        def notification():
            m.notify("notifications/initialized")
            assert m.call("ping")["result"] == {}, "the next answer is ping's - the notification got none"
        check("a notification gets no answer", notification)

        def unknownVersion():
            r = Mcp().call("initialize", { "protocolVersion": "1999-01-01", "capabilities": {} })["result"]
            assert r["protocolVersion"] == "2025-11-25"
        check("initialize: an unknown version gets the newest", unknownVersion)

        def toolsList():
            tools = m.call("tools/list")["result"]["tools"]
            names = [t["name"] for t in tools]
            assert names == ["list_types", "list_attributes", "query_entities", "get_entity", "count_entities", "temporal", "list_subscriptions", "query_registrations",
                             "broker_status", "broker_config", "diagnose_subscription", "broker_metrics", "list_indexes", "bridge_inventory",
                             "read_log", "diagnose_device", "security_review", "smart_data_model"], names
            for t in tools:
                assert t["inputSchema"]["type"] == "object", t["name"]
                assert t["annotations"]["readOnlyHint"] is True
                assert len(t["description"]) > 40
        check("tools/list: eighteen read tools, schemas, annotations", toolsList)

        def listTypes():
            types = ok(m.tool("list_types"))["types"]
            mine  = [t for t in types if t.get("typeName") == TYPE]
            assert mine, "no " + TYPE
            assert set(mine[0]["attributeNames"]) == { "speed", "status", "location", "depot" }
        check("list_types", listTypes)

        def listAttributes():
            r = ok(m.tool("list_attributes", type=TYPE))["type"]
            names = sorted(a["attributeName"] for a in r["attributeDetails"])
            assert names == ["depot", "location", "speed", "status"], names
        check("list_attributes: one type", listAttributes)

        def query():
            r = ok(m.tool("query_entities", type=TYPE, q='speed>90;status=="moving"', pick=["speed"]))
            assert r["returned"] == 2 and r["more"] is False
            assert sorted(e["id"] for e in r["entities"]) == [f"urn:ngsi-ld:{TYPE}:V1", f"urn:ngsi-ld:{TYPE}:V4"]
            e = r["entities"][0]
            assert set(e) == { "id", "type", "speed" }, "pick keeps id and type: " + str(set(e))
            assert e["speed"]["unitCode"] == "KMH" and "observedAt" in e["speed"], "concise keeps the sub-attributes"
        check("query_entities: q, pick (id and type kept), concise", query)

        def queryList():
            r = ok(m.tool("query_entities", type=TYPE, q='status=="parked","nothing"'))
            assert [e["id"] for e in r["entities"]] == [f"urn:ngsi-ld:{TYPE}:V3"]
            assert r["entities"][0]["depot"] == { "object": f"urn:ngsi-ld:{DEPOT}:D1" }, "concise Relationship"
        check("query_entities: q any-of, a concise Relationship", queryList)

        def geo():
            r = ok(m.tool("query_entities", type=TYPE, geoQ={ "georel": "near;maxDistance==3000", "geometry": "Point", "coordinates": [-3.70, 40.41] }, pick=["status"]))
            assert sorted(e["id"] for e in r["entities"]) == [f"urn:ngsi-ld:{TYPE}:V1", f"urn:ngsi-ld:{TYPE}:V2"]
        check("query_entities: geoQ near", geo)

        def page():
            r = ok(m.tool("query_entities", type=TYPE, limit=2, offset=1, format="simplified"))
            assert r["returned"] == 2
            if "count" in r:
                assert r["count"] == 4 and r["more"] is True
            assert isinstance(r["entities"][0]["speed"], (int, float)), "simplified: values alone"
        check("query_entities: limit, offset, more, simplified", page)

        def limitMax():
            r = ok(m.tool("query_entities", type=TYPE, limit=100000))
            assert r["returned"] == 4
        check("query_entities: a limit above the maximum is lowered, not refused", limitMax)

        def count():
            assert ok(m.tool("count_entities", type=TYPE))["count"] == 4
            assert ok(m.tool("count_entities", type=TYPE, q="speed>50"))["count"] == 2
        check("count_entities", count)

        def get():
            r = ok(m.tool("get_entity", id=f"urn:ngsi-ld:{DEPOT}:D1"))
            assert r["name"].endswith("…[+513 characters cut]"), r["name"][-40:]
            ring = r["area"]["coordinates"][0]
            assert len(ring) == 101 and ring[100] == "…[+51 items cut]", ring[-1]
        check("get_entity: a long string and a big array cut, with markers", get)

        def notFound():
            r = m.tool("get_entity", id=f"urn:ngsi-ld:{TYPE}:nope")
            assert r["isError"] is True and r["structuredContent"]["status"] == 404
        check("get_entity: not found - a tool error with the status", notFound)

        def badArgs():
            r = m.tool("get_entity")
            assert r["isError"] is True and "'id' is required" in r["structuredContent"]["error"]
            r = m.tool("query_entities", type=TYPE, limit="5")
            assert r["isError"] is True and "'limit'" in r["structuredContent"]["error"]
            r = m.tool("query_entities", type=TYPE, geoQ="near")
            assert r["isError"] is True and "'geoQ'" in r["structuredContent"]["error"]
            r = m.tool("query_entities", type=TYPE, format="xml")
            assert r["isError"] is True and "'format'" in r["structuredContent"]["error"]
        check("bad arguments - tool errors the model can act on", badArgs)

        def badQ():
            r = m.tool("query_entities", type=TYPE, q="speed>>")
            assert r["isError"] is True and r["structuredContent"]["status"] == 400 and "title" in r["structuredContent"]
        check("a broker 400 - status, title, detail", badQ)

        def temporal():
            r = okOrSkip(m.tool("temporal", id=f"urn:ngsi-ld:{TYPE}:V1", attrs=["speed"], timerel="after", timeAt="2026-10-15T09:00:00Z", format="temporalValues"))
            assert r["speed"]["values"][0][0] == 95, r
        check("temporal: one entity, temporalValues", temporal)

        def temporalTimes():
            id = f"urn:ngsi-ld:{TYPE}:V1"
            r = m.tool("temporal", id=id, timerel="after")
            assert r["isError"] is True and "'timeAt' is required" in r["structuredContent"]["error"], r
            r = m.tool("temporal", id=id, timerel="between", timeAt="2026-10-15T09:00:00Z")
            assert r["isError"] is True and "'endTimeAt' is required" in r["structuredContent"]["error"], r
            r = m.tool("temporal", id=id, timerel="during", timeAt="2026-10-15T09:00:00Z")
            assert r["isError"] is True and "'timerel'" in r["structuredContent"]["error"], r
        check("temporal: timeAt always, endTimeAt with between", temporalTimes)

        def subscriptions():
            r = ok(m.tool("list_subscriptions"))
            assert isinstance(r["subscriptions"], list) and r["returned"] == len(r["subscriptions"])
        check("list_subscriptions", subscriptions)

        def registrations():
            r = ok(m.tool("query_registrations", type=TYPE))
            assert r["registrations"] == []
        check("query_registrations: a type", registrations)

        def brokerStatus():
            r = okOrSkip(m.tool("broker_status"))
            assert r["broker"] == BROKER and r["findings"], r
            if "version" in r:
                assert r["health"] == "ok" and "requests" in r and "notifications" in r, r
        check("broker_status: version, health, counters, findings", brokerStatus)

        def brokerConfig():
            r = okOrSkip(m.tool("broker_config", match="port"))
            assert any(o["name"] == "--port" and "from" in o and "env" in o for o in r["list"]), r
            r = ok(m.tool("broker_config", all=True))
            assert len(r["list"]) == r["options"], r
        check("broker_config: by name, all", brokerConfig)

        def brokerMetrics():
            r = okOrSkip(m.tool("broker_metrics", match="batch_create"))
            assert r["metrics"].get("ngsild_batch_create_total", 0) >= 1, r     # the seed is one batch
        check("broker_metrics: match", brokerMetrics)

        def indexes():
            r = okOrSkip(m.tool("list_indexes"))
            assert "indexes" in r and "historyIndexes" in r, r
        check("list_indexes", indexes)

        def bridgeUnknown():
            r = m.tool("bridge_inventory", bridge="nope" + RUN)
            assert r["isError"] is True, r
        check("bridge_inventory: no such bridge - a tool error", bridgeUnknown)

        def diagnose():
            sub = f"urn:ngsi-ld:Subscription:mcp{RUN}"
            status = http("POST", "/subscriptions", { "id": sub, "type": "Subscription", "entities": [{ "type": TYPE }], "watchedAttributes": ["speed"],
                                                          "q": "speed>100", "notification": { "endpoint": { "uri": "http://example.org/alerts" } } })
            assert status == 201, f"POST /subscriptions answered {status}"
            try:
                r = ok(m.tool("diagnose_subscription", id=sub, entityId=f"urn:ngsi-ld:{TYPE}:V4"))
                c = { (x["check"], i): x for i, x in enumerate(r["checks"]) }
                byName = lambda n: [x for (k, _), x in c.items() if k == n]
                assert byName("status")[0]["ok"] is True, r
                assert byName("endpoint")[0]["ok"] is False and "placeholder" in byName("endpoint")[0]["detail"], r
                assert byName("filters")[0]["ok"] is True and ": 1 entity match" in byName("filters")[0]["detail"], r
                assert byName("entity.selected")[0]["ok"] is True, r
                assert byName("entity.filters")[0]["ok"] is True, r
                w = byName("entity.watchedAttribute")[0]
                if "last changed" in w["detail"]:
                    assert w["ok"] is False and "before the subscription existed" in w["detail"], w
                r = m.tool("diagnose_subscription", id=sub + "x")
                assert r["isError"] is True and "no subscription" in r["structuredContent"]["error"], r
            finally:
                http("DELETE", f"/subscriptions/{sub}")
        check("diagnose_subscription: placeholder endpoint, one match, the entity changed before it existed", diagnose)

        def security():
            r = ok(m.tool("security_review"))
            assert r["summary"] and isinstance(r["findings"], list) and r["findings"], r
            assert all(f["severity"] in ("high", "medium", "info", "ok") for f in r["findings"]), r
            assert "notChecked" in r, r
        check("security_review: findings by severity, what it cannot see", security)

        def readLog():
            r = m.tool("read_log", types="EW", lines=5)
            if r.get("isError"):
                assert "0.8" in json.dumps(r) or "--logLines" in json.dumps(r), r      # an older coraine: said so
            else:
                assert isinstance(r["structuredContent"]["lines"], list), r
        check("read_log: the recent lines, or why there are none", readLog)

        def device():
            r = m.tool("diagnose_device", entity="urn:ngsi-ld:Nothing:here")
            if not r.get("isError"):
                checks = r["structuredContent"]["checks"]
                assert any(c["ok"] is False for c in checks), r                           # nothing maps it: said so
            r = m.tool("diagnose_device")
            assert r["isError"] is True, r
        check("diagnose_device: an entity nothing maps; the entity required", device)

        def sdm():
            r = m.tool("smart_data_model", search="parking")
            if r.get("isError"):
                assert "GitHub" in json.dumps(r), r                                       # no network here: said so
                return
            names = [x["model"] for x in r["structuredContent"]["models"]]
            assert "OffStreetParking" in names, names
            r = ok(m.tool("smart_data_model", model="OffStreetParking"))
            assert r["subject"] == "dataModel.Parking" and "attributes" in r, r
            assert m.tool("smart_data_model", model="NoSuchModelAtAll")["isError"] is True
        check("smart_data_model: a search, a model's attributes, an unknown model", sdm)

        def docs():
            import os
            d = Mcp("--docs", os.path.join(os.path.dirname(os.path.abspath(__file__)), "docs"))
            d.call("initialize", { "protocolVersion": "2025-06-18", "capabilities": {} })
            names = [t["name"] for t in d.call("tools/list")["result"]["tools"]]
            assert names[-2:] == ["search_docs", "read_doc"], names
            r = ok(d.tool("search_docs", query="declared index slow"))
            assert r["sections"][0]["file"] in ("kb/notes.md", "guide.md"), r
            files = { s["file"] for s in r["sections"] }
            assert files == { "kb/notes.md", "guide.md" }, r
            r = ok(d.tool("read_doc"))
            assert r["files"] == ["guide.md", "kb/notes.md"], r
            r = ok(d.tool("read_doc", file="guide"))
            assert r["sections"] == ["The guide", "  Indexes", "    Building one", "  Memory"], r      # the code block's # is no heading
            r = ok(d.tool("read_doc", file="guide.md", section="indexes"))
            assert "Building one" in r["text"] and "Memory" not in r["text"] and r["path"] == "The guide > Indexes", r
            r = d.tool("read_doc", file="nope.md")
            assert r["isError"] is True, r
            r = d.tool("search_docs", query="the")
            assert r["isError"] is True, r
        check("search_docs, read_doc: --docs directories, sections, subsections, a code block's #", docs)

        def noDocs():
            names = [t["name"] for t in m.call("tools/list")["result"]["tools"]]
            assert "search_docs" not in names, names
        check("no --docs: no documentation tools", noDocs)

        def rpcErrors():
            a = m.call("tools/call", { "name": "nope", "arguments": {} })
            assert a["error"]["code"] == -32602
            a = m.call("resources/list")
            assert a["error"]["code"] == -32601
            a = m.raw('{"jsonrpc":"2.0","id":7,"method":')
            assert a["error"]["code"] == -32700 and a["id"] is None
            a = m.raw('[{"jsonrpc":"2.0","id":8,"method":"ping"}]')
            assert a["error"]["code"] == -32600
            assert m.call("ping")["result"] == {}
        check("JSON-RPC: unknown tool, unknown method, parse error, batch, ping", rpcErrors)
    finally:
        m.close()
        cleanup()

    print(f"{passed} passed, {skipped} skipped, {failed} failed - {BROKER}")
    sys.exit(1 if failed else 0)



main()
