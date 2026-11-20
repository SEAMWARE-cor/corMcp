//
// FILE            docs.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The documentation tools - search_docs and read_doc, over the Markdown files of the directories --docs names
// (dir1:dir2: coraine's doc/, a knowledge base), and the Markdown files it names (a CHANGELOG.md). A file is cut in sections at its headings; a search ranks the
// sections by the words of the question (in the heading weighs more, every word found weighs most), and
// read_doc returns one section with its subsections - so the model reads what it needs, not 240,000 words.
//
// The files are read once, at the first call.
//
#include <ctype.h>                               // tolower, isalnum
#include <dirent.h>                              // opendir, readdir
#include <stdio.h>                               // snprintf, fopen
#include <stdlib.h>                              // malloc, free, qsort
#include <string.h>                              // strlen, strstr, strncmp
#include <sys/stat.h>                            // stat

#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/corAllocStrdup.h"             // corAllocStrdup
#include "corTree/corTreeBuilder.h"              // corTreeObject, corTreeString, ...
#include "corTree/corTreeLookup.h"               // corTreeLookup

#include "corMcp.h"



#define FILES_MAX       256
#define SECTIONS_MAX    8192
#define TERMS_MAX       12
#define HITS_MAX        8
#define READ_MAX        16000                    // characters of a section read_doc returns at once



// -----------------------------------------------------------------------------
//
// DocFile, Section
//
typedef struct DocFile
{
  char*  name;                                   // as the model names it: performance.md (or dir/name when two dirs have it)
  char*  dir;                                    // the --docs directory it came from
  char*  text;
  char*  lower;                                  // text in lower case, for the search
  int    len;
} DocFile;

typedef struct Section
{
  int    file;
  int    level;                                  // 1-6: the heading's #s
  int    start;                                  // the heading line's offset
  int    end;                                    // the next heading's offset (of any level)
  int    endAll;                                 // the end of its subsections too: the next heading of its level or above
  char*  title;
  int    parent;                                 // the enclosing section, -1: none
} Section;

static DocFile  files[FILES_MAX];
static int      nFiles    = 0;
static Section  sections[SECTIONS_MAX];
static int      nSections = 0;
static bool     loaded    = false;



// -----------------------------------------------------------------------------
//
// fileRead - a whole file, zero-terminated (malloc'ed); NULL: not readable
//
static char* fileRead(const char* path, int* lenP)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  fseek(fP, 0, SEEK_END);
  long size = ftell(fP);
  fseek(fP, 0, SEEK_SET);
  if ((size < 0) || (size > 16 * 1024 * 1024))
  {
    fclose(fP);
    return NULL;
  }

  char* buf = malloc(size + 1);
  if (buf == NULL)
  {
    fclose(fP);
    return NULL;
  }
  size_t n = fread(buf, 1, size, fP);
  fclose(fP);
  buf[n] = 0;
  *lenP  = (int) n;
  return buf;
}



// -----------------------------------------------------------------------------
//
// sectionsOf - the file's headings (outside ``` blocks) as sections
//
static void sectionsOf(int fx)
{
  DocFile* dP      = &files[fx];
  bool     fenced  = false;
  int      first   = nSections;
  int      stack[7] = { -1, -1, -1, -1, -1, -1, -1 };

  for (int off = 0; off < dP->len; )
  {
    char* line = &dP->text[off];
    char* nl   = strchr(line, '\n');
    int   llen = (nl != NULL) ? (int) (nl - line) : (int) strlen(line);

    if (strncmp(line, "```", 3) == 0)
      fenced = !fenced;
    else if ((fenced == false) && (line[0] == '#'))
    {
      int level = 0;
      while ((level < llen) && (line[level] == '#'))
        ++level;
      if ((level <= 6) && (level < llen) && (line[level] == ' ') && (nSections < SECTIONS_MAX))
      {
        Section* sP = &sections[nSections];
        sP->file   = fx;
        sP->level  = level;
        sP->start  = off;
        sP->title  = strndup(&line[level + 1], llen - level - 1);
        sP->parent = -1;
        for (int l = level - 1; l >= 1; l--)
        {
          if (stack[l] != -1)
          {
            sP->parent = stack[l];
            break;
          }
        }
        stack[level] = nSections;
        for (int l = level + 1; l <= 6; l++)
          stack[l] = -1;
        ++nSections;
      }
    }

    off += llen + 1;
  }

  //
  // The text before the first heading is a section of its own (the file's preamble), when there is any
  //
  for (int ix = first; ix < nSections; ix++)
  {
    sections[ix].end    = (ix + 1 < nSections) ? sections[ix + 1].start : dP->len;
    sections[ix].endAll = dP->len;
    for (int jx = ix + 1; jx < nSections; jx++)
    {
      if (sections[jx].level <= sections[ix].level)
      {
        sections[ix].endAll = sections[jx].start;
        break;
      }
    }
  }

  if ((nSections == first) && (nSections < SECTIONS_MAX))     // no heading: the file is one section
  {
    Section* sP = &sections[nSections++];
    sP->file = fx; sP->level = 1; sP->start = 0; sP->end = dP->len; sP->endAll = dP->len; sP->title = strdup(dP->name); sP->parent = -1;
  }
}



