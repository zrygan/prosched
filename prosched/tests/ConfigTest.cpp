#include "Config.h"
#include "Context.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <gtest/gtest.h>
#include <sstream>

namespace ConfigMakeDefault {

// makeDefault always returns a valid pointer, never nullptr
TEST(ConfigMakeDefault, ReturnsNonNull) {
  ConfigStruct *cs = makeDefault();
  ASSERT_NE(cs, nullptr);
  delete cs;
}

// Default num_cpu is 4
TEST(ConfigMakeDefault, num_cpuIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->num_cpu, 4);
  delete cs;
}

// Default scheduler is "rr"
TEST(ConfigMakeDefault, SchedulerIsRr) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->scheduler, "rr");
  delete cs;
}

// Default rr_quantum_cycles is 5
TEST(ConfigMakeDefault, rr_quantum_cyclesIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->rr_quantum_cycles, 5);
  delete cs;
}

// Default batch_process_freq is 1
TEST(ConfigMakeDefault, batch_process_frequencyIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->batch_process_freq, 1);
  delete cs;
}

// Default min_ins is 1000
TEST(ConfigMakeDefault, min_insIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->min_ins, 1000);
  delete cs;
}

// Default max_ins is 2000
TEST(ConfigMakeDefault, max_insIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->max_ins, 2000);
  delete cs;
}

// Default delay_per_exec is 0
TEST(ConfigMakeDefault, delay_per_executionIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->delay_per_exec, 0);
  delete cs;
}

// Default min_mem_per_proc is 4096
TEST(ConfigMakeDefault, min_mem_per_procIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->min_mem_per_proc, 4096);
  delete cs;
}

// Default max_mem_per_proc is 4096
TEST(ConfigMakeDefault, max_mem_per_procIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->max_mem_per_proc, 4096);
  delete cs;
}

// Default mem_per_frame is 16
TEST(ConfigMakeDefault, mem_per_frameIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->mem_per_frame, 16);
  delete cs;
}

// Default max_overall_mem is 16384
TEST(ConfigMakeDefault, max_overall_memIsDefault) {
  ConfigStruct *cs = makeDefault();
  EXPECT_EQ(cs->max_overall_mem, 16384);
  delete cs;
}

} // namespace ConfigMakeDefault

// These tests check that fromFile() carries each key in prosched/config.txt
// into the matching field - not that config.txt holds any particular numbers.
// The values there are demo parameters and the MO2 quiz changes them without a
// recompile, so asserting literals turned a config edit into a dozen failures
// that said nothing about the parser. The expected value is read back out of
// the file instead, by a reader with no code in common with the one under test.
namespace {

// Reads config.txt as plain "key value" lines, the way the file is written.
std::map<std::string, std::string> ReadConfigFileDirectly() {
  std::map<std::string, std::string> entries;
  std::ifstream file(CONFIG_FILENAME);
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream parts(line);
    std::string key, value;
    if (parts >> key >> value) {
      entries[key] = value;
    }
  }
  return entries;
}

// The value config.txt gives for a key, as an int. Fails the calling test when
// the key is missing rather than silently comparing against a default.
int ConfigFileInt(const std::string &key) {
  const std::map<std::string, std::string> entries = ReadConfigFileDirectly();
  const auto it = entries.find(key);
  EXPECT_NE(it, entries.end())
      << "config.txt has no \"" << key << "\" line; run the suite from the "
      << "repo root so " << CONFIG_FILENAME << " resolves";
  if (it == entries.end()) {
    return -1;
  }
  return std::stoi(it->second);
}

} // namespace

namespace ConfigFromFile {

// prosched/config.txt is present and readable from the repo root CWD
TEST(ConfigFromFile, ValidFileReturnsNonNull) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  delete cs;
}

// "num-cpu" from config.txt is parsed to num_cpu
TEST(ConfigFromFile, Parsesnum_cpu) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->num_cpu, ConfigFileInt("num-cpu"));
  delete cs;
}

// "scheduler rr" in config.txt is parsed to scheduler
TEST(ConfigFromFile, ParsesScheduler) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->scheduler, "rr");
  delete cs;
}

// "quantum-cycles" from config.txt is parsed to rr_quantum_cycles
TEST(ConfigFromFile, Parsesrr_quantum_cycles) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->rr_quantum_cycles, ConfigFileInt("quantum-cycles"));
  delete cs;
}

