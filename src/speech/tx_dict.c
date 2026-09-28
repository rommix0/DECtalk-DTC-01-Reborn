/* The dictionaries (REFERENCE.md s7.2, s15.9, s15.25): the user dictionary, a 32-bucket hash table in RAM that
 * DT_DICT fills; the built-in dictionary, a trie in ROM (0x20000-0x3fe81, 6,508 words); the suffix-stripping lookup
 * in front of both; and pronounce_dictionary_word, which speaks a hit.
 *
 * A user entry is {next, name length, name, NUL, substitution, NUL}. Its substitution is phonemic text, spoken by
 * parse_phonemic_text. A built-in hit is a string of symbol codes (phonemes, stress, boundaries), the last one with
 * bit 7 set.
 *
 * The trie has one root per first character (dict_root: three pointers per character, 0x20 to 0x7a). A node
 * is a sorted list of characters, the last one with bit 7 set, and a parallel list of 16-bit words. A word is either
 * the distance to the child node, in list entries (the same count for both lists), or, with bit 15 set, the offset
 * of a pronunciation. The character 0x7f stands for the end of the word.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "tx_text.h"
#include "tx_rom.h"

#define SYM_STRESS1 0x39                /* ' primary stress */
#define SYM_VERB 0x40                   /* ) in a pronunciation */
#define SYM_SPACE 0x3e                  /* word boundary */
#define SYM_T 0x2f
#define SYM_D 0x30
#define FEAT_VOICED 0x800u              /* LTS feature bits of the last symbol */
#define FEAT_SIBILANT 0x400000u
#define CLASS_VOWEL 0x04                /* char_class bit */
#define TRIE_END 0x7f                   /* the end-of-word character in the trie */

dict_entry_t *dict_user_table[DICT_NBUCKET];
int16_t dict_user_count;
const char *dict_user_text;
int32_t dict_hits;

static uint32_t features(int32_t sym) { return lts_code_features[sym]; }

/* the hash of a name: the sum of its characters, capitals counted as small letters */
static int32_t fold(int8_t c) { return c >= 'A' && c <= 'Z' ? c + 0x20 : c; }

/* 0xfb10: empty the user dictionary (RIS, power-up) */
void dict_hash_clear_all(void)
{
    dict_entry_t **b = dict_user_table, *e, *next;
    dict_user_count = 0;
    while (b < dict_user_table + DICT_NBUCKET) {
        e = *b;
        *b++ = NULL;
        for (; e; e = next) {
            next = e->next;
            free(e);
        }
    }
}

/* 0xfb56: the substitution for the word p..last, or NULL. A capital in the name matches only a capital; a small
 * letter matches either. */
const char *dict_hash_lookup(const char *p, const char *last)
{
    const char *s = p, *t;
    uint32_t sum = 0;
    int8_t len;
    const dict_entry_t *e;
    int32_t c, d;

    while (s <= last) sum += (uint32_t)fold((int8_t)*s++);
    len = (int8_t)(s - p);
    for (e = dict_user_table[sum & 0x1f]; e; e = e->next) {
        if (e->len != len) continue;
        s = p;
        t = e->text;
        do {
            c = (int8_t)*s++;
            d = (int8_t)*t++;
            if (d >= 'a' && d <= 'z' && c >= 'A' && c <= 'Z') c += 0x20;
            if (c != d) break;
        } while (s <= last);
        if (c == d && *t++ == 0) return t;
    }
    return NULL;
}

/* 0xfc36: DT_DICT. Removes the entry with exactly this name, then adds name -> subst at the end of its bucket
 * unless subst is empty. Returns the new entry, or NULL (deleted only, or no memory). */
