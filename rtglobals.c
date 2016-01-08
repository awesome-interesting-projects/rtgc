// (C) Copyright 2015 - 2016 by Wade L. Hennessey. All rights reserved.

/* State shared by rtalloc, rtgc, and vizmem goes here */

#define _USE_GNU
#define _GNU_SOURCE


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include "mem-config.h"
#include "infoBits.h"
#include "mem-internals.h"
#include "allocate.h"

GROUP_INFO *groups;
PAGE_INFO *pages;

SEGMENT *segments;
int total_segments;

THREAD_INFO *threads;
int total_threads;

char **global_roots;
int total_global_roots;

/* HEY! only 1 static segment while these are global! */
BPTR first_static_ptr;
BPTR last_static_ptr;

BPTR first_partition_ptr;
BPTR last_partition_ptr;

BPTR write_vector;
size_t write_vector_size;

long total_partition_pages;
int unmarked_color;
int marked_color;
int enable_write_barrier;
int memory_mutex;

int visual_memory_on;
char *last_gc_state;

pthread_key_t thread_index_key;

pthread_mutex_t total_threads_lock;
pthread_mutex_t empty_pages_lock;
pthread_mutex_t global_roots_lock;

COUNTER stacks_copied_counter;
sem_t gc_semaphore;
int run_gc = 0;
int atomic_gc = 1;
