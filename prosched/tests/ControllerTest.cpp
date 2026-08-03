#include "controller/Controller.h"
#include "Constants.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

namespace ControllerIdentifyCommand {

// "exit" token maps to CLI_EXIT
TEST(ControllerIdentifyCommand, ExitCommand) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"exit"}), CLI_COMMAND::CLI_EXIT);
}

// "scheduler-start" token maps to CLI_SCHEDULER_START
TEST(ControllerIdentifyCommand, SchedulerStart) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"scheduler-start"}),
            CLI_COMMAND::CLI_SCHEDULER_START);
}

// "scheduler-stop" token maps to CLI_SCHEDULER_STOP
TEST(ControllerIdentifyCommand, SchedulerStop) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"scheduler-stop"}),
            CLI_COMMAND::CLI_SCHEDULER_STOP);
}

// "report-util" token maps to CLI_REPORT_UTIL
TEST(ControllerIdentifyCommand, ReportUtil) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"report-util"}), CLI_COMMAND::CLI_REPORT_UTIL);
}

// "vmstat" token maps to CLI_VMSTAT
TEST(ControllerIdentifyCommand, VmStat) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"vmstat"}), CLI_COMMAND::CLI_VMSTAT);
}

// "process-smi" token maps to CLI_PROCESS_SMI
TEST(ControllerIdentifyCommand, ProcessSmi) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"process-smi"}), CLI_COMMAND::CLI_PROCESS_SMI);
}

// {"screen", "-ls"} pair maps to CLI_SCREEN_LS
TEST(ControllerIdentifyCommand, ScreenLs) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"screen", "-ls"}), CLI_COMMAND::CLI_SCREEN_LS);
}

// {"screen", "-s"} pair maps to CLI_SCREEN_S
TEST(ControllerIdentifyCommand, ScreenS) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"screen", "-s"}), CLI_COMMAND::CLI_SCREEN_S);
}

// "screen" without a flag has no valid subcommand
TEST(ControllerIdentifyCommand, ScreenWithNoSubcommandIsUnknown) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"screen"}), CLI_COMMAND::UNKNOWN);
}

// Unrecognized flag after "screen" is treated as unknown
TEST(ControllerIdentifyCommand, ScreenWithBadSubcommandIsUnknown) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"screen", "-x"}), CLI_COMMAND::UNKNOWN);
}

// Completely unrecognized token maps to UNKNOWN
TEST(ControllerIdentifyCommand, UnrecognizedCommandIsUnknown) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"garbage"}), CLI_COMMAND::UNKNOWN);
}

} // namespace ControllerIdentifyCommand

namespace ControllerGetParsedInput {

// Empty input tokenizes to nothing — produces UNKNOWN
TEST(ControllerGetParsedInput, EmptyStringReturnsUnknown) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("").cliCommand, CLI_COMMAND::UNKNOWN);
}

// "exit" string tokenizes to CLI_EXIT
TEST(ControllerGetParsedInput, ExitCommand) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("exit").cliCommand, CLI_COMMAND::CLI_EXIT);
}

// "scheduler-start" string tokenizes to CLI_SCHEDULER_START
TEST(ControllerGetParsedInput, SchedulerStart) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("scheduler-start").cliCommand,
            CLI_COMMAND::CLI_SCHEDULER_START);
}

// "scheduler-stop" string tokenizes to CLI_SCHEDULER_STOP
TEST(ControllerGetParsedInput, SchedulerStop) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("scheduler-stop").cliCommand,
            CLI_COMMAND::CLI_SCHEDULER_STOP);
}

// "screen -ls" string tokenizes to CLI_SCREEN_LS
TEST(ControllerGetParsedInput, ScreenLs) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("screen -ls").cliCommand,
            CLI_COMMAND::CLI_SCREEN_LS);
}

// Third token "myproc" is stored in Command.processName
TEST(ControllerGetParsedInput, ScreenSExtractsProcessName) {
  Controller c;
  Command cmd = c.GetParsedInput("screen -s myproc");
  EXPECT_EQ(cmd.cliCommand, CLI_COMMAND::CLI_SCREEN_S);
  EXPECT_EQ(cmd.processName, "myproc");
}

// "process-smi" string tokenizes to CLI_PROCESS_SMI
TEST(ControllerGetParsedInput, ProcessSmi) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("process-smi").cliCommand,
            CLI_COMMAND::CLI_PROCESS_SMI);
}

// Unrecognized string tokenizes to UNKNOWN
TEST(ControllerGetParsedInput, GarbageInputReturnsUnknown) {
  Controller c;
  EXPECT_EQ(c.GetParsedInput("garbage").cliCommand, CLI_COMMAND::UNKNOWN);
}

} // namespace ControllerGetParsedInput

