/* 
 * Exports called by instruction feeder.
 * Main entry point for simulated instructions.
 * Copyright, Svilen Kanev, 2011
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/io.h>

#include <map>

#include "interface.h"
#include "callbacks.h"
#include "host.h"
#include "misc.h"
#include "machine.h"
#include "endian.h"
#include "version.h"
#include "options.h"
#include "stats.h"
#include "sim.h"
#include "synchronization.h"

#include "zesto-core.h"
#include "zesto-fetch.h"
#include "zesto-oracle.h"
#include "zesto-decode.h"
#include "zesto-bpred.h"
#include "zesto-alloc.h"
#include "zesto-exec.h"
#include "zesto-commit.h"
#include "zesto-dram.h"
#include "zesto-uncore.h"
#include "zesto-MC.h"
#include "zesto-power.h"

#include "zesto-repeater.h"

extern void sim_main_slave_pre_pin(int coreID);
extern void sim_main_slave_pre_pin();
extern void sim_main_slave_post_pin(int coreID);
extern void sim_main_slave_post_pin(void);
extern bool sim_main_slave_fetch_insn(int coreID);

/* options database */
extern struct opt_odb_t *sim_odb;

/* stats database */
extern struct stat_sdb_t *sim_sdb;

/* power stats database */
extern struct stat_sdb_t *rtp_sdb;

/* redirected program/simulator output file names */
extern const char *sim_simout;
extern const char *sim_progout;
extern FILE *sim_progfd;

/* track first argument orphan, this is the program to execute */
extern int exec_index;

/* dump help information */
extern bool help_me;

/* random number generator seed */
extern int rand_seed;

/* default simulator scheduling priority */
#define NICE_DEFAULT_VALUE      0

extern int orphan_fn(int i, int argc, char **argv);
extern void banner(FILE *fd, int argc, char **argv);
extern  void usage(FILE *fd, int argc, char **argv);

extern bool sim_slave_running;

extern void sim_print_stats(FILE *fd);

extern void start_slice(unsigned int slice_num);
extern void end_slice(unsigned int slice_num, unsigned long long slice_length, unsigned long long slice_weight_times_1000);
extern void scale_all_slices(void);

extern int min_coreID;

