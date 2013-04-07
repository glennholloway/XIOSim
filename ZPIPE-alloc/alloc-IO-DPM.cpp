/* alloc-IO-DPM.cpp - Detailed In-Order Pipeline Model */
/*
 * Derived from Zesto OO model
 * Svilen Kanev, 2011
 */


#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(alloc_opt_string,"IO-DPM"))
    return new core_alloc_IO_DPM_t(core);
#else

#include <list>
using namespace std;
    
class core_alloc_IO_DPM_t:public core_alloc_t
{
  enum alloc_stall_t {ASTALL_NONE,   /* no stall */
                      ASTALL_EMPTY,
                      ASTALL_ROB,
                      ASTALL_LDQ,
                      ASTALL_STQ,
                      ASTALL_RS,
                      ASTALL_DRAIN,
                      ASTALL_WAIT_EXEC,
                      ASTALL_num
                     };

  public:

  core_alloc_IO_DPM_t(struct core_t * const core);
  virtual void reg_stats(struct stat_sdb_t * const sdb);

  virtual void step(void);
  virtual void recover(void);
  virtual void recover(const struct Mop_t * const Mop);

  virtual void RS_deallocate(const struct uop_t * const uop);
  virtual void start_drain(void); /* prevent allocation from proceeding to exec */

  protected:

  struct uop_t *** pipe;
  int * occupancy;
  /* for load-balancing port binding */
  int * port_loading;
  bool * can_alloc;

  static const char *alloc_stall_str[ASTALL_num];
  bool oldest_in_alloc(struct uop_t * uop);
};

const char *core_alloc_IO_DPM_t::alloc_stall_str[ASTALL_num] = {
  "no stall                   ",
  "no uops to allocate        ",
  "ROB is full                ",
  "LDQ is full                ",
  "STQ is full                ",
  "RS is full                 ",
  "OOO core draining          ",
  "waiting for IO exec        "
};

/*******************/
/* SETUP FUNCTIONS */
/*******************/

core_alloc_IO_DPM_t::core_alloc_IO_DPM_t(struct core_t * const arg_core)
{
  struct core_knobs_t * knobs = arg_core->knobs;
  core = arg_core;
  int i;

  if(knobs->alloc.depth <= 0)
    fatal("allocation pipeline depth must be > 0");
  if(knobs->alloc.width <= 0)
    fatal("allocation pipeline width must be > 0");

  pipe = (struct uop_t***) calloc(knobs->alloc.depth,sizeof(*pipe));
  if(!pipe)
    fatal("couldn't calloc alloc pipe");

  for(i=0;i<knobs->alloc.depth;i++)
  {
    pipe[i] = (struct uop_t**) calloc(knobs->alloc.width,sizeof(**pipe));
    if(!pipe[i])
      fatal("couldn't calloc alloc pipe stage");
  }

  occupancy = (int*) calloc(knobs->alloc.depth,sizeof(*occupancy));
  if(!occupancy)
    fatal("couldn't calloc alloc occupancy array");

  port_loading = (int*) calloc(knobs->exec.num_exec_ports,sizeof(*port_loading));
  if(!port_loading)
    fatal("couldn't calloc allocation port-loading scoreboard");

  can_alloc = (bool*) calloc(knobs->exec.num_exec_ports,sizeof(*can_alloc));
  if(!can_alloc)
    fatal("couldn't calloc can_alloc array");
}

