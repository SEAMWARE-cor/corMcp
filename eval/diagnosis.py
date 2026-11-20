#
# FILE            diagnosis.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The evaluation set "diagnosis": a small set-up that is broken on purpose - six subscriptions that never
# notify, each for its own reason - and the questions a user asks about the broker itself: is it all right,
# is there an index, what does leaving a feature out save. The answers are the causes, so these cases score
# the whole answer (on: text), not an ANSWER line.
#
# Needs coraine with the admin API (--apiPlugins admin, or -api admin); the "docs" cases need corMcp --docs:
# CORMCP_DOCS (dir1:dir2), else ~/git/coraine/doc when it is there - skipped without.
#
import datetime, os, time, urllib.request
from common import *

TENANT = "mcpdiag"

SENSORS = [("T1", 20.5), ("T2", 22.0), ("T3", 25.5)]          # TemperatureSensor, temperature (CEL)
UNREACHABLE = "http://127.0.0.1:9/notify"                     # the discard port: nothing listens

def sid(short):
  return f"urn:ngsi-ld:Subscription:{short}"

def sub(short, **kw):
  s = { "id": sid(short), "type": "Subscription",
        "entities": [{ "type": "TemperatureSensor" }],
        "notification": { "endpoint": { "uri": "http://example.org/hot", "accept": "application/json" } } }
  s.update(kw)
  return s

def subscriptions():
  soon = (datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=4)).strftime("%Y-%m-%dT%H:%M:%SZ")
  return [
    sub("typo",        watchedAttributes=["temprature"]),                    # no entity has the attribute
    sub("wrongType",   entities=[{ "type": "Sensor" }]),                     # no entity of the type
    sub("neverTrue",   q="temperature>40"),                                  # the hottest is 25.5
    sub("paused",      isActive=False),
    sub("expired",     expiresAt=soon),                                      # expires during the seed
    sub("unreachable", notification={ "endpoint": { "uri": UNREACHABLE, "accept": "application/json" } }),
  ]

def entities():
  return [ { "id": f"urn:ngsi-ld:TemperatureSensor:{t}", "type": "TemperatureSensor",
             "temperature": { "type": "Property", "value": v, "unitCode": "CEL" } } for t, v in SENSORS ]

MCP_ARGS = []
_docs = os.environ.get("CORMCP_DOCS") or (os.path.expanduser("~/git/coraine/doc") if os.path.isdir(os.path.expanduser("~/git/coraine/doc")) else "")
if _docs:
  MCP_ARGS = ["--docs", _docs]



# -----------------------------------------------------------------------------
#
# seed / temporal - the set, then a write of every sensor (the unreachable one fails to deliver), and the
# expired one's time out
#
def seed(broker):
  seedSet(broker, TENANT, entities(), [], subscriptions())
  for t, v in SENSORS:
    http(broker, TENANT, "PATCH", f"/entities/urn:ngsi-ld:TemperatureSensor:{t}/attrs", { "temperature": { "type": "Property", "value": v + 0.5, "unitCode": "CEL" } })
  time.sleep(5)

def temporal(broker):
  return True

def docs():
  return bool(_docs)



# -----------------------------------------------------------------------------
#
# The cases
#
CASES = [
  # Why does my subscription not notify - one call each, diagnose_subscription
  { "id": "subTypo", "on": "text", "q": "The subscription 'typo' never sends us anything. Why?",
    "must": [r"temprature", r"temperature"], "tools": ["diagnose_subscription"], "calls": 2 },
  { "id": "subWrongType", "on": "text", "q": "Why does the subscription 'wrongType' not notify?",
    "must": [r"\bSensor\b", r"TemperatureSensor"], "tools": ["diagnose_subscription"], "calls": 2 },
  { "id": "subNeverTrue", "on": "text", "q": "We never get a notification from subscription neverTrue. What is wrong with it?",
    "must": [r"40", r"25[.,]5|26|hottest|highest|maximum|none of"], "tools": ["diagnose_subscription"], "calls": 3 },
  { "id": "subPaused", "on": "text", "q": "Subscription 'paused' is silent. Why?",
    "must": [r"paus|isActive|inactive|not active"], "tools": ["diagnose_subscription"], "calls": 2 },
  { "id": "subExpired", "on": "text", "q": "The 'expired' subscription worked this morning and now sends nothing. What happened?",
    "must": [r"expir"], "tools": ["diagnose_subscription"], "calls": 2 },
  { "id": "subUnreachable", "on": "text", "q": "Does subscription 'unreachable' deliver its notifications?",
    "must": [r"127\.0\.0\.1:9|refused|unreachable|fail"], "mustNot": [r"^\W*yes\b"], "tools": ["diagnose_subscription"], "calls": 2 },

  # Which of them are broken - the list, then a diagnosis of the suspects
  { "id": "subsBroken", "on": "text", "q": "Which of our subscriptions are not working, and why? One line each.",
    "must": [r"typo", r"wrongType", r"neverTrue", r"paused", r"expired", r"unreachable"], "tools": ["list_subscriptions|diagnose_subscription"], "calls": 9 },

  # The broker itself
  { "id": "brokerOk", "on": "text", "q": "Is the broker all right? Which version is it, and which database does it use?",
    "must": [r"\d+\.\d+\.\d+", r"ramDB|corDB|mongo"], "tools": ["broker_status"], "calls": 2 },
  { "id": "indexTemperature", "on": "text", "q": "Is there an index on temperature in this broker?",
    "must": [r"\bno\b|none|not"], "tools": ["list_indexes"], "calls": 2 },

  # From the documentation
  { "id": "docsGeoSize", "on": "text", "docs": True, "q": "How much smaller does the broker get without geo-queries?",
    "must": [r"3[.,]17|3[.,]2 ?MiB"], "tools": ["search_docs", "read_doc|search_docs"], "calls": 4 },
  { "id": "docsIndexOption", "on": "text", "docs": True, "q": "Which start option declares an index on an attribute, and what does a write of that attribute cost?",
    "must": [r"dbValueIndexes", r"µs|microsecond|%"], "tools": ["search_docs"], "calls": 4 },
]
