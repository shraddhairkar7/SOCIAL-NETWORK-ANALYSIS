/*
 * social_common.h  --  shared code for the sequential and parallel programs.
 *
 * Both programs run the SAME analysis pipeline on the SAME data structure
 * (compressed adjacency lists / CSR).  The only difference is that the
 * parallel build compiles with -DPARALLEL, which switches on the OpenMP
 * pragmas and the parallel connected-components algorithm.
 *
 * Timed pipeline ("Execution time"):
 *   1. build graph   (count degrees, fill, sort, remove duplicates)
 *   2. top users     (degree + top-K selection, wedge count)
 *   3. components    (BFS sequentially / lock-free union-find in parallel)
 *   4. triangles     (triangle counting -> global clustering coefficient)
 *   5. path sampling (BFS from 512 evenly spaced users -> avg path, diameter)
 *
 * Reading the text file from disk is NOT timed (it is identical, disk-bound
 * work in both programs and is the main source of timing noise).
 *
 * To make the reported time stable, the whole pipeline is run once as a
 * warm-up and then REPS times; the run with the MEDIAN total is reported.
 */
#ifndef SOCIAL_COMMON_H
#define SOCIAL_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

#define TOP_K        10
#define PATH_SAMPLES 512
#define REPS         5

#ifdef PARALLEL
#define TITLE "PARALLEL SOCIAL NETWORK"
static inline int fetchInc(int *p) { return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED); }
#else
#define TITLE "SEQUENTIAL SOCIAL NETWORK"
static inline int fetchInc(int *p) { return (*p)++; }
#endif

typedef struct { int n; long *off; int *adj; long long edges; } Graph;
typedef struct { int user, deg; } TopEntry;

typedef struct {
    int n; long long edges;
    TopEntry top[TOP_K]; int limit;
    int components, largest;
    long long triangles; double clustering;
    double avgPath; int diameter;
    double tBuild, tTop, tComp, tTri, tPath, tTotal;
} Result;

static void die(const char *msg) { fprintf(stderr, "%s\n", msg); exit(1); }
static void *xcalloc(size_t n, size_t s) { void *p = calloc(n ? n : 1, s); if (!p) die("Memory allocation failed."); return p; }
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("Memory allocation failed."); return p; }

/* ---------------------------------------------------------------- input */
static int *loadEdges(const char *path, long *mOut, int *nOut)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error opening dataset: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("Read error.");
    buf[sz] = 0; fclose(f);

    long cap = 1024, m = 0; int maxV = -1;
    int *e = xmalloc(cap * 2 * sizeof(int));
    char *p = buf;
    for (;;) {
        char *q; long u = strtol(p, &q, 10); if (q == p) break; p = q;
        long v = strtol(p, &q, 10);          if (q == p) break; p = q;
        if (u < 0 || v < 0) die("Invalid vertex in dataset.");
        if (m == cap) { cap *= 2; e = realloc(e, cap * 2 * sizeof(int)); if (!e) die("Memory allocation failed."); }
        e[2*m] = (int)u; e[2*m+1] = (int)v; m++;
        if (u > maxV) maxV = (int)u;
        if (v > maxV) maxV = (int)v;
    }
    free(buf);
    if (maxV < 0) { fprintf(stderr, "Dataset is empty or invalid.\n"); exit(1); }
    *mOut = m; *nOut = maxV + 1;
    return e;
}

/* ---------------------------------------------------------- 1. build */
static int cmpInt(const void *a, const void *b) { int x = *(const int*)a, y = *(const int*)b; return (x > y) - (x < y); }

static Graph buildGraph(const int *e, long m, int n)
{
    long *cnt = xcalloc((size_t)n + 1, sizeof(long));
    int  *fill = xcalloc(n, sizeof(int));
    int  *deg = xcalloc(n, sizeof(int));

#ifdef PARALLEL
    #pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < m; i++) {
        int u = e[2*i], v = e[2*i+1];
        if (u != v) { fetchInc(&deg[u]); fetchInc(&deg[v]); }
    }
    for (int i = 0; i < n; i++) cnt[i+1] = cnt[i] + deg[i];
    int *adj = xmalloc((size_t)cnt[n] * sizeof(int));

