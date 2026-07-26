#include "commands/Interpreter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace prosched {
namespace {

/** @brief Character counts of each instruction's opening prefix.

    Used to locate the first argument. For example "DECLARE(" is 8 characters,
    so the first argument of a DECLARE statement begins at index 8.
*/
constexpr std::size_t kPrintPrefixLen = 6;    /*!< "PRINT(" */
constexpr std::size_t kDeclarePrefixLen = 8;  /*!< "DECLARE(" */
constexpr std::size_t kAddPrefixLen = 4;      /*!< "ADD(" */
constexpr std::size_t kSubtractPrefixLen = 9; /*!< "SUBTRACT(" */
constexpr std::size_t kSleepPrefixLen = 6;    /*!< "SLEEP(" */
constexpr std::size_t kForPrefixLen = 5;      /*!< "FOR([" */
constexpr std::size_t kReadPrefixLen = 5;     /*!< "READ(" */
constexpr std::size_t kWritePrefixLen = 6;    /*!< "WRITE(" */

/** @brief Index of the opening '[' in "FOR([", where FOR body scanning begins.
 */
constexpr std::size_t kForBodyScanStart = 4;

/** @brief One SLEEP tick, in milliseconds. */
constexpr int kTickMs = 10;

/** @brief Number of arguments each keyword requires.

    A user-supplied instruction that does not carry exactly this many
    arguments is rejected instead of silently executing as a no-op.

    @param kw The keyword to describe
    @return The expected argument count
*/
std::size_t ExpectedArgCount(Keyword kw) {
  switch (kw) {
    case Keyword::kPrint:
    case Keyword::kSleep:
      return 1;
    case Keyword::kDeclare:
    case Keyword::kRead:
    case Keyword::kWrite:
    case Keyword::kFor:
      return 2;
    case Keyword::kAdd:
    case Keyword::kSubtract:
      return 3;
    case Keyword::kDebug:
    case Keyword::kUnknown:
      return 0;
  }
  return 0;
}

/** @brief Checks a canonical instruction carries exactly the arguments its
           keyword takes.

    ExtractArgs stops at the expected number of separators, so a trailing
    "ADD(a, b, c, d)" would otherwise parse as three arguments with the excess
    silently folded into the last one. Counting the separators the caller
    actually wrote catches that. Separators nested inside quotes, parentheses
    or brackets belong to an argument and are not counted.

    @param canonical A single instruction in canonical parenthesised form
    @param kw Its keyword
    @return true when the argument count matches exactly
*/
bool ArgListMatchesArity(const std::string& canonical, Keyword kw) {
  const std::size_t expected = ExpectedArgCount(kw);
  if (expected == 0) {
    return true;  // "DBG!" takes no argument list at all.
  }

  const std::size_t open = canonical.find('(');
  const std::size_t close = canonical.rfind(')');
  if (open == std::string::npos || close == std::string::npos ||
      close < open) {
    return false;
  }

  std::size_t separators = 0;
  int depth = 0;
  bool in_quotes = false;
  for (std::size_t i = open + 1; i < close; ++i) {
    const char c = canonical[i];
    if (c == '"') {
      in_quotes = !in_quotes;
    } else if (!in_quotes && (c == '(' || c == '[')) {
      ++depth;
    } else if (!in_quotes && (c == ')' || c == ']')) {
      --depth;
    } else if (c == ',' && depth == 0 && !in_quotes) {
      ++separators;
    }
  }
  return separators + 1 == expected;
}

}  // namespace

void Interpreter::SetMemoryBounds(uint32_t start, uint32_t end) {
  mem_start_ = start;
  mem_end_ = end;
  bounds_set_ = true;
}

void Interpreter::SetPageSize(uint32_t page_size) {
  page_size_bytes_ = page_size;
}

void Interpreter::SetPageFaultHandler(std::function<bool(int)> handler) {
  page_fault_handler_ = std::move(handler);
}

bool Interpreter::GetLastInstructionPageFault() const {
  return last_instruction_page_fault_;
}

