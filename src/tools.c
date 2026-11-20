//
// FILE            tools.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The tools - what the model sees (name, description, input schema, annotations) and what each call
// becomes: one NGSI-LD request. The descriptions are the product: each says what the tool is for and
// names the NGSI-LD syntax the model must produce.
//
#include <stdio.h>                               // snprintf
#include <string.h>                              // strcmp, strlen

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corMcp.h"



// -----------------------------------------------------------------------------
//
// toolsInstructions - initialize's 'instructions': what the server is, and where to start
//
const char* toolsInstructions =
  "Read access to an NGSI-LD context broker (ETSI TS 104 175). The data are entities: each has an id (a URI, "
  "often urn:ngsi-ld:<Type>:<name>), a type, and attributes - a Property (a value, maybe with unitCode and "
  "observedAt), a GeoProperty (GeoJSON, longitude first) or a Relationship (the object: another entity's id). "
  "Attributes can have sub-attributes. Short names (Store, stock) are expanded by the session's @context; "
  "use them as list_types and list_attributes show them. Start with list_types to see what the broker holds. "
  "The session is bound to one broker and one tenant, chosen by the user - no tool selects another; another tenant "
  "is a new session, which only the user starts.";



// -----------------------------------------------------------------------------
//
// Schemas - the filters that select entities, shared by query_entities, count_entities and temporal
//
#define FILTER_PROPERTIES                                                                                            \
  "\"type\": { \"type\": \"string\", \"description\": \"Entity type(s): Store; Store,Warehouse (either); a type expression with ; (and), | (or), parentheses\" },"  \
  "\"ids\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"Entity ids\" },"            \
  "\"idPattern\": { \"type\": \"string\", \"description\": \"A regular expression the entity id matches, e.g. urn:ngsi-ld:Store:S-.*\" }," \
  "\"q\": { \"type\": \"string\", \"description\": \"Attribute filter. stock<10 ; status==\\\"open\\\" (strings in double quotes) ; "  \
          "a;b = and, a|b = or, parentheses ; stock==10..50 (range) ; status==\\\"a\\\",\\\"b\\\" (any of) ; "  \
          "name~=\\\"^[Bb]akery\\\" (a POSIX extended regular expression, case-sensitive: [Pp]aris, never (?i)) ; "     \
          "stock.accuracy<2 (a sub-attribute: attr.sub) ; dimensions[width]>2 (a key of an object value: attr[key]) ; stock.observedAt>2026-01-01T00:00:00Z ; "  \
          "!stock (absent) ; stock (present)\" },"                                                                  \
  "\"scopeQ\": { \"type\": \"string\", \"description\": \"Scope filter: /France/# (the scope and everything below), /France/+ (one level down)\" }," \
  "\"geoQ\": { \"type\": \"object\", \"description\": \"Geographical filter, e.g. { georel: 'near;maxDistance==2000', geometry: 'Point', coordinates: [2.2945, 48.8584] } "  \
             "- coordinates as GeoJSON, longitude first; distances in metres. A well-known place (the Eiffel Tower, a city) is not "  \
             "an entity unless list_types shows its type: use the coordinates you know\", \"properties\": {"                   \
     "\"georel\": { \"type\": \"string\", \"description\": \"near;maxDistance==<m> | near;minDistance==<m> | within | contains | intersects | disjoint | overlaps | equals\" }," \
     "\"geometry\": { \"type\": \"string\", \"enum\": [\"Point\", \"LineString\", \"Polygon\", \"MultiPoint\", \"MultiLineString\", \"MultiPolygon\"] }," \
     "\"coordinates\": { \"type\": \"array\", \"description\": \"GeoJSON coordinates of the geometry\" },"           \
     "\"geoproperty\": { \"type\": \"string\", \"description\": \"The GeoProperty to test (default: location)\" } }," \
     "\"required\": [\"georel\", \"geometry\", \"coordinates\"] },"                                                  \
  "\"csf\": { \"type\": \"string\", \"description\": \"Context source filter (registrations' information)\" }"

#define SHAPE_PROPERTIES                                                                                             \
  "\"pick\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"Only these attributes (and id, type) in each entity\" }," \
  "\"omit\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"These attributes left out of each entity\" }," \
  "\"datasetId\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"Only the attribute instances with these datasetIds\" }," \
  "\"lang\": { \"type\": \"string\", \"description\": \"Language for LanguageProperties: en, es, *\" },"            \
  "\"format\": { \"type\": \"string\", \"enum\": [\"concise\", \"normalized\", \"simplified\"], "                     \
              "\"description\": \"concise (default): every attribute's value with its sub-attributes, unitCode, observedAt, datasetId - nothing lost. "  \
              "simplified: the values alone - use only when values alone answer the question. normalized: the full NGSI-LD form\" }"

#define PAGE_PROPERTIES                                                                                              \
  "\"limit\": { \"type\": \"integer\", \"minimum\": 1, \"description\": \"At most this many (default 20); the result tells how many there are in all\" }," \
  "\"offset\": { \"type\": \"integer\", \"minimum\": 0, \"description\": \"Skip this many (paging)\" }"



// -----------------------------------------------------------------------------
//
// Param - a tool argument that becomes a URL parameter
//
typedef enum ParamKind
{
  PkText,                                        // a string, percent-encoded whole
  PkList,                                        // a string or an array of strings, joined with commas - the separators kept
  PkInt
} ParamKind;

typedef struct Param
{
  const char*  arg;
  const char*  url;
  ParamKind    kind;
  const char*  keep;                             // for PkList: the separators sent as they are
} Param;

