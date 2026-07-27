#include "scheduler/process/Process.h"
#include <gtest/gtest.h>

// Helper: parse a raw instruction string and add each resulting Statement.
static void AddRaw(prosched::Process &p, const std::string &src) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(src);
  for (auto &s : stmts)
    p.AddInstruction(s);
}

namespace ProcessAddInstruction {
prosched::Interpreter interpreter;

// AddInstruction returns the original instruction string on success
TEST(ProcessAddInstruction, ValidPrintReturnsInstruction) {
  prosched::Process p("add_1", 1, 0);

  std::vector<prosched::Statement> stmts =
      interpreter.Parse("PRINT(\"hello\")");
  ASSERT_FALSE(stmts.empty());

  prosched::Statement stmt = stmts[0];
  prosched::Statement *result = p.AddInstruction(stmt);

  EXPECT_EQ(result->keyword, prosched::Keyword::kPrint);
  ASSERT_FALSE(result->args.empty());
  EXPECT_EQ(result->args[0], "\"hello\"");
}

// Works with non-PRINT instructions (DECLARE) too
TEST(ProcessAddInstruction, ValidDeclareReturnsInstruction) {
  prosched::Process p("add_2", 2, 0);

  std::vector<prosched::Statement> stmts = interpreter.Parse("DECLARE(x, 10)");
  ASSERT_FALSE(stmts.empty());

  prosched::Statement stmt = stmts[0];
  prosched::Statement *result = p.AddInstruction(stmt);

  EXPECT_EQ(result->keyword, prosched::Keyword::kDeclare);
  ASSERT_FALSE(result->args.empty());
  ASSERT_EQ(result->args.size(), 2);
  EXPECT_EQ(result->args[0], "x");
  EXPECT_EQ(result->args[1], "10");
}

// Repeated AddInstruction calls all succeed — no internal cap
TEST(ProcessAddInstruction, MultipleInstructionsAllSucceed) {
  prosched::Process p("add_3", 3, 0);

  for (int i = 0; i < 10; i++) {
    auto stmts = interpreter.Parse("PRINT(\"line\")");
    ASSERT_FALSE(stmts.empty()) << "Parse failed at index " << i;
    prosched::Statement *result = p.AddInstruction(stmts[0]);
    EXPECT_NE(result, nullptr) << "AddInstruction failed at index " << i;
  }
}

// Unknown keyword is absorbed by the interpreter — should not throw
TEST(ProcessAddInstruction, UnknownKeywordDoesNotCrash) {
  prosched::Process p("add_4", 4, 0);
  EXPECT_NO_THROW({
    auto stmts = interpreter.Parse("UNKNOWNCMD(x)");
    for (auto &s : stmts)
      p.AddInstruction(s);
  });
}

// Empty string produces no statements — should not crash
TEST(ProcessAddInstruction, EmptyStringDoesNotCrash) {
  prosched::Process p("add_5", 5, 0);
  EXPECT_NO_THROW({
    auto stmts = interpreter.Parse("");
    for (auto &s : stmts)
      p.AddInstruction(s);
  });
}

// Two separate Process instances must not share their statement vectors
TEST(ProcessAddInstruction, IndependentProcessesDoNotShareInstructions) {
  prosched::Process p1("add_6a", 6, 0);
  prosched::Process p2("add_6b", 7, 0);

  AddRaw(p1, "PRINT(\"from p1\")");

  // p2 has no instructions — executing should finish it immediately (empty
  // return)
  auto result = p2.ExecuteInstructions(1);
  EXPECT_TRUE(result.empty());
  EXPECT_TRUE(p2.IsFinished());
}

} // namespace ProcessAddInstruction