bool Interpreter::GetLastInstructionAccessViolation() const {
  return last_instruction_access_violation_;
}

uint32_t Interpreter::GetLastViolationAddress() const {
  return last_violation_address_;
}

void Interpreter::ResetLastInstructionPageFault() {
  last_instruction_page_fault_ = false;
}

void Interpreter::ResetLastInstructionAccessViolation() {
  last_instruction_access_violation_ = false;
  last_violation_address_ = 0;
}

std::vector<Statement> Interpreter::Parse(const std::string& program) {
  return ParseBlock(program);
}

std::vector<std::string> Interpreter::SplitUserInstructions(
    const std::string& program) {
  std::vector<std::string> instructions;
  std::string current;
  int depth = 0;
  bool in_quotes = false;

  for (char c : program) {
    if (c == '"') {
      in_quotes = !in_quotes;
    } else if (!in_quotes && (c == '(' || c == '[')) {
      ++depth;
    } else if (!in_quotes && (c == ')' || c == ']')) {
      --depth;
    } else if (c == ';' && depth == 0 && !in_quotes) {
      if (!Trim(current).empty()) {
        instructions.push_back(Trim(current));
      }
      current.clear();
      continue;
    }
    current += c;
  }

  if (!Trim(current).empty()) {
    instructions.push_back(Trim(current));
  }
  return instructions;
}

std::string Interpreter::CanonicalizeUserInstruction(
    const std::string& instruction) {
  const std::string trimmed = Trim(instruction);
  if (trimmed.empty()) {
    return "";
  }

  const std::size_t first_space = trimmed.find_first_of(" \t");
  const std::size_t first_paren = trimmed.find('(');

  // Already parenthesised ("PRINT(...)", "FOR([...], 3)") or a bare token
  // such as "DBG!" — the canonical parser handles both as they stand.
  if (first_space == std::string::npos || first_paren < first_space) {
    return trimmed;
  }

  const std::string keyword = trimmed.substr(0, first_space);
  std::string canonical = keyword + "(";

  std::size_t pos = first_space;
  bool first_arg = true;
  while (pos < trimmed.size()) {
    const std::size_t start = trimmed.find_first_not_of(" \t", pos);
    if (start == std::string::npos) {
      break;
    }
    const std::size_t end = trimmed.find_first_of(" \t", start);
    const std::string arg = trimmed.substr(
        start, end == std::string::npos ? std::string::npos : end - start);

    if (!first_arg) {
      canonical += ", ";
    }
    canonical += arg;
    first_arg = false;

    pos = (end == std::string::npos) ? trimmed.size() : end;
  }

  return canonical + ")";
}

bool Interpreter::ParseUserProgram(const std::string& program,
                                   std::vector<Statement>& out) {
  const std::vector<std::string> instructions = SplitUserInstructions(program);
  if (instructions.empty()) {
    return false;
  }

  std::vector<Statement> parsed;
  parsed.reserve(instructions.size());

  for (const std::string& instruction : instructions) {
    const std::string canonical = CanonicalizeUserInstruction(instruction);
    const Statement stmt = ParseStatement(canonical);
    if (stmt.keyword == Keyword::kUnknown ||
        stmt.args.size() != ExpectedArgCount(stmt.keyword) ||
        !ArgListMatchesArity(canonical, stmt.keyword)) {
      return false;
    }
    parsed.push_back(stmt);
  }

  out = std::move(parsed);
  return true;
}

void Interpreter::ExecuteStatements(const std::vector<Statement>& stmts) {
  ResetAccessState();
  for (const Statement& stmt : stmts) {
    ExecuteStatement(stmt);
  }
}

void Interpreter::ExecuteString(std::string program) {
  program = Trim(program);
  if (program.empty()) {
    return;
  }

  try {
    ExecuteStatements(Parse(program));
  } catch (const std::exception&) {
    screen_buffer_.push_back(
        "[!] Interpreter Error: Malformed syntax near -> " + program);
  }
}

std::vector<std::string> Interpreter::FlushBuffer() {
  std::vector<std::string> copy = screen_buffer_;
  screen_buffer_.clear();
  return copy;
}

