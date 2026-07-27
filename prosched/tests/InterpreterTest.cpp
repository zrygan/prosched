#include "commands/Interpreter.h"
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <memory>

static prosched::Statement
makeStmt(prosched::Keyword kw, std::vector<std::string> args = {},
         std::vector<prosched::Statement> nested = {}) {
  prosched::Statement s;
  s.keyword = kw;
  s.args = std::move(args);
  s.nested = std::move(nested);
  return s;
}

// ─── FlushBuffer
// ──────────────────────────────────────────────────────────────

namespace InterpreterFlushBuffer {

// No output produced yet — flush returns an empty vector
TEST(InterpreterFlushBuffer, EmptyBufferReturnsEmptyVector) {
  prosched::Interpreter interp;
  EXPECT_TRUE(interp.FlushBuffer().empty());
}

// A PRINT call adds one entry; flush returns it
TEST(InterpreterFlushBuffer, ReturnsAccumulatedOutput) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(PRINT("hello"))");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "hello");
}

// Second flush after the first returns empty — buffer was cleared
TEST(InterpreterFlushBuffer, ClearedAfterFlush) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(PRINT("hello"))");
  interp.FlushBuffer();
  EXPECT_TRUE(interp.FlushBuffer().empty());
}

// All three PRINT outputs accumulate and are returned in order before flush
TEST(InterpreterFlushBuffer, MultiplePrintsAllAppearBeforeFlush) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(PRINT("a"), PRINT("b"), PRINT("c"))");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 3u);
  EXPECT_EQ(buf[0], "a");
  EXPECT_EQ(buf[1], "b");
  EXPECT_EQ(buf[2], "c");
}

} // namespace InterpreterFlushBuffer

// ─── ExecuteString
// ────────────────────────────────────────────────────────────

namespace InterpreterExecuteString {

// Parsing empty input must not throw or produce output
TEST(InterpreterExecuteString, EmptyStringNocrash) {
  prosched::Interpreter interp;
  EXPECT_NO_THROW(interp.ExecuteString(""));
  EXPECT_TRUE(interp.FlushBuffer().empty());
}

// Whitespace-only input is treated the same as empty
TEST(InterpreterExecuteString, WhitespaceOnlyNocrash) {
  prosched::Interpreter interp;
  EXPECT_NO_THROW(interp.ExecuteString("   "));
  EXPECT_TRUE(interp.FlushBuffer().empty());
}

// PRINT("hello world") buffers the literal string value
TEST(InterpreterExecuteString, ValidPrintProducesOutput) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(PRINT("hello world"))");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "hello world");
}

// Unrecognized keywords push a "[!]" warning entry to the buffer
TEST(InterpreterExecuteString, UnknownKeywordPushesWarning) {
  prosched::Interpreter interp;
  interp.ExecuteString("UNKNOWNCMD(x)");
  auto buf = interp.FlushBuffer();
  ASSERT_FALSE(buf.empty());
  EXPECT_NE(buf[0].find("[!]"), std::string::npos);
}

// DECLARE stores a value; the subsequent PRINT reads it correctly
TEST(InterpreterExecuteString, DeclareAndPrintWorkTogether) {
  prosched::Interpreter interp;
  interp.ExecuteString("DECLARE(x, 42), PRINT(x)");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "42");
}

// DECLARE+ADD pipeline: PRINT shows the computed sum
TEST(InterpreterExecuteString, AddAndPrintWorkTogether) {
  prosched::Interpreter interp;
  interp.ExecuteString("DECLARE(a, 3), DECLARE(b, 4), ADD(c, a, b), PRINT(c)");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "7");
}

} // namespace InterpreterExecuteString

// ─── ExecutePrint
// ─────────────────────────────────────────────────────────────

namespace InterpreterExecutePrint {

// Quoted arg strips the quotes; inner text is returned and buffered
TEST(InterpreterExecutePrint, StringLiteralReturnsCorrectly) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kPrint, {R"("hello")"});
  EXPECT_EQ(interp.ExecutePrint(stmt), "hello");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "hello");
}

// Empty quoted string prints and buffers an empty-string entry
TEST(InterpreterExecutePrint, EmptyStringLiteralReturnsEmpty) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kPrint, {R"("")"});
  EXPECT_EQ(interp.ExecutePrint(stmt), "");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "");
}

// Undeclared variable resolves to "0" — the default value
TEST(InterpreterExecutePrint, UndeclaredVariablePrintsZero) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kPrint, {"notdeclared"});
  EXPECT_EQ(interp.ExecutePrint(stmt), "0");
}

// "str=" + var concatenates a string literal with a variable's value
TEST(InterpreterExecutePrint, ConcatStringAndVariable) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"x", "5"}));
  interp.FlushBuffer();

  auto stmt = makeStmt(prosched::Keyword::kPrint, {R"("val=" + x)"});
  EXPECT_EQ(interp.ExecutePrint(stmt), "val=5");
}

// A PRINT whose concat left-part is empty ("+x") makes ExecutePrint call
// str_part.front() on an empty string — UB, which aborts under
// -D_GLIBCXX_ASSERTIONS (the flag the real exe builds with). Reachable via
// screen -c "PRINT(+x)". Run as a death test: the child must exit cleanly, not
// abort. (Fix: guard the .front()/.back() calls with !empty().)
TEST(InterpreterExecutePrint, EmptyConcatLeftPartDoesNotCrash) {
  EXPECT_EXIT(
      {
        prosched::Interpreter interp;
        interp.ExecuteString("PRINT(+x)");
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), ".*");
}

} // namespace InterpreterExecutePrint