dict_entry_t *dict_hash_insert_or_delete(const char *name, const char *subst)
{
    const char *s = name, *t;
    uint32_t sum = 0;
    int8_t len;
    dict_entry_t **link, *e;
    char *q;

    while (*s) sum += (uint32_t)fold((int8_t)*s++);
    len = (int8_t)(s - name);
    sum &= 0x1f;
    for (link = &dict_user_table[sum]; (e = *link) != NULL; link = &e->next) {
        if (e->len != len) continue;
        for (s = name, t = e->text; *s++ == *t; )
            if (*t++ == 0) {                    /* the same name: delete it */
                dict_user_count--;
                *link = e->next;
                free(e);
                goto add;
            }
    }
add:
    if (!subst || !*subst) return NULL;
    for (link = &dict_user_table[sum]; *link; link = &(*link)->next) {}
    /* the ROM asks for strlen(subst) + strlen(name) + 8 bytes */
    e = (dict_entry_t *)malloc(offsetof(dict_entry_t, text) + strlen(subst) + strlen(name) + 2);
    if (e) {
        e->len = len;
        q = e->text;
        s = name;
        while ((*q++ = *s++) != 0) {}
        strcpy(q, subst);
        e->next = *link;
        *link = e;
        dict_user_count++;
    }
    return e;
}

/* 0x65ee: does p..last hold a vowel letter? */
static int stem_has_vowel(const char *p, const char *last)
{
    for (; p <= last; p++)
        if (char_class[(int8_t)*p] & CLASS_VOWEL) return 1;
    return 0;
}

/* 0x68fc: look the word p..last up, the user dictionary first. Returns the built-in pronunciation, DICT_USER_HIT
 * (the substitution is then in dict_user_text), or NULL. */
const uint8_t *lookup_word(const char *p, const char *last)
{
    const char *s;
    const dict_root_t *root;
    const int8_t *chars;                /* the node's character list */
    const int16_t *links;               /* and its link words, at the same index */
    int32_t c;
    int16_t first = (int8_t)*p;

    if (dict_user_count > 0) {
        dict_user_text = dict_hash_lookup(p, last);
        if (dict_user_text) return DICT_USER_HIT;
    }
    if (first < dict_first || first > dict_last) return NULL;
    root = &dict_root[first - dict_first];
    chars = root->chars;
    links = root->links;
    for (s = p + 1;;) {
        c = s > last ? TRIE_END : (int8_t)*s++;
        while (c > (*chars & 0x7f) && !(*chars & 0x80)) {
            chars++;
            links++;
        }
        if (c != (*chars & 0x7f)) return NULL;
        if (c == TRIE_END) break;
        if (*links & 0x8000) {                  /* a pronunciation: only if the word ends here */
            if (s > last) break;
            return NULL;
        }
        c = *links;                             /* the distance to the child node */
        chars += c;
        links += c;
    }
    dict_hits++;
    /* a first character without words ("$") has a stub root whose end-of-word entry gives base 0 + offset 0: the
     * address 0, "not found", though the hit is counted (gen_tx_rom.py checks that its offset is 0) */
    return root->prons ? root->prons + (*links & 0x7fff) : NULL;
}

/* 0x65c8: lookup_word on the word p..end-1, then on its stem without -ed, -ing, -s, -es or -ly (restoring a final e
 * or y, or undoing a doubled consonant). *suffix: 0 none, 1 -ly, 2 -ing, 3 -s, 5 -es, 6 -ed; it is set by the last
 * suffix tried, also on a miss, and left alone when the word is empty. */