std::vector<std::pair<uint32_t, uint16_t>> Interpreter::GetPageSnapshot(
    uint32_t start_address, uint32_t page_size) const {
  std::vector<std::pair<uint32_t, uint16_t>> snapshot;
  for (const auto& entry : address_space_) {
    if (entry.first >= start_address &&
        entry.first < start_address + page_size) {
      snapshot.emplace_back(entry.first, entry.second);
    }
  }
  return snapshot;
}

void Interpreter::ClearPageRange(uint32_t start_address, uint32_t page_size) {
  std::vector<uint32_t> to_erase;
  for (const auto& entry : address_space_) {
    if (entry.first >= start_address &&
        entry.first < start_address + page_size) {
      to_erase.push_back(entry.first);
    }
  }
  for (uint32_t address : to_erase) {
    address_space_.erase(address);
  }
}

void Interpreter::RestorePageSnapshot(
    const std::vector<std::pair<uint32_t, uint16_t>>& page_entries) {
  for (const auto& entry : page_entries) {
    address_space_[entry.first] = entry.second;
  }
}

std::string Interpreter::Trim(const std::string& s) {
  const std::size_t start = s.find_first_not_of(" \t\r\n");
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::vector<std::string> Interpreter::SplitInstructions(
    const std::string& list) {
  std::vector<std::string> instrs;
  int depth = 0;
  std::string current;

  for (char c : list) {
    if (c == '[' || c == '(') {
      ++depth;
    } else if (c == ']' || c == ')') {
      --depth;
    } else if (c == ',' && depth == 0) {
      if (!Trim(current).empty()) {
        instrs.push_back(Trim(current));
      }
      current.clear();
      continue;
    }
    current += c;
  }

  if (!Trim(current).empty()) {
    instrs.push_back(Trim(current));
  }
  return instrs;
}

Keyword Interpreter::IdentifyKeyword(const std::string& stmt) {
  if (stmt.rfind("PRINT(", 0) == 0) return Keyword::kPrint;
  if (stmt.rfind("DECLARE(", 0) == 0) return Keyword::kDeclare;
  if (stmt.rfind("ADD(", 0) == 0) return Keyword::kAdd;
  if (stmt.rfind("SUBTRACT(", 0) == 0) return Keyword::kSubtract;
  if (stmt.rfind("SLEEP(", 0) == 0) return Keyword::kSleep;
  if (stmt.rfind("FOR([", 0) == 0) return Keyword::kFor;
  if (stmt.rfind("READ(", 0) == 0) return Keyword::kRead;
  if (stmt.rfind("WRITE(", 0) == 0) return Keyword::kWrite;
  if (stmt == "DBG!") return Keyword::kDebug;
  return Keyword::kUnknown;
}

std::vector<std::string> Interpreter::ExtractArgs(const std::string& stmt,
                                                  Keyword kw) {
  std::vector<std::string> args;

  switch (kw) {
    // Single argument spanning the whole parenthesised body.
    case Keyword::kPrint:
    case Keyword::kSleep: {
      const std::size_t offset =
          (kw == Keyword::kPrint) ? kPrintPrefixLen : kSleepPrefixLen;
      // kPrint bodies may themselves contain parentheses, so scan from the
      // right; kSleep bodies are numeric and cannot.
      const std::size_t end =
          (kw == Keyword::kPrint) ? stmt.find_last_of(')') : stmt.find(')');
      if (end != std::string::npos && end >= offset) {
        args.push_back(Trim(stmt.substr(offset, end - offset)));
      }
      break;
    }

    // Two comma-separated arguments.
    case Keyword::kDeclare:
    case Keyword::kRead:
    case Keyword::kWrite: {
      std::size_t offset = kDeclarePrefixLen;
      if (kw == Keyword::kRead) offset = kReadPrefixLen;
      if (kw == Keyword::kWrite) offset = kWritePrefixLen;

      const std::size_t comma = stmt.find(',');
      const std::size_t end = stmt.find(')');
      if (comma != std::string::npos && end != std::string::npos &&
          comma > offset) {
        args.push_back(Trim(stmt.substr(offset, comma - offset)));
        args.push_back(Trim(stmt.substr(comma + 1, end - comma - 1)));
      }
      break;
    }

    // Three comma-separated arguments.
    case Keyword::kAdd:
    case Keyword::kSubtract: {
      const std::size_t offset =
          (kw == Keyword::kAdd) ? kAddPrefixLen : kSubtractPrefixLen;
      const std::size_t comma1 = stmt.find(',');
      if (comma1 == std::string::npos) break;
      const std::size_t comma2 = stmt.find(',', comma1 + 1);
      const std::size_t end = stmt.find(')');
      if (comma2 != std::string::npos && end != std::string::npos) {
        args.push_back(Trim(stmt.substr(offset, comma1 - offset)));
        args.push_back(Trim(stmt.substr(comma1 + 1, comma2 - comma1 - 1)));
        args.push_back(Trim(stmt.substr(comma2 + 1, end - comma2 - 1)));
      }
      break;
    }

    // A bracketed body followed by a repeat count. The body is returned raw;
    // ParseStatement recurses into it.
    case Keyword::kFor: {
      int depth = 0;
      std::size_t closing_bracket = std::string::npos;
      for (std::size_t i = kForBodyScanStart; i < stmt.size(); ++i) {
        if (stmt[i] == '[') {
          ++depth;
        } else if (stmt[i] == ']') {
          --depth;
          if (depth == 0) {
            closing_bracket = i;
            break;
          }
        }
      }
      if (closing_bracket == std::string::npos) break;

      const std::size_t comma = stmt.find(',', closing_bracket);
      const std::size_t end = stmt.rfind(')');
      if (comma != std::string::npos && end != std::string::npos) {
        args.push_back(
            stmt.substr(kForPrefixLen, closing_bracket - kForPrefixLen));
        args.push_back(Trim(stmt.substr(comma + 1, end - comma - 1)));
      }
      break;
    }

    case Keyword::kDebug:
    case Keyword::kUnknown:
      break;
  }

  return args;
}

std::vector<Statement> Interpreter::ParseBlock(const std::string& block_str) {
  std::vector<Statement> stmts;
  for (const std::string& instr : SplitInstructions(block_str)) {
    stmts.push_back(ParseStatement(instr));
  }
  return stmts;
}

Statement Interpreter::ParseStatement(const std::string& stmt_str) {
  const std::string trimmed = Trim(stmt_str);

  Statement stmt;
  stmt.keyword = IdentifyKeyword(trimmed);
  if (stmt.keyword == Keyword::kUnknown) {
    return stmt;
  }

  stmt.args = ExtractArgs(trimmed, stmt.keyword);
  if (stmt.keyword == Keyword::kFor && !stmt.args.empty()) {
    stmt.nested = ParseBlock(stmt.args[0]);
  }
  return stmt;
}

uint32_t Interpreter::ParseAddress(const std::string& address_string) {
  const std::string t = Trim(address_string);
  try {
    uint64_t raw;
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
      raw= std::stoul(t, nullptr, 16);
    } else {
      raw= std::stoul(t);
    }

    if (raw > UINT32_MAX) {
      return UINT32_MAX;
    }

    return static_cast<uint32_t>(raw);

  } catch(const std::exception&) {
    return UINT32_MAX;
  }
  
}

