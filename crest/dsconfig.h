#ifndef CREST_DS_CONFIG_H
#define CREST_DS_CONFIG_H

/*
 * Build-time data-structure selection.
 *
 * The active structure is chosen in the Makefile:
 *
 *     make all DS=bpt_mass      (default)
 *     make all DS=ht_pugh
 *     make all DS=trie_art
 *     ...any of: ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy
 *                bpt_seq bpt_occ bpt_mass trie_art
 *
 * The Makefile passes -DCREST_DS=<name>; this header turns that token into
 * the include and the namespace. Every structure lives in a namespace named
 * after its file and exposes the same `dict` API, so nothing else changes.
 * No source edits are needed to switch structures.
 */

#ifndef CREST_DS
#define CREST_DS bpt_mass
#endif

#define CREST_DS_STR(x) #x
#define CREST_DS_XSTR(x) CREST_DS_STR(x)

// "<name>.h" -- resolved via -Idatastructures
#include CREST_DS_XSTR(CREST_DS.h)

using namespace CREST_DS;

// Human-readable name for logs ("bpt_mass", ...)
#define CREST_DS_NAME CREST_DS_XSTR(CREST_DS)

#endif // CREST_DS_CONFIG_H