const uint8_t *lookup_word_with_suffix_stripping(char *p, char *end, int16_t *suffix)
{
    char *l = end - 1;                          /* the last letter */
    const uint8_t *r;
    int32_t suf;

    if (p > l) return NULL;
    suf = 0;
    r = lookup_word(p, l);
    if (!r) {
        switch (*l) {
        case 'd':                               /* -ed: hoped, walked, carried, hopped */
            if (l[-1] != 'e' || !stem_has_vowel(p, l - 2)) break;
            r = lookup_word(p, l - 1);
            if (!r) r = lookup_word(p, l - 2);
            if (!r && l[-2] == 'i' && stem_has_vowel(p, l - 3)) {
                l[-2] = 'y';
                r = lookup_word(p, l - 2);
                l[-2] = 'i';
            }
            if (!r && l[-2] == l[-3]) r = lookup_word(p, l - 3);
            suf = 6;
            break;
        case 'g':                               /* -ing: hoping, going, hopping */
            if (!stem_has_vowel(p, l - 3) || l[-1] != 'n' || l[-2] != 'i') break;
            l[-2] = 'e';
            r = lookup_word(p, l - 2);
            l[-2] = 'i';
            if (!r) r = lookup_word(p, l - 3);
            if (!r && l[-3] == l[-4]) r = lookup_word(p, l - 4);
            suf = 2;
            break;
        case 's':                               /* -s, -es, -ies; not -ss */
            if (l[-1] == 's' || !stem_has_vowel(p, l - 2)) break;
            suf = 3;
            r = lookup_word(p, l - 1);
            if (r || !stem_has_vowel(p, l - 3) || l[-1] != 'e') break;
            suf = 5;
            r = lookup_word(p, l - 2);
            if (r || l[-2] != 'i' || !stem_has_vowel(p, l - 3)) break;
            suf = 3;
            l[-2] = 'y';
            r = lookup_word(p, l - 2);
            l[-2] = 'i';
            break;
        case 'y':                               /* -ly */
            if (l[-1] != 'l' || !stem_has_vowel(p, l - 2)) break;
            suf = 1;
            r = lookup_word(p, l - 2);
            break;
        }
    }
    *suffix = (int16_t)suf;
    return r;
}

/* the -s tail after sym: ix z after a sibilant, s after a voiceless sound, else dflt */
static const int8_t *plural_tail(int32_t sym, const int8_t *dflt)
{
    if (features(sym) & FEAT_SIBILANT) return tail_ix_z;
    if (!(features(sym) & FEAT_VOICED)) return tail_s;
    return dflt;
}

/* 0x62d8: speak the word p..end-1 from a dictionary, trying it without a final 's too. Sends the pronunciation, the
 * stripped suffix's tail, the possessive's tail and a word boundary to clause_putsym. Returns 1, or 0 (not found:
 * the caller then tries the letter-to-sound rules). */
int pronounce_dictionary_word(char *p, char *end)
{
    int16_t possessive = 0, suffix;
    const uint8_t *r;
    const int8_t *tail;
    int32_t sym = 0;
    char *e = end;

    r = lookup_word_with_suffix_stripping(p, e, &suffix);
    if (!r && *--e == 's' && *--e == '\'') {
        possessive = 1;
        r = lookup_word_with_suffix_stripping(p, e, &suffix);
    }
    if (!r) return 0;

    if (r == DICT_USER_HIT) {
        char in_comment = 0, in_dv = in_comment;
        sym = parse_phonemic_text(dict_user_text, 1, &in_dv, &in_comment, suffix);
    } else {
        do {
            sym = (int8_t)*r & 0x7f;
            if (sym == SYM_STRESS1) {
                prev_word_stressed = 1;
            } else if (sym == SYM_VERB) {
                if (prev_word_stressed < 0) {
                    sym = SYM_SPACE;
                } else {
                    switch (suffix) {
                    case 2: case 6: case 7: case 8: /* -ing, -ed (7 and 8 never occur) */
                        sym = SYM_SPACE;
                        break;
                    default:
                        break;
                    }
                }
            }
            clause_putsym(sym, 0);
        } while (!((int8_t)*r++ & 0x80));
    }

    tail = dict_tails[suffix];
    if (tail) {
        switch (suffix) {
        case 1: case 2:                         /* l iy, ih nx */
            break;
        case 3: case 5:                         /* z, s or ix z (5 keeps ix z after a voiced sound) */
            tail = plural_tail(sym, tail);
            break;
        case 6:                                 /* d, t or ix d */
            if (sym == SYM_T || sym == SYM_D) tail = tail_ix_d;
            else if (!(features(sym) & FEAT_VOICED)) tail = tail_t;
            else tail = tail_d;
            break;
        default:
            panic(dict_illegal_suffix);
        }
        for (; *tail; tail++) {
            sym = *tail;
            clause_putsym(sym, 0);
        }
    }
    if (possessive) {
        for (tail = plural_tail(sym, tail_z); *tail; tail++) {
            sym = *tail;
            clause_putsym(sym, 0);
        }
    }
    clause_putsym(SYM_SPACE, 0);
    return 1;
}