uint16_t Interpreter::ResolveOperand(const std::string& op) {
  const std::string t = Trim(op);
  if (t.empty()) {
    return 0;
  }
  if (std::isdigit(static_cast<unsigned char>(t[0]))) {
    try {
      const unsigned long raw = std::stoul(t);
      return static_cast<uint16_t>(std::min(raw, static_cast<unsigned long>(UINT16_MAX)));
    } catch (const std::out_of_range&) {
      return UINT16_MAX;
    } catch (const std::invalid_argument&) {
      return 0;
    }
  }

  const auto it = memory_.find(t);
  if (it != memory_.end()) {
    return it->second;
  }
  SetVariable(t, 0);
  return 0;
}

bool Interpreter::SetVariable(const std::string& name, uint16_t value) {
  const auto it = memory_.find(name);
  if (it != memory_.end()) {
    it->second = value;
    return true;
  }
  if (memory_.size() >= kMaxSymbols) {
    screen_buffer_.push_back("[!] Interpreter Warning: Symbol table full (" +
                             std::to_string(kMaxSymbols) +
                             " variables), ignoring " + name);
    return false;
  }
  memory_[name] = value;
  return true;
}

bool Interpreter::IsValidAddress(uint32_t address) const {
  if (!bounds_set_) {
    return true;
  }
  return address >= mem_start_ && address < mem_end_;
}

