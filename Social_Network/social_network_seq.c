/* Sequential social network analysis.  Build: gcc -O2 -fopenmp social_network_seq.c -o social_network_seq */
#include "social_common.h"

int main(int argc, char *argv[])
{
    return socialMain(argc, argv);
}
