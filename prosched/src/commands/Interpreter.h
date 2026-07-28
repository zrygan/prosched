#ifndef PROSCHED_COMMANDS_INTERPRETER_H_
#define PROSCHED_COMMANDS_INTERPRETER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "commands/Statement.h"

namespace prosched {

/** @class Interpreter
    The interpreter operates in two distinct phases:
    - **Parsing**: converts the input string into an abstract syntax tree (AST)
    - **Execution**: walks the AST and executes instructions recursively

    The interpreter owns two independent stores: a symbol table of named
    variables, and a sparse address space keyed by numeric address. Paging is
    not implemented here; instead the owner installs a page-fault handler (see
    SetPageFaultHandler) that the interpreter consults before every memory
    access.
*/
class Interpreter {
 public:
  /** @brief Create a new interpreter instance with empty state.
   */
  Interpreter() = default;

  /** @brief sets the limits of the memory on initialize

      Accesses outside [start, end) raise an access violation. Without this
      call, every address is considered valid.

      @param start First valid address
      @param end One past the last valid address
  */
  void SetMemoryBounds(uint32_t start, uint32_t end);

  /** @brief Configure the page size used for paging-aware READ/WRITE access.

      @param page_size Size of one page in bytes. Zero disables paging.
  */
  void SetPageSize(uint32_t page_size);

  /** @brief Install a handler that is invoked when a READ/WRITE hits a missing
             page.

      The handler receives the page number derived from the target address and
      returns true if the access faulted, in which case the instruction is
      abandoned so its caller can retry it. Returning false lets the access
      proceed. An interpreter with no handler installed never faults.

      @param handler Callback taking a page number, returning true on fault
  */
  void SetPageFaultHandler(std::function<bool(int)> handler);

  /** @brief Returns whether the most recently executed instruction hit a page
             fault. */
  bool GetLastInstructionPageFault() const;

  /** @brief Returns whether the most recently executed instruction caused an
             access violation. */
  bool GetLastInstructionAccessViolation() const;

  /** @brief Returns the offending address from the most recent access
             violation. */
  uint32_t GetLastViolationAddress() const;

  /** @brief Clears the page-fault flag before executing a new instruction. */
  void ResetLastInstructionPageFault();

  /** @brief Clears the access-violation state before executing a new
             instruction. */
  void ResetLastInstructionAccessViolation();

  /** @brief Parse a program string into an abstract syntax tree (AST).

      Returns a vector of Statement nodes ready for execution.
      Performs no evaluation—just parsing and validation.

      @param program The program string to parse
      @return Vector of Statement AST nodes
  */
  std::vector<Statement> Parse(const std::string& program);

  /** @brief Parse a user-supplied instruction list from "screen -c".

      Unlike Parse, this rejects anything it cannot understand rather than
      producing kUnknown nodes, so a bad program never becomes a process. An
      instruction fails when its keyword is unrecognised or it carries the
      wrong number of arguments.

      @param program Raw instruction string, with the surrounding quotes
                     already stripped
      @param out Receives the parsed statements; left untouched on failure
      @return true if the program was non-empty and every instruction parsed
  */
  bool ParseUserProgram(const std::string& program, std::vector<Statement>& out);

  /** @brief Split a user program on top-level semicolons.

      Semicolons inside quotes, parentheses or brackets are part of an
      instruction and do not split it.

      @param program The raw instruction string
      @return The individual instruction strings, trimmed, empties dropped
  */
  static std::vector<std::string> SplitUserInstructions(
      const std::string& program);

  /** @brief Rewrite one user instruction into the canonical parenthesised
             form.

      "DECLARE varA 10" becomes "DECLARE(varA, 10)". Instructions already
      written with parentheses, and bare tokens such as "DBG!", are returned
      unchanged.

      @param instruction A single trimmed instruction
      @return The canonical form, or an empty string for empty input
  */
  static std::string CanonicalizeUserInstruction(
      const std::string& instruction);

  /** @brief Execute a pre-parsed AST (vector of Statements).

      Useful if you want to parse once and execute multiple times.

      @param stmts Vector of Statement nodes to execute
  */
  void ExecuteStatements(const std::vector<Statement>& stmts);

  /** @brief Parse and execute a program string in one call.

      Convenience method combining parsing and execution.
      Catches exceptions and adds error messages to the output buffer.

      @param program The program string to parse and execute
  */
  void ExecuteString(std::string program);

  /** @brief Retrieve and clear the output buffer.

      Gets all output written by PRINT, DBG!, or error messages
      since the last flush, then clears the buffer.

      @return Vector of output strings
  */
  std::vector<std::string> FlushBuffer();

