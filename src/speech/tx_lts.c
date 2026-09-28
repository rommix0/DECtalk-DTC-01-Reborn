/* Letter-to-sound rules: lts_rule_engine 0x6a64 and its helpers (REFERENCE.md s15.10, s15.24).
 *
 * The word becomes a doubly linked list of LTS codes wrapped in word boundaries (62). Then all 373 rules are tried
 * in ROM order, each at every position of the list; a rule matches its right context (which includes the letters it
 * replaces) forwards and its left context backwards, and then replaces the matched items in place. The list that is
 * left is sent to clause_putsym, dropping repeats. This is Hunnicutt's 1976 rule set as DEC compiled it (s14.9).
 *
 * A rule record is a byte stream (lts_rules, tx_rom_lts.c): {match_len, right_len, left_len, right items, left items, rep_count,
 * replacement items}. An item byte < 0x80 is a literal code. A byte >= 0x80 gives n = (int8)b + 0x1C: n < 10 is a
 * test group (an optional literal c | 0x80, then n (class, polarity) pairs against the class masks), n >= 10 lets the
 * next item repeat up to n - 10 times.
 */
#include <stddef.h>
#include "tx_text.h"
#include "tx_rom.h"

#define LTS_SPACE 62                    /* word boundary */
#define LTS_STAR 61                     /* '*' in a result: spoken as a boundary */

lts_node_t lts_pool[LTS_MAXNODE];
int lts_pool_used;
lts_node_t *lts_free;
lts_node_t lts_hdr;
lts_node_t lts_nil = { 0x00ff, 0, NULL, NULL };  /* the bytes at ROM 0x13bc8 */
int16_t lts_nnodes, lts_rule, lts_npos, lts_pos;
int16_t prev_word_stressed;

static uint32_t features(int code) { return lts_code_features[code]; }
static uint32_t class_mask(int cls) { return lts_class_masks[cls]; }

/* 0x7108: a node linked in after prev; NULL when the pool is used up */
static lts_node_t *lts_alloc(lts_node_t *prev)
{
    lts_node_t *n = lts_free;
    if (n) {
        lts_free = n->next;
    } else {
        if (lts_pool_used >= LTS_MAXNODE) return NULL;
        n = &lts_pool[lts_pool_used++];
    }
    n->next = prev->next;
    prev->next = n;
    n->prev = prev;
    if (n->next != &lts_nil) n->next->prev = n;
    return n;
}

/* 0x7150: take n out of the list and give it back; returns the node after it */
static lts_node_t *lts_unlink(lts_node_t *n)
{
    lts_node_t *next = n->next, *prev = n->prev;
    prev->next = next;
    if (next != &lts_nil) next->prev = prev;
    n->next = lts_free;
    lts_free = n;
    return next;
}

/* 0x6c0c: match nitems pattern items at pat against the list from node on, forwards (dir 1) or backwards (dir 0).
 * Returns the byte after the pattern, or NULL. A repeated item is matched greedily. */
static const int8_t *lts_env_match(int dir, int nitems, lts_node_t *node, const int8_t *pat)
{
    int16_t repeat = 0, i = 0, n, j;
    const int8_t *repeat_at = NULL;
    int8_t c;

    for (i = 1; i <= nitems; i++) {
        if (node == &lts_nil) return NULL;
        for (;;) {                              /* one item (after any repeat prefixes) */
            int ok = 1;
            c = *pat;
            if (c >= 0) {                       /* literal */
                ok = c == node->code;
                if (ok) pat++;
            } else {
                n = (int16_t)(c + 0x1c);
                pat++;
                c = *pat;
                if (n >= 10) {                  /* repeat prefix: the next item up to n - 10 times */
                    repeat = (int16_t)(n - 10);
                    repeat_at = pat;
                    continue;
                }
                if (c < 0 && (int8_t)(c + 0x80) != node->code) {
                    pat += 2 * n;               /* to the last byte of the group */
                    ok = 0;
                } else {
                    if (c < 0) { pat++; c = *pat; }
                    for (j = 1; j <= n; j++) {  /* (class, polarity) pairs */
                        int has = (node->feat & class_mask(c)) != 0;
                        pat++;
                        c = *pat;
                        if ((c > 0) != has) {
                            pat += 2 * (n - j); /* to the last byte of the group */
                            ok = 0;
                            break;
                        }
                        pat++;
                        c = *pat;
                    }
                    if (ok) goto matched_group;
                }
            }
            if (!ok) {                          /* no match: fails, unless the item was a repeat (then skip it) */
                if (repeat <= 0) return NULL;
                repeat = 0;
                i++;
                pat++;
                continue;                       /* the next item, same node */
            }
            break;
        }
    matched_group:
        if (--repeat > 0) {                     /* may repeat again: back to the item, same item count */
            pat = repeat_at;
            i--;
        }
        node = dir ? node->next : node->prev;
    }
    return pat;
}

/* 0x6ad4: replace the m items from node on by the rep_count items at rep (its count byte).
 * Returns the node to continue after, or NULL when the pool is used up. */