void Interpreter::ResetAccessState() {
  last_instruction_page_fault_ = false;
  last_instruction_access_violation_ = false;
  last_violation_address_ = 0;
}

Interpreter::AccessStatus Interpreter::CheckAccess(uint32_t address) {
  // Bounds first: an address the process does not own must never reach the
  // pager, or it would cause a frame to be allocated for foreign memory.
  if (!IsValidAddress(address)) {
    last_instruction_access_violation_ = true;
    last_violation_address_ = address;
    return AccessStatus::kViolation;
  }

  // With no handler or no page size configured, paging is not in play.
  if (page_fault_handler_ == nullptr || page_size_bytes_ == 0) {
    return AccessStatus::kOk;
  }

  const int page_number = static_cast<int>(address / page_size_bytes_);
  if (page_fault_handler_(page_number)) {
    last_instruction_page_fault_ = true;
    return AccessStatus::kFault;
  }
  return AccessStatus::kOk;
}

void Interpreter::ExecuteStatement(const Statement& stmt) {
  try {
    switch (stmt.keyword) {
      case Keyword::kPrint:
        if (!stmt.args.empty()) {
          ExecutePrint(stmt);
        }
        break;
      case Keyword::kDeclare:
        ExecuteDeclare(stmt);
        break;
      case Keyword::kAdd:
        ExecuteAdd(stmt);
        break;
      case Keyword::kSubtract:
        ExecuteSubtract(stmt);
        break;
      case Keyword::kSleep:
        ExecuteSleep(stmt);
        break;
      case Keyword::kFor:
        ExecuteFor(stmt);
        break;
      case Keyword::kRead:
        ExecuteRead(stmt);
        break;
      case Keyword::kWrite:
        ExecuteWrite(stmt);
        break;
      case Keyword::kDebug:
        ExecuteDebug();
        break;
      case Keyword::kUnknown:
        screen_buffer_.push_back(
            "[!] Interpreter Warning: Unrecognized instruction");
        break;
    }
  } catch (const std::exception&) {
    screen_buffer_.push_back(
        "[!] Interpreter Error: Failed to execute statement");
  }
}

std::string Interpreter::ExecutePrint(const Statement& stmt) {
  const std::string arg = stmt.args[0];
  std::string output;

  const std::size_t plus_idx = arg.find('+');
  if (plus_idx != std::string::npos) {
    const std::string str_part = Trim(arg.substr(0, plus_idx));
    const std::string var_part = Trim(arg.substr(plus_idx + 1));
    if (str_part.front() == '"' && str_part.back() == '"') {
      output += str_part.substr(1, str_part.size() - 2);
    }
    output += std::to_string(ResolveOperand(var_part));
  } else if (arg.front() == '"' && arg.back() == '"') {
    output = arg.substr(1, arg.size() - 2);
  } else {
    output = std::to_string(ResolveOperand(arg));
  }

  screen_buffer_.push_back(output);
  return output;
}

std::optional<uint16_t> Interpreter::ExecuteDeclare(const Statement& stmt) {
  if (stmt.args.size() < 2) {
    return std::nullopt;
  }

  const long value = std::stol(stmt.args[1]);
  const uint16_t clamped = static_cast<uint16_t>(
      std::clamp(value, 0L, static_cast<long>(UINT16_MAX)));
  if (!SetVariable(stmt.args[0], clamped)) {
    return std::nullopt;
  }
  return clamped;
}