  /** @brief Snapshot a page-sized range of the address space for paging.

      @param start_address The first address in the page range.
      @param page_size The size of the page range in bytes.
      @return A vector of address/value pairs in the requested range.
  */
  std::vector<std::pair<uint32_t, uint16_t>> GetPageSnapshot(
      uint32_t start_address, uint32_t page_size) const;

  /** @brief Clear any address-space entries within a page-sized range.

      @param start_address The first address in the page range.
      @param page_size The size of the page range in bytes.
  */
  void ClearPageRange(uint32_t start_address, uint32_t page_size);

  /** @brief Restore a page-sized range of the address space from a snapshot.

      @param page_entries The address/value pairs to restore.
  */
  void RestorePageSnapshot(
      const std::vector<std::pair<uint32_t, uint16_t>>& page_entries);

  /** @brief Execute PRINT: output text or numeric values.

      Handles string literals and variable substitution.
      Results are appended to the screen buffer.

      @param stmt The PRINT statement to execute

      @returns the value printed to the buffer

      @warning side effect on screen_buffer_ attribute
  */
  std::string ExecutePrint(const Statement& stmt);

  /** @brief Execute DECLARE: create and initialize a variable.

      @param stmt The DECLARE statement to execute, or none at failure

      @return the value of the declaration, or none if the symbol table is full

      @warning side effect on memory_ attribute
  */
  std::optional<uint16_t> ExecuteDeclare(const Statement& stmt);

  /** @brief Execute ADD: compute first_operand + second_operand, store in
             variable.

      @param stmt The ADD statement to execute

      @return the resulting value of the ADD, or none at failure

      @warning side effect on memory_ attribute
      @warning arithmetic wraps at 16 bits
  */
  std::optional<uint16_t> ExecuteAdd(const Statement& stmt);

  /** @brief Execute SUBTRACT: compute first_operand - second_operand, store in
             variable.

      @param stmt The SUBTRACT statement to execute

      @return the resulting value of the SUBTRACT, or none at failure

      @warning side effect on memory_ attribute
      @warning arithmetic wraps at 16 bits
  */
  std::optional<uint16_t> ExecuteSubtract(const Statement& stmt);

  /** @brief Execute SLEEP: pause for N ticks (1 tick = 10ms).
      @param stmt The SLEEP statement to execute
  */
  void ExecuteSleep(const Statement& stmt);

  /** @brief Execute FOR: run nested instructions repeatedly.

      The repeat count is resolved from the second argument.

      @param stmt The FOR statement to execute
  */
  void ExecuteFor(const Statement& stmt);

  /** @brief Execute READ

      @param stmt The READ statement to execute

      @return An ordered pair of the variable name and the value
      assigned to it.

      On failure, this returns null.

      For any case, side effects occur at THIS class (see @warning).

      @warning Side effect on *this* class. Specifically on the fields
      of the class relevant to IO.
  */
  std::optional<std::pair<std::string, uint16_t>> ExecuteRead(
      const Statement& stmt);

  /** @brief Execute WRITE

      @param stmt The WRITE statement to execute

      @return An ordered pair of the address and the value written to
      that address.

      On failure, this returns null.

      For any case, side effects occur at THIS class (see @warning).

      @warning Side effect on *this* class. Specifically on the fields
      of the class relevant to IO.
  */
  std::optional<std::pair<uint32_t, uint16_t>> ExecuteWrite(
      const Statement& stmt);

  /** @brief Execute DBG!: dump all variables and their values to the screen
             buffer.

      @returns the memory of the class
  */
  std::unordered_map<std::string, uint16_t> ExecuteDebug();

 private:
  /** @enum AccessStatus
      @brief Outcome of validating a memory access, before it is performed.
  */
  enum class AccessStatus {
    kOk,        /*!< The access may proceed */
    kViolation, /*!< Address outside the configured bounds; fatal */
    kFault,     /*!< Page not resident; the instruction should be retried */
  };

  /** @brief Hard cap on the symbol table: 32 variables.
      Any declaration or write that would introduce a 33rd variable is
      ignored. */
  static constexpr std::size_t kMaxSymbols = 32;

  /** @brief Removes leading/trailing whitespace and newlines.
      @param s The string to trim
      @return Trimmed string, or empty string if the input is all whitespace
  */
  static std::string Trim(const std::string& s);

  /** @brief Splits a single string of instructions to a list of
      strings. Each string is a "sub-instruction" in the whole string.

      Example:
      - Input: "FOO(x), FOR([PRINT(1), PRINT(2)], 3)"
      - Output: ["FOO(x)", "FOR([PRINT(1), PRINT(2)], 3)"]

      @param list The comma-separated instruction list to split
      @return Vector of trimmed instruction strings
  */
  static std::vector<std::string> SplitInstructions(const std::string& list);

  /** @brief Figures out what kind of instruction this statement is.

      Uses prefix matching to determine the keyword type.
      For example, "PRINT(...)" will match the PRINT keyword.

      @param stmt The statement string to analyze
      @return The identified Keyword, or Keyword::kUnknown if not recognized
  */
  static Keyword IdentifyKeyword(const std::string& stmt);