static lts_node_t *lts_rule_apply(lts_node_t *node, int16_t m, const int8_t *rep)
{
    int8_t nrep = *rep, c;
    int16_t keep = m, del = (int16_t)(m - nrep), ins = (int16_t)(nrep - m), k, n;
    lts_node_t *ret = node, *add;
    if (m > nrep) {
        keep = nrep;
        del = (int16_t)(m - keep);
        ins = (int16_t)(keep - m);
    }
    rep++;
    c = *rep;

    for (k = 0; k < ins; k++) {                 /* new nodes before node */
        add = lts_alloc(node->prev);
        if (!add) return NULL;
        if (c < 0) {
            n = (int16_t)(c + 0x1c);
            rep++;
            c = *rep;
            if (c < 0) {                        /* with a literal: that code */
                c = (int8_t)(c + 0x80);
                add->code = c;
                add->feat = features(c);
                rep++;
                c = *rep;
            } else {                            /* without: a copy of node */
                add->code = node->code;
                add->feat = node->feat;
            }
            while (--n >= 0) {                  /* set or clear feature classes */
                uint32_t mask = class_mask(c);
                rep++;
                c = *rep;
                if (c == 0) add->feat &= ~mask;
                else add->feat |= mask;
                rep++;
                c = *rep;
            }
        } else {
            add->code = c;
            add->feat = features(c);
            rep++;
            c = *rep;
        }
        lts_nnodes++;
        lts_npos++;
        lts_pos++;
    }
    for (k = 0; k < keep; k++) {                /* nodes changed in place */
        if (c < 0) {
            n = (int16_t)(c + 0x1c);
            rep++;
            c = *rep;
            if (c < 0) {
                c = (int8_t)(c + 0x80);
                if (c != node->code) {
                    node->code = c;
                    node->feat = features(c);
                }
                rep++;
                c = *rep;
            }
            while (--n >= 0) {
                uint32_t mask = class_mask(c);
                rep++;
                c = *rep;
                if (c == 0) node->feat &= ~mask;
                else node->feat |= mask;
                rep++;
                c = *rep;
            }
        } else {
            node->code = c;
            node->feat = features(c);
            rep++;
            c = *rep;
        }
        node = node->next;
    }
    for (k = 1; k <= del; k++) {                /* nodes removed */
        node = lts_unlink(node);
        lts_nnodes--;
        lts_npos--;
        ret = node->prev;                       /* NIL's prev is NULL: the ROM then gives up */
        if (k == 1) lts_pos--;
    }
    return ret;
}

/* 0x6a64: speak the word [p, end) by rule. Returns 0 when a character has no LTS code or the pool runs out. */
int lts_rule_engine(const char *p, const char *end)
{
    lts_node_t *node, *pos;
    int16_t off = -1, code, last;
    int8_t match_len, right_len, left_len;
    const int8_t *rec, *pat;            /* the rule's fields; rec[3] on are its items */

    lts_free = NULL;
    lts_pool_used = 0;
    lts_hdr.prev = &lts_nil;
    lts_hdr.next = lts_hdr.prev;
    node = lts_alloc(&lts_hdr);                 /* the pool is empty, so this cannot fail */
    node->code = LTS_SPACE;
    node->feat = features(LTS_SPACE);
    lts_nnodes = 2;
    while (p < end) {
        code = (int8_t)*p++;                    /* signed, as move.b + ext.w */
        if (code < 0x20 || code > 0x7e) return 0;
        code = lts_ascii_map[code];
        if (code == 0xff) return 0;             /* never true: the byte 0xFF is sign-extended to -1 */
        node = lts_alloc(node);
        if (!node) return 0;
        node->code = code;
        node->feat = features(code);
        lts_nnodes++;
    }
    node = lts_alloc(node);
    node->code = LTS_SPACE;
    node->feat = features(LTS_SPACE);
    lts_hdr.next->prev = &lts_nil;

    for (lts_rule = 0; lts_rule < lts_rule_count;) {
        off = (int16_t)(off + lts_rule_deltas[lts_rule]);
        lts_rule++;
        rec = lts_rules + off + 1;              /* the ROM reads the record at off + 1 on (off is -1 for rule 0) */
        match_len = rec[0];
        right_len = rec[1];
        left_len = rec[2];
        lts_npos = (int16_t)(lts_nnodes - match_len + 1);
        pos = lts_hdr.next;
        for (lts_pos = 0; lts_pos < lts_npos; lts_pos++) {
            if (right_len <= 0 || rec[3] < 0 || rec[3] == pos->code) {
                pat = lts_env_match(1, right_len, pos, rec + 3);
                if (pat) pat = lts_env_match(0, left_len, pos->prev, pat);
                if (pat) {
                    pos = lts_rule_apply(pos, match_len, pat);
                    if (!pos) return 0;
                }
            }
            if (pos != &lts_nil) pos = pos->next;
        }
    }

    prev_word_stressed = 1;
    node = lts_hdr.next;
    if (node->code == LTS_SPACE) node = node->next;
    last = 0xff;
    for (; node != &lts_nil; node = node->next) {
        if (node->code == LTS_STAR) node->code = LTS_SPACE;
        if (node->code != last) {
            last = node->code;
            clause_putsym(last, 0);
        }
    }
    if (last != LTS_SPACE) clause_putsym(LTS_SPACE, 0);
    return 1;
}
