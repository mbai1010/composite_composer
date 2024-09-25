/*
 * Copyright 2018, Phani Gadepalli and Gabriel Parmer, GWU, gparmer@gwu.edu.
 *
 * This uses a two clause BSD License.
 */

#include <llprint.h>
#include <res_spec.h>
#include <sched.h>
#include <cos_time.h>
#include <cos_component.h>
#include <initargs.h>

#define SL_FPRR_NPRIOS 32
#define LOWEST_PRIORITY (SL_FPRR_NPRIOS - 1)
#define LOW_PRIORITY (LOWEST_PRIORITY - 1)

enum {
	STATIC_HI_STEP,
	STATIC_LO_STEP,
	DYNAMIC_STEP,
	NUM_OF_STEPS
} test_steps_t;

int do_continue = 1;
int test_failed = 0;
int test_passed[NUM_OF_STEPS] = {0, 0, 0};

// Array to store the idx of the static threads
unsigned int static_thread_idx[MAX_NUM_STATIC_THD_COMP] = {0};

static void static_thread_hi(void);
static void static_thread_lo(void);

static void
dynamic_thread(void *d)
{
	PRINTLOG(PRINT_DEBUG, "Dynamic thread running on core %ld\n", cos_cpuid());
	PRINTLOG(PRINT_DEBUG, "Test step %d passed\n", DYNAMIC_STEP);
	test_passed[DYNAMIC_STEP] = 1;
	sched_thd_block(0);
}

int
cos_init(void)
{
	PRINTLOG(PRINT_DEBUG, "Unit-test creation of static threads\n");
	
	// Parse the static thread parameters from initargs.c
	struct initargs static_thds, curr_thd;
	struct initargs_iter i;
	int ret, cont, cont2;

	ret = args_get_entry("comp_virt_resources/sched", &static_thds);
	if(ret == 0) {
		for( cont = args_iter(&static_thds, &i, &curr_thd) ; cont ; cont = args_iter_next(&i, &curr_thd) ) {
			thdclosure_index_t idx;
			char *id_str, *name_str = NULL;

			// Get the thread idx
			id_str = args_get_from("id", &curr_thd);
			assert(id_str != NULL && atoi(id_str) > 0);
			idx = atoi(id_str);

			// Get the name
			name_str = args_get_from("name", &curr_thd);
			assert(name_str != NULL);
			
			PRINTLOG(PRINT_DEBUG, "Static thread idx %d, name %s\n", idx, name_str);
			if(strcmp(name_str, "name_static_hi") == 0) {
				static_thread_idx[STATIC_HI_STEP] = idx;
			} else if(strcmp(name_str, "name_static_lo") == 0) {
				static_thread_idx[STATIC_LO_STEP] = idx;
			}
		}
	}

	PRINTLOG(PRINT_DEBUG, "COS_INIT done\n");

	return 0;
}

int
main(void)
{
	thdid_t dyn_thdid;
	int ret;

	dyn_thdid = sched_thd_create(dynamic_thread, NULL);
	sched_thd_param_set(dyn_thdid, sched_param_pack(SCHEDP_PRIO, LOWEST_PRIORITY));

	// Wake up the static threads
	compid_t compid = cos_compid();
	ret = sched_thd_wakeup_static(compid, static_thread_idx[STATIC_HI_STEP]); // High priority static thread
	assert(ret == 0);
	ret = sched_thd_wakeup_static(compid, static_thread_idx[STATIC_LO_STEP]); // Low priority static thread
	assert(ret == 0);

	PRINTLOG(PRINT_DEBUG, "Waiting for threads to complete\n");
	while (do_continue && !test_failed) {
		cycles_t wakeup;

		wakeup = time_now() + time_usec2cyc(1000 * 1000);
		sched_thd_block_timeout(0, wakeup);

		do_continue = !(test_passed[STATIC_HI_STEP] 
						&& test_passed[STATIC_LO_STEP] 
						&& test_passed[DYNAMIC_STEP]);
	}

	if (test_failed) {
		PRINTLOG(PRINT_ERROR, "Test failed in one or more steps\n");
	} else {
		PRINTLOG(PRINT_DEBUG, "Test passed\n");
	}
	return 0;
}

static void
static_thread_hi(void)
{
	// We expect this to run on core 1
	PRINTLOG(PRINT_DEBUG, "High priority static thread is running on core %ld\n", cos_cpuid());
	if (cos_cpuid() != 0) {
		PRINTLOG(PRINT_ERROR, "Test failed! High priority static thread is not running on core 0\n");
		test_failed = 1;
	} else {
		PRINTLOG(PRINT_DEBUG, "Test step %d passed, spinning\n", STATIC_HI_STEP);
		test_passed[STATIC_HI_STEP] = 1;
	}
	sched_thd_block(0);
}

static void
static_thread_lo(void)
{
	//We expect this to run on core 2
	PRINTLOG(PRINT_DEBUG, "Low priority static thread is running on core %ld\n", cos_cpuid());
	if (cos_cpuid() != 2) {
		PRINTLOG(PRINT_ERROR, "Test failed! Low priority static thread is not running on core 2\n");
		test_failed = 1;
	} else {
		PRINTLOG(PRINT_DEBUG, "Test step %d passed\n", STATIC_LO_STEP);
		test_passed[STATIC_LO_STEP] = 1;
	}
	sched_thd_block(0);
}

int
cos_thd_entry_static(unsigned int idx)
{
	PRINTLOG(PRINT_DEBUG, "Component %ld: Static thread idx %d\n", cos_compid(), idx);
	// Increment idx to match the thread index
	if (idx == static_thread_idx[STATIC_HI_STEP]) {
		static_thread_hi();
	} else if (idx == static_thread_idx[STATIC_LO_STEP]) {
		static_thread_lo();
	}

	// Should never reach here
	assert(0);

	return 0;
}