// "batch-process-freq" from config.txt is parsed to batch_process_freq
TEST(ConfigFromFile, Parsesbatch_process_frequency) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->batch_process_freq, ConfigFileInt("batch-process-freq"));
  delete cs;
}

// "min-ins" from config.txt is parsed to min_ins
TEST(ConfigFromFile, Parsesmin_ins) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->min_ins, ConfigFileInt("min-ins"));
  delete cs;
}

// "max-ins" from config.txt is parsed to max_ins
TEST(ConfigFromFile, Parsesmax_ins) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->max_ins, ConfigFileInt("max-ins"));
  delete cs;
}

// "delay-per-exec" from config.txt is parsed to delay_per_exec
TEST(ConfigFromFile, Parsesdelay_per_execution) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->delay_per_exec, ConfigFileInt("delay-per-exec"));
  delete cs;
}

// "min-mem-per-proc" from config.txt is parsed to min_mem_per_proc
TEST(ConfigFromFile, Parsesmin_mem_per_proc) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->min_mem_per_proc, ConfigFileInt("min-mem-per-proc"));
  delete cs;
}

// "max-mem-per-proc" from config.txt is parsed to max_mem_per_proc
TEST(ConfigFromFile, Parsesmax_mem_per_proc) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->max_mem_per_proc, ConfigFileInt("max-mem-per-proc"));
  delete cs;
}

// "mem-per-frame" from config.txt is parsed to mem_per_frame
TEST(ConfigFromFile, Parsesmem_per_frame) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->mem_per_frame, ConfigFileInt("mem-per-frame"));
  delete cs;
}

// "max-overall-mem" from config.txt is parsed to max_overall_mem
TEST(ConfigFromFile, Parsesmax_overall_mem) {
  ConfigStruct *cs = fromFile();
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->max_overall_mem, ConfigFileInt("max-overall-mem"));
  delete cs;
}

} // namespace ConfigFromFile

namespace ConfigBuildConfig {

// num_cpu field transfers to AlgoContext.num_cpu
TEST(ConfigBuildConfig, Mapsnum_cpu) {
  ConfigStruct cs{.num_cpu = 8,
                  .scheduler = "fcfs",
                  .rr_quantum_cycles = 10,
                  .batch_process_freq = 2,
                  .min_ins = 500,
                  .max_ins = 1000,
                  .delay_per_exec = 1};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.num_cpu, 8);
}

// "fcfs" string becomes SchedulerType::FCFS enum
TEST(ConfigBuildConfig, MapsSchedulerFcfs) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "fcfs",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.schedulerType, SchedulerType::FCFS);
}

// "rr" string becomes SchedulerType::RR enum
TEST(ConfigBuildConfig, MapsSchedulerRr) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "rr",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.schedulerType, SchedulerType::RR);
}

// Unrecognized scheduler string becomes SchedulerType::UNKNOWN
TEST(ConfigBuildConfig, UnknownSchedulerMapsToUnknown) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "sjf",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.schedulerType, SchedulerType::UNKNOWN);
}

// buildConfig must fully initialise what it returns, on every branch.
// Context.h:29-36 assigns rr_quantum_cycles only in the fcfs branch (-1) and
// the rr branch (the configured value). The else branch sets schedulerType and
// nothing more, and AlgoContext declares `int rr_quantum_cycles;` with no
// default member initialiser — so on an unrecognised scheduler string the
// field is indeterminate and reading it is undefined behaviour.
//
// HOW THIS DETECTS IT: the first call plants a distinctive quantum, and the
// second call's local AlgoContext generally reuses the same stack slot, so the
// stale 4242 shows through. That reuse is not guaranteed by the standard — if
// a future compiler lays the frames out differently this could pass while the
// bug is still there. It is a detector, not a proof; MSan is the rigorous tool.
// The real fix is default member initialisers on AlgoContext, after which this
// passes for the right reason.
//
// NOTE: this is defence in depth. The consequence is already covered by
// SchedulerConfigValidation.UnknownSchedulerTypeRejectedAtStartup — once an
// unrecognised scheduler string is rejected at startup, nothing in production
// ever reaches this branch to read the field.
TEST(ConfigBuildConfig, UnknownSchedulerLeavesQuantumInitialised) {
  ConfigStruct planted{.num_cpu = 4,
                       .scheduler = "rr",
                       .rr_quantum_cycles = 4242,
                       .batch_process_freq = 1,
                       .min_ins = 1000,
                       .max_ins = 2000,
                       .delay_per_exec = 0};
  AlgoContext seeded = AlgoContext::buildConfig(&planted);
  ASSERT_EQ(seeded.rr_quantum_cycles, 4242); // the value is now on the stack

  ConfigStruct unknown{.num_cpu = 4,
                       .scheduler = "sjf",
                       .rr_quantum_cycles = 5,
                       .batch_process_freq = 1,
                       .min_ins = 1000,
                       .max_ins = 2000,
                       .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&unknown);

  ASSERT_EQ(ctx.schedulerType, SchedulerType::UNKNOWN);
  EXPECT_EQ(ctx.rr_quantum_cycles, -1)
      << "the UNKNOWN branch left rr_quantum_cycles = " << ctx.rr_quantum_cycles
      << " (uninitialised; 4242 means it inherited the previous call's stack "
         "slot). It should be set to -1, the same 'no quantum' sentinel the "
         "fcfs branch uses";
}

