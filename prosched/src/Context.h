#pragma once

#include <stdio.h>
#include <string>
#include <vector>

#include "Config.h"

enum class SchedulerType { FCFS, RR, UNKNOWN };

struct AlgoContext {
  SchedulerType schedulerType = SchedulerType::UNKNOWN;
  int num_cpu;
  int batch_process_frequency;
  int min_ins;
  int max_ins;
  int delay_per_execution;
  // for rr
  int rr_quantum_cycles;
  int min_mem_per_proc;
  int max_mem_per_proc;
  int mem_per_frame;
  int max_overall_mem;

  static AlgoContext buildConfig(ConfigStruct *config) {
    AlgoContext ctx;

    std::string sched = config->scheduler;
    if (sched == "fcfs") {
      ctx.schedulerType = SchedulerType::FCFS;
      ctx.rr_quantum_cycles = -1;
    } else if (sched == "rr") {
      ctx.schedulerType = SchedulerType::RR;
      ctx.rr_quantum_cycles = config->rr_quantum_cycles;
    } else {
      ctx.schedulerType = SchedulerType::UNKNOWN;
      ctx.rr_quantum_cycles = -1;
    }

    ctx.num_cpu = config->num_cpu;
    ctx.batch_process_frequency = config->batch_process_freq;
    ctx.min_ins = config->min_ins;
    ctx.max_ins = config->max_ins;
    ctx.delay_per_execution = config->delay_per_exec;
    ctx.min_mem_per_proc = config->min_mem_per_proc;
    ctx.max_mem_per_proc = config->max_mem_per_proc;
    ctx.mem_per_frame = config->mem_per_frame;
    ctx.max_overall_mem = config->max_overall_mem;
    return ctx;
  }
};