// -----------------------------------------------------------------------------
//
// docsLoad - the Markdown files of every --docs directory (and one level of subdirectories)
//
static void fileLoad(const char* full, const char* name, const char* dir)
{
  int   len;
  char* text = fileRead(full, &len);
  if ((text == NULL) || (nFiles >= FILES_MAX))
  {
    free(text);
    return;
  }

  DocFile* dP = &files[nFiles];
  dP->name  = strdup(name);
  dP->dir   = strdup(dir);
  dP->text  = text;
  dP->len   = len;
  dP->lower = malloc(len + 1);
  for (int ix = 0; ix <= len; ix++)
    dP->lower[ix] = tolower((unsigned char) text[ix]);

  sectionsOf(nFiles);
  ++nFiles;
}

static void dirLoad(const char* dir, const char* sub)
{
  char path[2048];
  snprintf(path, sizeof(path), "%s%s%s", dir, (sub != NULL) ? "/" : "", (sub != NULL) ? sub : "");

  DIR* dirP = opendir(path);
  if (dirP == NULL)
    return;

  struct dirent* eP;
  while (((eP = readdir(dirP)) != NULL) && (nFiles < FILES_MAX))
  {
    if (eP->d_name[0] == '.')
      continue;

    char        full[4096];
    struct stat st;
    snprintf(full, sizeof(full), "%s/%s", path, eP->d_name);
    if (stat(full, &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode))
    {
      if ((sub == NULL) && (strcmp(eP->d_name, "images") != 0) && (strcmp(eP->d_name, "history") != 0))
        dirLoad(dir, eP->d_name);
      continue;
    }

    int nlen = strlen(eP->d_name);
    if ((nlen < 4) || (strcmp(&eP->d_name[nlen - 3], ".md") != 0))
      continue;

    char name[1024];
    snprintf(name, sizeof(name), "%s%s%s", (sub != NULL) ? sub : "", (sub != NULL) ? "/" : "", eP->d_name);
    fileLoad(full, name, dir);
  }
  closedir(dirP);
}

static void docsLoad(void)
{
  if ((loaded == true) || (mcpSession.docs == NULL))
    return;
  loaded = true;

  char* dirs = strdup(mcpSession.docs);
  char* save = NULL;
  for (char* d = strtok_r(dirs, ":", &save); d != NULL; d = strtok_r(NULL, ":", &save))
  {
    int         before = nFiles;
    struct stat st;
    int         dlen   = strlen(d);

    if ((stat(d, &st) == 0) && S_ISREG(st.st_mode) && (dlen > 3) && (strcmp(&d[dlen - 3], ".md") == 0))
    {
      const char* base = strrchr(d, '/');
      fileLoad(d, (base != NULL) ? base + 1 : d, d);    // a file: CHANGELOG.md
    }
    else
      dirLoad(d, NULL);

    //
    // A name another directory has already: dir-qualified, so read_doc can tell them apart
    //
    for (int ix = before; ix < nFiles; ix++)
    {
      for (int jx = 0; jx < before; jx++)
      {
        if (strcmp(files[ix].name, files[jx].name) == 0)
        {
          const char* base = strrchr(d, '/');
          char        q[1200];
          snprintf(q, sizeof(q), "%s/%s", (base != NULL) ? base + 1 : d, files[ix].name);
          free(files[ix].name);
          files[ix].name = strdup(q);
          break;
        }
      }
    }
  }
  free(dirs);

  //
  // The files in name order (a directory lists them as it likes); the sections' file numbers follow
  //
  int order[FILES_MAX];
  for (int ix = 0; ix < nFiles; ix++)
    order[ix] = ix;
  for (int ix = 1; ix < nFiles; ix++)
  {
    for (int jx = ix; (jx > 0) && (strcmp(files[order[jx - 1]].name, files[order[jx]].name) > 0); jx--)
    {
      int t = order[jx]; order[jx] = order[jx - 1]; order[jx - 1] = t;
    }
  }
  DocFile sorted[FILES_MAX];
  int     newIx[FILES_MAX];
  for (int ix = 0; ix < nFiles; ix++)
  {
    sorted[ix]       = files[order[ix]];
    newIx[order[ix]] = ix;
  }
  memcpy(files, sorted, nFiles * sizeof(DocFile));
  for (int sx = 0; sx < nSections; sx++)
    sections[sx].file = newIx[sections[sx].file];
}