// ─── ExecuteDeclare
// ───────────────────────────────────────────────────────────

namespace InterpreterExecuteDeclare {

// Returns the stored uint16 value on success
TEST(InterpreterExecuteDeclare, BasicDeclarationReturnsValue) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kDeclare, {"x", "10"});
  auto result = interp.ExecuteDeclare(stmt);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 10);
}

// Zero is a valid uint16 value
TEST(InterpreterExecuteDeclare, ZeroValueReturnsZero) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kDeclare, {"x", "0"});
  auto result = interp.ExecuteDeclare(stmt);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 0);
}

// 65535 is the uint16 max — must not overflow or truncate
TEST(InterpreterExecuteDeclare, MaxUint16ReturnsCorrectly) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kDeclare, {"x", "65535"});
  auto result = interp.ExecuteDeclare(stmt);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 65535);
}

// Single-arg DECLARE is malformed — returns nullopt without crashing
TEST(InterpreterExecuteDeclare, MissingSecondArgReturnsNullopt) {
  prosched::Interpreter interp;
  auto stmt = makeStmt(prosched::Keyword::kDeclare, {"x"});
  EXPECT_FALSE(interp.ExecuteDeclare(stmt).has_value());
}

// Declared variable is accessible in subsequent PRINT calls
TEST(InterpreterExecuteDeclare, VariableIsAvailableForSubsequentPrint) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"myvar", "42"}));
  interp.ExecutePrint(makeStmt(prosched::Keyword::kPrint, {"myvar"}));
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "42");
}

} // namespace InterpreterExecuteDeclare

// ─── ExecuteAdd
// ───────────────────────────────────────────────────────────────

namespace InterpreterExecuteAdd {

// Reads two declared vars, computes their sum, stores in a new var
TEST(InterpreterExecuteAdd, TwoVariablesAdded) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"a", "3"}));
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"b", "7"}));
  auto result =
      interp.ExecuteAdd(makeStmt(prosched::Keyword::kAdd, {"c", "a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 10);
}

// Numeric string literals are parsed inline — no prior DECLARE needed
TEST(InterpreterExecuteAdd, LiteralNumericOperands) {
  prosched::Interpreter interp;
  auto result =
      interp.ExecuteAdd(makeStmt(prosched::Keyword::kAdd, {"z", "5", "3"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 8);
}

// dest can be one of the source operands — overwrites correctly
TEST(InterpreterExecuteAdd, OverwritesExistingVariable) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"x", "5"}));
  auto result =
      interp.ExecuteAdd(makeStmt(prosched::Keyword::kAdd, {"x", "x", "3"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 8);
}

// Fewer than 3 args is malformed — returns nullopt
TEST(InterpreterExecuteAdd, MissingArgsReturnsNullopt) {
  prosched::Interpreter interp;
  auto result = interp.ExecuteAdd(makeStmt(prosched::Keyword::kAdd, {"x", "5"}));
  EXPECT_FALSE(result.has_value());
}

// MO2: operands are uint16 (clamped). ResolveOperand uses an unguarded
// std::stoul, so an operand larger than ULONG_MAX throws instead of clamping.
// (ADD, SUBTRACT, and WRITE's value all resolve operands this way.)
TEST(InterpreterExecuteAdd, OverflowOperandDoesNotThrow) {
  prosched::Interpreter interp;
  std::optional<uint16_t> result;
  EXPECT_NO_THROW(result = interp.ExecuteAdd(makeStmt(
      prosched::Keyword::kAdd, {"z", "99999999999999999999999", "0"})));
  EXPECT_TRUE(result.has_value());
}

} // namespace InterpreterExecuteAdd

// ─── ExecuteSubtract
// ──────────────────────────────────────────────────────────

namespace InterpreterExecuteSubtract {

// Subtracts two declared variables and stores the result in a new var
TEST(InterpreterExecuteSubtract, BasicSubtractionCorrect) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"a", "10"}));
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"b", "3"}));
  auto result = interp.ExecuteSubtract(
      makeStmt(prosched::Keyword::kSubtract, {"c", "a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 7);
}

// uint16 underflow wraps around — no exception thrown
TEST(InterpreterExecuteSubtract, Uint16UnderflowWrapsWithoutThrow) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"a", "3"}));
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"b", "10"}));
  auto result = interp.ExecuteSubtract(
      makeStmt(prosched::Keyword::kSubtract, {"c", "a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), static_cast<uint16_t>(3 - 10));
}

// Fewer than 3 args is malformed — returns nullopt
TEST(InterpreterExecuteSubtract, MissingArgsReturnsNullopt) {
  prosched::Interpreter interp;
  auto result =
      interp.ExecuteSubtract(makeStmt(prosched::Keyword::kSubtract, {"x", "5"}));
  EXPECT_FALSE(result.has_value());
}

} // namespace InterpreterExecuteSubtract

// ─── ExecuteSleep
// ─────────────────────────────────────────────────────────────

namespace InterpreterExecuteSleep {

// SLEEP(0) is a no-op and must not block the thread
TEST(InterpreterExecuteSleep, ZeroTicksCompletesWithoutHang) {
  prosched::Interpreter interp;
  EXPECT_NO_THROW(
      interp.ExecuteSleep(makeStmt(prosched::Keyword::kSleep, {"0"})));
}

// SLEEP(1) completes in finite time
TEST(InterpreterExecuteSleep, OneTickCompletes) {
  prosched::Interpreter interp;
  EXPECT_NO_THROW(
      interp.ExecuteSleep(makeStmt(prosched::Keyword::kSleep, {"1"})));
}

// ExecuteSleep uses an unguarded std::stoul on the tick count — a value larger
// than ULONG_MAX throws instead of being handled gracefully.
TEST(InterpreterExecuteSleep, OverflowArgDoesNotThrow) {
  prosched::Interpreter interp;
  EXPECT_NO_THROW(interp.ExecuteSleep(
      makeStmt(prosched::Keyword::kSleep, {"99999999999999999999999"})));
}

} // namespace InterpreterExecuteSleep

// ─── executeFor
// ───────────────────────────────────────────────────────────────

namespace InterpreterExecuteFor {

// FOR body runs exactly N times, one PRINT per iteration
TEST(InterpreterExecuteFor, BasicThreeIterations) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(FOR([PRINT("x")], 3))");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 3u);
  for (const auto &line : buf)
    EXPECT_EQ(line, "x");
}

// FOR with count 0 skips the body entirely
TEST(InterpreterExecuteFor, ZeroIterationsProducesNothing) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(FOR([PRINT("x")], 0))");
  EXPECT_TRUE(interp.FlushBuffer().empty());
}

