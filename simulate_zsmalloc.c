/*
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>

// --- CONFIGURATION ---
#define PAGE_SIZE 4096
#define CONFIG_ZSMALLOC_CHAIN_SIZE 8

#define ZS_ALIGN 8
#define ZS_HANDLE_SIZE 8
#define ZS_MAX_PAGES_PER_ZSPAGE CONFIG_ZSMALLOC_CHAIN_SIZE
// Simplified ZS_MIN_ALLOC_SIZE for simulation
#define ZS_MIN_ALLOC_SIZE 32
#define ZS_MAX_ALLOC_SIZE PAGE_SIZE

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

struct size_class {
	int size;
	int index;
	int pages_per_zspage;
	int objs_per_zspage;

	// Simulation Stats
	int waste;
	double waste_pct;
	int span;
	int merge_count;
	struct size_class *merged_into;
};

static bool is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

static int calculate_zspage_chain_size_old(int class_size)
{
	int i, min_waste = INT_MAX;
	int chain_size = 1;

	if (is_power_of_2(class_size))
		return chain_size;

	for (i = 1; i <= ZS_MAX_PAGES_PER_ZSPAGE; i++) {
		int waste = (i * PAGE_SIZE) % class_size;
		if (waste < min_waste) {
			min_waste = waste;
			chain_size = i;
		}
	}

	return chain_size;
}

static int calculate_zspage_chain_size_new(int class_size)
{
	int i, min_waste = INT_MAX;
	int best_chain_size = 1;

	if (is_power_of_2(class_size))
		return best_chain_size;

	for (i = 1; i <= ZS_MAX_PAGES_PER_ZSPAGE; i++) {
		int curr_waste = (i * PAGE_SIZE) % class_size;

		if (curr_waste == 0)
			return i;

		/*
		 * Accept the new chain size if:
		 * 1. The current best is wasteful (> 10% of zspage size),
		 *    accept anything that is better.
		 * 2. The current best is efficient, accept only significant
		 *    (25%) improvement.
		 */
		if (min_waste * 10 > best_chain_size * PAGE_SIZE) {
			if (curr_waste < min_waste) {
				min_waste = curr_waste;
				best_chain_size = i;
			}
		} else {
			if (curr_waste * 4 < min_waste * 3) {
				min_waste = curr_waste;
				best_chain_size = i;
			}
		}

		/*
		 * If the current best chain has low waste (approx < 1.5%
		 * relative to zspage size) then accept it right away.
		 */
		if (min_waste * 64 <= best_chain_size * PAGE_SIZE)
			break;
	}

	return best_chain_size;
}

static int calculate_spanning(int size, int chain)
{
	int total_len = chain * PAGE_SIZE;
	int num_objs = total_len / size;
	int spans = 0;
	int i;

	for (i = 0; i < num_objs; i++) {
		int start = i * size;
		int end = start + size;
		if ((start / PAGE_SIZE) != ((end - 1) / PAGE_SIZE))
			spans++;
	}
	return spans;
}

static bool can_merge(struct size_class *prev, int pages_per_zspage, int objs_per_zspage)
{
	if (prev->pages_per_zspage == pages_per_zspage &&
	    prev->objs_per_zspage == objs_per_zspage)
		return true;
	return false;
}

struct size_class **build_pool(int delta, int (*chain_func)(int))
{
	int i;
	int num_classes = DIV_ROUND_UP(ZS_MAX_ALLOC_SIZE - ZS_MIN_ALLOC_SIZE, delta) + 1;
	struct size_class **pool = calloc(num_classes, sizeof(struct size_class *));
	struct size_class *prev_class = NULL;

	num_classes -= 1;
	printf("\nBuilding Pool: Delta=%d, NumClasses=%d\n", delta, num_classes);

