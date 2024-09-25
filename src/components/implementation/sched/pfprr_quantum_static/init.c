/**
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2018, The George Washington University
 * Author: Phani Gadepalli, phanikishoreg@gwu.edu & Gabe Parmer, gparmer@gwu.edu
 */

/**
 * Initialization of the scheduler, and all of the components the
 * scheduler is responsible for initializing. Note that the `init`
 * interface is what encodes which components are dependent on the
 * scheduler for initialization. If the scheduler is managed by the
 * capmgr, then the capmgr must have access to the captbl/pgtbl/comp
 * capabilities for the component, thus it must also depend on the
 * capmgr for the capmgr_create interface.
 *
 * The current implementation is not ideal in multiple ways.
 *
 * 1. It does not provide parallel execution.
 * 2. It executes the initialization of the client components during
 *    the *main* execution of this scheduler. That means that
 *    initialization does *not* compose correctly. For example,
 *    consider a component, `a`, that relies on another initialization
 *    component, and also depends on a component, `b`, that depends on
 *    us for initialization. We will finish initialization, and allow
 *    initialization to proceed in `a`, *before* we actually
 *    initialize `b`. This breaks the initialization ordering
 *    requirements. It doesn't make much sense to have such
 *    cross-scheduler dependencies, so this is less of an issue than
 *    it seems.
 *
 * The former is simple because it is TBD. The latter is because the
 * scheduler loop executes in `main`, and we orchestrate
 * initialization within the normal scheduling loop. To fix this, we'd
 * have to move a version of the scheduling loop (that returns
 * sporadically) to `parallel_init`.
 */

#include <slm.h>
#include <barrier.h>
#include <init.h>
#include <initargs.h>

#include <crt.h>

/* This schedule is used by the initializer_thd to ascertain order */
static unsigned long init_schedule_off = 0;
static compid_t init_schedule[MAX_NUM_COMPS] = { 0 };

/* Static threads for the components */
struct slm_thd *static_thds_arr[MAX_NUM_COMPS][MAX_NUM_STATIC_THD_COMP] = { 0 };

/*
 * Coordinating the initialization of components requires tracking the
 * initialization state, and barriers across cores.
 */
typedef enum {
	SCHEDINIT_FREE,
	SCHEDINIT_INITING,
	SCHEDINIT_PARINIT,
	SCHEDINIT_MAIN,	/* main, or parallel_main depending on main_type */
} schedinit_t;

struct schedinit_status {
	struct simple_barrier barrier;
	schedinit_t status;
	unsigned long init_core;
	struct slm_thd *initialization_thds[NUM_CPU];
};

static struct schedinit_status initialization_state[MAX_NUM_COMPS] = { 0 };

static void
create_static_thds(unsigned int coreid)
{	
	// Read from initargs.c
	struct initargs static_thds, curr_thd, params, clients, curr_client;
	struct initargs_iter i;
	int ret, cont, cont2;

	ret = args_get_entry("sys_virt_resources/sched", &static_thds);
	if(ret == 0) {
		for( cont = args_iter(&static_thds, &i, &curr_thd) ; cont ; cont = args_iter_next(&i, &curr_thd) ) {
			thdclosure_index_t idx;
			compid_t compid;
			struct slm_thd *t;
			sched_param_t param[4] = { 0 };
			char *id_str, *core_id_str, *comp_id_str, *prio_str, *period_us_str, *budget_us_str = NULL;
			int keylen, cnt = 0;

			// Get the thread idx
			id_str = args_get_from("id", &curr_thd);
			assert(id_str != NULL && atoi(id_str) > 0);
			idx = atoi(id_str);

			// Get the parameters
			ret = args_get_entry_from("params", &curr_thd, &params);
			assert(!ret);

			// Check if the thread is for the current core
			core_id_str = args_get_from("core", &params);
			assert(core_id_str != NULL && atoi(core_id_str) >= 0);
			if ((unsigned int)atoi(core_id_str) != coreid) continue;			
			
			prio_str = args_get_from("priority", &params);
			assert(prio_str != NULL);
			param[cnt++] = sched_param_pack(SCHEDP_PRIO, atoi(prio_str));

			period_us_str = args_get_from("period_us", &params);
			if (period_us_str != NULL) {
				param[cnt++] = sched_param_pack(SCHEDP_WINDOW, atoi(period_us_str));
			}

			budget_us_str = args_get_from("budget_us", &params);
			if (budget_us_str != NULL) {
				param[cnt++] = sched_param_pack(SCHEDP_BUDGET, atoi(budget_us_str));
			}
			param[cnt] = 0;
			
			//ret = args_get_entry_from("clients", &curr_thd, &clients);
			//assert(!ret);
			// There must be only one client
			//assert(args_len(&clients) == 1);
			ret = args_get_entry_from("client", &curr_thd, &curr_client);
			assert(!ret);
			// Get the component id
			comp_id_str = args_get_from("comp_id", &curr_client);
			assert(comp_id_str != NULL && atoi(comp_id_str) > 0);
			compid = atoi(comp_id_str);

			// idx values starts from COS_THD_INIT_REGION_SIZE
			// idx decremented by 1 in cos_component.c
			idx = idx + COS_THD_INIT_REGION_SIZE;
			assert(idx > COS_THD_INIT_REGION_SIZE);

			// Create the thread
			extern struct slm_thd *thd_alloc_in_static(compid_t id, thdclosure_index_t idx, sched_param_t *parameters);
			t = thd_alloc_in_static(compid, idx, param);
			assert(t);

			// Add the thread to the static_thds array
			// to wake up the thread when the client triggers the scheduler
			idx = atoi(id_str);
			assert(idx <= MAX_NUM_STATIC_THD_COMP);
			printc("\tScheduler %ld: Creating static thread %ld for component %ld\n", cos_compid(), idx, compid);
			static_thds_arr[compid][--idx] = t;	
		}
	}	
}