void
core_alloc_IO_DPM_t::reg_stats(struct stat_sdb_t * const sdb)
{
  char buf[1024];
  char buf2[1024];
  struct thread_t * arch = core->current_thread;

  stat_reg_note(sdb,"#### ALLOC STATS ####");
  sprintf(buf,"c%d.alloc_insn",arch->id);
  stat_reg_counter(sdb, true, buf, "total number of instructions alloced", &core->stat.alloc_insn, 0, TRUE, NULL);
  sprintf(buf,"c%d.alloc_uops",arch->id);
  stat_reg_counter(sdb, true, buf, "total number of uops alloced", &core->stat.alloc_uops, 0, TRUE, NULL);
  sprintf(buf,"c%d.alloc_eff_uops",arch->id);
  stat_reg_counter(sdb, true, buf, "total number of effective uops alloced", &core->stat.alloc_eff_uops, 0, TRUE, NULL);
  sprintf(buf,"c%d.alloc_IPC",arch->id);
  sprintf(buf2,"c%d.alloc_insn/c%d.sim_cycle",arch->id,arch->id);
  stat_reg_formula(sdb, true, buf, "IPC at alloc", buf2, NULL);
  sprintf(buf,"c%d.alloc_uPC",arch->id);
  sprintf(buf2,"c%d.alloc_uops/c%d.sim_cycle",arch->id,arch->id);
  stat_reg_formula(sdb, true, buf, "uPC at alloc", buf2, NULL);
  sprintf(buf,"c%d.alloc_euPC",arch->id);
  sprintf(buf2,"c%d.alloc_eff_uops/c%d.sim_cycle",arch->id,arch->id);
  stat_reg_formula(sdb, true, buf, "effective uPC at alloc", buf2, NULL);

  sprintf(buf,"c%d.regfile_reads",arch->id);
  stat_reg_counter(sdb, true, buf, "number of register file reads", &core->stat.regfile_reads, 0, TRUE, NULL);
  sprintf(buf,"c%d.fp_regfile_reads",arch->id);
  stat_reg_counter(sdb, true, buf, "number of fp refister file reads", &core->stat.fp_regfile_reads, 0, TRUE, NULL);

  sprintf(buf,"c%d.alloc_stall",core->current_thread->id);
  core->stat.alloc_stall = stat_reg_dist(sdb, buf,
                                          "breakdown of stalls at alloc",
                                          /* initial value */0,
                                          /* array size */ASTALL_num,
                                          /* bucket size */1,
                                          /* print format */(PF_COUNT|PF_PDF),
                                          /* format */NULL,
                                          /* index map */alloc_stall_str,
                                          /* scale_me */TRUE,
                                          /* print fn */NULL);
}

/************************/
/* MAIN ALLOC FUNCTIONS */
/************************/
bool core_alloc_IO_DPM_t::oldest_in_alloc(struct uop_t * uop)
{
  struct core_knobs_t * knobs = core->knobs;

  for(int stage=knobs->alloc.depth-1; stage>=0; stage--)
   for(int i=0; i<knobs->alloc.width; i++)
     if(pipe[stage][i] && pipe[stage][i]->decode.uop_seq < 
         uop->decode.uop_seq)
         return false;

  return true;

}

