// (C) Copyright 2015 - 2016 by Wade L. Hennessey. All rights reserved.

// Real time storage allocater

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include "mem-config.h"
#include "info-bits.h"
#include "mem-internals.h"
#include "allocate.h"

static inline int size_to_group_index(int size) {
  int s = size;
  int index = 0;
  
  s = s - 1;
  while (s != 0) {
    s = s / 2;
    index = index + 1;
  }
  return(MAX(MIN_GROUP_INDEX, index));
}

static
void init_group_info() {
  int index;
  for (index = MIN_GROUP_INDEX; index <= MAX_GROUP_INDEX; index = index + 1) {
    int size = 1 << index;
    groups[index].size = size;
    groups[index].index = index;
    groups[index].free = NULL;
    groups[index].last = NULL;
    groups[index].white = NULL;
    groups[index].black = NULL;
    groups[index].gray = NULL;
    groups[index].white_count = 0;
    groups[index].black_scanned_count = 0;
    groups[index].black_alloc_count = 0;
    pthread_mutex_init(&(groups[index].free_lock), NULL);
    pthread_mutex_init(&(groups[index].black_and_last_lock), NULL);
  }
}

static 
void init_page_info() {
  for (int i = 0; i < total_partition_pages; i++) {
    pages[i].base = NULL;
    pages[i].group = SYSTEM_PAGE;
  }
}

void RTinit_empty_pages(int first_page, int page_count, int type) {
  int last_page = first_page + page_count;
  for (int i = first_page; i < last_page; i++) {
    pages[i].base = NULL;
    pages[i].group = EMPTY_PAGE;
  }

  if (type == HEAP_SEGMENT) {
    pthread_mutex_lock(&empty_pages_lock);
    // Add the pages to the front of the empty page list
    HOLE_PTR new_hole = (HOLE_PTR) PAGE_INDEX_TO_PTR(first_page);
    new_hole->page_count = page_count;
    new_hole->next = empty_pages;
    empty_pages = new_hole;
    pthread_mutex_unlock(&empty_pages_lock);
  } else {
    Debugger("Can only init heap pages");
  }
}

static
long allocate_segment(size_t desired_bytes, int type) {
  size_t actual_bytes = 0;
  BPTR first_segment_ptr, last_segment_ptr;
  int segment_page_count, first_segment_page;
  int segment = total_segments;

  if ((desired_bytes > 0) &&
      (desired_bytes == (desired_bytes & ~PAGE_ALIGNMENT_MASK)) &&
      (total_segments < MAX_SEGMENTS)) {
    first_segment_ptr = RTbig_malloc(desired_bytes);

    if (NULL != first_segment_ptr) {
      total_segments = total_segments + 1;
      actual_bytes = desired_bytes;
      segment_page_count = actual_bytes / BYTES_PER_PAGE;
      segments[segment].first_segment_ptr = first_segment_ptr;
      last_segment_ptr = first_segment_ptr +
	(segment_page_count * BYTES_PER_PAGE);
      segments[segment].last_segment_ptr = last_segment_ptr;
      segments[segment].segment_page_count = segment_page_count;
      segments[segment].type = type;

      // for now we only support a single static segment and
      // a single heap segment
      switch (type) {
      case HEAP_SEGMENT:
	first_partition_ptr = first_segment_ptr;
	last_partition_ptr = last_segment_ptr;
	first_segment_page = PTR_TO_PAGE_INDEX(first_segment_ptr);
	RTinit_empty_pages(first_segment_page, segment_page_count, type);
	break;
      case STATIC_SEGMENT:
	last_static_ptr = segments[0].last_segment_ptr;
	first_static_ptr = segments[0].first_segment_ptr;
	static_frontier_ptr = first_static_ptr;
	break;
      default: break;
      }
    } else {
      Debugger("Add support for more than 2 segments");
    }
  }
  return(actual_bytes);
}

static
GCPTR allocate_empty_pages(int page_count) {
  int remaining_page_count, best_remaining_page_count;
  GCPTR base = NULL;
  HOLE_PTR prev = NULL;
  HOLE_PTR best = NULL;
  HOLE_PTR best_prev = NULL;

  pthread_mutex_lock(&empty_pages_lock);
  HOLE_PTR next = empty_pages;
  // Search for a best fit hole
  best_remaining_page_count = total_partition_pages + 1;
  while ((best_remaining_page_count > 0) && (next != NULL)) {
    if (next->page_count >= page_count) {
      remaining_page_count = next->page_count - page_count;
      if (remaining_page_count < best_remaining_page_count) {
	best_remaining_page_count = remaining_page_count;
	best = next;
	best_prev = prev;
      }
    }
    prev = next;
    next = next->next;
  }

  if (best != NULL) {
    HOLE_PTR rest;
    if (best_remaining_page_count == 0) {
      rest = best->next;
    } else {
      rest = (HOLE_PTR) ((BPTR) best + (page_count * BYTES_PER_PAGE));
      rest->page_count = best_remaining_page_count;
      rest->next = best->next;
    }
    if (best_prev == NULL) {
      empty_pages = rest;
    } else {
      best_prev->next = rest;
    }
    base = (GCPTR) best;
  }
  pthread_mutex_unlock(&empty_pages_lock);
  return(base);
}

