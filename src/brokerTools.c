//
// FILE            brokerTools.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The broker tools - about the broker itself, not its data: is it all right, how is it configured, why does a
// subscription not notify, what do its counters say. All read-only. The admin API they read (/admin/...) is
// coraine's; another broker answers 404 there, and the tools say so.
//
#include <regex.h>                               // regcomp, regexec
#include <stdarg.h>                              // va_list
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // strtod, atoll
#include <string.h>                              // strcmp, strncmp, strstr, strlen
#include <strings.h>                             // strcasestr
#include <time.h>                                // time, gmtime_r, strftime

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corMcp.h"



// -----------------------------------------------------------------------------
//
// get - a GET to the broker; the response in *respP, true: a 2xx
//
static bool get(CorAlloc* allocP, const char* path, const char* query, McpResponse* respP)
{
  mcpBackend->request(allocP, CorVerbGet, path, query, NULL, respP);
  return (respP->status >= 200) && (respP->status <= 299);
}



// -----------------------------------------------------------------------------
//
// adminWhy - why an admin call did not answer, in words the model can pass on
//
static const char* adminWhy(McpResponse* respP)
{
  if (respP->status == 0)   return "the broker did not answer";
  if (respP->status == 401) return "401 - the user's token is not accepted for the admin API";
  if (respP->status == 403) return "403 - the user's access does not include the admin API";
  if (respP->status == 404) return "404 - no such admin resource (not a coraine broker, or a plugin not loaded)";
  return "an error from the broker";
}



// -----------------------------------------------------------------------------
//
// encoded - a path segment, percent-encoded (':' kept: an entity id is a URI)
//
static char* encoded(CorAlloc* allocP, const char* prefix, const char* segment)
{
  McpStr s;
  strInit(&s, allocP, 256);
  strAdd(&s, prefix);
  strAddUrlEncoded(&s, segment, ":");
  return s.buf;
}



static const char* str(CorNode* objectP, const char* name)
{
  CorNode* nP = (objectP != NULL) ? corTreeLookup(objectP, name) : NULL;
  return ((nP != NULL) && (nP->type == CorString)) ? nP->value.s : NULL;
}

static double num(CorNode* objectP, const char* name, double dflt)
{
  CorNode* nP = (objectP != NULL) ? corTreeLookup(objectP, name) : NULL;
  if (nP == NULL)            return dflt;
  if (nP->type == CorInt)    return (double) nP->value.i;
  if (nP->type == CorFloat)  return nP->value.f;
  return dflt;
}



// -----------------------------------------------------------------------------
//
// Metrics - the Prometheus text of /admin/metrics, summed per name (labels added up)
//
#define METRICS_MAX 256

typedef struct Metric
{
  char    name[96];
  double  value;
} Metric;

typedef struct Metrics
{
  Metric  m[METRICS_MAX];
  int     n;
} Metrics;

static void metricsParse(const char* text, Metrics* mP)
{
  mP->n = 0;

  for (const char* line = text; (line != NULL) && (*line != 0); line = strchr(line, '\n'), line = (line != NULL) ? line + 1 : NULL)
  {
    if ((*line == '#') || (*line == '\n'))
      continue;

    char name[96];
    int  len = 0;
    while ((line[len] != 0) && (line[len] != ' ') && (line[len] != '{') && (line[len] != '\n') && (len < (int) sizeof(name) - 1))
    {
      name[len] = line[len];
      ++len;
    }
    name[len] = 0;

    const char* v = line + len;
    if (*v == '{')
    {
      v = strchr(v, '}');
      if (v == NULL)
        break;
      ++v;
    }
    while (*v == ' ')
      ++v;

    double value = strtod(v, NULL);
    int    ix;
    for (ix = 0; ix < mP->n; ix++)
    {
      if (strcmp(mP->m[ix].name, name) == 0)
        break;
    }
    if (ix == mP->n)
    {
      if (mP->n == METRICS_MAX)
        continue;
      snprintf(mP->m[mP->n].name, sizeof(mP->m[0].name), "%s", name);
      mP->m[mP->n].value = 0;
      ++mP->n;
    }
    mP->m[ix].value += value;
  }
}

static double metric(Metrics* mP, const char* name)
{
  for (int ix = 0; ix < mP->n; ix++)
  {
    if (strcmp(mP->m[ix].name, name) == 0)
      return mP->m[ix].value;
  }
  return -1;
}