#ifdef PARALLEL
    #pragma omp parallel for schedule(static)
#endif
    for (long i = 0; i < m; i++) {
        int u = e[2*i], v = e[2*i+1];
        if (u != v) {
            adj[cnt[u] + fetchInc(&fill[u])] = v;
            adj[cnt[v] + fetchInc(&fill[v])] = u;
        }
    }

    /* sort every list and drop duplicate relationships */
    int *uniq = xcalloc(n, sizeof(int));
#ifdef PARALLEL
    #pragma omp parallel for schedule(dynamic, 64)
#endif
    for (int v = 0; v < n; v++) {
        int len = (int)(cnt[v+1] - cnt[v]), *a = adj + cnt[v], k = 0;
        qsort(a, len, sizeof(int), cmpInt);
        for (int i = 0; i < len; i++) if (i == 0 || a[i] != a[i-1]) a[k++] = a[i];
        uniq[v] = k;
    }
    long *off = xcalloc((size_t)n + 1, sizeof(long));
    for (int i = 0; i < n; i++) off[i+1] = off[i] + uniq[i];
    int *fin = xmalloc((size_t)off[n] * sizeof(int));
#ifdef PARALLEL
    #pragma omp parallel for schedule(static)
#endif
    for (int v = 0; v < n; v++) memcpy(fin + off[v], adj + cnt[v], (size_t)uniq[v] * sizeof(int));

    free(cnt); free(fill); free(deg); free(adj); free(uniq);
    Graph g = { n, off, fin, off[n] / 2 };
    return g;
}

static void freeGraph(Graph *g) { free(g->off); free(g->adj); }

/* ------------------------------------------------------- 2. top users */
static int better(TopEntry a, TopEntry b) { return a.deg > b.deg || (a.deg == b.deg && a.user < b.user); }

static void topInsert(TopEntry *t, int *cnt, TopEntry x)
{
    int c = *cnt;
    if (c == TOP_K && !better(x, t[c-1])) return;
    int i = (c < TOP_K) ? c++ : c - 1;
    while (i > 0 && better(x, t[i-1])) { t[i] = t[i-1]; i--; }
    t[i] = x; *cnt = c;
}

static long long topUsers(const Graph *g, TopEntry *out, int *limit)
{
    TopEntry best[TOP_K]; int cnt = 0; long long wedges = 0;
#ifdef PARALLEL
    #pragma omp parallel
    {
        TopEntry loc[TOP_K]; int lc = 0;
        #pragma omp for schedule(static) reduction(+:wedges)
        for (int v = 0; v < g->n; v++) {
            int d = (int)(g->off[v+1] - g->off[v]);
            wedges += (long long)d * (d - 1) / 2;
            TopEntry x = { v, d }; topInsert(loc, &lc, x);
        }
        #pragma omp critical
        for (int i = 0; i < lc; i++) topInsert(best, &cnt, loc[i]);
    }
#else
    for (int v = 0; v < g->n; v++) {
        int d = (int)(g->off[v+1] - g->off[v]);
        wedges += (long long)d * (d - 1) / 2;
        TopEntry x = { v, d }; topInsert(best, &cnt, x);
    }
#endif
    for (int i = 0; i < cnt; i++) out[i] = best[i];
    *limit = cnt;
    return wedges;
}