// The gc counterpart to this function is recycle_group_garbage.
// We could try to unify the small common part.
// Whoever calls this function has to be holding the group->free_lock.
// So far only RTallocate calls this.
static
void init_pages_for_group(GPTR group, int min_pages) {
  int pages_per_object = group->size / BYTES_PER_PAGE;
  int byte_count = MAX(pages_per_object,min_pages) * BYTES_PER_PAGE;
  int num_objects = byte_count >> group->index;
  int page_count = (num_objects * group->size) / BYTES_PER_PAGE;
  GCPTR base = allocate_empty_pages(page_count);

  if (base == NULL) {
    int actual_bytes = allocate_segment(MAX(DEFAULT_HEAP_SEGMENT_SIZE,
					    page_count * BYTES_PER_PAGE),
					HEAP_SEGMENT);
    assert(0 == actual_bytes);	// while we only have 1 segment
    if (actual_bytes < byte_count) {
      // atomic and concurrent gc can't flip without
      // unlocking all group free locks
      pthread_mutex_unlock(&(group->free_lock));
      if (RTatomic_gc) {
	// atomic gc
	run_gc = 1;
	while (1 == run_gc);
      } else {
	// concurrent gc
	// need to wait until gc count increases by 2
	// need to make gc_count a COUNTER counter instead of a lone int

	printf("alloc out ran gc, sync collect\n");
	int current_gc_count = gc_count;
	while (gc_count < (current_gc_count + 2)) {
	  // Should be able to remove this sched_yield now that 
	  // gc_count is declared volatile
	  sched_yield();
	}
	if (NULL == group->free) {
	  out_of_memory("Heap", group->size);
	}
      }
      pthread_mutex_lock(&(group->free_lock));
    }
    if (NULL == group->free) {
      base = allocate_empty_pages(page_count);
    } else {
      // printf("full_gc added to empty free list, gc_count = %d\n", gc_count);
    }
  }

  if (base != NULL) {
    GCPTR current = NULL;
    GCPTR next = base;
    for (int i = 0; i < num_objects; i++) {
      GCPTR prev = current;
      current = next;
      next = (GCPTR) ((BPTR) current + group->size);
      current->prev = prev;
      current->next = next;
      SET_COLOR(current,GREEN);
    }
    SET_LINK_POINTER(current->next,NULL);
    assert(NULL == group->free);
    WITH_LOCK((group->black_and_last_lock),
	      GCPTR last = group->last;
	      group->free = base;
	      if (last == NULL) { 	// No gray, black, or green objects?
		group->black = base;
	      } else {
		SET_LINK_POINTER(base->prev, last);
		SET_LINK_POINTER(last->next,base);
	      }
	      group->last = current;);
    // Only now can we initialize EMPTY page table entries.
    // Doing it before object GCHDRs are correctly setup and colored green
    // allows conservative pointers and exposed and uncleared
    // dead pointers on the stack to incorrectly make_gray 
    // random bits that look like white objects!
    if (base != NULL) {
      int i;
      int next_page_index = PTR_TO_PAGE_INDEX(((BPTR) base));
      for (i = 1; i <= page_count; i++) {
	pages[next_page_index].base = base;
	pages[next_page_index].group = group;
	next_page_index = next_page_index + 1;
      }
    }
  }
}

static inline
GPTR allocation_group(long *metadata, int size) {
  int data_size, real_size;
  if (size >= 0) {
    switch ((long) metadata) {
    case (long) RTnopointers:
    case (long) RTpointers:
    case (long) RTcustom1:
      data_size = size;
      real_size = size + sizeof(GC_HEADER); 
      break;
    default:
      data_size = (metadata[0] * size) + sizeof(void *);
      real_size = data_size + sizeof(GC_HEADER);
      break;
    }
    
    GPTR group;
    int group_index = size_to_group_index(real_size);
    if (group_index > MAX_GROUP_INDEX) {
      printf("%d", real_size);
      Debugger(" exceeds the maximum object size\n");
    } else {
      group = &(groups[group_index]);
    }
    return(group);
  } else {
    Debugger("Negative object size\n");
  }
}