// Inner loop (2) × outer loop (3) = 6 total PRINT calls
TEST(InterpreterExecuteFor, NestedForMultipliesIterationCount) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(FOR([FOR([PRINT("n")], 2)], 3))");
  auto buf = interp.FlushBuffer();
  EXPECT_EQ(buf.size(), 6u);
}

} // namespace InterpreterExecuteFor

// ─── ExecuteDebug
// ─────────────────────────────────────────────────────────────

namespace InterpreterExecuteDebug {

// No variables declared — buffer contains the "[No variables declared]" message
TEST(InterpreterExecuteDebug, EmptyMemoryShowsNoVarsMessage) {
  prosched::Interpreter interp;
  interp.ExecuteDebug();
  auto buf = interp.FlushBuffer();
  bool found = false;
  for (const auto &line : buf)
    if (line.find("[No variables declared]") != std::string::npos)
      found = true;
  EXPECT_TRUE(found);
}

// Declared variable name appears somewhere in the debug output lines
TEST(InterpreterExecuteDebug, DeclaredVariableAppearsInDump) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"myvar", "99"}));
  interp.FlushBuffer();
  interp.ExecuteDebug();
  auto buf = interp.FlushBuffer();
  bool found = false;
  for (const auto &line : buf)
    if (line.find("myvar") != std::string::npos)
      found = true;
  EXPECT_TRUE(found);
}

// Return value is the live memory map containing all declared variables
TEST(InterpreterExecuteDebug, ReturnsMemoryMapWithAllDeclaredVars) {
  prosched::Interpreter interp;
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"a", "1"}));
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"b", "2"}));
  auto mem = interp.ExecuteDebug();
  EXPECT_EQ(mem.at("a"), 1);
  EXPECT_EQ(mem.at("b"), 2);
}

} // namespace InterpreterExecuteDebug

// ─── ExecuteRead / ExecuteWrite address validation ─────────────────────────

// MO2: memory_address is hexadecimal; reading/writing an address outside the
// process's dedicated space "will throw an access violation error and shut down
// the process." Process memory is 256 bytes (power of 2, >= 64), so [0, 0x100).
namespace InterpreterReadWriteAddressValidation {

// Out-of-bounds WRITE (0x200 vs a 0x100-byte space) is an access violation
TEST(InterpreterReadWriteAddressValidation, OutOfBoundsWriteSetsViolationAndFails) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  auto result = interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x200", "5"}));
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(interp.GetLastInstructionAccessViolation());
  EXPECT_EQ(interp.GetLastViolationAddress(), 0x200u);
}

// Out-of-bounds READ (0x1F4) is an access violation
TEST(InterpreterReadWriteAddressValidation, OutOfBoundsReadSetsViolationAndFails) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  auto result = interp.ExecuteRead(makeStmt(prosched::Keyword::kRead, {"x", "0x1F4"}));
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(interp.GetLastInstructionAccessViolation());
  EXPECT_EQ(interp.GetLastViolationAddress(), 0x1F4u);
}

// MO2: an invalid (non-hex) address must fail gracefully (nullopt), not throw.
// A real process always has memory bounds set, so a malformed address parses out
// of range and is rejected rather than written.
TEST(InterpreterReadWriteAddressValidation,
     MalformedAddressShouldFailGracefullyNotThrow) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  std::optional<std::pair<uint32_t, uint16_t>> result;
  EXPECT_NO_THROW(
      result = interp.ExecuteWrite(
          makeStmt(prosched::Keyword::kWrite, {"notanumber", "5"})));
  EXPECT_FALSE(result.has_value());
}

// MO2: an invalid address must shut the process down — a malformed address (out
// of any real range) raises the access violation, like an out-of-bounds one
TEST(InterpreterReadWriteAddressValidation,
     MalformedAddressShouldSetViolationLikeOutOfBounds) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  interp.ExecuteString("WRITE(notanumber, 5)");
  EXPECT_TRUE(interp.GetLastInstructionAccessViolation());
}

