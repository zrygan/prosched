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
// `screen -c <process_name> <process_memory_size> "<instructions>"`. The
// missing token has to be recognisable as missing, so ExecuteCommand can roll
// a size from the config instead of rejecting the whole command.
TEST(ScreenCQuestion5Parsing, CommandWithoutAMemorySizeIsMarkedUnspecified) {
  const std::string input =
      std::string("screen -c faulty_process \"") + kQuestion5Program + "\"";

  Controller controller;
  Command parsed = controller.GetParsedInput(input);

  EXPECT_EQ(parsed.cliCommand, CLI_COMMAND::CLI_SCREEN_C);
  EXPECT_EQ(parsed.processName, "faulty_process");
  EXPECT_FALSE(parsed.memorySizeSpecified)
      << "no size token was typed, so the command must not be treated as "
         "carrying one - otherwise it is rejected with \"invalid memory "
         "allocation\" and the process is never created";

  // The program itself is well-formed - the first word of it must never be
  // mistaken for the size.
  std::vector<prosched::Statement> program;
  prosched::Interpreter parser;
  EXPECT_TRUE(parser.ParseUserProgram(parsed.instructions, program));
  EXPECT_EQ(program.size(), 7u);
}

// An omitted size and a malformed one are different failures: only the second
// is an error. Nothing about "screen -c myproc abc" should now be forgiven.
TEST(ScreenCQuestion5Parsing, MalformedSizeIsStillSpecifiedAndInvalid) {
  Controller controller;
  Command parsed =
      controller.GetParsedInput(R"~(screen -c myproc abc "PRINT(x)")~");

  EXPECT_EQ(parsed.cliCommand, CLI_COMMAND::CLI_SCREEN_C);
  EXPECT_TRUE(parsed.memorySizeSpecified);
  EXPECT_FALSE(Controller::IsValidMemoryAllocation(parsed.memorySize));
}

TEST(ScreenCQuestion5Parsing, ExplicitSizeIsStillParsedAndMarkedSpecified) {
  Controller controller;
  Command parsed =
      controller.GetParsedInput(R"~(screen -c myproc 256 "PRINT(x)")~");

  EXPECT_TRUE(parsed.memorySizeSpecified);
  EXPECT_EQ(parsed.memorySize, 256);
}

// "screen -s" takes the same size argument and the same omission.
TEST(ScreenCQuestion5Parsing, ScreenSWithoutASizeIsMarkedUnspecified) {
  Controller controller;
  Command parsed = controller.GetParsedInput("screen -s myproc");

  EXPECT_EQ(parsed.cliCommand, CLI_COMMAND::CLI_SCREEN_S);
  EXPECT_EQ(parsed.processName, "myproc");
  EXPECT_FALSE(parsed.memorySizeSpecified);
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
//
// The machine is sized to 2048 bytes here rather than the config's 256. A
// process may no longer be dispatched while its declared address space exceeds
// physical memory (Scheduler::FitsInPhysicalMemory, required by grading
// question 7's deadlock), so asking for a 2048-byte address space means asking
// for a machine that can hold one. Question 5 itself is unaffected: it types
// "screen -c faulty_process" with no size, which rolls the config's 256 bytes
// and fits - that path is pinned by
// ConfigSizedProcessDiesOnTheOutOfRangeWrite above.
TEST(ScreenCQuestion5Execution, ProgramPrintsBothVariablesWhenTheAddressFits) {
  prosched::PagingManager pm(256, 2048);
  ASSERT_EQ(pm.GetTotalFrameCount(), 8)
      << "precondition: 2048 bytes of memory in 256-byte frames = 8 frames, "
         "enough to hold the 2048-byte process the program needs";

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
         "  index 0 - the process was never dispatched at all, which here "
         "would mean Scheduler::FitsInPhysicalMemory rejected a process that "
         "does fit (2048 bytes of process, 2048 bytes of memory); the rule is "
         "meant to be exclusive only ABOVE physical memory.\n"
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

namespace ScreenCQuestion5LowAddress {

// The same scenario with the address moved to 0x000, which is the form the
// grading script now types. 0 is inside a 256-byte space and sits on page 0 -
// the page the symbol table already occupies - so the WRITE needs only the one
// frame this config has, and the whole program runs on the config's own size.
const char *kLowAddressProgram =
    "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x000 varA; "
    "READ varC 0x000; PRINT(\"Variable A: \" + varA); "
    "PRINT(\"Result: \" + varC)";

TEST(ScreenCQuestion5LowAddress, ProgramPrintsBothVariablesOnAConfigSizedProc) {
  prosched::PagingManager pm(256, 256);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1);

  prosched::Scheduler scheduler(makeQuestion5Ctx(), &pm);

  std::vector<prosched::Statement> program;
  prosched::Interpreter parser;
  ASSERT_TRUE(parser.ParseUserProgram(kLowAddressProgram, program));
  ASSERT_EQ(program.size(), 7u);

  prosched::Process *p =
      scheduler.CreateProcessWithInstructions("faulty_process", 256, program);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(scheduler.AddProcess(p), nullptr);

  ASSERT_TRUE(scheduler.Start());
  const bool ended = RunUntilEnded(scheduler, p, std::chrono::seconds(3));
  scheduler.Stop();

  ASSERT_TRUE(ended) << "the program stalled at instruction "
                     << p->GetCurrentInstructionIndex() << " of 7. Logs:"
                     << JoinLogs(p);
  EXPECT_FALSE(p->IsTerminated())
      << "0x000 is inside a 256-byte address space - no access violation";
  EXPECT_TRUE(LogsContain(p, "Variable A: 15"))
      << "expected \"Variable A: 15\" (10 + 5). Logs:" << JoinLogs(p);
  EXPECT_TRUE(LogsContain(p, "Result: 15"))
      << "expected \"Result: 15\" read back from 0x000. Logs:" << JoinLogs(p);

  // The scenario reads that output through "screen -r" after the program has
  // already ended, which is only possible if a finished process stays
  // reachable by name.
  EXPECT_EQ(scheduler.FindProcessByName("faulty_process"), nullptr);
  EXPECT_EQ(scheduler.FindFinishedProcessByName("faulty_process"), p);
}

// A process shut down by an access violation must keep reporting the violation
// message, not get quietly attached to as if it had completed.
TEST(ScreenCQuestion5LowAddress, TerminatedProcessIsNotTreatedAsFinished) {
  prosched::PagingManager pm(256, 256);
  prosched::Scheduler scheduler(makeQuestion5Ctx(), &pm);

  std::vector<prosched::Statement> program = ParseQuestion5Program(); // 0x500
  ASSERT_EQ(program.size(), 7u);

  prosched::Process *p =
      scheduler.CreateProcessWithInstructions("faulty_process", 256, program);
  ASSERT_NE(scheduler.AddProcess(p), nullptr);

  ASSERT_TRUE(scheduler.Start());
  ASSERT_TRUE(RunUntilEnded(scheduler, p, std::chrono::seconds(3)));
  scheduler.Stop();

  ASSERT_TRUE(p->IsTerminated());
  EXPECT_EQ(scheduler.FindFinishedProcessByName("faulty_process"), nullptr);
  EXPECT_EQ(scheduler.FindTerminatedProcessByName("faulty_process"), p);
}

} // namespace ScreenCQuestion5LowAddress