static const Param filterParams[] =
{
  { "type",       "type",       PkList, ",;|()" },
  { "ids",        "id",         PkList, ","     },
  { "idPattern",  "idPattern",  PkText, NULL    },
  { "q",          "q",          PkText, NULL    },
  { "scopeQ",     "scopeQ",     PkText, NULL    },
  { "csf",        "csf",        PkText, NULL    },
  { NULL,         NULL,         PkText, NULL    }
};

static const Param shapeParams[] =
{
  { "pick",       "pick",       PkList, ","     },
  { "omit",       "omit",       PkList, ","     },
  { "datasetId",  "datasetId",  PkList, ","     },
  { "lang",       "lang",       PkList, ","     },
  { NULL,         NULL,         PkText, NULL    }
};

static const Param attrsParams[] =
{
  { "attrs",      "attrs",      PkList, ","     },
  { NULL,         NULL,         PkText, NULL    }
};

static const Param temporalParams[] =
{
  { "timerel",            "timerel",            PkText, NULL },
  { "timeAt",             "timeAt",             PkText, NULL },
  { "endTimeAt",          "endTimeAt",          PkText, NULL },
  { "timeproperty",       "timeproperty",       PkText, NULL },
  { "aggrMethods",        "aggrMethods",        PkList, ","  },
  { "aggrPeriodDuration", "aggrPeriodDuration", PkText, NULL },
  { "lastN",              "lastN",              PkInt,  NULL },
  { NULL,                 NULL,                 PkText, NULL }
};



// -----------------------------------------------------------------------------
//
// Call - one tool call's state
//
typedef struct Call
{
  CorAlloc*  allocP;
  CorNode*   argsP;
  McpStr     query;
  char       error[512];                         // an argument the call cannot use
} Call;



// -----------------------------------------------------------------------------
//
// queryAdd - one URL parameter: name=value, the value percent-encoded but for 'keep'
//
static void queryAdd(Call* cP, const char* name, const char* value, const char* keep)
{
  if (cP->query.len > 0)
    strAdd(&cP->query, "&");
  strAdd(&cP->query, name);
  strAdd(&cP->query, "=");
  strAddUrlEncoded(&cP->query, value, keep);
}