static inline
void initialize_object_metadata(void *metadata, GCPTR gcptr, GPTR group) {
  long md = (long) metadata;
  if (md < SC_METADATA) {
    SET_STORAGE_CLASS(gcptr, md);
  } else {
    SET_STORAGE_CLASS(gcptr, SC_METADATA);
  }
}

static inline
void initialize_object_body(void *metadata, LPTR base, GPTR group) {
  long md = (long) metadata;
  if (metadata != RTnopointers) {
    memset(base, 0, group->size - sizeof(GC_HEADER));
  }
  if (md > SC_METADATA) {
    LPTR last_ptr = base + (group->size / sizeof(LPTR)) - 3;
    *last_ptr = md;
  }
}

void *RTallocate(void *metadata, int size) {
  GCPTR new;
  LPTR base;
  GPTR group;

  group = allocation_group(metadata,size);
  pthread_mutex_lock(&(group->free_lock));
  if (group->free == NULL) {
    init_pages_for_group(group,1);
    if (group->free == NULL) {
      out_of_memory("Heap", group->size);
    }
  }
  new = group->free;
  group->free = GET_LINK_POINTER(new->next);
  // No need for an explicit flip lock here. During a flip the gc will
  // hold the green lock for every group, so no allocator can get here
  // when the marked_color is being changed.
  SET_COLOR(new,marked_color);	// Must allocate black!
  group->black_alloc_count = group->black_alloc_count + 1;
    
  initialize_object_metadata(metadata, new, group);
  // Unlock only after storage class initialization because
  // gc recyling garbage can read and write next ptr
  pthread_mutex_unlock(&(group->free_lock));
  base = (LPTR) (new + 1);
  initialize_object_body(metadata, base, group);
  return(base);
}

void *RTstatic_allocate(void *metadata, int size) {
  size = ROUND_UPTO_LONG_ALIGNMENT(size);
  
  // Should we lock during flip and copy frontier_ptr at that time?
  // Seems like it shouldn't be needed
  pthread_mutex_lock(&static_frontier_ptr_lock);
  // Static object headers are only 1 word long instead of 2 
  LPTR ptr = (LPTR) static_frontier_ptr;
  static_frontier_ptr = static_frontier_ptr + size + sizeof(long);

  if (static_frontier_ptr > last_static_ptr) {
    out_of_memory("Static", size);
  } else {
    LPTR base = (LPTR) ptr;
    *ptr = (size << LINK_INFO_BITS);
    // need to set storage class and clear body
    //base = RTInitializeObject(metadata, base, real_size, real_size);
    if ((((long) metadata) <= SC_POINTERS) || (((long) metadata) == SC_CUSTOM1)) {
      SET_STORAGE_CLASS(((GCPTR) (ptr - 1)), (long) metadata); 
    } else {
      Debugger("Add static support for SC_METADATA");
    }
    ptr = ptr + 1;
    memset(ptr, 0, size);
    pthread_mutex_unlock(&static_frontier_ptr_lock);
    return(ptr);
  }
}

// Thread 0 is considered the main stack that started this process.
// The gc itself runs on Thread 0.
void init_gc_thread() {
  pthread_attr_t attr;
  void *stackaddr;
  size_t stacksize;
  pthread_t self = pthread_self();
  pthread_getattr_np(self, &attr);
  pthread_attr_getstack(&attr, &stackaddr, &stacksize);
  //printf("Main Stackaddr is %p\n", stackaddr);
  //printf("Main Stacksize is 0x%x\n", stacksize);
  threads[0].stack_base = stackaddr + stacksize;
  threads[0].stack_size = stacksize;
  threads[0].stack_bottom = (char *) &stacksize;
  threads[0].saved_stack_base = 0;
  threads[0].saved_stack_size = 0;
  total_threads = 1;
}

void register_global_root(void *root) {
  pthread_mutex_lock(&global_roots_lock);
  if (total_global_roots == MAX_GLOBAL_ROOTS) {
    Debugger("global roots full!\n");
  } else {
    global_roots[total_global_roots] = (char *) root;
    total_global_roots = total_global_roots + 1;
  }
  pthread_mutex_unlock(&global_roots_lock);
}