void core_alloc_IO_DPM_t::step(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int stage, i;
  enum alloc_stall_t stall_reason = ASTALL_NONE;


  list<struct uop_t *> alloced_uops;
  list<struct uop_t *>::iterator it;
  /*========================================================================*/
  /*== Dispatch insts to the appropriate execution ports ==*/
  stage = knobs->alloc.depth-1;
  if(occupancy[stage]) /* are there uops in the last stage of the alloc pipe? */
  {
    for(i=0; i < knobs->alloc.width; i++) /* if so, scan all slots (width) of this stage */
    {
       struct uop_t * uop = pipe[stage][i];
       if(uop)
       {
          for(it=alloced_uops.begin(); it!=alloced_uops.end(); it++)
          {
             if((*it)->decode.uop_seq > uop->decode.uop_seq)
               break;
          }
          alloced_uops.insert(it, uop);
       }
    }

    for(it=alloced_uops.begin(); it!=alloced_uops.end(); it++)
    {
      struct uop_t * uop = *it;
      i = -1;
      for (int j=0; j < knobs->alloc.width; j++)
        for (int k=0; k < knobs->alloc.depth; k++)
          if (pipe[k][j] == uop) {
            i = j;
            break;
          }
      zesto_assert(i != -1, (void)0);
      int abort_alloc = false;

      if(!oldest_in_alloc(uop))
      {
         abort_alloc = true;
         break;
      }


      /* if using drain flush: */
      /* is the back-end still draining from a misprediction recovery? */

      if(knobs->alloc.drain_flush && drain_in_progress)
      {
        if(!core->commit->pipe_empty() || !core->exec->exec_empty())
        {
          stall_reason = ASTALL_DRAIN;
          break;
        }
        else
          drain_in_progress = false;
      }

      if(uop)
      {
        while(uop) /* this while loop is to handle multiple uops fused together into the same slot */
        {
          if(uop->timing.when_allocated == TICK_T_MAX)
          {
            /* all store uops had better be marked is_std */
            zesto_assert((!(uop->decode.opflags & F_STORE)) || uop->decode.is_std,(void)0);
            zesto_assert((!(uop->decode.opflags & F_LOAD)) || uop->decode.is_load,(void)0);

            /* port bindings */

            /* fused instructions go together as atomic ops */
            if(uop->decode.in_fusion && !uop->decode.is_fusion_head)
               uop->alloc.port_assignment = uop->decode.fusion_head->alloc.port_assignment;
            else
            {
              /* assign uop to least loaded port */
              int min_load = INT_MAX;
              int index = -1;
              int j;
              struct uop_t * exec_uop = uop;
              for(j=0;j<knobs->exec.num_exec_ports;j++)
                 can_alloc[j] = true;

              /* fused uops should all go toghether - we look for a port that can execute the whole fusion */
              while(exec_uop)
              {
                /* nops and traps can issue everywhere (we alloc them to keep program order */
                if(exec_uop->decode.is_nop || exec_uop->Mop->decode.is_trap)
                {
                  zesto_assert(exec_uop->decode.FU_class == FU_NA, (void)0); 
                }
                else
                {
                  /* step through all exec ports and find if exec_uop can issue there; if not, we can't issue the whole fussion there */
                  for(j=0;j<knobs->exec.num_exec_ports;j++)
                  {
                    bool port_possible = false;
                    for(int k=0;k<knobs->exec.port_binding[exec_uop->decode.FU_class].num_FUs;k++)
                    {
                      if(knobs->exec.port_binding[exec_uop->decode.FU_class].ports[k] == j)
                      {
                        port_possible = true;
                        break;
                      }
                    }
                    can_alloc[j] &= port_possible;
                  }
                }


                if(exec_uop->decode.in_fusion)
                  exec_uop = exec_uop->decode.fusion_next;
                else
                  exec_uop = NULL;                  
              }

              for(j=0;j<knobs->exec.num_exec_ports;j++)
              {
                 if(can_alloc[j] && core->exec->port_available(j) && port_loading[j] < min_load)
                 {
                    min_load = port_loading[j];
                    index = j;
                 }
              }

              /* no available port */
              if(index == -1)
              {
                 stall_reason = ASTALL_WAIT_EXEC;
                 abort_alloc = true;
                 break;		                       
              }
              uop->alloc.port_assignment = index;
            }
            port_loading[uop->alloc.port_assignment]++;


            //SK - add loads to LDQ here (not in issue pipe anymore)
            if(uop->decode.is_load)
            {
              /* due to IO pipe, LDQ should always be available */
              zesto_assert(core->exec->LDQ_available(), (void)0);
              zesto_assert(uop->alloc.LDQ_index == -1, (void)0);

              core->exec->LDQ_insert(uop);
            }

            /* only allocate for non-fused or fusion-head */
            if((!uop->decode.in_fusion) || uop->decode.is_fusion_head)
              core->exec->exec_insert(uop);
            else
              core->exec->exec_fuse_insert(uop);

            /* Get input mappings - this is a proxy for explicit register numbers, which
               you can always get from idep_uop->alloc.ROB_index */
            for(int j=0;j<MAX_IDEPS;j++)
            {
              /* This use of oracle info is valid: at this point the processor would be
                 looking up this information in the RAT, but this saves us having to
                 explicitly store/track the RAT state. */
              uop->exec.idep_uop[j] = uop->oracle.idep_uop[j];

              /* Add self onto parent's output list.  This output list doesn't
                 have a real microarchitectural counter part, but it makes the
                 simulation faster by not having to perform a whole mess of
                 associative searches each time any sort of broadcast is needed.
                 The parent's odep list only points to uops which have dispatched
                 into the OOO core (i.e. has left the alloc pipe). */
              if(uop->exec.idep_uop[j])
              {
                struct odep_t * odep = core->get_odep_link();
                odep->next = uop->exec.idep_uop[j]->exec.odep_uop;
                uop->exec.idep_uop[j]->exec.odep_uop = odep;
                odep->uop = uop;
                odep->aflags = (uop->decode.idep_name[j] == DCREG(MD_REG_AFLAGS));
                odep->op_num = j;
              }
            }

            /* Update read stats */
            for(int j=0;j<MAX_IDEPS;j++)
            {
              if(REG_IS_GPR(uop->decode.idep_name[j]))
                core->stat.regfile_reads++;
              else if(REG_IS_FPR(uop->decode.idep_name[j]))
                core->stat.fp_regfile_reads++;
            }

            /* check "scoreboard" for operand readiness (we're not actually
               explicitly implementing a scoreboard); if value is ready, read
               it into data-capture window or payload RAM. */
            tick_t when_ready = 0;
            for(int j=0;j<MAX_IDEPS;j++) /* for possible input argument */
            {
              if(uop->exec.idep_uop[j]) /* if the parent uop exists (i.e., still in the processor) */
              {
                uop->timing.when_itag_ready[j] = uop->exec.idep_uop[j]->timing.when_otag_ready;
                if(uop->exec.idep_uop[j]->exec.ovalue_valid)
                {
                  uop->timing.when_ival_ready[j] = uop->exec.idep_uop[j]->timing.when_completed;
                  uop->exec.ivalue_valid[j] = true;
                  if(uop->decode.idep_name[j] == DCREG(MD_REG_AFLAGS))
                    uop->exec.ivalue[j].dw = uop->exec.idep_uop[j]->exec.oflags;
                  else
                    uop->exec.ivalue[j] = uop->exec.idep_uop[j]->exec.ovalue;
                }
              }
              else /* read from ARF */
              {
                uop->timing.when_itag_ready[j] = core->sim_cycle;
                uop->timing.when_ival_ready[j] = core->sim_cycle;
                uop->exec.ivalue_valid[j] = true; /* applies to invalid (DNA) inputs as well */
                if(uop->decode.idep_name[j] != DNA)
                  uop->exec.ivalue[j] = uop->oracle.ivalue[j]; /* oracle value == architected value */
              }
              if(when_ready < uop->timing.when_itag_ready[j])
                when_ready = uop->timing.when_itag_ready[j];
            }
            uop->timing.when_ready = when_ready;
            uop->timing.when_allocated = core->sim_cycle;

#ifdef ZTRACE
            if(uop->decode.in_fusion && !uop->decode.is_fusion_head)
              ztrace_print_cont("f");
            ztrace_print_cont(":pb=%d",uop->alloc.port_assignment);
            ztrace_print_finish("|uop alloc'd and dispatched");
#endif

          }

          if(uop->decode.in_fusion)
            uop = uop->decode.fusion_next;
          else
            uop = NULL;
        }

        if(abort_alloc)
          break;

        if((!pipe[stage][i]->decode.in_fusion) || !uop) /* either not fused, or complete fused uops alloc'd */
        {
          uop = pipe[stage][i]; /* may be NULL if we just finished a fused set */

          /* update stats */
          if(uop->decode.EOM)
            ZESTO_STAT(core->stat.alloc_insn++;)

          ZESTO_STAT(core->stat.alloc_uops++;)
          if(uop->decode.in_fusion)
            ZESTO_STAT(core->stat.alloc_eff_uops += uop->decode.fusion_size;)
          else
            ZESTO_STAT(core->stat.alloc_eff_uops++;)

          /* remove from alloc pipe */
          pipe[stage][i] = NULL;
          occupancy[stage]--;
          zesto_assert(occupancy[stage] >= 0,(void)0);
        }
      }
    }
  }
  else
    stall_reason = ASTALL_EMPTY;

  /*=============================================*/
  /*== Shuffle uops down the rename/alloc pipe ==*/

  /* walk pipe backwards */
  for(stage=knobs->alloc.depth-1; stage > 0; stage--)
  {
//    if(0 == occupancy[stage]) /* implementing non-serpentine pipe (no compressing) - can't advance until stage is empty */
    if(occupancy[stage] < knobs->alloc.width)
    {
      /* move everyone from previous stage forward */
      for(i=0;i<knobs->alloc.width;i++)
      {
        if(pipe[stage][i] == NULL)
        {
           pipe[stage][i] = pipe[stage-1][i];
           pipe[stage-1][i] = NULL;
           if(pipe[stage][i])
           {
             occupancy[stage]++;
             occupancy[stage-1]--;
             zesto_assert(occupancy[stage] <= knobs->alloc.width,(void)0);
             zesto_assert(occupancy[stage-1] >= 0,(void)0);
           }
        }
      }
    }
  }

  /*==============================================*/
  /*== fill first alloc stage from decode stage ==*/
//  if(0 == occupancy[0])
  if(occupancy[0] < knobs->alloc.width)
  {
    /* while the uopQ sitll has uops in it, allocate up to alloc.width uops per cycle */
    for(i=0;(i<knobs->alloc.width) && core->decode->uop_available();i++)
    {
      if(pipe[0][i] != NULL)
         continue;

      pipe[0][i] = core->decode->uop_peek(); core->decode->uop_consume();
      occupancy[0]++;
      zesto_assert(occupancy[0] <= knobs->alloc.width,(void)0);
#ifdef ZTRACE
      ztrace_print(pipe[0][i],"a|alloc-pipe|enqueue");
#endif
    }
  }

  ZESTO_STAT(stat_add_sample(core->stat.alloc_stall, (int)stall_reason);)
}