// batch_process_freq transfers to batch_process_frequency
TEST(ConfigBuildConfig, Mapsbatch_process_frequency) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "fcfs",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 3,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.batch_process_frequency, 3);
}

// min_ins and max_ins both transfer correctly
TEST(ConfigBuildConfig, MapsMinAndmax_ins) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "fcfs",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 200,
                  .max_ins = 800,
                  .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.min_ins, 200);
  EXPECT_EQ(ctx.max_ins, 800);
}

// rr_quantum_cycles transfers to rr_quantum_cycles
TEST(ConfigBuildConfig, Mapsrr_quantum_cycles) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "rr",
                  .rr_quantum_cycles = 7,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.rr_quantum_cycles, 7);
}

// delay_per_exec transfers to delay_per_execution
TEST(ConfigBuildConfig, Mapsdelay_per_execution) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "fcfs",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 5};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.delay_per_execution, 5);
}

// min_mem_per_proc and max_mem_per_proc transfer correctly; distinct values
// catch a swapped mapping
TEST(ConfigBuildConfig, MapsMinAndmax_mem_per_proc) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "rr",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0,
                  .min_mem_per_proc = 1024,
                  .max_mem_per_proc = 8192,
                  .mem_per_frame = 16,
                  .max_overall_mem = 16384};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.min_mem_per_proc, 1024);
  EXPECT_EQ(ctx.max_mem_per_proc, 8192);
}

// mem_per_frame transfers to mem_per_frame
TEST(ConfigBuildConfig, Mapsmem_per_frame) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "rr",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0,
                  .min_mem_per_proc = 4096,
                  .max_mem_per_proc = 4096,
                  .mem_per_frame = 32,
                  .max_overall_mem = 16384};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.mem_per_frame, 32);
}

// max_overall_mem transfers to max_overall_mem
TEST(ConfigBuildConfig, Mapsmax_overall_mem) {
  ConfigStruct cs{.num_cpu = 4,
                  .scheduler = "rr",
                  .rr_quantum_cycles = 5,
                  .batch_process_freq = 1,
                  .min_ins = 1000,
                  .max_ins = 2000,
                  .delay_per_exec = 0,
                  .min_mem_per_proc = 4096,
                  .max_mem_per_proc = 4096,
                  .mem_per_frame = 16,
                  .max_overall_mem = 32768};
  AlgoContext ctx = AlgoContext::buildConfig(&cs);
  EXPECT_EQ(ctx.max_overall_mem, 32768);
}

} // namespace ConfigBuildConfig

// ── Helpers used only by ConfigValidation tests ──────────────────────────────

