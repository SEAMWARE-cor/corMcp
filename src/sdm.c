//
// FILE            sdm.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// smart_data_model - FIWARE's Smart Data Models, read from their GitHub repositories: the official list (searched
// by name, subject, domain), and a model's normalized NGSI-LD example and its attributes' descriptions - so the
// assistant models a user's data on a published model instead of inventing one. A plain GET to
// raw.githubusercontent.com: no broker, no token, nothing of the user's sent.
//
#include <stdio.h>                               // snprintf
#include <string.h>                              // strlen, memcpy, strcasestr
#include <strings.h>                             // strcasecmp

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corRest/corRestClient.h"               // corRestClientMulti*
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corMcp.h"



#define RAW            "https://raw.githubusercontent.com/smart-data-models"
#define LIST_URL       RAW "/data-models/master/specs/AllSubjects/official_list_data_models.json"
#define SDM_MAX        (4 * 1024 * 1024)
#define SDM_TIMEOUT_MS 20000
#define FOUND_MAX      40



// -----------------------------------------------------------------------------
//
// fetch - a GET of a public URL: the parsed JSON (NULL: not there, not JSON - *statusP says which)
//
static CorNode* fetch(CorAlloc* allocP, const char* url, int* statusP)
{
  CorRestKeyValue headers[1] = { { (char*) "Accept", (char*) "application/json" } };

  *statusP = 0;
  CorRestClientMulti* multiP = corRestClientMultiCreate(1);
  if (multiP == NULL)
    return NULL;

  corRestClientMultiMaxResponse(multiP, SDM_MAX);
  if (corRestClientMultiAdd(multiP, CorVerbGet, url, headers, 1, NULL, 0, NULL, NULL) < 0)
  {
    corRestClientMultiDestroy(multiP);
    return NULL;
  }
  corRestClientMultiPerform(multiP, SDM_TIMEOUT_MS);

  CorRestClientResponse* rP   = corRestClientMultiResponse(multiP, 0);
  CorNode*               body = NULL;

  if ((rP != NULL) && (rP->error == CORR_OK))
  {
    *statusP = rP->statusCode;
    if ((rP->statusCode == 200) && (rP->body != NULL) && (rP->bodyLen > 0))
    {
      char*   copy = corAlloc(allocP, rP->bodyLen + 1);
      CorJson json;
      memcpy(copy, rP->body, rP->bodyLen);
      copy[rP->bodyLen] = 0;
      corJsonCreate(&json, allocP);
      body = corJsonParse(&json, copy);
    }
  }
  corRestClientMultiDestroy(multiP);
  return body;
}