// -----------------------------------------------------------------------------
//
// paramsAdd - the arguments of a Param table that the call has
//
static bool paramsAdd(Call* cP, const Param* params)
{
  for (const Param* pP = params; pP->arg != NULL; ++pP)
  {
    CorNode* argP = corTreeLookup(cP->argsP, pP->arg);
    if ((argP == NULL) || (argP->type == CorNull))
      continue;

    if (pP->kind == PkInt)
    {
      if (argP->type != CorInt)
      {
        snprintf(cP->error, sizeof(cP->error), "'%s' must be an integer", pP->arg);
        return false;
      }

      char n[32];
      snprintf(n, sizeof(n), "%lld", argP->value.i);
      queryAdd(cP, pP->url, n, NULL);
    }
    else if (argP->type == CorString)
    {
      if (argP->value.s[0] == 0)
        continue;

      if (strcmp(pP->arg, "pick") == 0)
      {
        McpStr list;
        strInit(&list, cP->allocP, 256);
        strAdd(&list, argP->value.s);
        strAdd(&list, ",id,type");               // a repeated name in pick is harmless
        queryAdd(cP, pP->url, list.buf, pP->keep);
      }
      else
        queryAdd(cP, pP->url, argP->value.s, pP->keep);
    }
    else if ((pP->kind == PkList) && (argP->type == CorArray))
    {
      McpStr list;
      bool   id   = false;
      bool   type = false;

      strInit(&list, cP->allocP, 256);

      for (CorNode* itemP = argP->value.head; itemP != NULL; itemP = itemP->next)
      {
        if (itemP->type != CorString)
        {
          snprintf(cP->error, sizeof(cP->error), "'%s' must be an array of strings", pP->arg);
          return false;
        }
        if (list.len > 0)
          strAdd(&list, ",");
        strAdd(&list, itemP->value.s);

        id   = id   || (strcmp(itemP->value.s, "id")   == 0);
        type = type || (strcmp(itemP->value.s, "type") == 0);
      }

      // pick projects id and type too: an entity without them is no use to the model - always kept
      if ((list.len > 0) && (strcmp(pP->arg, "pick") == 0))
      {
        if (!id)
          strAdd(&list, ",id");
        if (!type)
          strAdd(&list, ",type");
      }

      if (list.len > 0)
        queryAdd(cP, pP->url, list.buf, pP->keep);
    }
    else
    {
      snprintf(cP->error, sizeof(cP->error), "'%s' must be a string%s", pP->arg, (pP->kind == PkList) ? " or an array of strings" : "");
      return false;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// geoQAdd - geoQ { georel, geometry, coordinates, geoproperty } as georel=, geometry=, coordinates=, geoproperty=
//
static bool geoQAdd(Call* cP)
{
  CorNode* geoQP = corTreeLookup(cP->argsP, "geoQ");

  if ((geoQP == NULL) || (geoQP->type == CorNull))
    return true;

  if (geoQP->type != CorObject)
  {
    snprintf(cP->error, sizeof(cP->error), "'geoQ' must be an object: { georel, geometry, coordinates, geoproperty }");
    return false;
  }

  CorNode* georelP   = corTreeLookup(geoQP, "georel");
  CorNode* geometryP = corTreeLookup(geoQP, "geometry");
  CorNode* coordsP   = corTreeLookup(geoQP, "coordinates");
  CorNode* geopropP  = corTreeLookup(geoQP, "geoproperty");

  if ((georelP == NULL) || (georelP->type != CorString) || (geometryP == NULL) || (geometryP->type != CorString) || (coordsP == NULL))
  {
    snprintf(cP->error, sizeof(cP->error), "'geoQ' needs georel and geometry (strings) and coordinates (a GeoJSON coordinates array)");
    return false;
  }

  queryAdd(cP, "georel",   georelP->value.s,   NULL);
  queryAdd(cP, "geometry", geometryP->value.s, NULL);

  if (coordsP->type == CorString)               // already rendered: "[-3.7,40.4]"
    queryAdd(cP, "coordinates", coordsP->value.s, NULL);
  else if (coordsP->type == CorArray)
    queryAdd(cP, "coordinates", shapeRender(cP->allocP, coordsP), NULL);
  else
  {
    snprintf(cP->error, sizeof(cP->error), "'geoQ.coordinates' must be a GeoJSON coordinates array, e.g. [-3.70, 40.41]");
    return false;
  }

  if ((geopropP != NULL) && (geopropP->type == CorString))
    queryAdd(cP, "geoproperty", geopropP->value.s, NULL);

  return true;
}



// -----------------------------------------------------------------------------
//
// formatAdd - format: concise (the default), normalized, simplified
//
static bool formatAdd(Call* cP, const char* dflt)
{
  CorNode*    formatP = corTreeLookup(cP->argsP, "format");
  const char* format  = dflt;

  if ((formatP != NULL) && (formatP->type == CorString))
    format = formatP->value.s;
  else if ((formatP != NULL) && (formatP->type != CorNull))
  {
    snprintf(cP->error, sizeof(cP->error), "'format' must be a string");
    return false;
  }

  if      (strcmp(format, "concise")          == 0) queryAdd(cP, "options", "concise", NULL);
  else if (strcmp(format, "simplified")       == 0) queryAdd(cP, "options", "keyValues", NULL);
  else if (strcmp(format, "temporalValues")   == 0) queryAdd(cP, "options", "temporalValues", NULL);
  else if (strcmp(format, "normalized")       != 0)
  {
    snprintf(cP->error, sizeof(cP->error), "'format' is concise, normalized or simplified - not '%s'", format);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// limitAdd - limit (default 20, at most the session's limitMax) and offset; the count asked for, always
//
static bool limitAdd(Call* cP, int* limitP)
{
  CorNode* limitNodeP  = corTreeLookup(cP->argsP, "limit");
  CorNode* offsetNodeP = corTreeLookup(cP->argsP, "offset");
  int      limit       = 20;
  char     n[32];

  if ((limitNodeP != NULL) && (limitNodeP->type != CorNull))
  {
    if ((limitNodeP->type != CorInt) || (limitNodeP->value.i < 1))
    {
      snprintf(cP->error, sizeof(cP->error), "'limit' must be an integer, 1 or more");
      return false;
    }
    limit = (limitNodeP->value.i > mcpSession.limitMax) ? mcpSession.limitMax : (int) limitNodeP->value.i;
  }

  snprintf(n, sizeof(n), "%d", limit);
  queryAdd(cP, "limit", n, NULL);

  if ((offsetNodeP != NULL) && (offsetNodeP->type != CorNull))
  {
    if ((offsetNodeP->type != CorInt) || (offsetNodeP->value.i < 0))
    {
      snprintf(cP->error, sizeof(cP->error), "'offset' must be an integer, 0 or more");
      return false;
    }
    snprintf(n, sizeof(n), "%lld", offsetNodeP->value.i);
    queryAdd(cP, "offset", n, NULL);
  }

  queryAdd(cP, "count", "true", NULL);
  *limitP = limit;
  return true;
}



// -----------------------------------------------------------------------------
//
// stringArg - a required string argument
//
static const char* stringArg(Call* cP, const char* name)
{
  CorNode* argP = corTreeLookup(cP->argsP, name);

  if ((argP == NULL) || (argP->type != CorString) || (argP->value.s[0] == 0))
  {
    snprintf(cP->error, sizeof(cP->error), "'%s' is required (a string)", name);
    return NULL;
  }

  return argP->value.s;
}



// -----------------------------------------------------------------------------
//
// pathAdd - a path segment, percent-encoded (an entity id is a URI: ':' stays, '/' and '#' do not)
//
static char* pathWith(Call* cP, const char* prefix, const char* segment)
{
  McpStr path;

  strInit(&path, cP->allocP, 256);
  strAdd(&path, prefix);
  strAddUrlEncoded(&path, segment, ":");
  return path.buf;
}



// -----------------------------------------------------------------------------
//
// listResult - a list answer: { count, returned, more, <member>: [...] }
//
static CorNode* listResult(Call* cP, McpResponse* respP, const char* member, int limit, int offset)
{
  if ((respP->body == NULL) || (respP->body->type != CorArray))
    return shapeError(cP->allocP, "the broker's answer is not a JSON array");

  int returned = 0;
  for (CorNode* itemP = respP->body->value.head; itemP != NULL; itemP = itemP->next)
    ++returned;

  CorNode* outP = corTreeObject(cP->allocP, NULL);

  if (respP->count >= 0)
  {
    corTreeChildAdd(outP, corTreeInteger(cP->allocP, "count", respP->count));
    corTreeChildAdd(outP, corTreeInteger(cP->allocP, "returned", returned));
    corTreeChildAdd(outP, corTreeBoolean(cP->allocP, "more", offset + returned < respP->count));
  }
  else
  {
    corTreeChildAdd(outP, corTreeInteger(cP->allocP, "returned", returned));
    corTreeChildAdd(outP, corTreeBoolean(cP->allocP, "more", returned >= limit));
  }

  shapeCut(cP->allocP, respP->body);
  respP->body->name = (char*) member;
  corTreeChildAdd(outP, respP->body);

  return shapeResult(cP->allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// offsetOf -
//
static int offsetOf(Call* cP)
{
  CorNode* offsetP = corTreeLookup(cP->argsP, "offset");
  return ((offsetP != NULL) && (offsetP->type == CorInt)) ? (int) offsetP->value.i : 0;
}



// -----------------------------------------------------------------------------
//
// send - the request, through the session's backend; NULL: a success the tool shapes itself
//
static CorNode* send(Call* cP, const char* path, McpResponse* respP)
{
  mcpBackend->request(cP->allocP, CorVerbGet, path, cP->query.buf, NULL, respP);

  if ((respP->status < 200) || (respP->status > 299))
    return shapeBrokerError(cP->allocP, respP);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// The tools
//
static CorNode* listTypes(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;

  queryAdd(cP, "details", "true", NULL);
  if ((errP = send(cP, "/types", &resp)) != NULL)
    return errP;

  if ((resp.body == NULL) || (resp.body->type != CorArray))
    return shapeError(cP->allocP, "the broker's answer to GET /types is not a JSON array");

  CorNode* outP = corTreeObject(cP->allocP, NULL);
  shapeCut(cP->allocP, resp.body);
  resp.body->name = (char*) "types";
  corTreeChildAdd(outP, resp.body);
  return shapeResult(cP->allocP, outP, false);
}

static CorNode* listAttributes(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;
  CorNode*    typeP = corTreeLookup(cP->argsP, "type");
  const char* path  = "/attributes";

  if ((typeP != NULL) && (typeP->type == CorString) && (typeP->value.s[0] != 0))
    path = pathWith(cP, "/types/", typeP->value.s);
  else
    queryAdd(cP, "details", "true", NULL);

  if ((errP = send(cP, path, &resp)) != NULL)
    return errP;

  if (resp.body == NULL)
    return shapeError(cP->allocP, "the broker's answer to GET %s is not JSON", path);

  CorNode* outP = corTreeObject(cP->allocP, NULL);
  shapeCut(cP->allocP, resp.body);
  resp.body->name = (char*) ((resp.body->type == CorArray) ? "attributes" : "type");
  corTreeChildAdd(outP, resp.body);
  return shapeResult(cP->allocP, outP, false);
}

static CorNode* queryEntities(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;
  int         limit;

  if (!paramsAdd(cP, filterParams) || !geoQAdd(cP) || !paramsAdd(cP, shapeParams) || !formatAdd(cP, "concise") || !limitAdd(cP, &limit))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((errP = send(cP, "/entities", &resp)) != NULL)
    return errP;

  return listResult(cP, &resp, "entities", limit, offsetOf(cP));
}

static CorNode* countEntities(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;

  if (!paramsAdd(cP, filterParams) || !geoQAdd(cP))
    return shapeError(cP->allocP, "%s", cP->error);

  queryAdd(cP, "count", "true", NULL);
  queryAdd(cP, "limit", "0", NULL);

  if ((errP = send(cP, "/entities", &resp)) != NULL)
    return errP;

  if (resp.count < 0)
    return shapeError(cP->allocP, "the broker did not answer with NGSILD-Results-Count");

  CorNode* outP = corTreeObject(cP->allocP, NULL);
  corTreeChildAdd(outP, corTreeInteger(cP->allocP, "count", resp.count));
  return shapeResult(cP->allocP, outP, false);
}

static CorNode* getEntity(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;
  const char* id = stringArg(cP, "id");

  if ((id == NULL) || !paramsAdd(cP, shapeParams) || !formatAdd(cP, "concise"))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((errP = send(cP, pathWith(cP, "/entities/", id), &resp)) != NULL)
    return errP;

  if ((resp.body == NULL) || (resp.body->type != CorObject))
    return shapeError(cP->allocP, "the broker's answer is not a JSON object");

  shapeCut(cP->allocP, resp.body);
  return shapeResult(cP->allocP, resp.body, false);
}

static CorNode* temporal(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;
  CorNode*    idP   = corTreeLookup(cP->argsP, "id");
  CorNode*    aggrP = corTreeLookup(cP->argsP, "aggrMethods");
  int         limit = 0;

  const char* timerel = stringArg(cP, "timerel");

  if (timerel == NULL)
    return shapeError(cP->allocP, "%s - before, after or between", cP->error);

  if ((strcmp(timerel, "before") != 0) && (strcmp(timerel, "after") != 0) && (strcmp(timerel, "between") != 0))
    return shapeError(cP->allocP, "'timerel' is before, after or between - not '%s'", timerel);

  if (stringArg(cP, "timeAt") == NULL)                               // every timerel has a timeAt (§ 4.11)
    return shapeError(cP->allocP, "%s - ISO 8601, with timerel %s", cP->error, timerel);

  if ((strcmp(timerel, "between") == 0) && (stringArg(cP, "endTimeAt") == NULL))
    return shapeError(cP->allocP, "%s - ISO 8601, with timerel between", cP->error);

  cP->error[0] = 0;
  if (!paramsAdd(cP, temporalParams) || !paramsAdd(cP, attrsParams) || !paramsAdd(cP, shapeParams))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((aggrP != NULL) && (aggrP->type != CorNull))
    queryAdd(cP, "options", "aggregatedValues", NULL);
  else if (!formatAdd(cP, "normalized"))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((idP != NULL) && (idP->type == CorString) && (idP->value.s[0] != 0))
  {
    if ((errP = send(cP, pathWith(cP, "/temporal/entities/", idP->value.s), &resp)) != NULL)
      return errP;

    if ((resp.body == NULL) || (resp.body->type != CorObject))
      return shapeError(cP->allocP, "the broker's answer is not a JSON object");

    shapeCut(cP->allocP, resp.body);
    return shapeResult(cP->allocP, resp.body, false);
  }

  if (!paramsAdd(cP, filterParams) || !geoQAdd(cP) || !limitAdd(cP, &limit))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((errP = send(cP, "/temporal/entities", &resp)) != NULL)
    return errP;

  return listResult(cP, &resp, "entities", limit, offsetOf(cP));
}

static CorNode* listSubscriptions(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;
  int         limit;

  if (!limitAdd(cP, &limit))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((errP = send(cP, "/subscriptions", &resp)) != NULL)
    return errP;

  return listResult(cP, &resp, "subscriptions", limit, offsetOf(cP));
}

static CorNode* queryRegistrations(Call* cP)
{
  McpResponse resp;
  CorNode*    errP;
  int         limit;

  if (!paramsAdd(cP, filterParams) || !paramsAdd(cP, attrsParams) || !limitAdd(cP, &limit))
    return shapeError(cP->allocP, "%s", cP->error);

  if ((errP = send(cP, "/csourceRegistrations", &resp)) != NULL)
    return errP;

  return listResult(cP, &resp, "registrations", limit, offsetOf(cP));
}



// The broker tools (brokerTools.c) - their arguments as they came
static CorNode* brokerStatusT(Call* cP)          { return brokerStatus(cP->allocP, cP->argsP); }
static CorNode* brokerConfigT(Call* cP)          { return brokerConfig(cP->allocP, cP->argsP); }
static CorNode* brokerMetricsT(Call* cP)         { return brokerMetrics(cP->allocP, cP->argsP); }
static CorNode* listIndexesT(Call* cP)           { return listIndexes(cP->allocP, cP->argsP); }
static CorNode* bridgeInventoryT(Call* cP)       { return bridgeInventory(cP->allocP, cP->argsP); }
static CorNode* diagnoseSubscriptionT(Call* cP)  { return diagnoseSubscription(cP->allocP, cP->argsP); }
static CorNode* readLogT(Call* cP)               { return readLog(cP->allocP, cP->argsP); }
static CorNode* securityReviewT(Call* cP)        { return securityReview(cP->allocP, cP->argsP); }
static CorNode* diagnoseDeviceT(Call* cP)        { return diagnoseDevice(cP->allocP, cP->argsP); }
static CorNode* smartDataModelT(Call* cP)        { return smartDataModel(cP->allocP, cP->argsP); }
static CorNode* listRulesT(Call* cP)             { return listRules(cP->allocP, cP->argsP); }
static CorNode* dryRunRuleT(Call* cP)            { return dryRunRule(cP->allocP, cP->argsP); }
static CorNode* searchDocsT(Call* cP)            { return searchDocs(cP->allocP, cP->argsP); }
static CorNode* readDocT(Call* cP)               { return readDoc(cP->allocP, cP->argsP); }



// -----------------------------------------------------------------------------
//
// Tool - the table tools/list renders and tools/call dispatches on
//
typedef struct Tool
{
  const char*  name;
  const char*  title;
  const char*  description;
  const char*  inputSchema;                      // JSON
  CorNode*     (*call)(Call* cP);
} Tool;

static const Tool tools[] =
{
  {
    "list_types", "Entity types",
    "The entity types the broker holds, each with its attribute names. Call it first, to learn what the data are "
    "and which short names to use in the other tools.",
    "{ \"type\": \"object\", \"properties\": {} }",
    listTypes
  },
  {
    "list_attributes", "Attributes",
    "The attribute names in use, with the types that have each one and its attribute types (Property, "
    "Relationship, GeoProperty, ...). With 'type': the attributes of that entity type only.",
    "{ \"type\": \"object\", \"properties\": { \"type\": { \"type\": \"string\", \"description\": \"An entity type, e.g. Store\" } } }",
    listAttributes
  },
  {
    "query_entities", "Query entities",
    "The entities that match the filters - at least one of type, q, geoQ; ids and idPattern narrow them, and "
    "need a type too. Example: { type: 'Store', q: 'stock<10;status==\"open\"', pick: ['stock', 'location'] }. "
    "The entities that have an attribute: q: 'stock'. "
    "An object value is filtered by its keys: q: 'dimensions[width]>2'. "
    "The result: { count (all matches), returned, more, entities }; when more is true, page with offset. "
    "For one entity whose id you can form, get_entity; when the number alone is the answer, count_entities.",
    "{ \"type\": \"object\", \"properties\": { " FILTER_PROPERTIES "," SHAPE_PROPERTIES "," PAGE_PROPERTIES " } }",
    queryEntities
  },
  {
    "get_entity", "Get an entity",
    "One entity by its id, every attribute (or pick/omit). Ids are URIs, usually urn:ngsi-ld:<type>:<name> - the "
    "user's 'S-17' of type Store is { id: 'urn:ngsi-ld:Store:S-17' }: try that before searching. "
    "A Relationship's object is the id of the entity it points to - get_entity it to follow the relationship.",
    "{ \"type\": \"object\", \"properties\": { \"id\": { \"type\": \"string\", \"description\": \"The entity id (a URI)\" }, "
      SHAPE_PROPERTIES " }, \"required\": [\"id\"] }",
    getEntity
  },
  {
    "count_entities", "Count entities",
    "How many entities match the filters (the same filters as query_entities) - no entities returned. The count is "
    "exact: no query is needed to check it. Example: { type: 'Store', q: 'stock<10' }.",
    "{ \"type\": \"object\", \"properties\": { " FILTER_PROPERTIES " } }",
    countEntities
  },
  {
    "temporal", "History",
    "The history of attribute values (temporal representation), for one entity (id) or the entities the filters "
    "match. timerel: before | after | between (with endTimeAt); timeAt and endTimeAt in ISO 8601 UTC. "
    "Example: { id: 'urn:ngsi-ld:Store:S-17', attrs: ['stock'], timerel: 'after', timeAt: '2026-10-01T00:00:00Z', lastN: 10 }. "
    "Aggregation: aggrMethods (avg, min, max, sum, sumsq, stddev, totalCount, distinctCount) over aggrPeriodDuration (ISO 8601: PT1H). "
    "The broker must keep history (temporal API); an error says when it does not.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"id\": { \"type\": \"string\", \"description\": \"One entity's id - or use the filters\" }, "
      FILTER_PROPERTIES ","
      "\"attrs\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"The attributes whose history is wanted\" },"
      "\"timerel\": { \"type\": \"string\", \"enum\": [\"before\", \"after\", \"between\"] },"
      "\"timeAt\": { \"type\": \"string\", \"description\": \"ISO 8601, e.g. 2026-10-01T00:00:00Z\" },"
      "\"endTimeAt\": { \"type\": \"string\", \"description\": \"ISO 8601 - with timerel between\" },"
      "\"timeproperty\": { \"type\": \"string\", \"enum\": [\"observedAt\", \"createdAt\", \"modifiedAt\", \"deletedAt\"], \"description\": \"The time the timerel applies to (default observedAt)\" },"
      "\"lastN\": { \"type\": \"integer\", \"minimum\": 1, \"description\": \"Only the last N instances of each attribute\" },"
      "\"aggrMethods\": { \"type\": \"array\", \"items\": { \"type\": \"string\", \"enum\": [\"totalCount\", \"distinctCount\", \"sum\", \"avg\", \"min\", \"max\", \"stddev\", \"sumsq\"] } },"
      "\"aggrPeriodDuration\": { \"type\": \"string\", \"description\": \"ISO 8601 duration of each aggregation period, e.g. PT1H\" },"
      "\"pick\": { \"type\": \"array\", \"items\": { \"type\": \"string\" } },"
      "\"omit\": { \"type\": \"array\", \"items\": { \"type\": \"string\" } },"
      "\"datasetId\": { \"type\": \"array\", \"items\": { \"type\": \"string\" } },"
      "\"lang\": { \"type\": \"string\" },"
      "\"format\": { \"type\": \"string\", \"enum\": [\"normalized\", \"temporalValues\", \"concise\"], \"description\": \"normalized (default): each instance in full; temporalValues: [value, time] pairs\" },"
      PAGE_PROPERTIES " }, \"required\": [\"timerel\", \"timeAt\"] }",
    temporal
  },
  {
    "list_subscriptions", "Subscriptions",
    "The subscriptions in the broker: what each one watches, where it notifies, its status.",
    "{ \"type\": \"object\", \"properties\": { " PAGE_PROPERTIES " } }",
    listSubscriptions
  },
  {
    "query_registrations", "Context source registrations",
    "The context source registrations: which other brokers or sources hold which entities and attributes, "
    "and how the broker forwards to them. NGSI-LD wants at least one filter (type, ids, idPattern, attrs, q, csf): "
    "to see them all, pass every type list_types shows, e.g. { type: 'Store,Warehouse' }.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"type\": { \"type\": \"string\", \"description\": \"Entity type(s) the registrations cover: Store; Store,Warehouse\" },"
      "\"ids\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"Entity ids the registrations cover\" },"
      "\"idPattern\": { \"type\": \"string\", \"description\": \"A regular expression for the entity ids\" },"
      "\"attrs\": { \"type\": \"array\", \"items\": { \"type\": \"string\" }, \"description\": \"Attributes the registrations cover\" },"
      "\"q\": { \"type\": \"string\", \"description\": \"A filter on the registrations' own properties\" },"
      "\"csf\": { \"type\": \"string\", \"description\": \"Context source filter, e.g. mode==\\\"inclusive\\\"\" },"
      PAGE_PROPERTIES " } }",
    queryRegistrations
  },
  {
    "broker_status", "Is the broker all right?",
    "The broker itself, in one call: its version, health, tenants and plugins, memory against its budget, requests "
    "and errors, notifications sent and failed, forwarding - and 'findings': what needs attention, worst first. "
    "For \"is my broker OK\", \"why is it slow\", \"what version is it\". The counters are since the broker started. "
    "Needs the broker's admin API (coraine); the answer says when it is not there.",
    "{ \"type\": \"object\", \"properties\": {} }",
    brokerStatusT
  },
  {
    "broker_config", "Configuration",
    "The broker's settings in effect, each with its value, default, where it came from (default, command line, "
    "environment, file), its environment variable and what it does. Without arguments: only the ones not at their "
    "default - what this broker does differently. 'match': the options whose name or description has this text (e.g. "
    "\"memory\", \"tls\", \"port\"); 'all': true for every option. Secrets are never shown.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"match\": { \"type\": \"string\", \"description\": \"Text in the option's name or description, e.g. memory\" }, "
      "\"all\": { \"type\": \"boolean\", \"description\": \"Every option, defaults included\" } } }",
    brokerConfigT
  },
  {
    "diagnose_subscription", "Why (not) notified?",
    "Why a subscription does or does not notify: its status and expiry, its endpoint, its delivery so far (times "
    "sent and failed, the last failure and why), whether any entity matches its entities, q, scopeQ, geoQ and "
    "watched attributes now, its triggers and throttling. With 'entityId': whether that entity is selected and "
    "matches, and whether its watched attributes changed after the subscription was made. Each check has ok "
    "true, false (a reason it does not notify) or null (to know). One call - no need to query the parts yourself.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"id\": { \"type\": \"string\", \"description\": \"The subscription's id, as list_subscriptions shows it\" }, "
      "\"entityId\": { \"type\": \"string\", \"description\": \"An entity the user expected a notification about\" } }, "
      "\"required\": [\"id\"] }",
    diagnoseSubscriptionT
  },
  {
    "broker_metrics", "Counters",
    "The broker's Prometheus counters and gauges, summed over their labels; 'match' keeps the names that contain "
    "it (e.g. \"latency\", \"subscription\", \"memory\", \"error\"). For the questions broker_status does not answer.",
    "{ \"type\": \"object\", \"properties\": { \"match\": { \"type\": \"string\", \"description\": \"Text in the metric name\" } } }",
    brokerMetricsT
  },
  {
    "list_indexes", "Indexes",
    "The database indexes the broker has added: for the current state, and for history (TRoE). A query on an "
    "attribute without an index reads every entity of its type - the reason a query is slow on a big tenant.",
    "{ \"type\": \"object\", \"properties\": {} }",
    listIndexesT
  },
  {
    "bridge_inventory", "Bridge inventory",
    "What a bridge (a device-protocol plugin: mqtt, modbus, ...) holds: its devices, their Channels and how each "
    "maps to an entity attribute. For \"why do this device's values not arrive\". broker_status lists the plugins.",
    "{ \"type\": \"object\", \"properties\": { \"bridge\": { \"type\": \"string\", \"description\": \"The bridge's name\" } }, \"required\": [\"bridge\"] }",
    bridgeInventoryT
  },
  {
    "read_log", "Read the broker's log",
    "The broker's recent log (coraine 0.8+: GET /admin/log/lines, the lines it keeps in memory), oldest first, "
    "each with its UTC time and type - E error, W warning, I info, V verbose, T trace. For \"what went wrong\", "
    "\"why was this refused\", \"is the bridge connected\": types EW first, match a text (an entity id, a "
    "bridge's name, an endpoint), since a time. A request the broker refused logs its status and reason. Info "
    "and verbose lines are there only when the broker logs them (its log levels).",
    "{ \"type\": \"object\", \"properties\": { "
      "\"lines\": { \"type\": \"integer\", \"minimum\": 1, \"maximum\": 500, \"description\": \"The last N lines that match (50)\" }, "
      "\"types\": { \"type\": \"string\", \"description\": \"The line types, letters: EW for errors and warnings\" }, "
      "\"match\": { \"type\": \"string\", \"description\": \"A text the line contains\" }, "
      "\"since\": { \"type\": \"string\", \"description\": \"A UTC time: 2026-11-20T12:00:00Z\" } } }",
    readLogT
  },
  {
    "diagnose_device", "Why a device's values do not arrive",
    "One call for \"my sensor's values don't show up\": the Bridges (a plugin not loaded), the Channels that map "
    "the entity - their status and its reason, samples in since the start, payloads refused, the direction - the "
    "attribute's value (\"uninitialized\": not one sample yet) and when it last changed, and the bridge's errors "
    "and warnings in the log. Give the entity's id, and the attribute when you know it.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"entity\": { \"type\": \"string\", \"description\": \"The entity the device's values should reach\" }, "
      "\"attribute\": { \"type\": \"string\", \"description\": \"Its attribute (optional)\" } }, \"required\": [\"entity\"] }",
    diagnoseDeviceT
  },
  {
    "security_review", "Review the broker's security",
    "Who can do what with this broker, from its configuration (coraine's /admin/config): auth, HTTPS, CORS, "
    "the admin API, the other ports (cor://, the policy API, health), the databases' credentials, passwords "
    "visible on the command line. Findings by severity (high, medium, info, ok), each with why and the fix, "
    "and what it cannot see (the network around it). For \"is my broker secure\", \"can I expose it\", "
    "\"who can change my data\". Then search_docs (authorization) for the details of a fix.",
    "{ \"type\": \"object\", \"properties\": {} }",
    securityReviewT
  },
  {
    "list_rules", "List the automation rules",
    "corCep's rules - when something happens (an attribute updated, a pattern of events, a schedule), do something "
    "(set, toggle, step, create, delete entities). The Rule entities of the rules tenant, keyValues: when, then, "
    "else, enabled, and corCep's statistics - fired, firedAt, lastError. For \"which rules are there\", \"why did "
    "my rule not fire\", \"what turns this light on\". match: a text of the id or name.",
    "{ \"type\": \"object\", \"properties\": { \"match\": { \"type\": \"string\", \"description\": \"A text of the rule's id or name\" } } }",
    listRulesT
  },
  {
    "dry_run_rule", "Try a rule out",
    "Replay rules against events made up for them, on corCep's POST /dry-run - what would fire, when, on which "
    "entities, and what an if stopped. Nothing is written. Use it on every rule you write before c³'s save_rule, "
    "and to explain an existing one. rules: Rule entities, keyValues, when/then/else as {\"json\": ...} (list_rules "
    "gives them so); events: [{at (ms or PT1S), entity, type, attribute, value}]; entities: the state where/if read; "
    "start: the clock at 0 (for cron, at); for: how long. The rule language: search_docs \"corCep rule\".",
    "{ \"type\": \"object\", \"properties\": { "
      "\"rules\": { \"type\": \"array\", \"items\": { \"type\": \"object\" } }, "
      "\"events\": { \"type\": \"array\", \"items\": { \"type\": \"object\" } }, "
      "\"entities\": { \"type\": \"array\", \"items\": { \"type\": \"object\" } }, "
      "\"start\": { \"type\": \"string\" }, \"for\": { \"type\": \"string\" } }, \"required\": [\"rules\", \"events\"] }",
    dryRunRuleT
  },
  {
    "smart_data_model", "FIWARE Smart Data Models",
    "The published data models (smartdatamodels.org, read from their GitHub repositories) - to model the user's "
    "data on one instead of inventing a shape, or to check existing entities against one. { search }: the models "
    "whose name, subject or domain has the text (WeatherObserved, Parking, Building, Device, AirQuality...); no "
    "argument: the subjects. { model } (and subject when two have it): its description, its attributes with "
    "theirs, the required ones, and a normalized NGSI-LD example with the model's @context.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"search\": { \"type\": \"string\", \"description\": \"A word of the model's name, subject or domain\" }, "
      "\"model\": { \"type\": \"string\", \"description\": \"A model's name: OffStreetParking\" }, "
      "\"subject\": { \"type\": \"string\", \"description\": \"Its repository, when two have the name: dataModel.Parking\" } } }",
    smartDataModelT
  },
  {
    "search_docs", "Search the documentation",
    "The broker's documentation and the knowledge base - how to install, configure, build, tune and troubleshoot "
    "it, every option, measured sizes and performance, what each build feature costs and saves. The sections that "
    "best match the words, each with its file, path and a matching line. Use the words the docs would use: an "
    "option's name (dbValueIndexes), a feature (COR_FEATURE_GEOQ), an endpoint (/admin/indexes), an error's title. "
    "Then read_doc for the text. Answer from what you read, and name the file and section.",
    "{ \"type\": \"object\", \"properties\": { \"query\": { \"type\": \"string\", \"description\": \"The words to look for\" } }, \"required\": [\"query\"] }",
    searchDocsT
  },
  {
    "read_doc", "Read the documentation",
    "A section of a documentation file, with its subsections - { file, section } as search_docs gave them; a long "
    "one comes in parts (nextOffset: pass it as offset). { file } alone: the file's headings. No arguments: the files.",
    "{ \"type\": \"object\", \"properties\": { "
      "\"file\": { \"type\": \"string\", \"description\": \"e.g. performance.md\" }, "
      "\"section\": { \"type\": \"string\", \"description\": \"The heading, or a part of it\" }, "
      "\"offset\": { \"type\": \"integer\", \"minimum\": 0, \"description\": \"Where to go on in a long section (nextOffset)\" } } }",
    readDocT
  }
};

#define TOOLS  ((int) (sizeof(tools) / sizeof(tools[0])))



// -----------------------------------------------------------------------------
//
// toolsList - tools/list's result
//
CorNode* toolsList(CorAlloc* allocP)
{
  CorNode* resultP = corTreeObject(allocP, NULL);
  CorNode* toolsP  = corTreeArray(allocP, "tools");

  for (int ix = 0; ix < TOOLS; ix++)
  {
    CorNode* toolP = corTreeObject(allocP, NULL);
    CorJson  json;

    corJsonCreate(&json, allocP);
    CorNode* schemaP = corJsonParse(&json, corAllocStrdup(allocP, tools[ix].inputSchema));
    if (schemaP == NULL)                         // a broken schema is a build error - tests catch it
      continue;
    if ((mcpSession.docs == NULL) && ((tools[ix].call == searchDocsT) || (tools[ix].call == readDocT)))
      continue;                                  // no --docs: no documentation tools

    schemaP->name = (char*) "inputSchema";

    CorNode* annP = corTreeObject(allocP, "annotations");
    corTreeChildAdd(annP, corTreeBoolean(allocP, "readOnlyHint",   true));
    corTreeChildAdd(annP, corTreeBoolean(allocP, "openWorldHint",  false));

    corTreeChildAdd(toolP, corTreeString(allocP, "name",        tools[ix].name));
    corTreeChildAdd(toolP, corTreeString(allocP, "title",       tools[ix].title));
    corTreeChildAdd(toolP, corTreeString(allocP, "description", tools[ix].description));
    corTreeChildAdd(toolP, schemaP);
    corTreeChildAdd(toolP, annP);
    corTreeChildAdd(toolsP, toolP);
  }

  corTreeChildAdd(resultP, toolsP);
  return resultP;
}



// -----------------------------------------------------------------------------
//
// toolsCall - tools/call's result; *unknownP: no such tool (a protocol error, not a tool error)
//
CorNode* toolsCall(CorAlloc* allocP, const char* name, CorNode* argsP, bool* unknownP)
{
  *unknownP = false;

  for (int ix = 0; ix < TOOLS; ix++)
  {
    if (strcmp(tools[ix].name, name) != 0)
      continue;

    Call call;
    call.allocP   = allocP;
    call.argsP    = (argsP != NULL) ? argsP : corTreeObject(allocP, NULL);
    call.error[0] = 0;
    strInit(&call.query, allocP, 512);

    if (call.argsP->type != CorObject)
      return shapeError(allocP, "the arguments must be an object");

    return tools[ix].call(&call);
  }

  *unknownP = true;
  return NULL;
}
