#include "Config.h"
#include "Context.h"
#include "commands/Interpreter.h"
#include "controller/Controller.h"
#include "memory/PagingManager.h"
#include "scheduler/Scheduler.h"
#include "scheduler/process/Process.h"
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

// MO2 grading scenario, question 5:
//
//     num-cpu 1 / scheduler rr / quantum-cycles 10 / batch-process-freq 1
//     min-ins 1000 / max-ins 1000 / delay-per-exec 0
//     max-overall-mem 256 / mem-per-frame 256
//     min-mem-per-proc 256 / max-mem-per-proc 256
//
//     screen -c faulty_process "DECLARE varA 10; DECLARE varB 5;
//         ADD varA varA varB; WRITE 0x500 varA; READ varC 0x500;
//         PRINT("Variable A: " + varA); PRINT("Result: " + varC)"
//
//   "Expected output: The correct variables, A and C are printed in the
//    console."
//
// varA = 10 + 5 = 15 is written to 0x500 and read back into varC, so the two
// PRINTs must produce "Variable A: 15" and "Result: 15".
//
// max-overall-mem / mem-per-frame = 256/256 = ONE frame, and 0x500 = 1280 is
// page 5 at a 256-byte page size. Both facts matter below.

namespace {

const char *kQuestion5Program =
    "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x500 varA; "
    "READ varC 0x500; PRINT(\"Variable A: \" + varA); "
    "PRINT(\"Result: \" + varC)";

AlgoContext makeQuestion5Ctx() {
  ConfigStruct *cs = makeDefault();
  cs->num_cpu = 1;
  cs->scheduler = "rr";
  cs->rr_quantum_cycles = 10;
  cs->batch_process_freq = 1000000; // generation off: only our process runs
  cs->min_ins = 1000;
  cs->max_ins = 1000;
  cs->delay_per_exec = 0;
  cs->max_overall_mem = 256;
  cs->mem_per_frame = 256;
  cs->min_mem_per_proc = 256;
  cs->max_mem_per_proc = 256;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;
  return ctx;
}

std::vector<prosched::Statement> ParseQuestion5Program() {
  std::vector<prosched::Statement> program;
  prosched::Interpreter parser;
  parser.ParseUserProgram(kQuestion5Program, program);
  return program;
}

// Runs the scheduler until the process ends, or until the deadline expires.
// Bounded so a process that cannot make progress fails the test instead of
// hanging the suite.
bool RunUntilEnded(prosched::Scheduler &scheduler, prosched::Process *p,
                   std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (p->IsFinished()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return p->IsFinished();
}

bool LogsContain(prosched::Process *p, const std::string &needle) {
  for (const std::string &line : p->GetLogs()) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string JoinLogs(prosched::Process *p) {
  std::string all;
  for (const std::string &line : p->GetLogs()) {
    all += "\n    " + line;
  }
  return all.empty() ? "\n    (no log output)" : all;
}

} // namespace

namespace ScreenCQuestion5Parsing {

// The command as the grading script types it carries NO memory-size argument,
// but "screen -c" is specified as
// `screen -c <process_name> <process_memory_size> "<instructions>"`. The parser
// therefore reads the first word of the program as the size.
TEST(ScreenCQuestion5Parsing, CommandWithoutAMemorySizeYieldsNoValidSize) {
  const std::string input =
      std::string("screen -c faulty_process \"") + kQuestion5Program + "\"";

  Controller controller;
  Command parsed = controller.GetParsedInput(input);

  EXPECT_EQ(parsed.cliCommand, CLI_COMMAND::CLI_SCREEN_C);
  EXPECT_EQ(parsed.processName, "faulty_process");
  EXPECT_FALSE(Controller::IsValidMemoryAllocation(parsed.memorySize))
      << "parsed memory size " << parsed.memorySize
      << " from a command that has no size argument. ExecuteCommand rejects "
         "this with \"invalid memory allocation\" and the process is never "
         "created, so nothing is printed. The scenario needs an explicit size, "
         "e.g. screen -c faulty_process 2048 \"...\".";

  // The program itself is well-formed - only the missing size is at issue.
  std::vector<prosched::Statement> program;
  prosched::Interpreter parser;
  EXPECT_TRUE(parser.ParseUserProgram(parsed.instructions, program));
  EXPECT_EQ(program.size(), 7u);
}

} // namespace ScreenCQuestion5Parsing

namespace ScreenCQuestion5Execution {

// With the size the config implies (256 bytes), 0x500 is outside the process's
// address space, so MO2 requires the shutdown path - not the two PRINTs. This
// pins what actually happens, and it is NOT the scenario's stated expectation.
TEST(ScreenCQuestion5Execution, ConfigSizedProcessDiesOnTheOutOfRangeWrite) {
  prosched::PagingManager pm(256, 256);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1);

  prosched::Scheduler scheduler(makeQuestion5Ctx(), &pm);
  std::vector<prosched::Statement> program = ParseQuestion5Program();
  ASSERT_EQ(program.size(), 7u);

  prosched::Process *p = scheduler.CreateProcessWithInstructions(
      "faulty_process", 256, program);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(scheduler.AddProcess(p), nullptr);

  ASSERT_TRUE(scheduler.Start());
  const bool ended = RunUntilEnded(scheduler, p, std::chrono::seconds(3));
  scheduler.Stop();

  ASSERT_TRUE(ended) << "the process neither finished nor terminated";
  EXPECT_TRUE(p->IsTerminated())
      << "0x500 (1280) lies outside a 256-byte address space, so the WRITE is "
         "an access violation and the process must be shut down";
  EXPECT_EQ(p->GetLastViolationAddress(), 0x500u);
  EXPECT_FALSE(LogsContain(p, "Variable A: 15"))
      << "the PRINTs come after the violating WRITE and must not run";
}

// The scenario's stated expectation, with a size that makes 0x500 legal
// (2048 = the smallest valid power of 2 above 0x502). The program must run to
// completion and print both variables.
TEST(ScreenCQuestion5Execution, ProgramPrintsBothVariablesWhenTheAddressFits) {
  prosched::PagingManager pm(256, 256);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1)
      << "precondition: max-overall-mem 256 / mem-per-frame 256 = one frame";

  prosched::Scheduler scheduler(makeQuestion5Ctx(), &pm);
  std::vector<prosched::Statement> program = ParseQuestion5Program();
  ASSERT_EQ(program.size(), 7u);

  prosched::Process *p = scheduler.CreateProcessWithInstructions(
      "faulty_process", 2048, program);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(scheduler.AddProcess(p), nullptr);

  ASSERT_TRUE(scheduler.Start());
  const bool ended = RunUntilEnded(scheduler, p, std::chrono::seconds(3));
  const int reached = p->GetCurrentInstructionIndex();
  scheduler.Stop();

  EXPECT_FALSE(p->IsTerminated())
      << "0x500 is inside a 2048-byte address space, so there is no access "
         "violation here";
  ASSERT_TRUE(ended)
      << "the program stalled at instruction " << reached
      << " of 7 after 3 s. Two separate blockers produce this, and which one "
         "is hit shows in the index above:\n"
         "  index 0 - the process was never dispatched at all. Scheduler's "
         "dispatch guard compares the process's WHOLE size against "
         "max-overall-mem, so a 2048-byte process can never run on a "
         "256-byte machine. Under demand paging it should: that is what "
         "paging is for.\n"
         "  index 3 - WRITE 0x500 needs TWO pages resident at once, page 5 "
         "for the data and page 0 for the symbol table holding varA, but "
         "there is only ONE frame. A faulting instruction restarts from the "
         "beginning, so the page it faulted in first is evicted to satisfy "
         "the second, forever.\n"
         "Logs:"
      << JoinLogs(p);

  EXPECT_TRUE(LogsContain(p, "Variable A: 15"))
      << "expected \"Variable A: 15\" (10 + 5). Logs:" << JoinLogs(p);
  EXPECT_TRUE(LogsContain(p, "Result: 15"))
      << "expected \"Result: 15\" read back from 0x500. Logs:" << JoinLogs(p);
}

} // namespace ScreenCQuestion5Execution