int
Zesto_SlaveInit(int argc, char **argv)
{
  char *s;
  int i;

  /* register an error handler */
  fatal_hook(sim_print_stats);

  sim_pre_init();

  /* register global options */
  sim_odb = opt_new(orphan_fn);
  opt_reg_flag(sim_odb, "-h", "print help message",
           &help_me, /* default */FALSE, /* !print */FALSE, NULL);

#ifdef DEBUG
  opt_reg_flag(sim_odb, "-d", "enable debug message",
           &debugging, /* default */FALSE, /* !print */FALSE, NULL);
#endif /* DEBUG */
  opt_reg_int(sim_odb, "-seed",
          "random number generator seed (0 for timer seed)",
          &rand_seed, /* default */1, /* print */TRUE, NULL);
  opt_reg_flag(sim_odb, "-ignore_notes", "suppresses printing of notes",
           &opt_ignore_notes, /* default */FALSE, /* !print */FALSE, NULL);

  /* stdio redirection options */
  opt_reg_string(sim_odb, "-redir:sim",
         "redirect simulator output to file (non-interactive only)",
         &sim_simout,
         /* default */NULL, /* !print */FALSE, NULL);
  opt_reg_string(sim_odb, "-redir:prog",
         "redirect simulated program output to file",
         &sim_progout, /* default */NULL, /* !print */FALSE, NULL);

  /* register all simulator-specific options */
  sim_reg_options(sim_odb);

  /* parse simulator options */
  exec_index = -1;
  opt_process_options(sim_odb, argc, argv);

  /* redirect I/O? */
  if (sim_simout != NULL)
    {
      /* send simulator non-interactive output (STDERR) to file SIM_SIMOUT */
      fflush(stderr);
      if (!freopen(sim_simout, "w", stderr))
        fatal("unable to redirect simulator output to file `%s'", sim_simout);
    }

  if (sim_progout != NULL)
    {
      /* redirect simulated program output to file SIM_PROGOUT */
      sim_progfd = fopen(sim_progout, "w");
      if (!sim_progfd)
        fatal("unable to redirect program output to file `%s'", sim_progout);
    }

  /* need at least two argv values to run */
  if (argc < 2)
    {
      banner(stderr, argc, argv);
      usage(stderr, argc, argv);
      exit(1);
    }

  /* opening banner */
  banner(stderr, argc, argv);

  if (help_me)
    {
      /* print help message and exit */
      usage(stderr, argc, argv);
      exit(1);
    }

  /* seed the random number generator */
  if (rand_seed == 0)
    {
      /* seed with the timer value, true random */
      srand(time((time_t *)NULL));
    }
  else
    {
      /* seed with default or user-specified random number generator seed */
      srand(rand_seed);
    }

  /* exec_index is set in orphan_fn() */
  if (exec_index == -1)
    {
      /* executable was not found */
      fprintf(stderr, "error: no executable specified\n");
      usage(stderr, argc, argv);
      exit(1);
    }
  /* else, exec_index points to simulated program arguments */

  /* check simulator-specific options */
  sim_check_options(sim_odb, argc, argv);

  /* initialize the instruction decoder */
  md_init_decoder();

  /* initialize all simulation modules */
  sim_post_init();

  /* register all simulator stats */
  sim_sdb = stat_new();
  sim_reg_stats(threads,sim_sdb);

  /* stat database for power computation */
  rtp_sdb = stat_new();
  sim_reg_stats(threads,rtp_sdb);
  stat_save_stats(rtp_sdb);

  /* record start of execution time, used in rate stats */
  time_t sim_start_time = time((time_t *)NULL);

  /* emit the command line for later reuse */
  fprintf(stderr, "sim: command line: ");
  for (i=0; i < argc; i++)
    fprintf(stderr, "%s ", argv[i]);
  fprintf(stderr, "\n");

  /* output simulation conditions */
  s = ctime(&sim_start_time);
  if (s[strlen(s)-1] == '\n')
    s[strlen(s)-1] = '\0';
  fprintf(stderr, "\nsim: simulation started @ %s, options follow:\n", s);
  opt_print_options(sim_odb, stderr, /* short */TRUE, /* notes */TRUE);
  fprintf(stderr, "\n");

  if(cores[0]->knobs->power.compute)
    init_power();

  sim_slave_running = true;

  //XXX: Zeroth cycle pre_pin missing in parallel version here. There's a good chance we don't need it though.
  /* return control to Pin and wait for first instruction */
  return 0;
}

void Zesto_SetBOS(int coreID, unsigned int stack_base)
{
  assert(coreID < num_cores);

  cores[coreID]->current_thread->memory.stack_base = (md_addr_t)stack_base;
  //  fprintf(stderr, "Stack base[%d]: %x; \n", coreID, cores[coreID]->current_thread->memory.stack_base);

}

int Zesto_Notify_Mmap(int coreID, unsigned int addr, unsigned int length, bool mod_brk)
{
  assert(coreID < num_cores);
  class core_t* core = cores[coreID];
  struct mem_t * mem = core->current_thread->mem;
  zesto_assert((num_cores == 1) || multi_threaded, 0);

  md_addr_t page_addr = ROUND_DOWN((md_addr_t)addr, MD_PAGE_SIZE);
  unsigned int page_length = ROUND_UP(length, MD_PAGE_SIZE);

  lk_lock(&memory_lock, coreID+1);
  md_addr_t retval = mem_newmap2(mem, page_addr, page_addr, page_length, 1);
  lk_unlock(&memory_lock);

  ZPIN_TRACE(coreID, "New memory mapping at addr: %x, length: %x ,endaddr: %x \n",addr, length, addr+length);

  bool success = (retval == addr);
  zesto_assert(success, 0);

  if(mod_brk && page_addr > core->current_thread->memory.brk_point)
    core->current_thread->memory.brk_point = page_addr + page_length;

  return success;
}

int Zesto_Notify_Munmap(int coreID, unsigned int addr, unsigned int length, bool mod_brk)
{
  assert(coreID < num_cores);
  class core_t *core = cores[coreID];
  struct mem_t * mem = cores[coreID]->current_thread->mem;
  zesto_assert((num_cores == 1) || multi_threaded, 0);

  lk_lock(&memory_lock, coreID+1);
  mem_delmap(mem, ROUND_UP((md_addr_t)addr, MD_PAGE_SIZE), length);
  lk_unlock(&memory_lock);

  ZPIN_TRACE(coreID, "Memory un-mapping at addr: %x, len: %x\n",addr, length);

  return 1;
}