  /** @brief Extracts raw arguments from a statement string for the given
             keyword type.

      Parses the statement syntax to extract its arguments.
      Arguments remain as strings until execution (not evaluated yet).

      @param stmt The statement string to parse
      @param kw The keyword type identifying which statement format to expect
      @return Vector of argument strings (format varies by keyword)
  */
  static std::vector<std::string> ExtractArgs(const std::string& stmt,
                                              Keyword kw);

  /** @brief Parse a comma-separated block of instructions into a vector of AST
             nodes.

      The recursive entry point for parsing. Splits the input by commas
      (respecting bracket nesting) and parses each instruction.

      @param block_str The instruction block string to parse
      @return Vector of Statement AST nodes
  */
  std::vector<Statement> ParseBlock(const std::string& block_str);

  /** @brief Parse a single statement string into an AST node.

      Handles recursive parsing of FOR loop bodies, extracting arguments
      and recursively parsing nested instructions.

      @param stmt_str The statement string to parse
      @return A Statement AST node, ready for execution
  */
  Statement ParseStatement(const std::string& stmt_str);

  /** @brief parses a hexadecimal address (0x1234) to an int (1234)

      @param address_string A decimal or 0x-prefixed hexadecimal literal
      @return The parsed address
  */
  static uint32_t ParseAddress(const std::string& address_string);

  /** @brief Resolves an operand to a numeric value.

      Handles three cases:
      - Numeric literal: "42" → 42
      - Variable reference: "x" → memory_[x] (auto-create with 0 if missing
        and the symbol table is not yet full)
      - Empty string: "" → 0

      @param op The operand string to resolve (number, variable name, or empty)
      @return The resolved 16-bit unsigned integer value (0 if the variable
              could not be created because the symbol table is full)
  */
  uint16_t ResolveOperand(const std::string& op);

  /** @brief Writes a variable, respecting the kMaxSymbols cap.

      Existing variables are always updated. A new variable is only created
      while the symbol table has room; otherwise the write is dropped and a
      warning is pushed to the screen buffer.

      @param name The variable name to write
      @param value The value to store
      @return true if the write happened, false if the table was full
  */
  bool SetVariable(const std::string& name, uint16_t value);

  /// @}
  /// @name Memory access
  /// @{

  /** @brief checks if a memory address is within the stated bounds

      @param address The address to check
      @return true when no bounds are set, or the address falls inside them
  */
  bool IsValidAddress(uint32_t address) const;

  /** @brief Clears the page-fault and access-violation state. */
  void ResetAccessState();

  /** @brief Validates an access and records the resulting state.

      Bounds are checked before paging, so an out-of-bounds address never
      reaches the page-fault handler.

      @param address The address about to be read or written
      @return Whether the access may proceed, violated its bounds, or faulted

      @warning side effect on the fields of the class relevant to IO
  */
  AccessStatus CheckAccess(uint32_t address);

  /** @brief
   * 
   * Checks if the symbol table occupies the first 64 bytes of the 
   * process address space
   * 
   * @return boolean value if spavce is allocated or not
   */
  bool CheckSymbolTableAccess();

  /** @brief Execute a single AST node.

      Dispatches to the appropriate execution handler based on the statement's
      keyword type, converting any exception into a screen-buffer error
      message.

      @param stmt The Statement AST node to execute
  */
  void ExecuteStatement(const Statement& stmt);

  std::unordered_map<std::string, uint16_t> memory_;     /*!< Variable storage
                                                              (all 16-bit
                                                              unsigned) */
  std::unordered_map<uint32_t, uint16_t> address_space_; /*!< Variable storage
                                                              in memory
                                                              addresses */
  std::vector<std::string> screen_buffer_;               /*!< Output buffer
                                                              (PRINT, DBG!,
                                                              errors) */

  uint32_t mem_start_ = 0;       /*!< First valid address */
  uint32_t mem_end_ = 0;         /*!< One past the last valid address */
  bool bounds_set_ = false;      /*!< Whether SetMemoryBounds was called */
  uint32_t page_size_bytes_ = 0; /*!< Page size; zero disables paging */

  bool last_instruction_page_fault_ = false;       /*!< Retry the instruction */
  bool last_instruction_access_violation_ = false; /*!< Kill the process */
  uint32_t last_violation_address_ = 0;            /*!< Offending address */

  std::function<bool(int)> page_fault_handler_; /*!< Empty when unpaged */

  uint16_t pending_read_value_ = 0;
  bool has_pending_read_value_ = false;
  std::string pending_read_variable_;
};

}  // namespace prosched

#endif  // PROSCHED_COMMANDS_INTERPRETER_H_