void RTinit_heap(size_t first_segment_bytes, size_t static_size) {
  enable_write_barrier = 0;

  total_partition_pages = first_segment_bytes / BYTES_PER_PAGE;
  groups = malloc(sizeof(GROUP_INFO) * (MAX_GROUP_INDEX + 1));
  pages = RTbig_malloc(sizeof(PAGE_INFO) * total_partition_pages);
  segments = malloc(sizeof(SEGMENT) * MAX_SEGMENTS);
  threads = malloc(sizeof(THREAD_INFO) * MAX_THREADS);
  global_roots = malloc(sizeof(char **) * MAX_GLOBAL_ROOTS);
#if USE_BIT_WRITE_BARRIER
  RTwrite_vector_length = first_segment_bytes / (MIN_GROUP_SIZE * BITS_PER_LONG);
  RTwrite_vector = RTbig_malloc(RTwrite_vector_length * sizeof(long));
  memset(RTwrite_vector, 0, RTwrite_vector_length * sizeof(long));
  //printf("using bit write barrier, ");
#else
  RTwrite_vector_length = (total_partition_pages * 
		       (BYTES_PER_PAGE / MIN_GROUP_SIZE));
  RTwrite_vector = RTbig_malloc(RTwrite_vector_length);
  memset(RTwrite_vector, 0, RTwrite_vector_length);
  //printf("using byte write barrier, ");
#endif
  if ((pages == 0) || (groups == 0) || (segments == 0) || 
      (threads == 0) || (global_roots == 0) || (RTwrite_vector == 0)) {
    out_of_memory("Heap Memory tables", 0);
  }

  init_page_info();
  empty_pages = NULL;
  init_gc_thread();
  total_segments = 0;
  
  if ((static_size > 0) &&
      (allocate_segment(static_size, STATIC_SEGMENT) == 0)) {
    out_of_memory("Static Memory Initialization", static_size/1024);
  }

  if (allocate_segment(first_segment_bytes, HEAP_SEGMENT) == 0) {
    out_of_memory("Heap Memory allocation", first_segment_bytes/1024);
  }
  
  marked_color = GENERATION0;
  unmarked_color = GENERATION1;
  init_group_info();
  init_realtime_gc();
}

void *rtalloc_start_thread(void *arg) {
  START_THREAD_ARGS *start_args = (START_THREAD_ARGS *) arg;
  void *(*real_start_func) (void *);

  // unpack start args and then free arg
  long thread_index = start_args->thread_index;
  real_start_func = start_args->real_start_func;
  char *real_args = start_args->real_args;
  free(arg);

  // setup code runs first
  pthread_t self;
  pthread_attr_t attr;
  void *stackaddr;
  size_t stacksize;
  
  printf("Thread %d started, live stack top is 0x%lx\n", 
	 thread_index, &thread_index);
  self = pthread_self();
  threads[thread_index].pthread = self;
  pthread_getattr_np(self, &attr);
  pthread_attr_getstack(&attr, &stackaddr, &stacksize);
  // We don't really need this info, but it might be nice for debugging
  // stackaddr is the LOWEST addressable byte of the stack
  // The stack pointer starts at stackaddr + stacksize!
  // printf("Stackaddr is %p\n", stackaddr);
  // printf("Stacksize is %x\n", stacksize);
  threads[thread_index].stack_base = stackaddr;
  threads[thread_index].stack_size = stacksize;
  threads[thread_index].stack_bottom = (char *)  &stacksize;
  timerclear(&(threads[thread_index].max_pause_tv));
  timerclear(&(threads[thread_index].total_pause_tv));
  fflush(stdout);

  if (0 != pthread_setspecific(thread_index_key, (void *) thread_index)) {
    printf("pthread_setspecific failed!\n"); 
  } else {
    // initializing saved_stack_base tells new_thread
    // that stack setup is done and it can return
    threads[thread_index].saved_stack_base = RTbig_malloc(stacksize);
    // after setup, invoke the real start func with the real args
    (real_start_func)(real_args);
  }
}

int new_thread(void *(*start_func) (void *), void *args) {
  pthread_t thread;

  pthread_mutex_lock(&total_threads_lock);
  if (total_threads < MAX_THREADS) {
    int index = total_threads;
    total_threads = total_threads + 1;
    pthread_mutex_unlock(&total_threads_lock);

    START_THREAD_ARGS *start_args = malloc(sizeof(START_THREAD_ARGS));
    start_args->thread_index = index;
    start_args->real_start_func = start_func;
    start_args->real_args = args;

    // this indicates that thread setup isn't complete
    threads[index].saved_stack_base = 0;
    if (0 != pthread_create(&thread, 
			    NULL, 
			    rtalloc_start_thread,
			    (void *) start_args)) {
      Debugger("pthread_create failed!\n");
    } else {
      // HEY! should do something smarter than busy wait
      // rtalloc_start_thread to complete thread init
      while (0 == threads[index].saved_stack_base) {
	// YOW! without this explicit sched_yield(), we hang in this
	// loop when compiled with -O1 and-O2
	// declaring saved_stack_base volatile doesn't seem to help
	sched_yield();
      }
      return(index);
    }
  } else {
    pthread_mutex_lock(&total_threads_lock);
    out_of_memory("Too many threads", MAX_THREADS);
  }
}

    

	
      
      

  
       
       