namespace {

static const char *TEMP_CFG = "/tmp/prosched_test_cfg.txt";

static void writeTestConfig(const std::string &content) {
  std::ofstream f(TEMP_CFG);
  f << content;
}

// Mirrors fromFile() logic but accepts a custom path, so tests can point at
// temp files or deliberately nonexistent paths without touching config.h.
static ConfigStruct *fromFileAt(const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open())
    return nullptr;
  ConfigStruct *cs = new ConfigStruct();
  std::string line;
  while (getline(f, line)) {
    std::istringstream iss(line);
    std::string key;
    iss >> key;
    if (key == "num-cpu")
      iss >> cs->num_cpu;
    else if (key == "scheduler")
      iss >> cs->scheduler;
    else if (key == "quantum-cycles")
      iss >> cs->rr_quantum_cycles;
    else if (key == "batch-process-freq")
      iss >> cs->batch_process_freq;
    else if (key == "min-ins")
      iss >> cs->min_ins;
    else if (key == "max-ins")
      iss >> cs->max_ins;
    else if (key == "delay-per-exec")
      iss >> cs->delay_per_exec;
    else if (key == "min-mem-per-proc")
      iss >> cs->min_mem_per_proc;
    else if (key == "max-mem-per-proc")
      iss >> cs->max_mem_per_proc;
    else if (key == "mem-per-frame")
      iss >> cs->mem_per_frame;
    else if (key == "max-overall-mem")
      iss >> cs->max_overall_mem;
  }
  return cs;
}

// Validates a loaded config and exits with code 1 on any problem.
// Called inside EXPECT_EXIT lambdas so the parent process can verify the exit.
static void validateOrDie(ConfigStruct *cs, const char *path = "") {
  if (cs == nullptr) {
    std::cerr << "[prosched] config file '" << path << "' not found\n";
    exit(1);
  }
  if (cs->num_cpu < 1) {
    std::cerr << "[prosched] 'num-cpu' missing or invalid (must be >= 1)\n";
    exit(1);
  }
  if (cs->scheduler != "fcfs" && cs->scheduler != "rr") {
    std::cerr << "[prosched] 'scheduler' missing or invalid (must be 'fcfs' or "
                 "'rr')\n";
    exit(1);
  }
  if (cs->rr_quantum_cycles < 1) {
    std::cerr
        << "[prosched] 'quantum-cycles' missing or invalid (must be >= 1)\n";
    exit(1);
  }
  if (cs->batch_process_freq < 1) {
    std::cerr << "[prosched] 'batch-process-freq' missing or invalid (must be "
                 ">= 1)\n";
    exit(1);
  }
  if (cs->min_ins < 1) {
    std::cerr << "[prosched] 'min-ins' missing or invalid (must be >= 1)\n";
    exit(1);
  }
  if (cs->max_ins < cs->min_ins) {
    std::cerr << "[prosched] 'max-ins' must be >= 'min-ins'\n";
    exit(1);
  }
  if (cs->delay_per_exec < 0) {
    std::cerr << "[prosched] 'delay-per-exec' must be >= 0\n";
    exit(1);
  }
}

} // anonymous namespace

namespace ConfigValidation {

// A nonexistent config file returns nullptr → program must exit
TEST(ConfigValidation, MissingFileExits) {
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt("/tmp/prosched_no_such_file.txt");
        validateOrDie(cs, "/tmp/prosched_no_such_file.txt");
      },
      ::testing::ExitedWithCode(1), ".*");
}

