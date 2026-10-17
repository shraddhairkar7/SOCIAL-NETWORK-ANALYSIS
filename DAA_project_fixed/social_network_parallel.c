/* Parallel (OpenMP) social network analysis.
 * Build: gcc -O2 -fopenmp -DPARALLEL social_network_parallel.c -o social_network_parallel
 * Run  : OMP_NUM_THREADS=4 ./social_network_parallel data/facebook_combined.txt   (or pass threads as 2nd arg)
 */
#define PARALLEL
#include "social_common.h"

int main(int argc, char *argv[])
{
    return socialMain(argc, argv);
}
