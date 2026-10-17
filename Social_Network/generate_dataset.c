#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    int u;
    int v;
} Edge;

int edgeExists(Edge *edges, int count, int u, int v)
{
    if (u > v) {
        int t = u; u = v; v = t;
    }

    for (int i = 0; i < count; i++) {
        if (edges[i].u == u && edges[i].v == v)
            return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        printf("Usage: %s <users> <edges> <output_file>\n", argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    int m = atoi(argv[2]);

    if (n < 2 || m < n - 1 || m > n * (n - 1) / 2) {
        printf("Invalid values.\n");
        printf("Require n >= 2 and n-1 <= edges <= n(n-1)/2.\n");
        return 1;
    }

    Edge *edges = (Edge *)malloc(m * sizeof(Edge));
    if (edges == NULL) {
        printf("Memory allocation failed.\n");
        return 1;
    }

    srand(42); /* Fixed seed for reproducible datasets. */

    int count = 0;

    /* Create a connected backbone. */
    for (int i = 1; i < n; i++) {
        int u;

        if (i < 10)
            u = rand() % i;
        else {
            int start = i - 10;
            u = start + rand() % (i - start);
        }

        int a = u, b = i;
        if (a > b) {
            int t = a; a = b; b = t;
        }

        edges[count].u = a;
        edges[count].v = b;
        count++;
    }

    /* Add unique sparse relationships with cluster preference. */
    while (count < m) {
        int u = rand() % n;
        int v;

        if (rand() % 100 < 80) {
            int clusterStart = (u / 100) * 100;
            int clusterEnd = clusterStart + 100;
            if (clusterEnd > n) clusterEnd = n;

            v = clusterStart + rand() % (clusterEnd - clusterStart);
        } else {
            v = rand() % n;
        }

        if (u == v)
            continue;

        int a = u, b = v;
        if (a > b) {
            int t = a; a = b; b = t;
        }

        if (!edgeExists(edges, count, a, b)) {
            edges[count].u = a;
            edges[count].v = b;
            count++;
        }
    }

    FILE *file = fopen(argv[3], "w");
    if (file == NULL) {
        printf("Could not create output file.\n");
        free(edges);
        return 1;
    }

    for (int i = 0; i < count; i++)
        fprintf(file, "%d %d\n", edges[i].u, edges[i].v);

    fclose(file);
    free(edges);

    printf("Synthetic dataset created.\n");
    printf("Users         : %d\n", n);
    printf("Relationships : %d\n", m);
    printf("Output        : %s\n", argv[3]);

    return 0;
}