/* start from most recently fetched, blow away everything until
   we find the Mop */
void
core_alloc_IO_DPM_t::recover(const struct Mop_t * const Mop)
{
  struct core_knobs_t * knobs = core->knobs;
  int stage,i;
  for(stage=0;stage<knobs->alloc.depth;stage++)
  {
    /* slot N-1 is most speculative, start from there */
    if(occupancy[stage])
      for(i=knobs->alloc.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          if(pipe[stage][i]->decode.Mop_seq <= Mop->oracle.seq)
            continue;
          pipe[stage][i] = NULL;
          occupancy[stage]--;
          zesto_assert(occupancy[stage] >= 0,(void)0);
        }
      }
  }
}

/* clean up on pipeline flush (blow everything away) */
void
core_alloc_IO_DPM_t::recover(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int stage,i;
  for(stage=0;stage<knobs->alloc.depth;stage++)
  {
    /* slot N-1 is most speculative, start from there */
    if(occupancy[stage])
      for(i=knobs->alloc.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          pipe[stage][i] = NULL;
          occupancy[stage]--;
          zesto_assert(occupancy[stage] >= 0,(void)0);
        }
      }
    zesto_assert(occupancy[stage] == 0,(void)0);
  }
}

void core_alloc_IO_DPM_t::RS_deallocate(const struct uop_t * const uop)
{
  fatal("shouldn't be called in an IO pipe");
}

void core_alloc_IO_DPM_t::start_drain(void)
{
  drain_in_progress = true;
}


#endif
