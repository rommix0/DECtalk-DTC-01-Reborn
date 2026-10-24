/* ltslog - how words find their phonemes, for the explainer video (videos/PLAN.md, Task 3). For each word: whether the
 * built-in dictionary has it, every letter-to-sound rule that fires while it is spoken (tx_lts.c, REFERENCE.md s15.10)
 * with the rule list after it, and the phonemes that come out. The speech engine (src/speech) runs with every library
 * fix off.
 *
 * usage: ltslog out.tsv word ...      (lower-case words)
 *
 * Output, one record per line, tab-separated:
 *   W word dict pron        dict: 1 if the built-in dictionary has the word exactly, pron its phonemic text ("-" if not)
 *   R word rule pos codes   a rule (1-373) matched at list position pos and was applied; codes: the LTS list after it,
 *                           comma-separated (phoneme codes, letter codes 70-79, 61 '*', 62 the word boundary)
 *   X word phonemes         the phoneme log for the word (ConvertToPhonemes)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "tx_text.h"

static FILE *lf;
static const char *word;

static void on_rule(int rule, int pos)
{
    const lts_node_t *n;
    fprintf(lf, "R\t%s\t%d\t%d\t", word, rule, pos);
    for (n = lts_hdr.next; n && n != &lts_nil; n = n->next) fprintf(lf, n == lts_hdr.next ? "%d" : ",%d", n->code);
    fprintf(lf, "\n");
}

int main(int argc, char **argv)
{
    static int16_t pcm[4096];
    static char out[4096];
    int i, k;
    if (argc < 3) {
        fprintf(stderr, "usage: ltslog out.tsv word ...\n");
        return 2;
    }
    lf = fopen(argv[1], "w");
    if (!lf) {
        perror(argv[1]);
        return 1;
    }
    if (!engine_init(0, NULL)) return 1;
    while (engine_run(pcm, 4096) == 4096) {}   /* whatever the boot queues */
    lts_rule_hook = on_rule;
    for (i = 2; i < argc; i++) {
        word = argv[i];
        k = engine_dict_builtin(word, out, sizeof out);
        fprintf(lf, "W\t%s\t%d\t%s\n", word, k, k && out[0] ? out : "-");
        out[0] = 0;
        engine_convert_begin(1);
        engine_write(word, (int)strlen(word));
        engine_write("\v", 1);                  /* CTRL-K: the clause ends here */
        for (k = 0; k < 100000; k++)
            if (engine_run(pcm, 4096) < 4096 && engine_quiet()) break;
        engine_convert_end(out, sizeof out);
        fprintf(lf, "X\t%s\t%s\n", word, out);
        out[0] = 0;
    }
    fclose(lf);
    engine_shutdown();
    return 0;
}