// -----------------------------------------------------------------------------
//
// Findings - the list of what is worth the user's attention
//
static void finding(CorAlloc* allocP, CorNode* listP, const char* severity, const char* text)
{
  CorNode* fP = corTreeObject(allocP, NULL);
  corTreeChildAdd(fP, corTreeString(allocP, "severity", severity));
  corTreeChildAdd(fP, corTreeString(allocP, "text", corAllocStrdup(allocP, text)));
  corTreeChildAdd(listP, fP);
}



// -----------------------------------------------------------------------------
//
// brokerStatus - is the broker all right: version, health, tenants, plugins, the counters that matter
//
CorNode* brokerStatus(CorAlloc* allocP, CorNode* argsP)
{
  (void) argsP;

  McpResponse resp;
  CorNode*    outP      = corTreeObject(allocP, NULL);
  CorNode*    problemsP = corTreeArray(allocP, "findings");
  char        text[512];

  corTreeChildAdd(outP, corTreeString(allocP, "broker", mcpSession.broker));

  if (get(allocP, "/admin/version", NULL, &resp) && (resp.body != NULL))
  {
    const char* v = str(resp.body, "coraine version");
    corTreeChildAdd(outP, corTreeString(allocP, "version", (v != NULL) ? v : "unknown"));
  }
  else if (resp.status == 0)
    return shapeError(allocP, "the broker at %s did not answer: %s", mcpSession.broker, resp.error);
  else
  {
    snprintf(text, sizeof(text), "the admin API is not available: %s - only the NGSI-LD API can be read", adminWhy(&resp));
    finding(allocP, problemsP, "warning", text);
    corTreeChildAdd(outP, problemsP);
    return shapeResult(allocP, outP, false);
  }

  if (get(allocP, "/admin/health", NULL, &resp) && (resp.body != NULL))
  {
    const char* h = str(resp.body, "status");
    corTreeChildAdd(outP, corTreeString(allocP, "health", (h != NULL) ? h : "unknown"));
    if ((h != NULL) && (strcmp(h, "ok") != 0))
    {
      snprintf(text, sizeof(text), "health is '%s', not ok", h);
      finding(allocP, problemsP, "problem", text);
    }
  }

  if (get(allocP, "/admin/tenants", NULL, &resp) && (resp.body != NULL))
  {
    resp.body->name = (char*) "tenants";
    corTreeChildAdd(outP, resp.body);
  }

  if (get(allocP, "/admin/plugins", NULL, &resp) && (resp.body != NULL))
  {
    resp.body->name = (char*) "plugins";
    corTreeChildAdd(outP, resp.body);
  }

  //
  // The counters - since the broker started
  //
  static Metrics m;                              // big: not on the stack; one call at a time
  if (get(allocP, "/admin/metrics", NULL, &resp) && (resp.text != NULL))
  {
    metricsParse(resp.text, &m);

    CorNode* memP    = corTreeObject(allocP, "memory");
    double   used    = metric(&m, "ngsild_memory_used_bytes");
    double   budget  = metric(&m, "ngsild_memory_budget_bytes");
    double   res     = metric(&m, "ngsild_memory_resident_bytes");

    if (used >= 0)   corTreeChildAdd(memP, corTreeInteger(allocP, "usedBytes", (long long) used));
    if (budget > 0)  corTreeChildAdd(memP, corTreeInteger(allocP, "budgetBytes", (long long) budget));
    if (res >= 0)    corTreeChildAdd(memP, corTreeInteger(allocP, "residentBytes", (long long) res));
    if ((used >= 0) && (budget > 0))
    {
      int pct = (int) (100 * used / budget + 0.5);
      corTreeChildAdd(memP, corTreeInteger(allocP, "usedPercent", pct));
      if (pct >= 85)
      {
        snprintf(text, sizeof(text), "memory is at %d %% of its budget - requests are refused when it is full", pct);
        finding(allocP, problemsP, (pct >= 95) ? "problem" : "warning", text);
      }
    }
    corTreeChildAdd(outP, memP);

    CorNode* reqP     = corTreeObject(allocP, "requests");
    double   worker   = metric(&m, "ngsild_requests_worker_total");
    double   inl      = metric(&m, "ngsild_requests_inline_total");
    double   e4       = metric(&m, "ngsild_errors_4xx_total");
    double   e5       = metric(&m, "ngsild_errors_5xx_total");
    double   refMem   = metric(&m, "ngsild_requests_refused_memory_total");
    double   refPause = metric(&m, "ngsild_requests_refused_store_paused_total");
    double   latSum   = metric(&m, "ngsild_request_latency_seconds_sum");
    double   latCount = metric(&m, "ngsild_request_latency_seconds_count");

    corTreeChildAdd(reqP, corTreeInteger(allocP, "total", (long long) ((worker > 0 ? worker : 0) + (inl > 0 ? inl : 0))));
    if (e4 >= 0)        corTreeChildAdd(reqP, corTreeInteger(allocP, "errors4xx", (long long) e4));
    if (e5 >= 0)        corTreeChildAdd(reqP, corTreeInteger(allocP, "errors5xx", (long long) e5));
    if (refMem >= 0)    corTreeChildAdd(reqP, corTreeInteger(allocP, "refusedMemory", (long long) refMem));
    if (refPause >= 0)  corTreeChildAdd(reqP, corTreeInteger(allocP, "refusedStorePaused", (long long) refPause));
    if (latCount > 0)   corTreeChildAdd(reqP, corTreeFloat(allocP, "latencyAvgMs", 1000 * latSum / latCount));
    corTreeChildAdd(outP, reqP);

    if (e5 > 0)
    {
      snprintf(text, sizeof(text), "%.0f requests failed with a 5xx (a broker-side error) since it started", e5);
      finding(allocP, problemsP, "problem", text);
    }
    if (refMem > 0)
    {
      snprintf(text, sizeof(text), "%.0f requests were refused for lack of memory", refMem);
      finding(allocP, problemsP, "problem", text);
    }
    if (metric(&m, "ngsild_store_paused") > 0)
      finding(allocP, problemsP, "problem", "the store is paused - writes are refused until it is resumed");

    CorNode* notP   = corTreeObject(allocP, "notifications");
    double   sent   = metric(&m, "ngsild_notifications_sent_total");
    double   failed = metric(&m, "ngsild_notifications_failed_total");
    if (sent >= 0)    corTreeChildAdd(notP, corTreeInteger(allocP, "sent", (long long) sent));
    if (failed >= 0)  corTreeChildAdd(notP, corTreeInteger(allocP, "failed", (long long) failed));
    corTreeChildAdd(outP, notP);
    if (failed > 0)
    {
      snprintf(text, sizeof(text), "%.0f of %.0f notifications failed - diagnose_subscription says which subscription", failed, sent + failed);
      finding(allocP, problemsP, "warning", text);
    }

    double fwd    = metric(&m, "ngsild_distop_forwarded_total");
    double fwdBad = metric(&m, "ngsild_distop_forward_failed_total");
    if (fwd > 0 || fwdBad > 0)
    {
      CorNode* fP = corTreeObject(allocP, "forwarding");
      corTreeChildAdd(fP, corTreeInteger(allocP, "forwarded", (long long) fwd));
      corTreeChildAdd(fP, corTreeInteger(allocP, "failed", (long long) fwdBad));
      corTreeChildAdd(outP, fP);
      if (fwdBad > 0)
      {
        snprintf(text, sizeof(text), "%.0f forwards to context sources (registrations) failed", fwdBad);
        finding(allocP, problemsP, "warning", text);
      }
    }

    double sse = metric(&m, "ngsild_sse_streams");
    if (sse >= 0)
      corTreeChildAdd(outP, corTreeInteger(allocP, "sseStreams", (long long) sse));

    corTreeChildAdd(outP, corTreeString(allocP, "countersSince", "the broker's start"));
  }
  else
  {
    snprintf(text, sizeof(text), "the metrics are not available: %s", adminWhy(&resp));
    finding(allocP, problemsP, "info", text);
  }

  if (problemsP->value.head == NULL)
    finding(allocP, problemsP, "ok", "nothing found that needs attention");
  corTreeChildAdd(outP, problemsP);

  return shapeResult(allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// secret - an option whose value is not shown
//
static bool secret(const char* name)
{
  return (strcasestr(name, "password") != NULL) || (strcasestr(name, "secret") != NULL) || (strcasestr(name, "token") != NULL) ||
         (strcasestr(name, "apikey") != NULL) || (strcasestr(name, "credential") != NULL) || (strcasestr(name, "privatekey") != NULL);
}



// -----------------------------------------------------------------------------
//
// brokerConfig - the settings in effect: by default the ones not at their default; 'name' narrows, 'all' lists all
//
CorNode* brokerConfig(CorAlloc* allocP, CorNode* argsP)
{
  McpResponse resp;
  const char* name = str(argsP, "match");
  CorNode*    allP = (argsP != NULL) ? corTreeLookup(argsP, "all") : NULL;
  bool        all  = (allP != NULL) && (allP->type == CorBoolean) && (allP->value.b == true);

  if (!get(allocP, "/admin/config", NULL, &resp) || (resp.body == NULL))
    return (resp.status == 0) ? shapeBrokerError(allocP, &resp) : shapeError(allocP, "the configuration is not available: %s", adminWhy(&resp));

  CorNode* optionsP = corTreeLookup(resp.body, "options");
  if ((optionsP == NULL) || (optionsP->type != CorArray))
    return shapeError(allocP, "the broker's /admin/config has no 'options' array");

  CorNode* outP   = corTreeObject(allocP, NULL);
  CorNode* listP  = corTreeArray(allocP, "options");
  int      total  = 0;
  int      shown  = 0;

  CorNode* next;
  for (CorNode* oP = optionsP->value.head; oP != NULL; oP = next)
  {
    const char* oName = str(oP, "name");

    next = oP->next;                             // oP may move to the answer's list
    const char* from  = str(oP, "from");

    ++total;
    if (oName == NULL)
      continue;
    if ((name != NULL) && (name[0] != 0))
    {
      const char* desc = str(oP, "description");
      if ((strcasestr(oName, name) == NULL) && ((desc == NULL) || (strcasestr(desc, name) == NULL)))
        continue;
    }
    else if ((all == false) && (from != NULL) && (strcmp(from, "default") == 0))
      continue;

    CorNode* vP = corTreeLookup(oP, "value");
    if ((vP != NULL) && secret(oName) && (vP->type != CorNull) && (vP->type != CorBoolean))
    {
      corTreeChildRemove(oP, vP);
      corTreeChildAdd(oP, corTreeString(allocP, "value", "(set - not shown)"));
    }
    corTreeChildAdd(listP, oP);
    ++shown;
  }

  corTreeChildAdd(outP, corTreeInteger(allocP, "options", total));
  corTreeChildAdd(outP, corTreeString(allocP, "shown", ((name != NULL) && (name[0] != 0)) ? "the options whose name or description has the text asked for" :
                                                         all ? "all" : "the ones not at their default (from: command line, environment, file) - all: true for every option"));
  listP->name = (char*) "list";
  corTreeChildAdd(outP, listP);
  (void) shown;

  return shapeResult(allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// brokerMetrics - the counters whose name has 'match' (Prometheus names), summed over their labels
//
CorNode* brokerMetrics(CorAlloc* allocP, CorNode* argsP)
{
  McpResponse resp;
  const char* match = str(argsP, "match");

  if (!get(allocP, "/admin/metrics", NULL, &resp) || (resp.text == NULL))
    return (resp.status == 0) ? shapeBrokerError(allocP, &resp) : shapeError(allocP, "the metrics are not available: %s", adminWhy(&resp));

  static Metrics m;
  metricsParse(resp.text, &m);

  CorNode* outP  = corTreeObject(allocP, NULL);
  CorNode* listP = corTreeObject(allocP, "metrics");
  int      n     = 0;

  for (int ix = 0; ix < m.n; ix++)
  {
    if (strstr(m.m[ix].name, "_bucket") != NULL)
      continue;
    if ((match != NULL) && (match[0] != 0) && (strstr(m.m[ix].name, match) == NULL))
      continue;
    corTreeChildAdd(listP, corTreeFloat(allocP, corAllocStrdup(allocP, m.m[ix].name), m.m[ix].value));
    ++n;
  }

  corTreeChildAdd(outP, corTreeString(allocP, "since", "the broker's start (counters); now (gauges: _size, _bytes, _streams, _paused)"));
  corTreeChildAdd(outP, listP);
  if (n == 0)
    corTreeChildAdd(outP, corTreeString(allocP, "note", "no metric has that in its name - call without 'match' to see them all"));

  return shapeResult(allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// listIndexes - the database indexes: of the current state and of the history
//
CorNode* listIndexes(CorAlloc* allocP, CorNode* argsP)
{
  (void) argsP;

  McpResponse resp;
  CorNode*    outP = corTreeObject(allocP, NULL);

  if (get(allocP, "/admin/indexes", NULL, &resp) && (resp.body != NULL))
  {
    resp.body->name = (char*) "indexes";
    corTreeChildAdd(outP, resp.body);
  }
  else if (resp.status == 0)
    return shapeBrokerError(allocP, &resp);
  else
    corTreeChildAdd(outP, corTreeString(allocP, "indexes", adminWhy(&resp)));

  if (get(allocP, "/admin/troeIndexes", NULL, &resp) && (resp.body != NULL))
  {
    resp.body->name = (char*) "historyIndexes";
    corTreeChildAdd(outP, resp.body);
  }
  else
    corTreeChildAdd(outP, corTreeString(allocP, "historyIndexes", "none - no history (TRoE) database, or not readable"));

  return shapeResult(allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// bridgeInventory - what a bridge holds: its devices and their Channels
//
CorNode* bridgeInventory(CorAlloc* allocP, CorNode* argsP)
{
  McpResponse resp;
  const char* bridge = str(argsP, "bridge");

  if ((bridge == NULL) || (bridge[0] == 0))
    return shapeError(allocP, "'bridge' is required - the bridge's name, as broker_status lists its plugins");

  McpStr path;
  strInit(&path, allocP, 128);
  strAdd(&path, encoded(allocP, "/admin/bridges/", bridge));
  strAdd(&path, "/inventory");

  if (!get(allocP, path.buf, NULL, &resp) || (resp.body == NULL))
    return (resp.status == 404) ? shapeError(allocP, "no bridge '%s' in this broker - broker_status lists the plugins loaded", bridge) : shapeBrokerError(allocP, &resp);

  shapeCut(allocP, resp.body);
  return shapeResult(allocP, resp.body, false);
}



// -----------------------------------------------------------------------------
//
// check - one line of a diagnosis: { check, ok (true, false, or null: for information), detail }
//
static void check(CorAlloc* allocP, CorNode* listP, const char* what, int ok, const char* fmt, ...) __attribute__((format(printf, 5, 6)));

static void check(CorAlloc* allocP, CorNode* listP, const char* what, int ok, const char* fmt, ...)
{
  char    text[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);

  CorNode* cP = corTreeObject(allocP, NULL);
  corTreeChildAdd(cP, corTreeString(allocP, "check", what));
  if (ok < 0)
    corTreeChildAdd(cP, corTreeNull(allocP, "ok"));
  else
    corTreeChildAdd(cP, corTreeBoolean(allocP, "ok", ok != 0));
  corTreeChildAdd(cP, corTreeString(allocP, "detail", corAllocStrdup(allocP, text)));
  corTreeChildAdd(listP, cP);
}



// -----------------------------------------------------------------------------
//
// countOf - how many entities match a query (-1: the broker said no)
//
static long long countOf(CorAlloc* allocP, const char* type, const char* id, const char* idPattern, const char* q, const char* scopeQ, CorNode* geoQP)
{
  McpStr      query;
  McpResponse resp;

  strInit(&query, allocP, 256);
  strAdd(&query, "count=true&limit=0");
  if (type != NULL)       { strAdd(&query, "&type=");      strAddUrlEncoded(&query, type, ",;|()"); }
  if (id != NULL)         { strAdd(&query, "&id=");        strAddUrlEncoded(&query, id, ":"); }
  if (idPattern != NULL)  { strAdd(&query, "&idPattern="); strAddUrlEncoded(&query, idPattern, NULL); }
  if (q != NULL)          { strAdd(&query, "&q=");         strAddUrlEncoded(&query, q, NULL); }
  if (scopeQ != NULL)     { strAdd(&query, "&scopeQ=");    strAddUrlEncoded(&query, scopeQ, NULL); }
  if (geoQP != NULL)
  {
    const char* georel   = str(geoQP, "georel");
    const char* geometry = str(geoQP, "geometry");
    CorNode*    coordsP  = corTreeLookup(geoQP, "coordinates");
    const char* geoprop  = str(geoQP, "geoproperty");

    if ((georel != NULL) && (geometry != NULL) && (coordsP != NULL))
    {
      strAdd(&query, "&georel=");      strAddUrlEncoded(&query, georel, NULL);
      strAdd(&query, "&geometry=");    strAddUrlEncoded(&query, geometry, NULL);
      strAdd(&query, "&coordinates="); strAddUrlEncoded(&query, (coordsP->type == CorString) ? coordsP->value.s : shapeRender(allocP, coordsP), NULL);
      if (geoprop != NULL) { strAdd(&query, "&geoproperty="); strAddUrlEncoded(&query, geoprop, NULL); }
    }
  }

  if (!get(allocP, "/entities", query.buf, &resp))
    return -1;
  return resp.count;
}



// -----------------------------------------------------------------------------
//
// isoNow - the time now, as NGSI-LD writes it (seconds, Z)
//
static void isoNow(char* buf, int size)
{
  time_t    now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}



// -----------------------------------------------------------------------------
//
// arrayHas - a string array has 's'
//
static bool arrayHas(CorNode* arrayP, const char* s)
{
  for (CorNode* iP = ((arrayP != NULL) && (arrayP->type == CorArray)) ? arrayP->value.head : NULL; iP != NULL; iP = iP->next)
  {
    if ((iP->type == CorString) && (strcmp(iP->value.s, s) == 0))
      return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// diagnoseSubscription - why a subscription does (not) notify: its state, its delivery, whether anything matches
// it now, and - with 'entityId' - whether that entity does, and whether it changed since the subscription exists
//
CorNode* diagnoseSubscription(CorAlloc* allocP, CorNode* argsP)
{
  McpResponse resp;
  const char* id       = str(argsP, "id");
  const char* entityId = str(argsP, "entityId");
  char        now[32];

  if ((id == NULL) || (id[0] == 0))
    return shapeError(allocP, "'id' is required - the subscription's id, as list_subscriptions shows it");

  isoNow(now, sizeof(now));

  if (!get(allocP, encoded(allocP, "/subscriptions/", id), "options=sysAttrs", &resp) || (resp.body == NULL) || (resp.body->type != CorObject))
  {
    if (resp.status == 404)
      return shapeError(allocP, "no subscription '%s' in this tenant - list_subscriptions shows the ones there are", id);
    return shapeBrokerError(allocP, &resp);
  }

  CorNode* subP      = resp.body;
  CorNode* outP      = corTreeObject(allocP, NULL);
  CorNode* checksP   = corTreeArray(allocP, "checks");
  CorNode* notifP    = corTreeLookup(subP, "notification");
  CorNode* entitiesP = corTreeLookup(subP, "entities");
  CorNode* watchedP  = corTreeLookup(subP, "watchedAttributes");
  CorNode* triggerP  = corTreeLookup(subP, "notificationTrigger");
  CorNode* geoQP     = corTreeLookup(subP, "geoQ");
  const char* q        = str(subP, "q");
  const char* scopeQ   = str(subP, "scopeQ");
  const char* status   = str(subP, "status");
  const char* expires  = str(subP, "expiresAt");
  const char* created  = str(subP, "createdAt");
  CorNode*    activeP  = corTreeLookup(subP, "isActive");

  corTreeChildAdd(outP, corTreeString(allocP, "subscription", id));
  shapeCut(allocP, subP);
  subP->name = (char*) "definition";
  corTreeChildAdd(outP, subP);

  //
  // 1. Its state
  //
  bool inactive = ((status != NULL) && (strcmp(status, "active") != 0)) || ((activeP != NULL) && (activeP->type == CorBoolean) && (activeP->value.b == false));
  check(allocP, checksP, "status", !inactive, "%s%s", (status != NULL) ? status : "(no status)",
        inactive ? " - a subscription notifies only while active (isActive true, status active)" : "");

  if (expires != NULL)
  {
    bool expired = strncmp(expires, now, 19) < 0;
    check(allocP, checksP, "expiresAt", !expired, "%s%s", expires, expired ? " - expired: it no longer notifies" : " - not yet");
  }

  //
  // 2. Where it notifies, and how delivery went
  //
  CorNode*    endpointP = corTreeLookup(notifP, "endpoint");
  const char* uri       = str(endpointP, "uri");
  if (uri == NULL)
    check(allocP, checksP, "endpoint", 0, "no notification.endpoint.uri");
  else if (strstr(uri, "example.org") || strstr(uri, "example.com") || strstr(uri, "example.net"))
    check(allocP, checksP, "endpoint", 0, "%s - a placeholder domain: no receiver is there", uri);
  else if (strstr(uri, "://localhost") || strstr(uri, "://127.") || strstr(uri, "://[::1]"))
    check(allocP, checksP, "endpoint", -1, "%s - 'localhost' is the broker's own machine: the receiver must run there, not on the user's", uri);
  else
    check(allocP, checksP, "endpoint", -1, "%s - the broker must be able to reach it (network, firewall, TLS)", uri);

  double      sent        = num(notifP, "timesSent", -1);
  double      failed      = num(notifP, "timesFailed", -1);
  const char* lastNotif   = str(notifP, "lastNotification");
  const char* lastFailure = str(notifP, "lastFailure");
  const char* lastSuccess = str(notifP, "lastSuccess");
  const char* nStatus     = str(notifP, "status");
  const char* reason      = str(notifP, "lastErrorReason");

  if ((sent < 0) && (lastNotif == NULL) && (lastFailure == NULL) && (lastSuccess == NULL))
    check(allocP, checksP, "delivery", -1, "the broker shows no notification attempt for it yet (its statistics are written every few seconds)");
  else
  {
    bool failing = (lastFailure != NULL) && ((lastSuccess == NULL) || (strcmp(lastFailure, lastSuccess) > 0));
    check(allocP, checksP, "delivery", !failing, "timesSent %.0f, timesFailed %.0f, lastNotification %s, lastSuccess %s, lastFailure %s%s%s%s",
          (sent < 0) ? 0 : sent, (failed < 0) ? 0 : failed, lastNotif ? lastNotif : "-", lastSuccess ? lastSuccess : "-", lastFailure ? lastFailure : "-",
          nStatus ? ", status " : "", nStatus ? nStatus : "", failing ? (reason ? " - failing now" : " - failing now: the receiver did not take the last one") : "");
    if (reason != NULL)
      check(allocP, checksP, "lastErrorReason", 0, "%s", reason);
  }

  //
  // 3. What it watches, and whether anything matches now
  //
  McpStr types;
  strInit(&types, allocP, 128);

  for (CorNode* eP = ((entitiesP != NULL) && (entitiesP->type == CorArray)) ? entitiesP->value.head : NULL; eP != NULL; eP = eP->next)
  {
    const char* type      = str(eP, "type");
    const char* eid       = str(eP, "id");
    const char* idPattern = str(eP, "idPattern");
    long long   n         = countOf(allocP, type, eid, idPattern, NULL, NULL, NULL);

    if (type != NULL)
    {
      if (types.len > 0)
        strAdd(&types, ",");
      strAdd(&types, type);
    }

    check(allocP, checksP, "entities", (n > 0), "type %s%s%s%s%s: %lld entit%s now%s", type ? type : "(any)",
          eid ? ", id " : "", eid ? eid : "", idPattern ? ", idPattern " : "", idPattern ? idPattern : "",
          (n < 0) ? 0 : n, (n == 1) ? "y" : "ies", (n == 0) ? " - nothing of that kind exists: check the type name (and the @context it was created with)" : "");
  }

  if ((q != NULL) || (scopeQ != NULL) || (geoQP != NULL))
  {
    long long n = countOf(allocP, (types.len > 0) ? types.buf : NULL, NULL, NULL, q, scopeQ, geoQP);
    check(allocP, checksP, "filters", (n > 0) ? 1 : -1, "%s%s%s%s%s: %lld entit%s match now%s", q ? "q " : "", q ? q : "", scopeQ ? " scopeQ " : "", scopeQ ? scopeQ : "",
          geoQP ? " geoQ" : "", (n < 0) ? 0 : n, (n == 1) ? "y" : "ies",
          (n == 0) ? " - none: a notification comes only once an entity changes so that it matches" : " - a notification comes when one of them changes (per its triggers)");
  }

  if ((watchedP != NULL) && (watchedP->type == CorArray) && (types.len > 0))
  {
    for (CorNode* aP = watchedP->value.head; aP != NULL; aP = aP->next)
    {
      if (aP->type != CorString)
        continue;
      long long n = countOf(allocP, types.buf, NULL, NULL, aP->value.s, NULL, NULL);    // q 'attr': the ones that have it
      check(allocP, checksP, "watchedAttribute", (n > 0), "%s: %lld entit%s of the types have it%s", aP->value.s, (n < 0) ? 0 : n, (n == 1) ? "y" : "ies",
            (n == 0) ? " - none has it: the name may be wrong, or expanded with another @context than the data's" : "");
    }
  }

  if ((triggerP != NULL) && (triggerP->type == CorArray))
  {
    char list[512] = "";
    for (CorNode* tP = triggerP->value.head; tP != NULL; tP = tP->next)
    {
      if (tP->type == CorString)
        snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", (list[0] != 0) ? ", " : "", tP->value.s);
    }
    bool creates = arrayHas(triggerP, "entityCreated") || arrayHas(triggerP, "attributeCreated");
    check(allocP, checksP, "notificationTrigger", -1, "%s%s", list,
          creates ? "" : " - not on creation: a new entity does not notify until it is updated");
  }

  const char* interval = NULL;
  double      ti       = num(subP, "timeInterval", -1);
  double      thr      = num(subP, "throttling", -1);
  if (ti > 0)
  {
    interval = "periodic";
    check(allocP, checksP, "timeInterval", -1, "every %.0f s - periodic: it notifies on time, not on change", ti);
  }
  if (thr > 0)
    check(allocP, checksP, "throttling", -1, "at most one notification every %.0f s - changes in between are not notified on their own", thr);
  (void) interval;

  const char* subContext = str(subP, "jsonldContext");
  if ((subContext != NULL) && (mcpSession.context != NULL) && (strcmp(subContext, mcpSession.context) != 0))
    check(allocP, checksP, "@context", -1, "the subscription was made with %s, this session reads with %s - its type and attribute names expand with its own", subContext, mcpSession.context);

  //
  // 4. One entity: does it match, and did it change since the subscription exists?
  //
  if ((entityId != NULL) && (entityId[0] != 0))
  {
    McpResponse er;

    if (!get(allocP, encoded(allocP, "/entities/", entityId), "options=sysAttrs", &er) || (er.body == NULL) || (er.body->type != CorObject))
      check(allocP, checksP, "entity", 0, "%s: %s", entityId, (er.status == 404) ? "not in this tenant" : "not readable");
    else
    {
      const char* etype   = str(er.body, "type");
      bool        matched = false;

      for (CorNode* eP = ((entitiesP != NULL) && (entitiesP->type == CorArray)) ? entitiesP->value.head : NULL; eP != NULL; eP = eP->next)
      {
        const char* type      = str(eP, "type");
        const char* eid       = str(eP, "id");
        const char* idPattern = str(eP, "idPattern");

        if ((type != NULL) && ((etype == NULL) || (strcmp(type, etype) != 0)))
          continue;
        if ((eid != NULL) && (strcmp(eid, entityId) != 0))
          continue;
        if (idPattern != NULL)
        {
          regex_t re;
          if (regcomp(&re, idPattern, REG_EXTENDED | REG_NOSUB) != 0)
            continue;
          bool m = (regexec(&re, entityId, 0, NULL, 0) == 0);
          regfree(&re);
          if (m == false)
            continue;
        }
        matched = true;
      }
      check(allocP, checksP, "entity.selected", matched, "%s (type %s)%s", entityId, etype ? etype : "?",
            matched ? " is one of the entities it watches" : " is not: its type or id is not in the subscription's 'entities'");

      if ((matched == true) && ((q != NULL) || (scopeQ != NULL) || (geoQP != NULL)))
      {
        long long n = countOf(allocP, etype, entityId, NULL, q, scopeQ, geoQP);
        check(allocP, checksP, "entity.filters", (n > 0), "%s", (n > 0) ? "it matches the subscription's q / scopeQ / geoQ now" :
              "it does not match the subscription's q / scopeQ / geoQ now - no notification while it does not");
      }

      //
      // The watched attributes: present, and changed after the subscription was made?
      //
      for (CorNode* aP = ((watchedP != NULL) && (watchedP->type == CorArray)) ? watchedP->value.head : NULL; aP != NULL; aP = aP->next)
      {
        if (aP->type != CorString)
          continue;
        CorNode* attrP = corTreeLookup(er.body, aP->value.s);
        if (attrP == NULL)
        {
          check(allocP, checksP, "entity.watchedAttribute", 0, "%s: the entity does not have it", aP->value.s);
          continue;
        }
        const char* modified = str(attrP, "modifiedAt");
        if ((modified != NULL) && (created != NULL))
        {
          bool before = strcmp(modified, created) < 0;     // both the broker's own ISO 8601 UTC, same precision
          check(allocP, checksP, "entity.watchedAttribute", !before, "%s: last changed %s, the subscription made %s%s", aP->value.s, modified, created,
                before ? " - before the subscription existed: it has not changed since, so nothing was notified" : " - after: that change should have notified");
        }
        else
          check(allocP, checksP, "entity.watchedAttribute", 1, "%s: present%s", aP->value.s, modified ? "" : " (no modifiedAt to compare)");
      }
    }
  }

  checksP->name = (char*) "checks";
  corTreeChildAdd(outP, checksP);

  //
  // The verdict: the failed checks, in order
  //
  int fails = 0;
  for (CorNode* cP = checksP->value.head; cP != NULL; cP = cP->next)
  {
    CorNode* okP = corTreeLookup(cP, "ok");
    if ((okP != NULL) && (okP->type == CorBoolean) && (okP->value.b == false))
      ++fails;
  }
  corTreeChildAdd(outP, corTreeString(allocP, "summary", (fails == 0) ?
                  "no check failed - if it still does not notify: did a watched attribute of a matching entity change since it was made (entityId tells), and can the broker reach the endpoint?" :
                  "the checks with ok false are why - the first one first"));

  return shapeResult(allocP, outP, false);
}
