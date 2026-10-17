# Social Network Analysis (Sequential vs Parallel, OpenMP)

## Build
    gcc -O2 -fopenmp social_network_seq.c -o social_network_seq
    gcc -O2 -fopenmp social_network_parallel.c -o social_network_parallel
(or just `make`)

## Run (terminal)
    ./social_network_seq data/facebook_combined.txt
    OMP_NUM_THREADS=4 ./social_network_parallel data/facebook_combined.txt   # or: ./social_network_parallel file 4

## Run (UI)
    python3 ui_server.py      # open http://localhost:8000
The UI runs the same executables and shows the exact "Execution time" line the terminal prints.

## What is measured
Both programs run the identical pipeline on the identical CSR graph: graph build, top-10 users,
connected components, triangle counting / clustering coefficient, and BFS path sampling
(512 users). File reading is excluded. The pipeline runs once as warm-up, then 5 times; the
median run is reported, which keeps the time steady between runs. Use 2+ threads for the parallel version.