/* ----------------------------------------------- 3. connected components */
#ifdef PARALLEL
/* lock-free union-find: always link the larger root under the smaller one */
static int ufFind(int *p, int x)
{
    for (;;) {
        int px = __atomic_load_n(&p[x], __ATOMIC_RELAXED);
        if (px == x) return x;
        int ppx = __atomic_load_n(&p[px], __ATOMIC_RELAXED);
        if (px != ppx) __atomic_compare_exchange_n(&p[x], &px, ppx, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        x = ppx;
    }
}
static void ufUnion(int *p, int a, int b)
{
    for (;;) {
        a = ufFind(p, a); b = ufFind(p, b);
        if (a == b) return;
        if (a < b) { int t = a; a = b; b = t; }          /* a is the larger root */
        int exp = a;
        if (__atomic_compare_exchange_n(&p[a], &exp, b, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }
}
static void components(const Graph *g, int *nComp, int *largest)
{
    int n = g->n;
    int *p = xmalloc(n * sizeof(int)), *size = xcalloc(n, sizeof(int));
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) p[i] = i;
    #pragma omp parallel for schedule(dynamic, 256)
    for (int u = 0; u < n; u++)
        for (long j = g->off[u]; j < g->off[u+1]; j++)
            if (g->adj[j] > u) ufUnion(p, u, g->adj[j]);
    int comps = 0, big = 0;
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) fetchInc(&size[ufFind(p, i)]);
    #pragma omp parallel for schedule(static) reduction(+:comps) reduction(max:big)
    for (int i = 0; i < n; i++) if (size[i] > 0) { comps++; if (size[i] > big) big = size[i]; }
    free(p); free(size);
    *nComp = comps; *largest = big;
}
#else
static void components(const Graph *g, int *nComp, int *largest)
{
    int n = g->n, comps = 0, big = 0;
    char *vis = xcalloc(n, 1); int *q = xmalloc(n * sizeof(int));
    for (int s = 0; s < n; s++) {
        if (vis[s]) continue;
        int front = 0, rear = 0;
        q[rear++] = s; vis[s] = 1;
        while (front < rear) {
            int u = q[front++];
            for (long j = g->off[u]; j < g->off[u+1]; j++) {
                int v = g->adj[j];
                if (!vis[v]) { vis[v] = 1; q[rear++] = v; }
            }
        }
        comps++; if (rear > big) big = rear;
    }
    free(vis); free(q);
    *nComp = comps; *largest = big;
}
#endif

/* ----------------------------------------------------- 4. triangles */
static long long countTriangles(const Graph *g)
{
    long long tri = 0;
#ifdef PARALLEL
    #pragma omp parallel for schedule(dynamic, 32) reduction(+:tri)
#endif
    for (int u = 0; u < g->n; u++) {
        for (long j = g->off[u]; j < g->off[u+1]; j++) {
            int v = g->adj[j];
            if (v <= u) continue;
            long a = j + 1, ae = g->off[u+1];          /* members of N(u) greater than v */
            long b = g->off[v], be = g->off[v+1];
            while (b < be && g->adj[b] <= v) b++;       /* members of N(v) greater than v */
            while (a < ae && b < be) {
                int x = g->adj[a], y = g->adj[b];
                if (x == y) { tri++; a++; b++; }
                else if (x < y) a++; else b++;
            }
        }
    }
    return tri;
}

/* ------------------------------------------------ 5. path sampling */
static void pathSampling(const Graph *g, double *avg, int *diam)
{
    int n = g->n, S = n < PATH_SAMPLES ? n : PATH_SAMPLES;
    long long sum = 0, pairs = 0; int ecc = 0;
#ifdef PARALLEL
    #pragma omp parallel reduction(+:sum,pairs) reduction(max:ecc)
#endif
    {
        int *seen = xcalloc(n, sizeof(int)), *dist = xmalloc(n * sizeof(int)), *q = xmalloc(n * sizeof(int));
#ifdef PARALLEL
        #pragma omp for schedule(dynamic, 4)
#endif
        for (int k = 0; k < S; k++) {
            int s = (int)((long long)k * n / S), stamp = k + 1, front = 0, rear = 0;
            q[rear++] = s; seen[s] = stamp; dist[s] = 0;
            while (front < rear) {
                int u = q[front++];
                if (dist[u] > ecc) ecc = dist[u];
                sum += dist[u]; pairs += (u != s);
                for (long j = g->off[u]; j < g->off[u+1]; j++) {
                    int v = g->adj[j];
                    if (seen[v] != stamp) { seen[v] = stamp; dist[v] = dist[u] + 1; q[rear++] = v; }
                }
            }
        }
        free(seen); free(dist); free(q);
    }
    *avg = pairs ? (double)sum / (double)pairs : 0.0;
    *diam = ecc;
}

/* ------------------------------------------------------ whole pipeline */
static void analyse(const int *e, long m, int n, Result *r)
{
    double t0 = omp_get_wtime();
    Graph g = buildGraph(e, m, n);
    double t1 = omp_get_wtime();
    long long wedges = topUsers(&g, r->top, &r->limit);
    double t2 = omp_get_wtime();
    components(&g, &r->components, &r->largest);
    double t3 = omp_get_wtime();
    r->triangles = countTriangles(&g);
    r->clustering = wedges ? 3.0 * (double)r->triangles / (double)wedges : 0.0;
    double t4 = omp_get_wtime();
    pathSampling(&g, &r->avgPath, &r->diameter);
    double t5 = omp_get_wtime();
    r->n = g.n; r->edges = g.edges;
    r->tBuild = t1 - t0; r->tTop = t2 - t1; r->tComp = t3 - t2; r->tTri = t4 - t3; r->tPath = t5 - t4;
    r->tTotal = t5 - t0;
    freeGraph(&g);
}

static int cmpRes(const void *a, const void *b)
{
    double x = ((const Result*)a)->tTotal, y = ((const Result*)b)->tTotal;
    return (x > y) - (x < y);
}

static int socialMain(int argc, char **argv)
{
    if (argc < 2 || argc > 3) { fprintf(stderr, "Usage: %s <dataset_file> [threads]\n", argv[0]); return 1; }
#ifdef PARALLEL
    if (argc == 3) { int t = atoi(argv[2]); if (t >= 1) omp_set_num_threads(t); }
#endif
    long m; int n;
    double tl = omp_get_wtime();
    int *e = loadEdges(argv[1], &m, &n);
    tl = omp_get_wtime() - tl;

    Result runs[REPS], warm;
    analyse(e, m, n, &warm);                       /* warm-up: caches, page faults, thread start-up */
    for (int i = 0; i < REPS; i++) analyse(e, m, n, &runs[i]);
    qsort(runs, REPS, sizeof(Result), cmpRes);
    Result *r = &runs[REPS / 2];                   /* median run */

    printf("\nTop %d Connected Users:\n", r->limit);
    for (int i = 0; i < r->limit; i++)
        printf("Rank %d -> User %d : %d connections\n", i + 1, r->top[i].user, r->top[i].deg);

    printf("\nCommunity / Connectivity Analysis:\n");
    printf("Connected components : %d\n", r->components);
    printf("Largest component    : %d users\n", r->largest);
    printf("Triangles            : %lld\n", r->triangles);
    printf("Clustering coeff.    : %.6f\n", r->clustering);
    printf("Avg shortest path    : %.4f  (BFS from %d users)\n", r->avgPath, n < PATH_SAMPLES ? n : PATH_SAMPLES);
    printf("Estimated diameter   : %d\n", r->diameter);

    printf("\nPhase times (median run of %d, file loading excluded):\n", REPS);
    printf("  Graph construction : %.6f seconds\n", r->tBuild);
    printf("  Top-user search    : %.6f seconds\n", r->tTop);
    printf("  Components         : %.6f seconds\n", r->tComp);
    printf("  Triangle counting  : %.6f seconds\n", r->tTri);
    printf("  Path sampling      : %.6f seconds\n", r->tPath);
    printf("  (file loading, not timed: %.6f seconds)\n", tl);

    printf("\n========================================\n");
    printf("         %s\n", TITLE);
    printf("========================================\n");
    printf("Users             : %d\n", r->n);
    printf("Relationships     : %lld\n", r->edges);
#ifdef PARALLEL
    printf("OpenMP threads    : %d\n", omp_get_max_threads());
#endif
    printf("Execution time    : %.6f seconds\n", r->tTotal);
    printf("========================================\n");
    free(e);
    return 0;
}

#endif