// MO2: 0x100000032 (2^32 + 0x32) is outside any real address space and must
// violate, not silently truncate to the in-bounds address 0x32
TEST(InterpreterReadWriteAddressValidation,
     OversizedAddressShouldNotSilentlyWrapIntoInBoundsAddress) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);

  interp.ExecuteWrite(
      makeStmt(prosched::Keyword::kWrite, {"0x100000032", "123"}));

  auto read_result = interp.ExecuteRead(
      makeStmt(prosched::Keyword::kRead, {"result", "0x32"}));
  ASSERT_TRUE(read_result.has_value());
  EXPECT_EQ(read_result->second, 0) << "address 0x32 should be untouched";
}

// A normal in-bounds address never raises a violation
TEST(InterpreterReadWriteAddressValidation, InBoundsAddressDoesNotSetViolation) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  auto result = interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x32", "5"}));
  EXPECT_TRUE(result.has_value());
  EXPECT_FALSE(interp.GetLastInstructionAccessViolation());
}

// The one-past-the-end address (0x100 for a 0x100-byte space) is out of bounds
TEST(InterpreterReadWriteAddressValidation, BoundaryAddressAtMemEndIsViolation) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  auto result = interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x100", "5"}));
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(interp.GetLastInstructionAccessViolation());
}

// A negative address is not a valid hex address and must violate
TEST(InterpreterReadWriteAddressValidation, NegativeAddressStringIsTreatedAsViolation) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  auto result = interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"-5", "5"}));
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(interp.GetLastInstructionAccessViolation());
}

} // namespace InterpreterReadWriteAddressValidation

// ─── Symbol table cap (MO2: 64-byte segment, max 32 variables) ──────────────

namespace InterpreterSymbolTableCap {

// MO2: DECLARE already honors the 32-variable cap (excess is ignored)
TEST(InterpreterSymbolTableCap, DeclareRespectsMaxThirtyTwoVariables) {
  prosched::Interpreter interp;
  for (int i = 0; i < 40; ++i)
    interp.ExecuteDeclare(
        makeStmt(prosched::Keyword::kDeclare, {"d" + std::to_string(i), "1"}));
  EXPECT_EQ(interp.ExecuteDebug().size(), 32u);
}

// MO2: "a maximum of 32 variables. If the limit is reached, succeeding
// instructions involving variable declarations will be ignored." READ creates a
// variable, so it must honor the same cap (now routed through SetVariable).
TEST(InterpreterSymbolTableCap, ReadRespectsMaxThirtyTwoVariables) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  for (int i = 0; i < 40; ++i)
    interp.ExecuteRead(
        makeStmt(prosched::Keyword::kRead, {"r" + std::to_string(i), "0x0"}));
  EXPECT_LE(interp.ExecuteDebug().size(), 32u);
}

} // namespace InterpreterSymbolTableCap

// ─── ExecuteRead / ExecuteWrite happy path ─────────────────────────────────

namespace InterpreterReadWriteHappyPath {

// MO2: WRITE stores a uint16 to a hex address; READ retrieves it back
TEST(InterpreterReadWriteHappyPath, WriteThenReadReturnsStoredValue) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);

  auto write_result =
      interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x32", "123"}));
  ASSERT_TRUE(write_result.has_value());
  EXPECT_EQ(write_result->second, 123);

  auto read_result =
      interp.ExecuteRead(makeStmt(prosched::Keyword::kRead, {"x", "0x32"}));
  ASSERT_TRUE(read_result.has_value());
  EXPECT_EQ(read_result->first, "x");
  EXPECT_EQ(read_result->second, 123);
}

// MO2: "If 0x1000 is uninitialized, returns a 0."
TEST(InterpreterReadWriteHappyPath, ReadUnwrittenAddressReturnsZero) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  auto read_result =
      interp.ExecuteRead(makeStmt(prosched::Keyword::kRead, {"x", "0x32"}));
  ASSERT_TRUE(read_result.has_value());
  EXPECT_EQ(read_result->second, 0);
}

// MO2 worked example: DECLARE/ADD/WRITE/READ/PRINT pipeline yields "Result: 15"
// (0x500 requires a 2048-byte space, a power of 2 >= 64).
TEST(InterpreterReadWriteHappyPath, WriteReadPrintMatchesSpecExample) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x800);
  interp.ExecuteString(
      "DECLARE(varA, 10), DECLARE(varB, 5), ADD(varA, varA, varB), "
      "WRITE(0x500, varA), READ(varC, 0x500), PRINT(\"Result: \" + varC)");
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "Result: 15");
}

// MO2: "uint16 variables are clamped between (0, max(uint16))." DECLARE clamps an
// out-of-range value to 65535; WRITE now clamps the same way (70000 -> 65535).
TEST(InterpreterReadWriteHappyPath, WriteClampsUint16OverflowLikeDeclare) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x10", "70000"}));
  auto read_result =
      interp.ExecuteRead(makeStmt(prosched::Keyword::kRead, {"v", "0x10"}));
  ASSERT_TRUE(read_result.has_value());
  EXPECT_EQ(read_result->second, 65535);
}