	// Iterate reversely
	for (i = num_classes; i >= 0; i--) {
		int size;
		int pages_per_zspage;
		int objs_per_zspage;
		struct size_class *class;

		size = ZS_MIN_ALLOC_SIZE + i * delta;
		if (size > ZS_MAX_ALLOC_SIZE)
			size = ZS_MAX_ALLOC_SIZE;

		pages_per_zspage = chain_func(size);
		objs_per_zspage = pages_per_zspage * PAGE_SIZE / size;
		if (prev_class) {
			if (can_merge(prev_class, pages_per_zspage, objs_per_zspage)) {
				pool[i] = prev_class;
				prev_class->merge_count++;
				continue;
			}
		}

		class = malloc(sizeof(struct size_class));
		class->size = size;
		class->index = i;
		class->pages_per_zspage = pages_per_zspage;
		class->objs_per_zspage = objs_per_zspage;
		class->merge_count = 1; // Self
		class->merged_into = class; // Self

		// Calc Stats
		class->waste = (pages_per_zspage * PAGE_SIZE) % size;
		class->waste_pct = (double)class->waste / (pages_per_zspage * PAGE_SIZE) * 100.0;
		class->span = calculate_spanning(size, pages_per_zspage);

		pool[i] = class;
		prev_class = class;
	}
	return pool;
}

void print_pool_stats(const char *label, struct size_class **pool, int num_classes)
{
	int i;
	printf("\n--- %s ---\n", label);
	printf("% -10s | % -5s | % -9s | % -12s | % -5s | % -6s\n",
	       "Phys Size", "Chain", "Objs/Page", "Waste", "Span", "Merge#");
	printf("-----------------------------------------------------------------\n");

	// Print unique physical classes (sorted by size)
	// Since we iterate reversely during build, the unique classes are at the 'start' of merge chains.
	// But pool[] has pointers. We can just iterate pool[] and print if pool[i] == pool[i]->merged_into?
	// No, pool[i] points to the *physical* class.
	// We need to print unique pointers.

	// Collect unique pointers
	struct size_class **unique = calloc(num_classes, sizeof(struct size_class *));
	int unique_count = 0;

	for (i = 0; i < num_classes; i++) {
		struct size_class *c = pool[i];
		int j;
		bool found = false;
		for (j = 0; j < unique_count; j++) {
			if (unique[j] == c) {
				found = true;
				break;
			}
		}
		if (!found) {
			unique[unique_count++] = c;
		}
	}

	// Sort unique by size
	for (i = 0; i < unique_count - 1; i++) {
		for (int j = 0; j < unique_count - i - 1; j++) {
			if (unique[j]->size > unique[j+1]->size) {
				struct size_class *temp = unique[j];
				unique[j] = unique[j+1];
				unique[j+1] = temp;
			}
		}
	}

	for (i = 0; i < unique_count; i++) {
		struct size_class *c = unique[i];
		printf("% -10d | % -5d | % -9d | %4d (%4.1f%%) | % -5d | % -6d\n",
		       c->size, c->pages_per_zspage, c->objs_per_zspage,
		       c->waste, c->waste_pct, c->span, c->merge_count);
	}

	printf("\nTotal Logical: %d\nTotal Physical: %d\n", num_classes, unique_count);

	free(unique);
}

