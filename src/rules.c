//
// FILE            rules.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corCep's rules - list_rules (the Rule entities of the rules tenant, with their statistics) and dry_run_rule (corCep's
// POST /dry-run: what a rule would do with events made up for it, nothing written). Writing a rule is c³'s, after
// the user's yes.
//
#include <stdio.h>                               // snprintf
#include <string.h>                              // strlen, memcpy, strcasestr

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corJson/CorJson.h"                     // CorJson
#include "corJson/corJsonCreate.h"               // corJsonCreate
#include "corJson/corJsonParse.h"                // corJsonParse
#include "corRest/corRestClient.h"               // corRestClientMulti*
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corMcp.h"



#define DRY_RUN_TIMEOUT_MS  30000
#define DRY_RUN_MAX         (8 * 1024 * 1024)



static const char* str(CorNode* objectP, const char* name)
{
  CorNode* nP = (objectP != NULL) ? corTreeLookup(objectP, name) : NULL;
  return ((nP != NULL) && (nP->type == CorString)) ? nP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// listRules - GET /entities?type=Rule in the rules tenant: each rule, what it watches and does, its statistics
//
CorNode* listRules(CorAlloc* allocP, CorNode* argsP)
{
  McpResponse resp;
  const char* match  = str(argsP, "match");
  const char* tenant = mcpSession.tenant;

  mcpSession.tenant = mcpSession.rulesTenant;    // the one tool call that reads another tenant: corCep's, not the model's choice
  mcpBackend->request(allocP, CorVerbGet, "/entities", "type=Rule&options=keyValues&limit=1000", NULL, &resp);
  mcpSession.tenant = tenant;

  if (resp.status == 0)
    return shapeBrokerError(allocP, &resp);
  if ((resp.status == 404) || ((resp.status >= 200) && (resp.status <= 299) && ((resp.body == NULL) || (resp.body->type != CorArray) || (resp.body->value.head == NULL))))
  {
    char b[256];
    snprintf(b, sizeof(b), "no Rule entity in the tenant '%s' - no corCep rules here, or corCep runs with another --rules-tenant (corMcp --rules-tenant)", mcpSession.rulesTenant);
    CorNode* outP = corTreeObject(allocP, NULL);
    corTreeChildAdd(outP, corTreeInteger(allocP, "rules", 0));
    corTreeChildAdd(outP, corTreeString(allocP, "note", corAllocStrdup(allocP, b)));
    return shapeResult(allocP, outP, false);
  }
  if ((resp.status < 200) || (resp.status > 299))
    return shapeBrokerError(allocP, &resp);

  CorNode* outP  = corTreeObject(allocP, NULL);
  CorNode* listP = corTreeArray(allocP, "list");
  int      n     = 0;
  CorNode* next;

  for (CorNode* rP = resp.body->value.head; rP != NULL; rP = next)
  {
    next = rP->next;
    if (match != NULL)
    {
      const char* id   = str(rP, "id");
      const char* name = str(rP, "name");
      if (((id == NULL) || (strcasestr(id, match) == NULL)) && ((name == NULL) || (strcasestr(name, match) == NULL)))
        continue;
    }
    corTreeChildAdd(listP, rP);
    ++n;
  }

  corTreeChildAdd(outP, corTreeInteger(allocP, "rules", n));
  corTreeChildAdd(outP, corTreeString(allocP, "tenant", mcpSession.rulesTenant));
  corTreeChildAdd(outP, listP);
  corTreeChildAdd(outP, corTreeString(allocP, "note", "keyValues: when/then/else are the rule; fired, firedElse, firedAt, lastError (with observedAt) are corCep's statistics; enabled false: not run"));
  return shapeResult(allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// dryRunRule - POST <cep>/dry-run with the user's token: the replay's fires, as corCep gives them
//
CorNode* dryRunRule(CorAlloc* allocP, CorNode* argsP)
{
  if (mcpSession.cep == NULL)
    return shapeError(allocP, "corMcp has no corCep to ask (--cep http://host:7602) - the rule cannot be replayed here; describe what it would do from its text, and say it is not tested");
  if ((argsP == NULL) || (corTreeLookup(argsP, "rules") == NULL) || (corTreeLookup(argsP, "events") == NULL))
    return shapeError(allocP, "'rules' (Rule entities, keyValues: when and then as {\"json\": ...}) and 'events' ([{at, entity, type, attribute, value}]) are required");

  char url[1024];
  snprintf(url, sizeof(url), "%s/dry-run", mcpSession.cep);

  char*            body   = shapeRender(allocP, argsP);
  CorRestKeyValue  headers[3];
  int              nh     = 0;
  char*            auth   = NULL;

  headers[nh].key = (char*) "Content-Type"; headers[nh].value = (char*) "application/json"; ++nh;
  headers[nh].key = (char*) "Accept";       headers[nh].value = (char*) "application/json"; ++nh;
  if (mcpSession.token != NULL)
  {
    int alen = strlen(mcpSession.token) + 8;
    auth     = corAlloc(allocP, alen);
    snprintf(auth, alen, "Bearer %s", mcpSession.token);
    headers[nh].key = (char*) "Authorization"; headers[nh].value = auth; ++nh;
  }

  CorRestClientMulti* multiP = corRestClientMultiCreate(1);
  if (multiP == NULL)
    return shapeError(allocP, "out of memory");
  corRestClientMultiMaxResponse(multiP, DRY_RUN_MAX);
  if (corRestClientMultiAdd(multiP, CorVerbPost, url, headers, nh, body, strlen(body), NULL, NULL) < 0)
  {
    corRestClientMultiDestroy(multiP);
    return shapeError(allocP, "the URL %s could not be used", url);
  }
  corRestClientMultiPerform(multiP, DRY_RUN_TIMEOUT_MS);

  CorRestClientResponse* rP = corRestClientMultiResponse(multiP, 0);
  int                    status = ((rP != NULL) && (rP->error == CORR_OK)) ? rP->statusCode : 0;
  CorNode*               outP   = NULL;
  char*                  text   = NULL;

  if ((rP != NULL) && (rP->body != NULL) && (rP->bodyLen > 0))
  {
    text = corAlloc(allocP, rP->bodyLen + 1);
    memcpy(text, rP->body, rP->bodyLen);
    text[rP->bodyLen] = 0;
    char*   copy = corAlloc(allocP, rP->bodyLen + 1);
    CorJson json;
    memcpy(copy, text, rP->bodyLen + 1);
    corJsonCreate(&json, allocP);
    outP = corJsonParse(&json, copy);
  }
  corRestClientMultiDestroy(multiP);

  if (status == 0)
    return shapeError(allocP, "corCep at %s did not answer", mcpSession.cep);
  if (status == 404)
    return shapeError(allocP, "corCep's dry run is off (it runs without --jwks, or it is coraine's cep.so, which has none) - the rule cannot be replayed; say it is not tested");
  if (status == 401)
    return shapeError(allocP, "corCep did not take the user's token for its dry run (401: none, expired, or not signed by its --jwks keys)");
  if (status == 403)
    return shapeError(allocP, "the user's token lacks corCep's editor role (--editor-role, default ngsild-write) - 403");
  if ((status < 200) || (status > 299) || (outP == NULL))
    return shapeError(allocP, "corCep answered %d: %s", status, (text != NULL) ? text : "");

  corTreeChildAdd(outP, corTreeString(allocP, "note", "nothing was written. fires: each time a rule was done - fired true with its actions, or false with why (the if that did not hold); leftOut: rules that do not parse, and why. Actions are not applied: one rule does not see another's"));
  return shapeResult(allocP, outP, false);
}
