/* cs246.cpp - Simple IPC-based frequency controller */

#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(opt_string, "cs246"))
    return new vf_controller_cs246_t(core);
#else

class vf_controller_cs246_t : public vf_controller_t {
  public:
    vf_controller_cs246_t(struct core_t * const _core);

    virtual void change_vf();

  protected:
    tick_t last_cycle;
    counter_t last_commit_insn;
	float min_freq;
	float max_freq;
};

vf_controller_cs246_t::vf_controller_cs246_t(struct core_t * const _core) : vf_controller_t(_core)
{
    last_cycle = 0;
    last_commit_insn = 0;
	//3.2 GHz
	max_freq = core->knobs->default_cpu_speed;
	//1.6 GHz
	min_freq = 0.5 * max_freq;
    
	core->cpu_speed = core->knobs->default_cpu_speed;
}

void vf_controller_cs246_t::change_vf()
{
    counter_t delta_insn = core->stat.commit_insn - last_commit_insn;
    tick_t delta_cycles = core->sim_cycle - last_cycle;
    float curr_ipc = delta_insn / (float) delta_cycles;
	//using A.cfg, the maximum issue width is 2. 
	if (curr_ipc < 0.6 )
		core->cpu_speed = min_freq;
	else
		core->cpu_speed = max_freq;

    fprintf(stderr, "%lld %lld %.3f cpu_speed %.1f\n", delta_cycles, delta_insn, curr_ipc, core->cpu_speed);

    last_cycle = core->sim_cycle;
    last_commit_insn = core->stat.commit_insn;
}

#endif
