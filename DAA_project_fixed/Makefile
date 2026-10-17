CC     = gcc
CFLAGS = -O2 -fopenmp -Wall -Wextra

all: social_network_seq social_network_parallel generate_dataset

social_network_seq: social_network_seq.c social_common.h
	$(CC) $(CFLAGS) $< -o $@

social_network_parallel: social_network_parallel.c social_common.h
	$(CC) $(CFLAGS) $< -o $@

generate_dataset: generate_dataset.c
	$(CC) -O2 $< -o $@

clean:
	rm -f social_network_seq social_network_parallel generate_dataset