void Zesto_UpdateBrk(int coreID, unsigned int brk_end, bool do_mmap)
{
  assert(coreID < num_cores);
  struct core_t * core = cores[coreID];

  zesto_assert(brk_end != 0, (void)0);

  if(do_mmap)
  {
    unsigned int old_brk_end = core->current_thread->memory.brk_point;

    if(brk_end > old_brk_end)
      Zesto_Notify_Mmap(coreID, ROUND_UP(old_brk_end, MD_PAGE_SIZE), 
                        ROUND_UP(brk_end - old_brk_end, MD_PAGE_SIZE), false);
    else if(brk_end < old_brk_end)
      Zesto_Notify_Munmap(coreID, ROUND_UP(brk_end, MD_PAGE_SIZE),
                          ROUND_UP(old_brk_end - brk_end, MD_PAGE_SIZE), false);
  }

  core->current_thread->memory.brk_point = brk_end;
}

void Zesto_Destroy()
{
  sim_slave_running = false;

  /* scale stats if running multiple simulation slices */
  scale_all_slices();

  /* print simulator stats */
  sim_print_stats(stderr);
  if(cores[0]->knobs->power.compute)
  {
    stat_save_stats(sim_sdb);
    compute_power(sim_sdb, true);
    deinit_power();
  }

  repeater_shutdown(cores[0]->knobs->exec.repeater_opt_str);

  for(int i=0; i<num_cores; i++)
    if(cores[i]->stat.oracle_unknown_insn / (double) cores[i]->stat.oracle_total_insn > 0.02)
      fprintf(stderr, "WARNING: [%d] More than 2%% instructions turned to NOPs (%lld out of %lld)\n",
              i, cores[i]->stat.oracle_unknown_insn, cores[i]->stat.oracle_total_insn);
}


void deactivate_core(int coreID)
{
  assert(coreID >= 0 && coreID < num_cores);
  ZPIN_TRACE(coreID, "deactivate %d\n", coreID);
  lk_lock(&cycle_lock, coreID+1);
  cores[coreID]->current_thread->active = false;
  cores[coreID]->current_thread->last_active_cycle = cores[coreID]->sim_cycle;
  int i;
  for (i=0; i < num_cores; i++)
    if (cores[i]->current_thread->active) {
      min_coreID = i;
      break;
    }
  if (i == num_cores)
    min_coreID = MAX_CORES;
  lk_unlock(&cycle_lock);
}

void activate_core(int coreID)
{
  assert(coreID >= 0 && coreID < num_cores);
  ZPIN_TRACE(coreID, "activate %d\n", coreID);
  lk_lock(&cycle_lock, coreID+1);
  cores[coreID]->current_thread->finished_cycle = false; // Make sure master core will wait
  cores[coreID]->exec->update_last_completed(cores[coreID]->sim_cycle);
  cores[coreID]->current_thread->active = true;
    if (coreID < min_coreID)
      min_coreID = coreID;
  lk_unlock(&cycle_lock);
}

bool is_core_active(int coreID)
{
  assert(coreID >= 0 && coreID < num_cores);
  bool result;
  lk_lock(&cycle_lock, coreID+1);
  result = cores[coreID]->current_thread->active;
  lk_unlock(&cycle_lock);
  return result;
}

void sim_drain_pipe(int coreID)
{
   struct core_t * core = cores[coreID];

   /* Just flush anything left */
   core->oracle->complete_flush();
   core->commit->recover();
   core->exec->recover();
   core->alloc->recover();
   core->decode->recover();
   core->fetch->recover(core->current_thread->regs.regs_NPC);

   if (core->memory.mem_repeater)
     core->memory.mem_repeater->flush(NULL);

   // Do this after fetch->recover, since the latest Mop might have had a rep prefix
   core->current_thread->rep_sequence = 0;
}

void Zesto_Slice_Start(unsigned int slice_num)
{
  start_slice(slice_num);
}

void Zesto_Slice_End(int coreID, unsigned int slice_num, unsigned long long feeder_slice_length, unsigned long long slice_weight_times_1000)
{
  // Blow away any instructions executing
  if (is_core_active(coreID))
    sim_drain_pipe(coreID);

  // Record stats values
  end_slice(slice_num, feeder_slice_length, slice_weight_times_1000);
}