// MO2 clamp must hold for ANY out-of-range value, but the clamp path uses an
// unguarded std::stol — a value larger than LONG_MAX throws std::out_of_range
// instead of clamping to 65535. (Same unguarded stol exists in ExecuteDeclare.)
TEST(InterpreterReadWriteHappyPath, WriteClampsValueExceedingLongDoesNotThrow) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  std::optional<std::pair<uint32_t, uint16_t>> result;
  EXPECT_NO_THROW(result = interp.ExecuteWrite(makeStmt(
                      prosched::Keyword::kWrite,
                      {"0x10", "99999999999999999999999"})));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->second, 65535);
}

} // namespace InterpreterReadWriteHappyPath

// ─── Page-fault retry (MO2) ─────────────────────────────────────────────────
// MO2: "instructions can only be performed when a valid page has been found.
// Page fault handling continuously occurs until a valid page has been returned,
// before an instruction is performed." A page fault must NOT be treated as an
// access violation (which shuts the process down) — the instruction is retried
// once the page is resident. This applies to both WRITE and READ memory access.

namespace InterpreterReadWritePageFault {

// A handler that faults the first time a page is touched, then reports it
// resident — mimicking a demand pager bringing the page in for the retry.
static std::function<bool(int)> faultOnceHandler() {
  auto faulted = std::make_shared<bool>(false);
  return [faulted](int) {
    if (!*faulted) {
      *faulted = true;
      return true; // not resident yet -> page fault, retry
    }
    return false; // resident now -> proceed
  };
}

// A faulting WRITE must page-fault (retryable), not raise an access violation;
// the retry then performs the write.
TEST(InterpreterReadWritePageFault, WriteFaultRetriesInsteadOfViolating) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  interp.SetPageSize(16);
  interp.SetPageFaultHandler(faultOnceHandler());

  auto first =
      interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x32", "123"}));
  EXPECT_FALSE(first.has_value());
  EXPECT_TRUE(interp.GetLastInstructionPageFault());
  EXPECT_FALSE(interp.GetLastInstructionAccessViolation())
      << "a page fault must not be treated as an access violation";

  auto retry =
      interp.ExecuteWrite(makeStmt(prosched::Keyword::kWrite, {"0x32", "123"}));
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(retry->second, 123);
}

// Same requirement for READ: a faulting READ retries, it does not violate.
TEST(InterpreterReadWritePageFault, ReadFaultRetriesInsteadOfViolating) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x100);
  interp.SetPageSize(16);
  interp.SetPageFaultHandler(faultOnceHandler());

  auto first =
      interp.ExecuteRead(makeStmt(prosched::Keyword::kRead, {"x", "0x32"}));
  EXPECT_FALSE(first.has_value());
  EXPECT_TRUE(interp.GetLastInstructionPageFault());
  EXPECT_FALSE(interp.GetLastInstructionAccessViolation())
      << "a page fault must not be treated as an access violation";

  auto retry =
      interp.ExecuteRead(makeStmt(prosched::Keyword::kRead, {"x", "0x32"}));
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(retry->second, 0); // unwritten address reads as 0
}

} // namespace InterpreterReadWritePageFault

// ─── Symbol-table segment paging (MO2) ──────────────────────────────────────
// MO2: "Symbol table segment: fixed 64 bytes, MAX 32 variables" (2 bytes/var ×
// 32 = 64). The symbol table is a region of the process's own memory, not a
// side table living outside it, and the spec states the consequence directly:
// "Even variable DECLAREs fault if the symbol-table segment isn't resident."
//
// So any instruction that stores a variable is a memory access and must consult
// the pager, exactly as READ/WRITE do. These tests deliberately do NOT assert
// which page the segment occupies — the spec fixes its size (64 bytes) but not
// its address — only that the access is routed through the pager at all.
//
// FAILING: ExecuteDeclare/ExecuteAdd/ExecuteSubtract call SetVariable directly
// and never reach CheckAccess, so the symbol table can never page-fault; only
// READ/WRITE touch the paged address space. Practical effect on the MO2 demo
// configs: 2 of the 8 generated keywords can fault, so paging traffic lands at
// roughly a quarter of what "for every instruction executed, the memory manager
// will attempt to page in/page out" implies.

namespace InterpreterSymbolSegmentPaging {

// Stands in for a pager with nothing resident: every page consulted is reported
// as a fault. Records the pages it was asked about, so a test can tell "the
// pager said resident" apart from "the pager was never consulted at all".
static std::function<bool(int)>
recordingAlwaysFaultHandler(std::vector<int> *consulted) {
  return [consulted](int pageNum) {
    consulted->push_back(pageNum);
    return true; // not resident -> page fault
  };
}

// Paging switched on, sized like the MO2 demo config: a 1024-byte process over
// 256-byte frames, so 4 pages.
static void enablePaging(prosched::Interpreter &interp,
                         std::vector<int> *consulted) {
  interp.SetMemoryBounds(0, 1024);
  interp.SetPageSize(256);
  interp.SetPageFaultHandler(recordingAlwaysFaultHandler(consulted));
  interp.ResetLastInstructionPageFault();
  interp.ResetLastInstructionAccessViolation();
}

// MO2, verbatim: "Even variable DECLAREs fault if the symbol-table segment
// isn't resident."
TEST(InterpreterSymbolSegmentPaging,
     DeclareFaultsWhenSymbolSegmentNotResident) {
  prosched::Interpreter interp;
  std::vector<int> consulted;
  enablePaging(interp, &consulted);

  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"varA", "10"}));

  EXPECT_FALSE(consulted.empty())
      << "DECLARE never consulted the pager: the symbol table lives outside "
         "the paged address space";
  EXPECT_TRUE(interp.GetLastInstructionPageFault())
      << "storing a variable writes the symbol-table segment, so it must fault "
         "while that segment is non-resident";
}