static void
component_initialize_next(compid_t cid)
{
	struct schedinit_status *s;

	printc("\tSched %ld: %ld is the %ldth component to initialize\n", cos_compid(), cid, init_schedule_off);
	init_schedule[init_schedule_off] = cid;
	init_schedule_off++;
	s = &initialization_state[cid];
	assert(s->status == SCHEDINIT_FREE);
	*s = (struct schedinit_status) {
		.status    = SCHEDINIT_INITING,
		.init_core = ~0,
		.initialization_thds = { 0 },
	};
	/* By default, lets assume that each component can initialize onto *all* cores */
	simple_barrier_init(&s->barrier, init_parallelism());

	return;
}

void
calculate_initialization_schedule(void)
{
	struct initargs exec_entries, curr;
	struct initargs_iter i;
	int ret, cont;

	ret = args_get_entry("execute", &exec_entries);
	assert(!ret);
	printc("\tSched %ld: %d components that need execution\n", cos_compid(), args_len(&exec_entries));
	for (cont = args_iter(&exec_entries, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		int      keylen;
		compid_t id        = atoi(args_key(&curr, &keylen));
		char    *exec_type = args_value(&curr);

		assert(exec_type);
		assert(id != cos_compid());

		/* Only init threads allowed */
		if (strcmp(exec_type, "init")) BUG();	/* TODO: no support for hierarchical scheduling yet */

		component_initialize_next(id);
	}

	return;
}

struct slm_thd *slm_thd_current_extern(void);

static __attribute__((noreturn)) void
exit_init_thd(void)
{
	struct slm_thd *current = slm_thd_current_extern();

	if (cos_coreid() == 0) printc("\tScheduler %ld: Exiting thread %ld from component %ld\n", cos_compid(), cos_thdid(), (compid_t)cos_inv_token());

	slm_cs_enter(current, SLM_CS_NONE);
	slm_thd_deinit(current);		/* I'm out! */
	slm_cs_exit(NULL, SLM_CS_NONE);

	/* Switch to the scheduler thread */
	if (cos_defswitch(BOOT_CAPTBL_SELF_INITTHD_CPU_BASE, TCAP_PRIO_MAX, TCAP_RES_INF, cos_sched_sync())) BUG();

	BUG();
	while (1) ;
}

void
init_done(int parallel_init, init_main_t cont)
{
	compid_t client = (compid_t)cos_inv_token();
	struct schedinit_status *s;

	s = &initialization_state[client];
	assert(s->status != SCHEDINIT_FREE);
	/*
	 * Set the initialization core as the first core to call
	 * `cos_init`, thus call `init_done` first. This is a little
	 * obtuse, but we're simply trying to set the `s->init_core`
	 * only if it is still set to `-1`. Thus the first core to try
	 * (and only the first core to try), should succeed.
	 */
	ps_cas(&s->init_core, ~0, cos_coreid());

	/*
	 * `init_done` should not be called once initialization is
	 * completed. This is an error.
	 */
	if (s->status == SCHEDINIT_MAIN) {
		exit_init_thd();
	}

	/*
	 * This should *ONLY* happen for the initialization thread
	 * *after* it executes `cos_init`.
	 */
	if (s->status == SCHEDINIT_INITING ) {
		/*
		 * Assumption: here we're moving on to parallel
		 * initialization regardless if it is requested or
		 * not. We *know* that we've created all parallel
		 * threads, and that the client will avoid calling the
		 * `parallel_init` function if it isn't requested.
		 */

		/* Continue with parallel initialization... */
		ps_store(&s->status, SCHEDINIT_PARINIT);

		return;
	}

	/*
 	 * If this barrier is hit *after* the parallel initialization
	 * has finished, no blocking will occur as its count has
	 * already been hit.
	 */
	simple_barrier(&s->barrier);
	s->status = SCHEDINIT_MAIN;

	s->initialization_thds[cos_coreid()] = slm_thd_current_extern();
	extern int thd_block(void);
	thd_block(); 		/* block until initialization is completed */

 	/*
	 * After initialization, we're done with the parallel threads
	 * in some cases.
	 */
	if ((cos_coreid() != s->init_core && cont == INIT_MAIN_SINGLE) || cont == INIT_MAIN_NONE) {
		exit_init_thd();
	}

	/*
	 * We've moved on to execute main, are in the initialization
	 * thread, or are in a parallel thread, and are to do parallel
	 * main execution. We'd expect the next API call here to be
	 * `init_exit`.
	 */

	return;
}

void
init_exit(int retval)
{
	exit_init_thd();
}

/**
 * This executes in the *idle thread* before the normal idle
 * processing. Thus, it runs as the lowest priority.
 */
static void
slm_comp_init_loop(void)
{
	unsigned long init_schedule_current = 0, i;
	struct slm_thd *current;

	/* Allocate static threads for current core */
	create_static_thds(cos_coreid());

	if (cos_coreid() == 0) printc("Scheduler %ld: Running initialization thread.\n", cos_compid());
	/* If there are more components to initialize */
	while (init_schedule_current != ps_load(&init_schedule_off)) {
		/* Which is the next component to initialize? */
		compid_t client = init_schedule[init_schedule_current];
		struct schedinit_status *n;
		struct slm_thd *t;
		sched_param_t param[2];

		param[0] = sched_param_pack(SCHEDP_INIT, 0);
		param[1] = 0;

		/* Create the thread for initialization of the next component */
		extern struct slm_thd *thd_alloc_in(compid_t id, thdclosure_index_t idx, sched_param_t *parameters, int reschedule);
		t = thd_alloc_in(client, 0, param, 1);
		assert(t);

		n = &initialization_state[client];
		init_schedule_current++;

		if (cos_coreid() == 0)	printc("\tScheduler %ld: initializing component %ld with thread %ld.\n", cos_compid(), client, t->tid);
		/*
		 * This waits till init_done effective runs before
		 * moving on. We need to be highest-priority, so that
		 * we can direct switch to the initialization thread
		 * here.
		 *
		 * If the initial thread blocked, we may execute. Lets
		 * just wait for it to finish! We're in the idle
		 * thread, so the spin isn't really wasting many
		 * resources.
		 */
		while (ps_load(&n->initialization_thds[cos_coreid()]) == NULL) ;
 	}

	if (cos_coreid() == 0) printc("Scheduler %ld, initialization completed.\n", cos_compid());

	/*
	 * We want to *atomically* awaken all of the threads that will
	 * go on to execute `parallel_main`s, thus we maintain the
	 * critical section while awaking them all.
	 */
	slm_cs_enter(slm_thd_special(), SLM_CS_NONE);
	for (i = 0; i < ps_load(&init_schedule_off); i++) {
		 compid_t client = init_schedule[i];
		struct slm_thd *t;

		t = initialization_state[client].initialization_thds[cos_coreid()];
		assert(t != NULL);

		slm_thd_wakeup(t, 0);
	}
	slm_cs_exit_reschedule(slm_thd_special(), SLM_CS_NONE);

	return;
}

void
slm_idle_comp_initialization(void)
{
	slm_comp_init_loop();
}