// Config with no 'num-cpu' line → num_cpu = 0 → must exit
TEST(ConfigValidation, Missingnum_cpuExits) {
  writeTestConfig("scheduler fcfs\nquantum-cycles 5\nbatch-process-freq 1\n"
                  "min-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// Config with no 'scheduler' line → scheduler = "" → must exit
TEST(ConfigValidation, MissingSchedulerExits) {
  writeTestConfig("num-cpu 4\nquantum-cycles 5\nbatch-process-freq 1\n"
                  "min-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// Config with no 'quantum-cycles' line → rr_quantum_cycles = 0 → must exit
TEST(ConfigValidation, Missingrr_quantum_cyclesExits) {
  writeTestConfig("num-cpu 4\nscheduler fcfs\nbatch-process-freq 1\n"
                  "min-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// Config with no 'batch-process-freq' line → batch_process_freq = 0 → must exit
TEST(ConfigValidation, Missingbatch_process_frequencyExits) {
  writeTestConfig("num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
                  "min-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// Config with no 'min-ins' line → min_ins = 0 → must exit
TEST(ConfigValidation, Missingmin_insExits) {
  writeTestConfig("num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
                  "batch-process-freq 1\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// Config with no 'max-ins' line → max_ins = 0 < min_ins → must exit
TEST(ConfigValidation, Missingmax_insExits) {
  writeTestConfig("num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
                  "batch-process-freq 1\nmin-ins 100\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// ── Invalid values ────────────────────────────────────────────────────────

// num-cpu 0 → must exit
TEST(ConfigValidation, Invalidnum_cpuZeroExits) {
  writeTestConfig(
      "num-cpu 0\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// num-cpu -1 → must exit
TEST(ConfigValidation, Invalidnum_cpuNegativeExits) {
  writeTestConfig(
      "num-cpu -1\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// scheduler sjf (unsupported) → must exit
TEST(ConfigValidation, InvalidSchedulerExits) {
  writeTestConfig(
      "num-cpu 4\nscheduler sjf\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// quantum-cycles 0 → must exit
TEST(ConfigValidation, Invalidrr_quantum_cyclesZeroExits) {
  writeTestConfig(
      "num-cpu 4\nscheduler fcfs\nquantum-cycles 0\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// batch-process-freq 0 → must exit
TEST(ConfigValidation, Invalidbatch_process_frequencyZeroExits) {
  writeTestConfig(
      "num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 0\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// min-ins 0 → must exit
TEST(ConfigValidation, Invalidmin_insZeroExits) {
  writeTestConfig(
      "num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 0\nmax-ins 200\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// max-ins < min-ins → must exit
TEST(ConfigValidation, Invalidmax_insLessThanmin_insExits) {
  writeTestConfig(
      "num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 1000\nmax-ins 500\ndelay-per-exec 0\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// delay-per-exec -1 → must exit
TEST(ConfigValidation, Invaliddelay_per_executionNegativeExits) {
  writeTestConfig(
      "num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec -1\n");
  EXPECT_EXIT(
      {
        ConfigStruct *cs = fromFileAt(TEMP_CFG);
        validateOrDie(cs);
      },
      ::testing::ExitedWithCode(1), ".*");
  remove(TEMP_CFG);
}

// A fully valid config must pass validation without exiting
TEST(ConfigValidation, ValidConfigPassesValidation) {
  writeTestConfig(
      "num-cpu 4\nscheduler fcfs\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  ConfigStruct *cs = fromFileAt(TEMP_CFG);
  ASSERT_NE(cs, nullptr);
  validateOrDie(cs); // must not call exit()
  delete cs;
  remove(TEMP_CFG);
}

// rr scheduler is also a valid choice
TEST(ConfigValidation, ValidConfigRrPassesValidation) {
  writeTestConfig(
      "num-cpu 2\nscheduler rr\nquantum-cycles 3\n"
      "batch-process-freq 2\nmin-ins 50\nmax-ins 500\ndelay-per-exec 10\n");
  ConfigStruct *cs = fromFileAt(TEMP_CFG);
  ASSERT_NE(cs, nullptr);
  validateOrDie(cs);
  delete cs;
  remove(TEMP_CFG);
}

// ── Memory keys ─────────────────────────────────────────────────────────────

// Memory keys present in the file are parsed into their fields
TEST(ConfigValidation, ParsesMemoryKeysWhenPresent) {
  writeTestConfig(
      "num-cpu 4\nscheduler rr\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n"
      "min-mem-per-proc 1024\nmax-mem-per-proc 8192\n"
      "mem-per-frame 32\nmax-overall-mem 32768\n");
  ConfigStruct *cs = fromFileAt(TEMP_CFG);
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->min_mem_per_proc, 1024);
  EXPECT_EQ(cs->max_mem_per_proc, 8192);
  EXPECT_EQ(cs->mem_per_frame, 32);
  EXPECT_EQ(cs->max_overall_mem, 32768);
  delete cs;
  remove(TEMP_CFG);
}

// Memory keys absent from the file stay value-initialized to 0
TEST(ConfigValidation, MissingMemoryKeysStayZero) {
  writeTestConfig(
      "num-cpu 4\nscheduler rr\nquantum-cycles 5\n"
      "batch-process-freq 1\nmin-ins 100\nmax-ins 200\ndelay-per-exec 0\n");
  ConfigStruct *cs = fromFileAt(TEMP_CFG);
  ASSERT_NE(cs, nullptr);
  EXPECT_EQ(cs->min_mem_per_proc, 0);
  EXPECT_EQ(cs->max_mem_per_proc, 0);
  EXPECT_EQ(cs->mem_per_frame, 0);
  EXPECT_EQ(cs->max_overall_mem, 0);
  delete cs;
  remove(TEMP_CFG);
}

} // namespace ConfigValidation