// ADD stores its result into a variable, i.e. into the symbol-table segment.
// The spec's "even ... DECLAREs" phrasing marks DECLARE as the least obvious
// case, so the ordinary variable writes must fault too.
TEST(InterpreterSymbolSegmentPaging, AddFaultsWhenSymbolSegmentNotResident) {
  prosched::Interpreter interp;
  std::vector<int> consulted;
  enablePaging(interp, &consulted);

  interp.ExecuteAdd(makeStmt(prosched::Keyword::kAdd, {"varA", "varB", "5"}));

  EXPECT_FALSE(consulted.empty())
      << "ADD never consulted the pager while writing a variable";
  EXPECT_TRUE(interp.GetLastInstructionPageFault())
      << "ADD writes the symbol-table segment, so it must fault while that "
         "segment is non-resident";
}

// Same requirement for SUBTRACT.
TEST(InterpreterSymbolSegmentPaging,
     SubtractFaultsWhenSymbolSegmentNotResident) {
  prosched::Interpreter interp;
  std::vector<int> consulted;
  enablePaging(interp, &consulted);

  interp.ExecuteSubtract(
      makeStmt(prosched::Keyword::kSubtract, {"varA", "varB", "5"}));

  EXPECT_FALSE(consulted.empty())
      << "SUBTRACT never consulted the pager while writing a variable";
  EXPECT_TRUE(interp.GetLastInstructionPageFault())
      << "SUBTRACT writes the symbol-table segment, so it must fault while "
         "that segment is non-resident";
}

// A faulting DECLARE must be retryable, not a shutdown: the symbol segment is
// inside the process's own space, so it can never be an access violation.
TEST(InterpreterSymbolSegmentPaging, DeclareFaultIsNotAnAccessViolation) {
  prosched::Interpreter interp;
  std::vector<int> consulted;
  enablePaging(interp, &consulted);

  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"varA", "10"}));

  EXPECT_FALSE(interp.GetLastInstructionAccessViolation())
      << "a symbol-segment page fault must not shut the process down";
}

// Reading a variable is a symbol-segment access too, not just writing one.
// b0cf116 routed the paging check through SetVariable, so DECLARE/ADD/SUBTRACT
// fault correctly on the WRITE. But ResolveOperand (Interpreter.cpp:509-511)
// returns straight out of `memory_` when the name is found, without consulting
// the pager at all — so a pure read of the symbol table never faults.
//
// PRINT of a bare variable isolates that path: it resolves an operand and
// writes nothing, so the only symbol-table access it makes is a read.
//
// FAILING: the pager is never consulted, meaning a PRINT can read variables
// out of a segment that is currently evicted to the backing store.
TEST(InterpreterSymbolSegmentPaging, ReadingAVariableFaultsWhenSegmentNotResident) {
  prosched::Interpreter interp;

  // Declare varA while paging is off, so it genuinely exists in the table.
  interp.ExecuteDeclare(makeStmt(prosched::Keyword::kDeclare, {"varA", "7"}));

  // Now switch the pager on with nothing resident.
  std::vector<int> consulted;
  enablePaging(interp, &consulted);

  interp.ExecutePrint(makeStmt(prosched::Keyword::kPrint, {"varA"}));

  EXPECT_FALSE(consulted.empty())
      << "PRINT read varA without consulting the pager: ResolveOperand hits "
         "memory_ directly, so the symbol-table segment is read even when it "
         "is not resident";
  EXPECT_TRUE(interp.GetLastInstructionPageFault())
      << "reading a variable touches the symbol-table segment, so it must "
         "fault while that segment is non-resident";
}

} // namespace InterpreterSymbolSegmentPaging

// ─── ParseUserProgram (screen -c, MO2) ──────────────────────────────────────
// MO2: screen -c instructions are semicolon-separated with space-separated args
// (e.g. "DECLARE varA 10; ADD varA varA varB").

namespace InterpreterParseUserProgram {

// A valid semicolon-separated program parses to one Statement per instruction
TEST(InterpreterParseUserProgram, ParsesValidSemicolonProgram) {
  prosched::Interpreter interp;
  std::vector<prosched::Statement> out;
  EXPECT_TRUE(interp.ParseUserProgram(
      "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB", out));
  EXPECT_EQ(out.size(), 3u);
}

// MO2: at least 1 instruction — an empty program is rejected
TEST(InterpreterParseUserProgram, EmptyProgramReturnsFalse) {
  prosched::Interpreter interp;
  std::vector<prosched::Statement> out;
  EXPECT_FALSE(interp.ParseUserProgram("", out));
}

// Any unrecognized instruction invalidates the whole program
TEST(InterpreterParseUserProgram, InvalidInstructionReturnsFalse) {
  prosched::Interpreter interp;
  std::vector<prosched::Statement> out;
  EXPECT_FALSE(
      interp.ParseUserProgram("DECLARE varA 10; NOTACOMMAND foo", out));
}

// MO2 worked example, executed through the screen -c parser (space-separated
// args, semicolons, hex addresses) — yields "Result: 15" end-to-end.
TEST(InterpreterParseUserProgram, Mo2ExampleExecutesToResultFifteen) {
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 0x800); // 2048 bytes covers 0x500
  std::vector<prosched::Statement> program;
  ASSERT_TRUE(interp.ParseUserProgram(
      "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; "
      "WRITE 0x500 varA; READ varC 0x500; PRINT(\"Result: \" + varC)",
      program));
  ASSERT_EQ(program.size(), 6u);
  interp.ExecuteStatements(program);
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "Result: 15");
}