namespace ControllerHandlePreInit {

// "garbage" never reads isInitialized — safe even before initialize() is called
TEST(ControllerHandlePreInit, NonInitializeInputReturnsFalse) {
  Controller c;
  EXPECT_FALSE(c.HandlePreInit("garbage"));
}

// Empty string is not "initialize" — returns false
TEST(ControllerHandlePreInit, EmptyInputReturnsFalse) {
  Controller c;
  EXPECT_FALSE(c.HandlePreInit(""));
}

// Always returns true for "initialize" regardless of whether config was found
TEST(ControllerHandlePreInit, InitializeInputReturnsTrue) {
  Controller c;
  EXPECT_TRUE(c.HandlePreInit("initialize"));
}

} // namespace ControllerHandlePreInit

// Expected values from prosched/config.txt:
//   num-cpu 4  |  scheduler fcfs  |  quantum-cycles 5  |  batch-process-freq 1
//   min-ins 1000  |  max-ins 2000  |  delay-per-exec 0


// initialize() reads prosched/config.txt, whose values are demo parameters the
// MO2 quiz changes without a recompile. These tests check that each key reaches
// the matching AlgoContext field, so the expected value comes from the file
// rather than from a literal that a config edit would falsify.
namespace {

int ConfigValue(const std::string &key) {
  std::ifstream file(CONFIG_FILENAME);
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream parts(line);
    std::string fileKey, value;
    if (parts >> fileKey >> value && fileKey == key) {
      return std::stoi(value);
    }
  }
  ADD_FAILURE() << "config.txt has no \"" << key << "\" line; run the suite "
                << "from the repo root so " << CONFIG_FILENAME << " resolves";
  return -1;
}

} // namespace

namespace ControllerInitialize {

// "rr" in config.txt maps to SchedulerType::RR enum
TEST(ControllerInitialize, ReturnsCorrectSchedulerType) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.schedulerType, SchedulerType::RR);
}

// num-cpu from config.txt is read into num_cpu
TEST(ControllerInitialize, ReturnsCorrectnum_cpu) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.num_cpu, ConfigValue("num-cpu"));
}

// batch-process-freq from config.txt is read into batch_process_frequency
TEST(ControllerInitialize, ReturnsCorrectbatch_process_frequency) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.batch_process_frequency, ConfigValue("batch-process-freq"));
}

// min-ins and max-ins are read together as the instruction range
TEST(ControllerInitialize, ReturnsCorrectInstructionBounds) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.min_ins, ConfigValue("min-ins"));
  EXPECT_EQ(ctx.max_ins, ConfigValue("max-ins"));
}

// config.txt uses rr, so quantum-cycles 5 is read into rr_quantum_cycles
TEST(ControllerInitialize, ReturnsCorrectrr_quantum_cycles) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.rr_quantum_cycles, ConfigValue("quantum-cycles"));
}

// delay-per-exec from config.txt is read into delay_per_execution
TEST(ControllerInitialize, ReturnsCorrectdelay_per_execution) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.delay_per_execution, ConfigValue("delay-per-exec"));
}

// Memory settings from config.txt are read into the context
TEST(ControllerInitialize, ReturnsCorrectMemorySettings) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_EQ(ctx.min_mem_per_proc, ConfigValue("min-mem-per-proc"));
  EXPECT_EQ(ctx.max_mem_per_proc, ConfigValue("max-mem-per-proc"));
  EXPECT_EQ(ctx.mem_per_frame, ConfigValue("mem-per-frame"));
  EXPECT_EQ(ctx.max_overall_mem, ConfigValue("max-overall-mem"));
}

// UNKNOWN means fromFile() failed (config.txt not found) or unrecognized
// scheduler string
TEST(ControllerInitialize, SchedulerTypeIsNotUnknownOnSuccess) {
  Controller c;
  AlgoContext ctx = c.initialize();
  EXPECT_NE(ctx.schedulerType, SchedulerType::UNKNOWN);
}

// initialize() is idempotent — no crash on repeated calls
TEST(ControllerInitialize, CalledTwiceDoesNotCrash) {
  Controller c;
  EXPECT_NO_THROW({
    c.initialize();
    c.initialize();
  });
}

} // namespace ControllerInitialize

// ─── screen -s memory size (MO2) ────────────────────────────────────────────
// MO2: screen -s <name> <size>. Memory sizes are decimal, a power of 2, and at
// least 64 bytes; an invalid size must be rejected as an allocation error.

namespace ControllerParseMemorySize {

// A plain decimal size parses to its numeric value
TEST(ControllerParseMemorySize, DecimalSizeParses) {
  EXPECT_EQ(Controller::ParseMemorySize("256"), 256);
}

// Empty token is not a size
TEST(ControllerParseMemorySize, EmptyReturnsZero) {
  EXPECT_EQ(Controller::ParseMemorySize(""), 0);
}

// Non-numeric token is rejected (returns 0)
TEST(ControllerParseMemorySize, NonNumericReturnsZero) {
  EXPECT_EQ(Controller::ParseMemorySize("abc"), 0);
}

// MO2: memory SIZE is decimal (only addresses are hex) — "0x100" is not a size
TEST(ControllerParseMemorySize, HexTokenIsRejected) {
  EXPECT_EQ(Controller::ParseMemorySize("0x100"), 0);
}

// A value far past long range does not throw; it is rejected as 0
TEST(ControllerParseMemorySize, OverflowReturnsZero) {
  EXPECT_EQ(Controller::ParseMemorySize("999999999999999999999999"), 0);
}

} // namespace ControllerParseMemorySize