// -----------------------------------------------------------------------------
//
// countIn - how many times 'term' is in [from, to) of 's'
//
static int countIn(const char* s, int from, int to, const char* term, int cap)
{
  int         n    = 0;
  int         tlen = strlen(term);
  const char* p    = s + from;
  const char* end  = s + to;

  while ((n < cap) && (p < end))
  {
    const char* hit = strstr(p, term);
    if ((hit == NULL) || (hit + tlen > end))
      break;
    ++n;
    p = hit + tlen;
  }
  return n;
}



// -----------------------------------------------------------------------------
//
// Hit - a section and its score
//
typedef struct Hit
{
  int     section;
  double  score;
} Hit;

static int hitCompare(const void* a, const void* b)
{
  double d = ((const Hit*) b)->score - ((const Hit*) a)->score;
  return (d > 0) ? 1 : (d < 0) ? -1 : 0;
}

static bool stopWord(const char* w)
{
  static const char* stop[] = { "the", "and", "for", "how", "what", "why", "which", "does", "with", "can", "are", "its", "from",
                                "this", "that", "into", "when", "not", "does", "should", "would", "about", "there", "have", "has",
                                "a", "an", "of", "to", "in", "on", "is", "it", "be", "do", "or", "by", "at", "as", "my", "i", NULL };
  for (int ix = 0; stop[ix] != NULL; ix++)
  {
    if (strcmp(stop[ix], w) == 0)
      return true;
  }
  return false;
}



// -----------------------------------------------------------------------------
//
// pathOf - "Performance and footprint > RAM > The stored trees"
//
static void pathOf(int sx, McpStr* sP)
{
  int chain[8];
  int n = 0;
  for (int ix = sx; (ix != -1) && (n < 8); ix = sections[ix].parent)
    chain[n++] = ix;
  for (int ix = n - 1; ix >= 0; ix--)
  {
    strAdd(sP, sections[chain[ix]].title);
    if (ix > 0)
      strAdd(sP, " > ");
  }
}



