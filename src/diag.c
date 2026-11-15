#include "diag.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

bool diag_on;
bool diag_show_all;

#define MAX_ENTRIES 600

typedef struct {
    char cat[32];
    char name[112];
    int  n;
} Entry;

static Entry           g_e[MAX_ENTRIES];
static int             g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void diag_note(const char *cat, const char *name) {
    if (!diag_on) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_e[i].cat, cat) && !strncmp(g_e[i].name, name, sizeof g_e[i].name - 1)) {
            g_e[i].n++;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    if (g_n < MAX_ENTRIES) {
        snprintf(g_e[g_n].cat, sizeof g_e[g_n].cat, "%s", cat);
        snprintf(g_e[g_n].name, sizeof g_e[g_n].name, "%s", name);
        g_e[g_n].n = 1;
        g_n++;
    }
    pthread_mutex_unlock(&g_mu);
}

static int cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int c = strcmp(x->cat, y->cat);
    return c ? c : y->n - x->n;
}

void diag_report(FILE *f) {
    qsort(g_e, (size_t)g_n, sizeof(Entry), cmp);
    const char *last = "";
    int         shown = 0, hidden_n = 0, hidden_occ = 0;
    for (int i = 0; i <= g_n; i++) {
        bool new_cat = i == g_n || strcmp(last, g_e[i].cat);
        if (new_cat) {
            if (hidden_n) fprintf(f, "         ... e mais %d itens (%d ocorrências). Use --all para ver tudo.\n", hidden_n, hidden_occ);
            shown = hidden_n = hidden_occ = 0;
            if (i == g_n) break;
            fprintf(f, "\n[%s]\n", g_e[i].cat);
            last = g_e[i].cat;
        }
        if (!diag_show_all && shown >= 25) { hidden_n++; hidden_occ += g_e[i].n; continue; }
        fprintf(f, "  %5dx  %s\n", g_e[i].n, g_e[i].name);
        shown++;
    }
}