namespace ControllerValidMemoryAllocation {

// A power of 2 within range is valid (spec sample: 256)
TEST(ControllerValidMemoryAllocation, PowerOfTwoInRangeIsValid) {
  EXPECT_TRUE(Controller::IsValidMemoryAllocation(256));
}

// MO2: minimum is 64 bytes — 64 is valid, 32 is not
TEST(ControllerValidMemoryAllocation, SixtyFourIsValidMinimum) {
  EXPECT_TRUE(Controller::IsValidMemoryAllocation(64));
}
TEST(ControllerValidMemoryAllocation, BelowSixtyFourIsInvalid) {
  EXPECT_FALSE(Controller::IsValidMemoryAllocation(32));
}

// MO2: must be a power of 2 — 100 is in range but not a power of 2
TEST(ControllerValidMemoryAllocation, NonPowerOfTwoIsInvalid) {
  EXPECT_FALSE(Controller::IsValidMemoryAllocation(100));
}

// Zero (e.g. a missing/malformed size) is invalid
TEST(ControllerValidMemoryAllocation, ZeroIsInvalid) {
  EXPECT_FALSE(Controller::IsValidMemoryAllocation(0));
}

} // namespace ControllerValidMemoryAllocation

namespace ControllerGetParsedInputMemory {

// The 4th token is parsed into Command.memorySize for screen -s
TEST(ControllerGetParsedInputMemory, ScreenSExtractsMemorySize) {
  Controller c;
  Command cmd = c.GetParsedInput("screen -s myproc 256");
  EXPECT_EQ(cmd.cliCommand, CLI_COMMAND::CLI_SCREEN_S);
  EXPECT_EQ(cmd.processName, "myproc");
  EXPECT_EQ(cmd.memorySize, 256);
}

} // namespace ControllerGetParsedInputMemory

// ─── screen -c user-defined instructions (MO2) ──────────────────────────────
// MO2: screen -c <name> <size> "<instructions>" runs a caller-supplied program
// of 1-50 semicolon-separated instructions.

namespace ControllerScreenC {

// "screen -c" maps to CLI_SCREEN_C
TEST(ControllerScreenC, IdentifyScreenC) {
  Controller c;
  EXPECT_EQ(c.IdentifyCommand({"screen", "-c"}), CLI_COMMAND::CLI_SCREEN_C);
}

// Name, size, and the quoted instruction list are all extracted
TEST(ControllerScreenC, GetParsedInputExtractsNameSizeInstructions) {
  Controller c;
  Command cmd = c.GetParsedInput(R"~(screen -c myproc 256 "PRINT(x)")~");
  EXPECT_EQ(cmd.cliCommand, CLI_COMMAND::CLI_SCREEN_C);
  EXPECT_EQ(cmd.processName, "myproc");
  EXPECT_EQ(cmd.memorySize, 256);
  EXPECT_EQ(cmd.instructions, "PRINT(x)");
}

// The instruction list is taken from between the first and last quote
TEST(ControllerScreenC, ExtractQuotedBetweenQuotes) {
  EXPECT_EQ(
      Controller::ExtractQuotedInstructions(R"~(screen -c p 64 "DECLARE varA 10")~"),
      "DECLARE varA 10");
}

// Escaped inner quotes (\") are unescaped to real quotes
TEST(ControllerScreenC, ExtractQuotedUnescapesInnerQuotes) {
  EXPECT_EQ(
      Controller::ExtractQuotedInstructions(R"~(screen -c p 64 "PRINT(\"hi\")")~"),
      R"~(PRINT("hi"))~");
}

// No quoted section yields an empty instruction string
TEST(ControllerScreenC, ExtractQuotedNoQuotesReturnsEmpty) {
  EXPECT_EQ(Controller::ExtractQuotedInstructions("screen -c p 64"), "");
}

// MO2: the user-instruction count limit is 1-50
TEST(ControllerScreenC, UserInstructionLimitsAreOneToFifty) {
  EXPECT_EQ(prosched::kMinUserInstructions, 1u);
  EXPECT_EQ(prosched::kMaxUserInstructions, 50u);
}

} // namespace ControllerScreenC

// ─── screen -r access-violation notice (MO2) ────────────────────────────────
// MO2: a process that prematurely shut down from a memory access violation is
// reported by screen -r as: "Process <name> shut down due to memory access
// violation error that occurred at <HH:MM:SS>. <Hex address> invalid."

namespace ControllerAccessViolationNotice {

// The notice matches the MO2 wording exactly, with an uppercase-hex address
TEST(ControllerAccessViolationNotice, MatchesSpecFormat) {
  EXPECT_EQ(
      Controller::FormatAccessViolationNotice("myproc", "14:15:30", 0x1F4),
      "Process myproc shut down due to memory access violation error that "
      "occurred at 14:15:30. 0x1F4 invalid.");
}

} // namespace ControllerAccessViolationNotice