// ParseUserProgram itself does NOT cap at 50 — a 51-instruction program parses
// fine; the <=50 limit is enforced by the caller (Controller::ExecuteCommand).
TEST(InterpreterParseUserProgram, AboveFiftyParsesCallerEnforcesUpperLimit) {
  prosched::Interpreter interp;
  std::string program;
  for (int i = 0; i < 51; ++i) {
    if (i)
      program += "; ";
    program += "DECLARE varA 10";
  }
  std::vector<prosched::Statement> out;
  EXPECT_TRUE(interp.ParseUserProgram(program, out));
  EXPECT_EQ(out.size(), 51u);
}

} // namespace InterpreterParseUserProgram

// ─── parse ─────────────────────────────────────────────────────────────────

namespace InterpreterParse {

// Empty string produces no AST nodes
TEST(InterpreterParse, EmptyStringReturnsEmptyVector) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse("");
  EXPECT_TRUE(stmts.empty());
}

// Single valid statement produces one node with the correct keyword
TEST(InterpreterParse, SingleStatementReturnsOneNodeWithCorrectKeyword) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(R"(PRINT("hi"))");
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(stmts[0].keyword, prosched::Keyword::kPrint);
}

// Multiple comma-separated statements produce the correct node count
TEST(InterpreterParse, MultipleStatementsReturnCorrectCount) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(R"(PRINT("a"), DECLARE(x, 1), ADD(y, x, x))");
  EXPECT_EQ(stmts.size(), 3u);
  EXPECT_EQ(stmts[0].keyword, prosched::Keyword::kPrint);
  EXPECT_EQ(stmts[1].keyword, prosched::Keyword::kDeclare);
  EXPECT_EQ(stmts[2].keyword, prosched::Keyword::kAdd);
}

// FOR node stores its body in the nested field, not flattened into args
TEST(InterpreterParse, ForStatementPopulatesNestedField) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(R"(FOR([PRINT("x")], 2))");
  ASSERT_EQ(stmts.size(), 1u);
  ASSERT_EQ(stmts[0].keyword, prosched::Keyword::kFor);
  EXPECT_FALSE(stmts[0].nested.empty());
}

// Unknown keyword is parsed without crashing and tagged UNKNOWN
TEST(InterpreterParse, UnknownKeywordTaggedAsUnknown) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse("BADCMD(x)");
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(stmts[0].keyword, prosched::Keyword::kUnknown);
}

// The top-level splitter respects bracket depth: commas inside a FOR body are
// NOT treated as statement separators. A FOR (whose body has its own commas)
// followed by a PRINT must yield exactly 2 top-level statements.
TEST(InterpreterParse, CommasInsideForBodyDoNotSplitTopLevel) {
  prosched::Interpreter interp;
  auto stmts =
      interp.Parse(R"(FOR([PRINT("a"), PRINT("b")], 2), PRINT("after"))");
  ASSERT_EQ(stmts.size(), 2u);
  EXPECT_EQ(stmts[0].keyword, prosched::Keyword::kFor);
  EXPECT_EQ(stmts[1].keyword, prosched::Keyword::kPrint);
  // The FOR's body keeps both of its inner statements
  EXPECT_EQ(stmts[0].nested.size(), 2u);
}

// Deeply nested FOR([FOR([...])]) parses into a tree of the right shape —
// the inner FOR is preserved as a nested child, not flattened or mis-split.
TEST(InterpreterParse, NestedForParsesIntoNestedTree) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(R"(FOR([FOR([PRINT("x")], 2)], 3))");
  ASSERT_EQ(stmts.size(), 1u);
  ASSERT_EQ(stmts[0].keyword, prosched::Keyword::kFor);
  ASSERT_EQ(stmts[0].nested.size(), 1u);
  ASSERT_EQ(stmts[0].nested[0].keyword, prosched::Keyword::kFor);
  ASSERT_EQ(stmts[0].nested[0].nested.size(), 1u);
  EXPECT_EQ(stmts[0].nested[0].nested[0].keyword, prosched::Keyword::kPrint);
}

} // namespace InterpreterParse

// ─── ExecuteStatements ─────────────────────────────────────────────────────