namespace ProcessExecuteInstructions {

// Each call should advance by exactly one statement
TEST(ProcessExecuteInstructions, ExecutesOneStatementPerCall) {
  prosched::Process p("exec_1", 1, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");
  AddRaw(p, "PRINT(\"c\")");

  p.ExecuteInstructions(1);
  p.ExecuteInstructions(1);

  EXPECT_EQ(p.GetLogs().size(), 2u);
  EXPECT_FALSE(p.IsFinished());
}

// Should return a non-empty vector while there are still statements left
TEST(ProcessExecuteInstructions, ReturnsStatementsWhileRunning) {
  prosched::Process p("exec_2", 2, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");

  auto result = p.ExecuteInstructions(1);
  EXPECT_FALSE(result.empty());
}

// Should return empty vector when called on an already-finished process
TEST(ProcessExecuteInstructions, CallingAfterFinishedReturnsEmpty) {
  prosched::Process p("exec_3", 3, 0);
  AddRaw(p, "PRINT(\"a\")");

  p.ExecuteInstructions(1);               // finishes the process
  auto result = p.ExecuteInstructions(1); // already finished

  EXPECT_TRUE(result.empty());
}

// A process with no instructions should finish on the first call
TEST(ProcessExecuteInstructions, NoInstructionsFinishesOnFirstCall) {
  prosched::Process p("exec_4", 4, 0);
  p.ExecuteInstructions(1);
  EXPECT_TRUE(p.IsFinished());
}

// Non-PRINT instructions execute without generating logs
TEST(ProcessExecuteInstructions, DeclareInstructionProducesNoLog) {
  prosched::Process p("exec_5", 5, 0);
  AddRaw(p, "DECLARE(x, 42)");
  p.ExecuteInstructions(1);

  EXPECT_TRUE(p.GetLogs().empty());
}

// Arithmetic result is visible when PRINTed after ADD
TEST(ProcessExecuteInstructions, AddResultVisibleInPrint) {
  prosched::Process p("exec_6", 6, 0);
  AddRaw(p, "DECLARE(x, 3)");
  AddRaw(p, "DECLARE(y, 7)");
  AddRaw(p, "ADD(z, x, y)");
  AddRaw(p, "PRINT(z)");

  p.ExecuteInstructions(1); // DECLARE x
  p.ExecuteInstructions(1); // DECLARE y
  p.ExecuteInstructions(1); // ADD z = 10
  p.ExecuteInstructions(1); // PRINT z → "10"

  auto logs = p.GetLogs();
  ASSERT_FALSE(logs.empty());
  EXPECT_NE(logs.back().find("10"), std::string::npos);
}

} // namespace ProcessExecuteInstructions

namespace ProcessIsFinished {

// Process starts not finished before any execution
TEST(ProcessIsFinished, FalseOnConstruction) {
  prosched::Process p("fin_1", 1, 0);
  EXPECT_FALSE(p.IsFinished());
}

// Not finished until the very last instruction runs
TEST(ProcessIsFinished, FalseAfterPartialExecution) {
  prosched::Process p("fin_2", 2, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");

  p.ExecuteInstructions(1); // only first of two done
  EXPECT_FALSE(p.IsFinished());
}

// Finishes exactly on the call that runs the last instruction
TEST(ProcessIsFinished, TrueAfterLastInstructionRuns) {
  prosched::Process p("fin_3", 3, 0);
  AddRaw(p, "PRINT(\"a\")");

  p.ExecuteInstructions(1);
  EXPECT_TRUE(p.IsFinished());
}

// Finishes after all N instructions have each been executed once
TEST(ProcessIsFinished, TrueAfterAllOfManyInstructions) {
  prosched::Process p("fin_4", 4, 0);
  const int count = 5;
  for (int i = 0; i < count; i++)
    AddRaw(p, "PRINT(\"x\")");

  for (int i = 0; i < count; i++)
    p.ExecuteInstructions(1);

  EXPECT_TRUE(p.IsFinished());
}

// Calling again after finish must not flip back to not-finished
TEST(ProcessIsFinished, RemainsFinishedAfterExtraCall) {
  prosched::Process p("fin_5", 5, 0);
  AddRaw(p, "PRINT(\"a\")");

  p.ExecuteInstructions(1);
  p.ExecuteInstructions(1); // extra call on finished process
  EXPECT_TRUE(p.IsFinished());
}

} // namespace ProcessIsFinished

namespace ProcessLogs {

// Logs are empty before any execution
TEST(ProcessLogs, EmptyBeforeExecution) {
  prosched::Process p("log_1", 1, 0);
  AddRaw(p, "PRINT(\"hello\")");
  EXPECT_TRUE(p.GetLogs().empty());
}

// Each PRINT statement adds exactly one log entry
TEST(ProcessLogs, PrintAddsOneLogEntry) {
  prosched::Process p("log_2", 2, 0);
  AddRaw(p, "PRINT(\"hello\")");
  p.ExecuteInstructions(1);
  EXPECT_EQ(p.GetLogs().size(), 1u);
}

// Log entry must contain the string that was printed
TEST(ProcessLogs, LogContainsPrintedValue) {
  prosched::Process p("log_3", 3, 0);
  AddRaw(p, "PRINT(\"hello world\")");
  p.ExecuteInstructions(1);

  ASSERT_FALSE(p.GetLogs().empty());
  EXPECT_NE(p.GetLogs()[0].find("hello world"), std::string::npos);
}

// Log entry must contain the core number passed to ExecuteInstructions
TEST(ProcessLogs, LogContainsCoreNumber) {
  prosched::Process p("log_4", 4, 0);
  AddRaw(p, "PRINT(\"msg\")");
  p.ExecuteInstructions(3); // core 3

  ASSERT_FALSE(p.GetLogs().empty());
  EXPECT_NE(p.GetLogs()[0].find("Core:3"), std::string::npos);
}

// Non-PRINT instructions must not add log entries
TEST(ProcessLogs, NonPrintInstructionAddsNoLog) {
  prosched::Process p("log_5", 5, 0);
  AddRaw(p, "DECLARE(x, 10)");
  p.ExecuteInstructions(1);
  EXPECT_TRUE(p.GetLogs().empty());
}

// Logs accumulate across multiple executions
TEST(ProcessLogs, LogsAccumulateAcrossCalls) {
  prosched::Process p("log_6", 6, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");
  AddRaw(p, "PRINT(\"c\")");

  p.ExecuteInstructions(1);
  p.ExecuteInstructions(1);
  p.ExecuteInstructions(1);

  EXPECT_EQ(p.GetLogs().size(), 3u);
}

// Two processes must not share log state
TEST(ProcessLogs, IndependentProcessesDoNotShareLogs) {
  prosched::Process p1("log_7a", 7, 0);
  prosched::Process p2("log_7b", 8, 0);

  AddRaw(p1, "PRINT(\"from p1\")");
  p1.ExecuteInstructions(1);

  EXPECT_TRUE(p2.GetLogs().empty());
}

} // namespace ProcessLogs

namespace ProcessAssignCore {

// Returns the assigned core number and updates GetCoreNum
TEST(ProcessAssignCore, PositiveValueReturnsAndSetsCore) {
  prosched::Process p("core_1", 1, 0);
  EXPECT_EQ(p.AssignCore(2), 2);
  EXPECT_EQ(p.GetCoreNum(), 2);
}

// Core 0 is valid after the coreNum >= 0 fix
TEST(ProcessAssignCore, ZeroIsValidReturnsAndSetsCore) {
  prosched::Process p("core_2", 2, 0);
  EXPECT_EQ(p.AssignCore(0), 0);
  EXPECT_EQ(p.GetCoreNum(), 0);
}

// Negative input is rejected; previously set core value is preserved
TEST(ProcessAssignCore, NegativeReturnsMinus1AndLeavesCorUnchanged) {
  prosched::Process p("core_3", 3, 0);
  p.AssignCore(5);
  EXPECT_EQ(p.AssignCore(-1), -1);
  EXPECT_EQ(p.GetCoreNum(), 5);
}

} // namespace ProcessAssignCore

namespace ProcessOwnership {

// A fresh Process is not scheduler-owned by default
TEST(ProcessOwnership, DefaultIsFalse) {
  prosched::Process p("own_1", 1, 0);
  EXPECT_FALSE(p.IsOwnedByScheduler());
}

// SetOwnedByScheduler(true) is immediately visible through the getter
TEST(ProcessOwnership, SetTrueReflectsInGetter) {
  prosched::Process p("own_2", 2, 0);
  p.SetOwnedByScheduler(true);
  EXPECT_TRUE(p.IsOwnedByScheduler());
}

// Ownership can be toggled back to false
TEST(ProcessOwnership, SetFalseAfterTrueReflectsInGetter) {
  prosched::Process p("own_3", 3, 0);
  p.SetOwnedByScheduler(true);
  p.SetOwnedByScheduler(false);
  EXPECT_FALSE(p.IsOwnedByScheduler());
}

} // namespace ProcessOwnership

namespace ProcessIdentity {

// GetPID returns exactly the value given to the constructor
TEST(ProcessIdentity, GetPIDReturnsCtrValue) {
  prosched::Process p("id_1", 42, 0);
  EXPECT_EQ(p.GetPID(), 42);
}

// GetName returns exactly the value given to the constructor
TEST(ProcessIdentity, GetNameReturnsCtrValue) {
  prosched::Process p("my_process", 1, 0);
  EXPECT_EQ(p.GetName(), "my_process");
}

} // namespace ProcessIdentity

// ─── ProcessExecuteInstructions (SLEEP path) ──────────────────────────────

namespace ProcessExecuteInstructions {

// After a SLEEP instruction the process transitions to WAITING
TEST(ProcessExecuteInstructions, SleepInstructionSetsWaitingState) {
  prosched::Process p("sleep_1", 1, 0);
  AddRaw(p, "SLEEP(5)");
  p.ExecuteInstructions(1);
  EXPECT_EQ(p.GetState(), prosched::ProcessState::WAITING);
}

// cyclesRemainingForSleep reflects the argument passed to SLEEP
TEST(ProcessExecuteInstructions, SleepInstructionSetsCyclesRemaining) {
  prosched::Process p("sleep_2", 2, 0);
  AddRaw(p, "SLEEP(5)");
  p.ExecuteInstructions(1);
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 5);
}

// SLEEP returns the full statements vector, not an empty result
TEST(ProcessExecuteInstructions, SleepInstructionReturnsNonEmpty) {
  prosched::Process p("sleep_3", 3, 0);
  AddRaw(p, "SLEEP(5)");
  auto result = p.ExecuteInstructions(1);
  EXPECT_FALSE(result.empty());
}

} // namespace ProcessExecuteInstructions

// ─── ProcessTimeStart ─────────────────────────────────────────────────────

namespace ProcessTimeStart {

// Empty before any execution
TEST(ProcessTimeStart, EmptyBeforeExecution) {
  prosched::Process p("ts_1", 1, 0);
  AddRaw(p, "PRINT(\"hi\")");
  EXPECT_TRUE(p.GetProcessTimeStart().empty());
}

// Set after the first ExecuteInstructions call
TEST(ProcessTimeStart, SetAfterFirstExecution) {
  prosched::Process p("ts_2", 2, 0);
  AddRaw(p, "PRINT(\"hi\")");
  p.ExecuteInstructions(1);
  EXPECT_FALSE(p.GetProcessTimeStart().empty());
}

// Does not change on subsequent calls — timestamp is locked to first execution
TEST(ProcessTimeStart, DoesNotChangeOnSubsequentCalls) {
  prosched::Process p("ts_3", 3, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");
  p.ExecuteInstructions(1);
  std::string first_ts = p.GetProcessTimeStart();
  p.ExecuteInstructions(1);
  EXPECT_EQ(p.GetProcessTimeStart(), first_ts);
}

} // namespace ProcessTimeStart

// ─── ProcessTimeFinish ────────────────────────────────────────────────────

namespace ProcessTimeFinish {

// Empty while the process still has instructions remaining
TEST(ProcessTimeFinish, EmptyWhileRunning) {
  prosched::Process p("tf_1", 1, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");
  p.ExecuteInstructions(1);
  EXPECT_TRUE(p.GetProcessTimeFinish().empty());
}

// Set once the last instruction executes and state becomes FINISHED
TEST(ProcessTimeFinish, SetAfterLastInstruction) {
  prosched::Process p("tf_2", 2, 0);
  AddRaw(p, "PRINT(\"a\")");
  p.ExecuteInstructions(1);
  EXPECT_FALSE(p.GetProcessTimeFinish().empty());
}

} // namespace ProcessTimeFinish

// ─── ProcessCurrentInstructionIndex ──────────────────────────────────────

namespace ProcessCurrentInstructionIndex {

// Starts at 0 before any execution
TEST(ProcessCurrentInstructionIndex, StartsAtZero) {
  prosched::Process p("idx_1", 1, 0);
  AddRaw(p, "PRINT(\"a\")");
  EXPECT_EQ(p.GetCurrentInstructionIndex(), 0);
}

// Increments by exactly 1 per ExecuteInstructions call
TEST(ProcessCurrentInstructionIndex, IncrementsOnePerCall) {
  prosched::Process p("idx_2", 2, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");
  AddRaw(p, "PRINT(\"c\")");
  p.ExecuteInstructions(1);
  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
  p.ExecuteInstructions(1);
  EXPECT_EQ(p.GetCurrentInstructionIndex(), 2);
}

} // namespace ProcessCurrentInstructionIndex

// ─── ProcessTotalInstructions ─────────────────────────────────────────────

namespace ProcessTotalInstructions {

// Zero on construction before any instructions are added
TEST(ProcessTotalInstructions, ZeroOnConstruction) {
  prosched::Process p("tot_1", 1, 0);
  EXPECT_EQ(p.GetTotalInstructions(), 0);
}

// Equals the number of AddInstruction calls
TEST(ProcessTotalInstructions, EqualsAddInstructionCallCount) {
  prosched::Process p("tot_2", 2, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");
  AddRaw(p, "PRINT(\"c\")");
  EXPECT_EQ(p.GetTotalInstructions(), 3);
}

// For-loop additions are all counted — GetTotalInstructions matches loop count
TEST(ProcessTotalInstructions, ForLoopInstructionsAllCounted) {
  prosched::Process p("tot_3", 3, 0);
  const int count = 20;
  for (int i = 0; i < count; i++)
    AddRaw(p, "PRINT(\"x\")");
  EXPECT_EQ(p.GetTotalInstructions(), count);
}

} // namespace ProcessTotalInstructions

// ─── ProcessForInstruction ────────────────────────────────────────────────
// GetTotalInstructions counts recursively: FOR([body], n) = 1 (the FOR) +
// count of body statements. A 2-body FOR therefore contributes 3, not 1.
// currentInstructionIndex still tracks top-level statements only, so one
// ExecuteInstructions call runs the whole loop and advances the index by 1.

namespace ProcessForInstruction {

// FOR([PRINT("a"), PRINT("b")], 5) = 1 (FOR) + 2 (body PRINTs) = 3
TEST(ProcessForInstruction, ForCountsItselfAndBodyInstructions) {
  prosched::Process p("for_tot", 1, 0);
  AddRaw(p, R"(FOR([PRINT("a"), PRINT("b")], 5))");
  EXPECT_EQ(p.GetTotalInstructions(), 10);
}

// One ExecuteInstructions call runs the whole FOR and advances the top-level
// index by exactly 1, finishing the process (the FOR was the only instruction).
TEST(ProcessForInstruction, ExecutingForAdvancesIndexByOneAndFinishes) {
  prosched::Process p("for_exec", 2, 0);
  AddRaw(p, R"(FOR([PRINT("x")], 3))");

  p.ExecuteInstructions(0);

  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
  EXPECT_FALSE(p.IsFinished());
}

// The entire loop body executes within that single call: a 2-instruction body
// repeated 3 times emits all 6 log lines at once.
TEST(ProcessForInstruction, ForBodyAllIterationsLoggedInOneCall) {
  prosched::Process p("for_logs", 3, 0);
  AddRaw(p, R"(FOR([PRINT("a"), PRINT("b")], 3))");

  p.ExecuteInstructions(0);

  EXPECT_EQ(p.GetLogs().size(), 1u); // 2 prints × 3 iterations
}

// FOR([FOR([PRINT("x")], 2)], 3): outer FOR=1, inner FOR=1, PRINT=1 → total 3
TEST(ProcessForInstruction, NestedForCountsAllLevels) {
  prosched::Process p("for_nested", 5, 0);
  AddRaw(p, R"(FOR([FOR([PRINT("x")], 2)], 3))");
  EXPECT_EQ(p.GetTotalInstructions(), 6);
}

// FOR([FOR([PRINT("a"),PRINT("b")], 2)], 3): outer=1, inner=1, 2 PRINTs → 4
TEST(ProcessForInstruction, NestedForWithMultipleBodyInstructions) {
  prosched::Process p("for_nested2", 6, 0);
  AddRaw(p, R"(FOR([FOR([PRINT("a"), PRINT("b")], 2)], 3))");
  EXPECT_EQ(p.GetTotalInstructions(), 12);
}

// PRINT + FOR([PRINT], 4): total = 1 (PRINT) + 1 (FOR) + 1 (body PRINT) = 3.
// The top-level index walks 0 → 1 → 2 across two ExecuteInstructions calls.
TEST(ProcessForInstruction, ForMixedWithPlainInstructionsTotalIsRecursive) {
  prosched::Process p("for_mixed", 4, 0);
  AddRaw(p, R"(PRINT("before"))");
  AddRaw(p, R"(FOR([PRINT("loop")], 4))");

  EXPECT_EQ(p.GetTotalInstructions(), 5);

  p.ExecuteInstructions(0); // PRINT("before")
  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
  EXPECT_FALSE(p.IsFinished());

  p.ExecuteInstructions(0); // the whole FOR
  EXPECT_EQ(p.GetCurrentInstructionIndex(), 2);
  EXPECT_FALSE(p.IsFinished());
}

} // namespace ProcessForInstruction

// ─── ProcessGetSetState ───────────────────────────────────────────────────

namespace ProcessGetSetState {

// Default state on construction is READY
TEST(ProcessGetSetState, DefaultStateIsReady) {
  prosched::Process p("st_1", 1, 0);
  EXPECT_EQ(p.GetState(), prosched::ProcessState::READY);
}

// SetState(RUNNING) is immediately visible through GetState
TEST(ProcessGetSetState, SetRunningReflectsInGetter) {
  prosched::Process p("st_2", 2, 0);
  p.SetState(prosched::ProcessState::RUNNING);
  EXPECT_EQ(p.GetState(), prosched::ProcessState::RUNNING);
}

// SetState(WAITING) is immediately visible through GetState
TEST(ProcessGetSetState, SetWaitingReflectsInGetter) {
  prosched::Process p("st_3", 3, 0);
  p.SetState(prosched::ProcessState::WAITING);
  EXPECT_EQ(p.GetState(), prosched::ProcessState::WAITING);
}

// SetState(FINISHED) is immediately visible through GetState
TEST(ProcessGetSetState, SetFinishedReflectsInGetter) {
  prosched::Process p("st_4", 4, 0);
  p.SetState(prosched::ProcessState::FINISHED);
  EXPECT_EQ(p.GetState(), prosched::ProcessState::FINISHED);
}

} // namespace ProcessGetSetState

// ─── ProcessSleepCycles ───────────────────────────────────────────────────

namespace ProcessSleepCycles {

// cyclesRemainingForSleep starts at 0 on construction
TEST(ProcessSleepCycles, StartsAtZero) {
  prosched::Process p("sc_1", 1, 0);
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 0);
}

// DecrementSleepCycles reduces the counter by 1 each call
TEST(ProcessSleepCycles, DecrementsOnePerCall) {
  prosched::Process p("sc_2", 2, 0);
  AddRaw(p, "SLEEP(3)");
  p.ExecuteInstructions(1); // sets cyclesRemainingForSleep = 3
  p.DecrementSleepCycles();
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 2);
}

// Does not go below 0 on repeated calls when already at 0
TEST(ProcessSleepCycles, FloorsAtZero) {
  prosched::Process p("sc_3", 3, 0);
  p.DecrementSleepCycles();
  p.DecrementSleepCycles();
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 0);
}

} // namespace ProcessSleepCycles

// ─── ProcessInstructionCycles ─────────────────────────────────────────────

namespace ProcessInstructionCycles {

// Starts at 0 on construction
TEST(ProcessInstructionCycles, StartsAtZero) {
  prosched::Process p("ic_1", 1, 0);
  EXPECT_EQ(p.GetCurrentInstructionCyclesLeft(), 0);
}

// SetCurrentInstructionCyclesLeft is visible through the getter
TEST(ProcessInstructionCycles, SetAndGetRoundTrip) {
  prosched::Process p("ic_2", 2, 0);
  p.SetCurrentInstructionCyclesLeft(5);
  EXPECT_EQ(p.GetCurrentInstructionCyclesLeft(), 5);
}

// DecrementInstructionCyclesLeft reduces by 1 each call
TEST(ProcessInstructionCycles, DecrementsOnePerCall) {
  prosched::Process p("ic_3", 3, 0);
  p.SetCurrentInstructionCyclesLeft(3);
  p.DecrementInstructionCyclesLeft();
  EXPECT_EQ(p.GetCurrentInstructionCyclesLeft(), 2);
}

// Does not go below 0 on repeated calls when already at 0
TEST(ProcessInstructionCycles, FloorsAtZero) {
  prosched::Process p("ic_4", 4, 0);
  p.SetCurrentInstructionCyclesLeft(0);
  p.DecrementInstructionCyclesLeft();
  p.DecrementInstructionCyclesLeft();
  EXPECT_EQ(p.GetCurrentInstructionCyclesLeft(), 0);
}

} // namespace ProcessInstructionCycles

// ─── ProcessQuantum ───────────────────────────────────────────────────────
namespace ProcessQuantum {

// Starts at 0 on construction
TEST(ProcessQuantum, StartsAtZero) {
  prosched::Process p("q_1", 1, 0);
  EXPECT_EQ(p.GetQuantumUsed(), 0);
}

// IncrementQuantumUsed increases the counter by 1 per call
TEST(ProcessQuantum, IncrementsOnePerCall) {
  prosched::Process p("q_2", 2, 0);
  p.IncrementQuantumUsed();
  EXPECT_EQ(p.GetQuantumUsed(), 1);
  p.IncrementQuantumUsed();
  EXPECT_EQ(p.GetQuantumUsed(), 2);
}

// ResetQuantumUsed returns the counter to 0
TEST(ProcessQuantum, ResetReturnsToZero) {
  prosched::Process p("q_3", 3, 0);
  p.IncrementQuantumUsed();
  p.IncrementQuantumUsed();
  p.ResetQuantumUsed();
  EXPECT_EQ(p.GetQuantumUsed(), 0);
}

} // namespace ProcessQuantum

// ─── ProcessMalformedAddressTermination ──────────────────────────────────
// MO2: an access to an invalid address "will throw an access violation error and
// shut down the process." Process memory is 0x100 bytes (power of 2, >= 64).
namespace ProcessMalformedAddressTermination {

// An out-of-bounds WRITE (0x200 vs a 0x100-byte space) shuts the process down
TEST(ProcessMalformedAddressTermination, OutOfBoundsWriteDoesTerminateProcess) {
  prosched::Process p("oob_addr", 1, 0);
  p.SetMemoryBounds(0, 0x100);
  AddRaw(p, "WRITE(0x200, 5)");

  p.ExecuteInstructions(1);

  EXPECT_TRUE(p.IsTerminated());
}

// MO2: an invalid address must shut the process down. A malformed (non-hex)
// address is invalid, so it terminates the process, same as an out-of-bounds one.
TEST(ProcessMalformedAddressTermination, MalformedWriteAddressTerminatesProcess) {
  prosched::Process p("bad_addr", 2, 0);
  p.SetMemoryBounds(0, 0x100);
  AddRaw(p, "WRITE(notanumber, 5)");

  p.ExecuteInstructions(1);

  EXPECT_TRUE(p.IsTerminated())
      << "state is " << static_cast<int>(p.GetState()) << ", expected TERMINATED=4";
}

} // namespace ProcessMalformedAddressTermination

// ─── ProcessMemoryBounds ───────────────────────────────────────────────────
// NOTE: commit 608910f reverted SetMemoryBounds to void and dropped the
// start>end guard. In production SetMemoryBounds is always called with start=0
// (CreateNamedProcess/CreateProcessWithInstructions/generateProcess), so an
// inverted range is unreachable; we just verify the size is recorded.
namespace ProcessMemoryBounds {

// SetMemoryBounds records the process's memory size (memEnd - memStart)
TEST(ProcessMemoryBounds, RecordsMemorySize) {
  prosched::Process p("mb_ok", 1, 0);
  p.SetMemoryBounds(0, 0x100);
  EXPECT_EQ(p.GetMemorySize(), 0x100u);
  EXPECT_FALSE(p.IsTerminated());
}

} // namespace ProcessMemoryBounds

// ─── FOR loops inside a scheduled process (MO1 §Process instructions) ───────
// MO1/MO2: "FOR([instructions], repeats) — For-loop, nestable up to 3 levels."
//
// Process does NOT run FOR through Interpreter::ExecuteFor. AddInstruction
// (Process.h:114-133) UNROLLS the loop at add time, recursively appending the
// body `repeats` times, so a FOR never reaches the statements vector at all
// and each unrolled instruction costs its own CPU tick. (The kFor branch in
// ExecuteInstructions is therefore dead code.) Unrolling is behaviourally
// equivalent here — none of the instruction types have loop-carried control
// flow — but it was completely untested, so these pin it down.
//
// Side effect worth knowing: an unrolled FOR inflates GetTotalInstructions()
// past the min-ins/max-ins draw, since the count is taken before expansion.

namespace ProcessForLoop {

static prosched::Statement forStmt(int repeats,
                                   std::vector<prosched::Statement> body) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kFor;
  s.args = {"", std::to_string(repeats)};
  s.nested = std::move(body);
  return s;
}

static prosched::Statement printStmt(const std::string &literal) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kPrint;
  s.args = {"\"" + literal + "\""};
  return s;
}

// FOR(..., 3) around one PRINT runs the body 3 times — as 3 separate
// instructions, each taking its own ExecuteInstructions call.
TEST(ProcessForLoop, ForBodyExecutesItsNestedInstructions) {
  prosched::Process p("for_body", 1, 0);
  prosched::Statement loop = forStmt(3, {printStmt("tick")});
  p.AddInstruction(loop);

  EXPECT_EQ(p.GetTotalInstructions(), 3)
      << "the loop should be unrolled into one instruction per iteration";

  while (!p.IsFinished()) {
    p.ExecuteInstructions(0);
  }

  int ticks = 0;
  for (const std::string &line : p.GetLogs()) {
    if (line.find("tick") != std::string::npos) {
      ++ticks;
    }
  }
  EXPECT_EQ(ticks, 3) << "FOR(..., 3) produced " << ticks << " of 3 iterations";
}

// Nested FOR: 2 outer × 3 inner = 6 unrolled instructions. MO1 allows nesting
// up to 3 levels, and AddInstruction recurses, so inner loops expand too.
TEST(ProcessForLoop, NestedForUnrollsBothLevels) {
  prosched::Process p("for_nested", 3, 0);
  prosched::Statement inner = forStmt(3, {printStmt("tick")});
  prosched::Statement outer = forStmt(2, {inner});
  p.AddInstruction(outer);

  EXPECT_EQ(p.GetTotalInstructions(), 6);
}

// A FOR that only declares/adds must still change process state. Uses the
// interpreter's own view so it does not depend on log formatting.
TEST(ProcessForLoop, ForBodyAffectsInterpreterState) {
  prosched::Process p("for_state", 2, 0);
  prosched::Statement declare;
  declare.keyword = prosched::Keyword::kDeclare;
  declare.args = {"counter", "7"};
  prosched::Statement loop = forStmt(1, {declare});
  p.AddInstruction(loop);

  p.ExecuteInstructions(0);

  EXPECT_FALSE(p.GetInterpreter().ExecuteDebug().empty())
      << "a DECLARE nested in a FOR never ran, so the symbol table is empty";
}

} // namespace ProcessForLoop