void print_summary(const char *label, struct size_class **pool, int pool_size, int pool_delta)
{
	int req_size;
	double total_waste_pct = 0;
	int count = 0;

	// Iterate over standard user requests (16-byte step)
	// This captures the "rounding up" waste.
	for (req_size = ZS_MIN_ALLOC_SIZE; req_size <= PAGE_SIZE; req_size += 16) {
		// Map request to pool class
		// Index in pool = (req_size - MIN + pool_delta - 1) / pool_delta
		int idx = (req_size - ZS_MIN_ALLOC_SIZE + pool_delta - 1) / pool_delta;
		if (idx >= pool_size) idx = pool_size - 1;

		struct size_class *logical = pool[idx];
		struct size_class *phys = logical->merged_into;

		// 1. Internal Fragmentation (Rounding Up + Merging)
		// phys->size is the slot size. req_size is user data.
		int internal_waste = phys->size - req_size;

		// 2. Tail Waste per object
		double tail_waste_per_obj = (double)phys->waste / phys->objs_per_zspage;

		// Total waste
		double total_waste = internal_waste + tail_waste_per_obj;
		double phys_mem_per_obj = (double)(phys->pages_per_zspage * PAGE_SIZE) / phys->objs_per_zspage;

		total_waste_pct += (total_waste / phys_mem_per_obj) * 100.0;
		count++;
	}

	printf("%s Avg Effective Waste (User Requests): %.2f%%\n", label, total_waste_pct / count);

	// Calculate Total Span for all logical classes
	long total_logical_span = 0;
	int unique_phys_count = 0;
	struct size_class **unique_list = calloc(pool_size, sizeof(struct size_class *));

	for (int i = 0; i < pool_size; i++) {
		struct size_class *phys = pool[i]->merged_into;
		total_logical_span += phys->span;

		bool found = false;
		for (int j = 0; j < unique_phys_count; j++) {
			if (unique_list[j] == phys) {
				found = true;
				break;
			}
		}
		if (!found) {
			unique_list[unique_phys_count++] = phys;
		}
	}
	printf("%s Total Logical Classes: %d\n", label, pool_size);
	printf("%s Total Physical Classes: %d\n", label, unique_phys_count);
	printf("%s Total Span (All Logicals): %ld\n", label, total_logical_span);
	free(unique_list);
}

void print_request_table(const char *label, struct size_class **pool, int pool_size, int pool_delta)
{
	int req_size;

	printf("\n--- %s (Detailed Request View) ---\n", label);
	printf("% -5s | % -5s | % -5s | % -9s | % -10s | % -10s | % -5s\n",
	       "Req", "Phys", "Chain", "Objs/Page", "TailWaste", "MergeWaste", "Span");
	printf("----------------------------------------------------------------------------\n");

	for (req_size = ZS_MIN_ALLOC_SIZE; req_size <= PAGE_SIZE; req_size += 16) {
		int idx = (req_size - ZS_MIN_ALLOC_SIZE + pool_delta - 1) / pool_delta;
		if (idx >= pool_size) idx = pool_size - 1;

		struct size_class *phys = pool[idx]->merged_into;

		// Tail Waste: Bytes unused at end of zspage
		int tail_waste = phys->waste;

		// Merge Waste: Padding per object * Number of objects
		// (phys->size - req_size) * phys->objs_per_zspage
		int padding = phys->size - req_size;
		int merge_waste = padding * phys->objs_per_zspage;

		printf("% -5d | % -5d | % -5d | % -9d | % -10d | % -10d | % -5d\n",
		       req_size, phys->size, phys->pages_per_zspage, phys->objs_per_zspage,
		       tail_waste, merge_waste, phys->span);
	}
}

int main()
{
	printf("PAGE_SIZE: %d, CHAIN_SIZE: %d\n", PAGE_SIZE, CONFIG_ZSMALLOC_CHAIN_SIZE);

	// Old Config
	int delta_old = PAGE_SIZE >> 8;
	if (delta_old < 16) delta_old = 16;
	struct size_class **pool_old = build_pool(delta_old, calculate_zspage_chain_size_old);
	int num_old = DIV_ROUND_UP(ZS_MAX_ALLOC_SIZE - ZS_MIN_ALLOC_SIZE, delta_old) + 1;

	print_request_table("BEFORE", pool_old, num_old, delta_old);
	print_summary("BEFORE", pool_old, num_old, delta_old);

	// New Config
	struct size_class **pool_new = build_pool(16, calculate_zspage_chain_size_new);
	int num_new = DIV_ROUND_UP(ZS_MAX_ALLOC_SIZE - ZS_MIN_ALLOC_SIZE, 16) + 1;

	print_request_table("AFTER", pool_new, num_new, 16);
	print_summary("AFTER ", pool_new, num_new, 16);

	return 0;
}