void Zesto_Resume(int coreID, handshake_container_t* handshake)
{
   assert(coreID >= 0 && coreID < num_cores);
   struct core_t * core = cores[coreID];
   thread_t * thread = core->current_thread;
   bool slice_start = handshake->flags.isFirstInsn;
   bool slice_end = handshake->flags.isLastInsn;


   if (!thread->active && !(slice_start || handshake->handshake.resume_thread ||
                            handshake->handshake.sleep_thread || handshake->handshake.flush_pipe)) {
     fprintf(stderr, "DEBUG DEBUG: Start/stop out of sync? %d PC: %x\n", coreID, handshake->handshake.pc);
     if(sim_release_handshake)
       ReleaseHandshake(coreID);
     return;
   }

   zesto_assert(core->oracle->num_Mops_before_feeder() == 0, (void)0);
   zesto_assert(!core->oracle->spec_mode, (void)0);

   if (handshake->handshake.sleep_thread)
   {
      deactivate_core(coreID);
      if(sim_release_handshake)
        ReleaseHandshake(coreID);
      return;
   }

   if (handshake->handshake.resume_thread)
   {
      activate_core(coreID);
      if(sim_release_handshake)
        ReleaseHandshake(coreID);
      return;
   }

   if(handshake->handshake.flush_pipe) {
      sim_drain_pipe(coreID);
      if(sim_release_handshake)
        ReleaseHandshake(coreID);
      return;
   }

   // XXX: Check if this is called and remove unnecessary fields from P2Z
   if(slice_end)
   {
      Zesto_Slice_End(coreID, handshake->handshake.slice_num, handshake->handshake.feeder_slice_length, handshake->handshake.slice_weight_times_1000);

      if(!slice_start) {//start and end markers can be the same
        if(sim_release_handshake)
          ReleaseHandshake(coreID);
        return;
      } else
        activate_core(coreID);
   }

   if(slice_start)
   {
      zesto_assert(thread->memory.stack_base, (void)0);

      /* Init stack pointer */
      md_addr_t sp = handshake->handshake.ctxt.regs_R.dw[MD_REG_ESP]; 
      thread->memory.stack_min = (md_addr_t)sp;

      /* Create local pages for stack */ 
      md_addr_t page_start = ROUND_DOWN(sp, MD_PAGE_SIZE);
      md_addr_t page_end = ROUND_UP(thread->memory.stack_base, MD_PAGE_SIZE);

      lk_lock(&memory_lock, coreID+1);
      md_addr_t stack_addr = mem_newmap2(thread->mem, page_start, page_start, page_end-page_start, 1);
      lk_unlock(&memory_lock);
      fprintf(stderr, "Stack pointer: %x; \n", sp);
      zesto_assert(stack_addr == ROUND_DOWN(thread->memory.stack_min, MD_PAGE_SIZE), (void)0);


      thread->regs.regs_PC = handshake->handshake.pc;
      thread->regs.regs_NPC = handshake->handshake.pc;
      core->fetch->PC = handshake->handshake.pc;
   }

   if(thread->first_insn) 
   {  
      thread->first_insn= false;
   }

   // Let the oracle grab any arch state it needs
   core->oracle->grab_feeder_state(handshake, true, !slice_start);

   thread->fetches_since_feeder = 0;
   md_addr_t NPC = handshake->handshake.brtaken ? handshake->handshake.tpc : handshake->handshake.npc;  
   ZPIN_TRACE(coreID, "PIN -> PC: %x, NPC: %x \n", handshake->handshake.pc, NPC);

   // The handshake can be recycled now
   if(sim_release_handshake)
     ReleaseHandshake(coreID);

   bool fetch_more = true;
   thread->consumed = false;

   /* XXX: This logic can be simplified significantly now that we have
    * the shadow_MopQ. Redo.
    */
   while(!thread->consumed || core->oracle->num_Mops_before_feeder() > 0)
   {
     fetch_more = sim_main_slave_fetch_insn(coreID);
     thread->fetches_since_feeder++;

     if(core->oracle->num_Mops_before_feeder() > 0)
     {
       while(fetch_more && core->oracle->num_Mops_before_feeder() > 0 &&
             !core->oracle->spec_mode)
       {       
         fetch_more = sim_main_slave_fetch_insn(coreID);
         thread->fetches_since_feeder++;

         //Fetch can get more insns this cycle, but they are needed from PIN
         if(fetch_more && core->oracle->num_Mops_before_feeder() == 0
                       && !core->oracle->spec_mode
                       && core->fetch->PC == NPC
                       && core->fetch->PC == thread->regs.regs_NPC)
         {
            zesto_assert(core->fetch->PC == NPC, (void)0);
            return;
         }
       }

       //Fetch can get more insns this cycle, but not on nuke path 
       if(fetch_more)
       {
          thread->consumed = false;
          continue;
       }
      
       sim_main_slave_post_pin(coreID);

       sim_main_slave_pre_pin(coreID);
       fetch_more = true;

       if(core->oracle->num_Mops_before_feeder() == 0)
       {
         //Nuke recovery instruction is a mispredicted branch
         if(core->fetch->PC != NPC || thread->regs.regs_NPC != NPC)
         {
            ZPIN_TRACE(coreID, "Going from nuke loop to spec loop. PC: %x\n",core->fetch->PC);
            thread->consumed = false;
            continue;
         }
         else //fetching from the correct addres, go back to Pin for instruction
         {
            zesto_assert(core->fetch->PC == NPC, (void)0);
            return;
         }
       }

     }
     /*XXX: here oracle still doesn't know if we're speculating or not. But if we predicted 
     the wrong path, we'd better not return to Pin, because that will mess the state up */
     else if(core->fetch->PC != NPC || core->oracle->spec_mode)
     {
       bool spec = false;
       do
       {
         while(fetch_more) 
         {
            fetch_more = sim_main_slave_fetch_insn(coreID);
            thread->fetches_since_feeder++;

            spec = (core->oracle->spec_mode || (core->fetch->PC != thread->regs.regs_NPC));
            /* If fetch can tolerate more insns, but needs to get them from PIN */
            if(fetch_more && !spec && core->oracle->num_Mops_before_feeder() == 0)
            {
               zesto_assert(core->fetch->PC == NPC, (void)0);
               return;
            }
         }
        
         sim_main_slave_post_pin(coreID);

         /* Next cycle */ 
         sim_main_slave_pre_pin(coreID);

         if(!thread->consumed)
         {
            fetch_more = true;
            continue;
         }

         /* Potentially different after exec (in pre_pin) where branches are resolved */
         spec = (core->oracle->spec_mode || (core->fetch->PC != thread->regs.regs_NPC));

         /* After recovering from spec, we find no nukes -> great, get control back to PIN */
         if(!spec && core->oracle->num_Mops_before_feeder() == 0)
         {
            //fprintf(stderr, "AAA\n");
            //while(1);
            zesto_assert(core->fetch->PC == NPC, (void)0);
            return;
         }

         /* After recovering from spec, nuke -> go to nuke recovery loop */
         if(!spec && core->oracle->num_Mops_before_feeder() > 0)
         {
            ZPIN_TRACE(coreID, "Going from spec loop to nuke loop. PC: %x\n",core->fetch->PC);
            break;
         }

         /* All other cases should stay in this loop until they get resolved */
         fetch_more = true;

       } while(spec);

     }
     else
     /* non-speculative, non-nuke */
     {
       /* Pass control back to Pin to get a new PC on the same cycle*/
       if(fetch_more)
       {
          zesto_assert(core->fetch->PC == NPC, (void)0);
          return;
       }
    
       sim_main_slave_post_pin(coreID);

       /* This is already next cycle, up to fetch */
       sim_main_slave_pre_pin(coreID);
     }
   }
   zesto_assert(core->fetch->PC == NPC, (void)0);
}

void Zesto_WarmLLC(unsigned int addr, bool is_write)
{
  int threadID = 0;
  struct core_t * core = cores[threadID];

  enum cache_command cmd = is_write ? CACHE_WRITE : CACHE_READ;
  md_paddr_t paddr = v2p_translate_safe(threadID,addr);
  if(!cache_is_hit(uncore->LLC,cmd,paddr,core))
  {
    struct cache_line_t * p = cache_get_evictee(uncore->LLC,paddr,core);
    p->dirty = p->valid = false;
    cache_insert_block(uncore->LLC,cmd,paddr,core);
  }
}