// -----------------------------------------------------------------------------
//
// searchDocs - the sections that best answer 'query'
//
CorNode* searchDocs(CorAlloc* allocP, CorNode* argsP)
{
  CorNode* queryP = (argsP != NULL) ? corTreeLookup(argsP, "query") : NULL;

  if ((queryP == NULL) || (queryP->type != CorString) || (queryP->value.s[0] == 0))
    return shapeError(allocP, "'query' is required: the words to look for, e.g. \"declared index dbValueIndexes\"");
  if (mcpSession.docs == NULL)
    return shapeError(allocP, "no documentation was given to corMcp (--docs)");

  docsLoad();

  //
  // The words of the query, in lower case, without the small ones
  //
  char  terms[TERMS_MAX][64];
  int   nTerms = 0;
  const char* q = queryP->value.s;

  while ((*q != 0) && (nTerms < TERMS_MAX))
  {
    while ((*q != 0) && !isalnum((unsigned char) *q) && (*q != '_') && (*q != '-'))
      ++q;
    int len = 0;
    while ((*q != 0) && (isalnum((unsigned char) *q) || (*q == '_') || (*q == '-')) && (len < 63))
      terms[nTerms][len++] = tolower((unsigned char) *q++);
    while ((*q != 0) && (isalnum((unsigned char) *q) || (*q == '_') || (*q == '-')))
      ++q;
    terms[nTerms][len] = 0;
    if ((len >= 2) && !stopWord(terms[nTerms]))
      ++nTerms;
  }

  if (nTerms == 0)
    return shapeError(allocP, "the query has no word to look for");

  //
  // Every section scored: the words found in it (a word in the heading counts five times); the share of the
  // query's words it has, squared - a section with all of them first
  //
  static Hit hits[SECTIONS_MAX];
  int        nHits = 0;

  for (int sx = 0; sx < nSections; sx++)
  {
    Section* sP     = &sections[sx];
    DocFile* dP     = &files[sP->file];
    double   score  = 0;
    int      found  = 0;
    int      endT   = sP->start;
    while ((endT < sP->end) && (dP->lower[endT] != '\n'))
      ++endT;

    for (int tx = 0; tx < nTerms; tx++)
    {
      int inTitle = countIn(dP->lower, sP->start, endT, terms[tx], 3);
      int inBody  = countIn(dP->lower, endT, sP->end, terms[tx], 10);
      if (inTitle + inBody > 0)
        ++found;
      score += 5 * inTitle + inBody;
    }

    if (found == 0)
      continue;

    double share = (double) found / nTerms;
    hits[nHits].section = sx;
    hits[nHits].score   = score * share * share;
    ++nHits;
  }

  qsort(hits, nHits, sizeof(Hit), hitCompare);

  CorNode* outP  = corTreeObject(allocP, NULL);
  CorNode* listP = corTreeArray(allocP, "sections");

  for (int hx = 0; (hx < nHits) && (hx < HITS_MAX); hx++)
  {
    Section* sP = &sections[hits[hx].section];
    DocFile* dP = &files[sP->file];
    CorNode* hP = corTreeObject(allocP, NULL);
    McpStr   path;

    strInit(&path, allocP, 256);
    pathOf(hits[hx].section, &path);

    corTreeChildAdd(hP, corTreeString(allocP, "file", dP->name));
    corTreeChildAdd(hP, corTreeString(allocP, "section", sP->title));
    corTreeChildAdd(hP, corTreeString(allocP, "path", path.buf));

    //
    // A line that has the query's words: the most of them
    //
    int bestStart = -1;
    int bestLen   = 0;
    int bestN     = 0;
    for (int off = sP->start; off < sP->end; )
    {
      char* nl   = memchr(&dP->lower[off], '\n', sP->end - off);
      int   llen = (nl != NULL) ? (int) (nl - &dP->lower[off]) : (sP->end - off);
      int   n    = 0;
      for (int tx = 0; tx < nTerms; tx++)
        n += (countIn(dP->lower, off, off + llen, terms[tx], 1) > 0);
      if ((n > bestN) && (off != sP->start))
      {
        bestN = n; bestStart = off; bestLen = llen;
      }
      off += llen + 1;
    }
    if (bestStart >= 0)
    {
      if (bestLen > 300)
        bestLen = 300;
      char* snip = corAlloc(allocP, bestLen + 4);
      memcpy(snip, &dP->text[bestStart], bestLen);
      snip[bestLen] = 0;
      corTreeChildAdd(hP, corTreeString(allocP, "line", snip));
    }
    corTreeChildAdd(hP, corTreeInteger(allocP, "characters", sP->endAll - sP->start));
    corTreeChildAdd(listP, hP);
  }

  corTreeChildAdd(outP, corTreeInteger(allocP, "found", nHits));
  corTreeChildAdd(outP, listP);
  if (nHits == 0)
    corTreeChildAdd(outP, corTreeString(allocP, "note", "nothing - other words (an option's name, an endpoint, an error's title) may find it"));
  else
    corTreeChildAdd(outP, corTreeString(allocP, "next", "read_doc { file, section } for a section's text"));

  return shapeResult(allocP, outP, false);
}