std::optional<uint16_t> Interpreter::ExecuteAdd(const Statement& stmt) {
  if (stmt.args.size() < 3) {
    return std::nullopt;
  }

  const uint16_t result = static_cast<uint16_t>(ResolveOperand(stmt.args[1]) +
                                                ResolveOperand(stmt.args[2]));
  if (!SetVariable(stmt.args[0], result)) {
    return std::nullopt;
  }
  return result;
}

std::optional<uint16_t> Interpreter::ExecuteSubtract(const Statement& stmt) {
  if (stmt.args.size() < 3) {
    return std::nullopt;
  }

  const uint16_t result = static_cast<uint16_t>(ResolveOperand(stmt.args[1]) -
                                                ResolveOperand(stmt.args[2]));
  if (!SetVariable(stmt.args[0], result)) {
    return std::nullopt;
  }
  return result;
}

void Interpreter::ExecuteSleep(const Statement& stmt) {
  if (stmt.args.empty()) {
    return;
  }
  
  uint8_t ticks = 0;

  try {
    const unsigned long raw = std::stoul(stmt.args[0]);
    ticks = static_cast<uint8_t>(std::min(raw, static_cast<unsigned long>(UINT8_MAX)));
  } catch (const std::out_of_range&) {
      ticks = UINT8_MAX;
  } catch (const std::invalid_argument&) {
      ticks = 0;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(ticks * kTickMs));
}

void Interpreter::ExecuteFor(const Statement& stmt) {
  if (stmt.args.size() < 2) {
    return;
  }

  const uint16_t repeats = ResolveOperand(stmt.args[1]);
  for (uint16_t i = 0; i < repeats; ++i) {
    for (const Statement& nested : stmt.nested) {
      ExecuteStatement(nested);
    }
  }
}

std::optional<std::pair<std::string, uint16_t>> Interpreter::ExecuteRead(
    const Statement& stmt) {
  ResetAccessState();
  if (stmt.args.size() < 2) {
    return std::nullopt;
  }

  const std::string variable_name = Trim(stmt.args[0]);
  const uint32_t address = ParseAddress(stmt.args[1]);
  AccessStatus status = CheckAccess(address);

  if (status == AccessStatus::kViolation) {
    last_instruction_access_violation_ = true;
    last_violation_address_ = address;
    return std::nullopt;
  }

  if (status == AccessStatus::kFault) {
    last_instruction_page_fault_ = true;
    return std::nullopt;
  }

  uint16_t value = 0;
  const auto it = address_space_.find(address);
  if (it != address_space_.end()) {
    value = it->second;
  }

  if (!SetVariable(variable_name, value)) {
    return std::nullopt;
  }
  return std::make_pair(variable_name, value);
}

std::optional<std::pair<uint32_t, uint16_t>> Interpreter::ExecuteWrite(
    const Statement& stmt) {
  ResetAccessState();
  if (stmt.args.size() < 2) {
    return std::nullopt;
  }

  const uint32_t address = ParseAddress(stmt.args[0]);
  AccessStatus status = CheckAccess(address);
  if (status == AccessStatus::kViolation) {
    last_instruction_access_violation_ = true;
    last_violation_address_ = address;
    return std::nullopt;
  }

  if (status == AccessStatus::kFault) {
    last_instruction_page_fault_ = true;
    return std::nullopt;
  }

  const uint16_t value = ResolveOperand(stmt.args[1]);
  address_space_[address] = value;
  return std::make_pair(address, value);
}

std::unordered_map<std::string, uint16_t> Interpreter::ExecuteDebug() {
  screen_buffer_.push_back("====================================");
  if (memory_.empty()) {
    screen_buffer_.push_back("  [No variables declared]");
  } else {
    for (const auto& pair : memory_) {
      screen_buffer_.push_back("|" + pair.first + "|\t" +
                               std::to_string(pair.second));
    }
  }
  screen_buffer_.push_back("====================================");
  return memory_;
}

}  // namespace prosched