static const char* str(CorNode* objectP, const char* name)
{
  CorNode* nP = (objectP != NULL) ? corTreeLookup(objectP, name) : NULL;
  return ((nP != NULL) && (nP->type == CorString)) ? nP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// smartDataModel -
//
CorNode* smartDataModel(CorAlloc* allocP, CorNode* argsP)
{
  const char* model  = str(argsP, "model");
  const char* search = str(argsP, "search");
  int         status;

  CorNode* listP = fetch(allocP, LIST_URL, &status);
  CorNode* offP  = (listP != NULL) ? corTreeLookup(listP, "officialList") : NULL;
  if ((offP == NULL) || (offP->type != CorArray))
    return shapeError(allocP, "the Smart Data Models list could not be read from GitHub (%d) - no network, or GitHub not reachable from here", status);

  //
  // No model: the models whose name, repository or domain has the text (all repositories without one)
  //
  if ((model == NULL) || (model[0] == 0))
  {
    CorNode* outP   = corTreeObject(allocP, NULL);
    CorNode* foundP = corTreeArray(allocP, "models");
    int      n      = 0;
    int      total  = 0;

    for (CorNode* rP = offP->value.head; rP != NULL; rP = rP->next)
    {
      const char* repo    = str(rP, "repoName");
      CorNode*    modelsP = corTreeLookup(rP, "dataModels");
      CorNode*    domsP   = corTreeLookup(rP, "domains");
      bool        repoHit = false;
      McpStr      doms;

      strInit(&doms, allocP, 128);
      if ((domsP != NULL) && (domsP->type == CorArray))
        for (CorNode* dP = domsP->value.head; dP != NULL; dP = dP->next)
          if (dP->type == CorString) { if (doms.len > 0) strAdd(&doms, ", "); strAdd(&doms, dP->value.s); }

      if ((search != NULL) && (search[0] != 0))
        repoHit = ((repo != NULL) && (strcasestr(repo, search) != NULL)) || (strcasestr(doms.buf, search) != NULL);

      if ((modelsP == NULL) || (modelsP->type != CorArray))
        continue;
      for (CorNode* mP = modelsP->value.head; mP != NULL; mP = mP->next)
      {
        if (mP->type != CorString)
          continue;
        ++total;
        if ((search != NULL) && (search[0] != 0) && !repoHit && (strcasestr(mP->value.s, search) == NULL))
          continue;
        if ((search == NULL) || (search[0] == 0))
          continue;                                // without a search: the subjects only, below
        if (++n > FOUND_MAX)
          continue;
        CorNode* hP = corTreeObject(allocP, NULL);
        corTreeChildAdd(hP, corTreeString(allocP, "model", mP->value.s));
        corTreeChildAdd(hP, corTreeString(allocP, "subject", repo ? repo : ""));
        corTreeChildAdd(hP, corTreeString(allocP, "domains", doms.buf));
        corTreeChildAdd(foundP, hP);
      }
      if ((search == NULL) || (search[0] == 0))
      {
        CorNode* hP = corTreeObject(allocP, NULL);
        corTreeChildAdd(hP, corTreeString(allocP, "subject", repo ? repo : ""));
        corTreeChildAdd(hP, corTreeString(allocP, "domains", doms.buf));
        int c = 0;
        for (CorNode* mP = modelsP->value.head; mP != NULL; mP = mP->next)
          ++c;
        corTreeChildAdd(hP, corTreeInteger(allocP, "models", c));
        corTreeChildAdd(foundP, hP);
      }
    }

    if ((search == NULL) || (search[0] == 0))
      foundP->name = (char*) "subjects";
    corTreeChildAdd(outP, corTreeInteger(allocP, "found", n));
    corTreeChildAdd(outP, corTreeInteger(allocP, "modelsInAll", total));
    corTreeChildAdd(outP, foundP);
    corTreeChildAdd(outP, corTreeString(allocP, "next", (n > FOUND_MAX) ? "more than shown - a narrower search; then { model } for one" : "smart_data_model { model } for its example and attributes"));
    return shapeResult(allocP, outP, false);
  }

  //
  // A model: its subject, then its example (normalized NGSI-LD) and its schema's descriptions
  //
  const char* subject = str(argsP, "subject");
  const char* name    = NULL;
  for (CorNode* rP = offP->value.head; (rP != NULL) && (name == NULL); rP = rP->next)
  {
    const char* repo    = str(rP, "repoName");
    CorNode*    modelsP = corTreeLookup(rP, "dataModels");
    if ((subject != NULL) && (subject[0] != 0) && ((repo == NULL) || (strcasecmp(repo, subject) != 0)))
      continue;
    if ((modelsP == NULL) || (modelsP->type != CorArray))
      continue;
    for (CorNode* mP = modelsP->value.head; mP != NULL; mP = mP->next)
    {
      if ((mP->type == CorString) && (strcasecmp(mP->value.s, model) == 0))
      {
        name    = mP->value.s;
        subject = repo;
        break;
      }
    }
  }
  if (name == NULL)
    return shapeError(allocP, "no Smart Data Model '%s'%s - smart_data_model { search } finds the names", model, (subject != NULL) ? " in that subject" : "");

  char url[1024];
  int  exStatus;
  int  scStatus;
  snprintf(url, sizeof(url), "%s/%s/master/%s/examples/example-normalized.jsonld", RAW, subject, name);
  CorNode* exampleP = fetch(allocP, url, &exStatus);
  snprintf(url, sizeof(url), "%s/%s/master/%s/schema.json", RAW, subject, name);
  CorNode* schemaP  = fetch(allocP, url, &scStatus);

  CorNode* outP = corTreeObject(allocP, NULL);
  corTreeChildAdd(outP, corTreeString(allocP, "model", name));
  corTreeChildAdd(outP, corTreeString(allocP, "subject", subject));
  snprintf(url, sizeof(url), "https://github.com/smart-data-models/%s/tree/master/%s", subject, name);
  corTreeChildAdd(outP, corTreeString(allocP, "spec", corAllocStrdup(allocP, url)));

  if (schemaP != NULL)
  {
    const char* desc = str(schemaP, "description");
    if (desc != NULL)
      corTreeChildAdd(outP, corTreeString(allocP, "description", desc));

    //
    // The attributes: the schema's own properties (allOf's inline ones too), each with its description, cut short
    //
    CorNode* attrsP = corTreeObject(allocP, "attributes");
    CorNode* parts[8];
    int      nParts = 0;
    CorNode* allOfP = corTreeLookup(schemaP, "allOf");
    CorNode* propsP = corTreeLookup(schemaP, "properties");
    if (propsP != NULL) parts[nParts++] = propsP;
    if ((allOfP != NULL) && (allOfP->type == CorArray))
      for (CorNode* aP = allOfP->value.head; (aP != NULL) && (nParts < 8); aP = aP->next)
        if ((propsP = corTreeLookup(aP, "properties")) != NULL) parts[nParts++] = propsP;

    for (int px = 0; px < nParts; px++)
    {
      for (CorNode* pP = parts[px]->value.head; pP != NULL; pP = pP->next)
      {
        if ((pP->name == NULL) || (corTreeLookup(attrsP, pP->name) != NULL))
          continue;
        const char* d = str(pP, "description");
        char        cut[300];
        snprintf(cut, sizeof(cut), "%s", (d != NULL) ? d : "");
        corTreeChildAdd(attrsP, corTreeString(allocP, pP->name, corAllocStrdup(allocP, cut)));
      }
    }
    corTreeChildAdd(outP, attrsP);
    CorNode* reqP = corTreeLookup(schemaP, "required");
    if (reqP != NULL)
    {
      corTreeChildRemove(schemaP, reqP);
      corTreeChildAdd(outP, reqP);
    }
  }

  if (exampleP != NULL)
  {
    exampleP->name = (char*) "example";
    corTreeChildAdd(outP, exampleP);
  }
  else
    corTreeChildAdd(outP, corTreeString(allocP, "example", "none published (normalized NGSI-LD) - build it from the attributes"));

  corTreeChildAdd(outP, corTreeString(allocP, "note", "The example is NGSI-LD, normalized, with the model's @context. Give the user's entities the model's type and attribute names; keep its @context (or full IRIs) so others understand them; create_entities after the user's yes"));
  return shapeResult(allocP, outP, false);
}