// -----------------------------------------------------------------------------
//
// readDoc - a section's text (with its subsections), or a file's headings
//
CorNode* readDoc(CorAlloc* allocP, CorNode* argsP)
{
  CorNode* fileP    = (argsP != NULL) ? corTreeLookup(argsP, "file")    : NULL;
  CorNode* sectionP = (argsP != NULL) ? corTreeLookup(argsP, "section") : NULL;
  CorNode* offsetP  = (argsP != NULL) ? corTreeLookup(argsP, "offset")  : NULL;
  int      offset   = ((offsetP != NULL) && (offsetP->type == CorInt) && (offsetP->value.i > 0)) ? (int) offsetP->value.i : 0;

  if (mcpSession.docs == NULL)
    return shapeError(allocP, "no documentation was given to corMcp (--docs)");

  docsLoad();

  if ((fileP == NULL) || (fileP->type != CorString) || (fileP->value.s[0] == 0))
  {
    CorNode* outP  = corTreeObject(allocP, NULL);
    CorNode* listP = corTreeArray(allocP, "files");
    for (int ix = 0; ix < nFiles; ix++)
      corTreeChildAdd(listP, corTreeString(allocP, NULL, files[ix].name));
    corTreeChildAdd(outP, listP);
    corTreeChildAdd(outP, corTreeString(allocP, "next", "read_doc { file } for a file's headings, { file, section } for a section"));
    return shapeResult(allocP, outP, false);
  }

  int fx = -1;
  for (int ix = 0; (ix < nFiles) && (fx == -1); ix++)
  {
    if (strcmp(files[ix].name, fileP->value.s) == 0)
      fx = ix;
  }
  for (int ix = 0; (ix < nFiles) && (fx == -1); ix++)          // performance (no .md), or the name without a directory
  {
    const char* base = strrchr(files[ix].name, '/');
    base = (base != NULL) ? base + 1 : files[ix].name;
    if ((strcmp(base, fileP->value.s) == 0) || ((strncmp(base, fileP->value.s, strlen(fileP->value.s)) == 0) && (strcmp(base + strlen(fileP->value.s), ".md") == 0)))
      fx = ix;
  }
  if (fx == -1)
    return shapeError(allocP, "no file '%s' - read_doc without arguments lists them", fileP->value.s);

  CorNode* outP = corTreeObject(allocP, NULL);
  corTreeChildAdd(outP, corTreeString(allocP, "file", files[fx].name));

  //
  // No section: the headings
  //
  if ((sectionP == NULL) || (sectionP->type != CorString) || (sectionP->value.s[0] == 0))
  {
    CorNode* listP = corTreeArray(allocP, "sections");
    for (int sx = 0; sx < nSections; sx++)
    {
      if ((sections[sx].file != fx) || (sections[sx].level > 4))
        continue;
      McpStr h;
      strInit(&h, allocP, 128);
      for (int l = 1; l < sections[sx].level; l++)
        strAdd(&h, "  ");
      strAdd(&h, sections[sx].title);
      corTreeChildAdd(listP, corTreeString(allocP, NULL, h.buf));
    }
    corTreeChildAdd(outP, listP);
    return shapeResult(allocP, outP, false);
  }

  //
  // The section: its heading exactly, else the first heading that has the text (any case)
  //
  int sx = -1;
  for (int ix = 0; (ix < nSections) && (sx == -1); ix++)
  {
    if ((sections[ix].file == fx) && (strcmp(sections[ix].title, sectionP->value.s) == 0))
      sx = ix;
  }
  for (int ix = 0; (ix < nSections) && (sx == -1); ix++)
  {
    if ((sections[ix].file == fx) && (strcasestr(sections[ix].title, sectionP->value.s) != NULL))
      sx = ix;
  }
  if (sx == -1)
    return shapeError(allocP, "no section '%s' in %s - read_doc { file } lists its headings", sectionP->value.s, files[fx].name);

  Section* sP    = &sections[sx];
  int      from  = sP->start + offset;
  int      total = sP->endAll - sP->start;

  if (from > sP->endAll)
    from = sP->endAll;

  int len = sP->endAll - from;
  bool cut = false;
  if (len > READ_MAX)
  {
    len = READ_MAX;
    while ((len > 0) && (files[fx].text[from + len] != '\n'))     // at a line's end
      --len;
    if (len == 0)
      len = READ_MAX;
    cut = true;
  }

  char* text = corAlloc(allocP, len + 1);
  memcpy(text, &files[fx].text[from], len);
  text[len] = 0;

  McpStr path;
  strInit(&path, allocP, 256);
  pathOf(sx, &path);

  corTreeChildAdd(outP, corTreeString(allocP, "path", path.buf));
  CorNode* textP = corTreeString(allocP, "text", text);
  corTreeChildAdd(outP, textP);
  if (cut)
  {
    corTreeChildAdd(outP, corTreeInteger(allocP, "nextOffset", offset + len + 1));
    corTreeChildAdd(outP, corTreeInteger(allocP, "characters", total));
  }

  return shapeResult(allocP, outP, false);
}