namespace InterpreterExecuteStatements {

// ExecuteStatements on a pre-parsed AST produces the same output as
// ExecuteString
TEST(InterpreterExecuteStatements, MatchesExecuteStringOutput) {
  prosched::Interpreter interp_a;
  interp_a.ExecuteString(R"(DECLARE(x, 5), PRINT(x))");
  auto expected = interp_a.FlushBuffer();

  prosched::Interpreter interp_b;
  auto stmts = interp_b.Parse(R"(DECLARE(x, 5), PRINT(x))");
  interp_b.ExecuteStatements(stmts);
  auto actual = interp_b.FlushBuffer();

  EXPECT_EQ(actual, expected);
}

// ExecuteStatements works when called with a manually constructed statement
// list
TEST(InterpreterExecuteStatements, WorksWithPreBuiltStatementList) {
  prosched::Interpreter interp;
  std::vector<prosched::Statement> stmts = {
      makeStmt(prosched::Keyword::kDeclare, {"n", "7"}),
      makeStmt(prosched::Keyword::kPrint, {"n"})};
  interp.ExecuteStatements(stmts);
  auto buf = interp.FlushBuffer();
  ASSERT_EQ(buf.size(), 1u);
  EXPECT_EQ(buf[0], "7");
}

// An UNKNOWN statement pushes a "[!]" warning to the buffer and does not throw
TEST(InterpreterExecuteStatements, UnknownStatementPushesWarning) {
  prosched::Interpreter interp;
  std::vector<prosched::Statement> stmts = {
      makeStmt(prosched::Keyword::kUnknown)};
  EXPECT_NO_THROW(interp.ExecuteStatements(stmts));
  auto buf = interp.FlushBuffer();
  ASSERT_FALSE(buf.empty());
  EXPECT_NE(buf[0].find("[!]"), std::string::npos);
}

} // namespace InterpreterExecuteStatements

// ─── executeFor (additional) ────────────────────────────────────────────────

namespace InterpreterExecuteFor {

// Outer(3) × middle(2) × inner(4) = 24 total PRINT calls — tests max nesting
// depth
TEST(InterpreterExecuteFor, TripleNestedForMultipliesIterationCount) {
  prosched::Interpreter interp;
  interp.ExecuteString(R"(FOR([FOR([FOR([PRINT("x")], 4)], 2)], 3))");
  auto buf = interp.FlushBuffer();
  EXPECT_EQ(buf.size(), 24u);
}

} // namespace InterpreterExecuteFor

// ─── ExecuteSleep (additional) ─────────────────────────────────────────────

namespace InterpreterExecuteSleep {

// SLEEP(0) resolves to 0 ms and finishes in negligible time
TEST(InterpreterExecuteSleep, ZeroTicksTakesNegligibleTime) {
  prosched::Interpreter interp;
  auto start = std::chrono::steady_clock::now();
  interp.ExecuteSleep(makeStmt(prosched::Keyword::kSleep, {"0"}));
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  EXPECT_LT(elapsed_ms, 5);
}

// SLEEP(1) resolves to 10 ms (1 tick × 10 ms per tick); measurably longer than
// SLEEP(0)
TEST(InterpreterExecuteSleep, OneTickTakesAtLeastOneTick) {
  prosched::Interpreter interp;
  auto start = std::chrono::steady_clock::now();
  interp.ExecuteSleep(makeStmt(prosched::Keyword::kSleep, {"1"}));
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  EXPECT_GE(elapsed_ms,
            8); // 10 ms nominal; 8 ms lower bound absorbs OS scheduling jitter
}

} // namespace InterpreterExecuteSleep

// ─── GetRandomStatement ────────────────────────────────────────────────────

namespace InterpreterGetRandomStatement {

// Over many calls the result is never UNKNOWN or DBG — both are not spec
// language keywords
TEST(InterpreterGetRandomStatement, AlwaysReturnsValidKeyword) {
  for (int i = 0; i < 100; i++) {
    auto stmt = prosched::GetRandomStatement("proc", 0);
    EXPECT_NE(stmt.keyword, prosched::Keyword::kUnknown)
        << "Got UNKNOWN on iteration " << i;
    EXPECT_NE(stmt.keyword, prosched::Keyword::kDebug)
        << "Got DBG on iteration " << i;
  }
}

// When maxDepth >= 3, FOR must never be selected
TEST(InterpreterGetRandomStatement, NeverReturnsForAtMaxDepth) {
  for (int i = 0; i < 100; i++) {
    auto stmt = prosched::GetRandomStatement("proc", 3);
    EXPECT_NE(stmt.keyword, prosched::Keyword::kFor)
        << "Got FOR at maxDepth=3 on iteration " << i;
  }
}

// Each keyword type's statement carries arguments in the correct shape for its
// keyword
TEST(InterpreterGetRandomStatement, ArgShapeMatchesKeyword) {
  for (int i = 0; i < 200; i++) {
    auto stmt = prosched::GetRandomStatement("proc", 0);
    switch (stmt.keyword) {
    case prosched::Keyword::kPrint:
      ASSERT_EQ(stmt.args.size(), 1u) << "PRINT must have 1 arg";
      EXPECT_EQ(stmt.args[0].front(), '"')
          << "PRINT arg must be a quoted string";
      EXPECT_EQ(stmt.args[0].back(), '"')
          << "PRINT arg must be a quoted string";
      break;
    case prosched::Keyword::kDeclare:
      EXPECT_EQ(stmt.args.size(), 2u) << "DECLARE must have 2 args";
      break;
    case prosched::Keyword::kAdd:
    case prosched::Keyword::kSubtract:
      EXPECT_EQ(stmt.args.size(), 3u) << "ADD/SUBTRACT must have 3 args";
      break;
    case prosched::Keyword::kSleep:
      ASSERT_EQ(stmt.args.size(), 1u) << "SLEEP must have 1 arg";
      EXPECT_NO_THROW(std::stoul(stmt.args[0])) << "SLEEP arg must be numeric";
      break;
    case prosched::Keyword::kFor:
      ASSERT_GE(stmt.args.size(), 2u) << "FOR must have at least 2 args";
      EXPECT_FALSE(stmt.nested.empty()) << "FOR must have nested statements";
      break;
    default:
      break;
    }
  }
}

} // namespace InterpreterGetRandomStatement
