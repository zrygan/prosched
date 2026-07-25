#ifndef PROSCHED_COMMANDS_STATEMENT_H_
#define PROSCHED_COMMANDS_STATEMENT_H_

#include <string>
#include <vector>

namespace prosched {

/** @enum Keyword
    @brief All the valid instruction types for the OS emulator.

    Defines the set of keywords that the interpreter recognizes and can execute.
*/
enum class Keyword {
  kPrint,    /*!< Output text or values to the screen buffer */
  kDeclare,  /*!< Create a variable and initialize it */
  kAdd,      /*!< Add two operands and store result in a variable */
  kSubtract, /*!< Subtract two operands and store result in a variable */
  kSleep,    /*!< Pause execution for a number of ticks (each tick = 10ms) */
  kFor,      /*!< Loop: execute instructions multiple times */
  kRead,     /*!< Retrieve a uint16 value from a memory address and stores it
                  in a variable */
  kWrite,    /*!< Writes a uint16 value to a memory address */
  kDebug,    /*!< Debug dump: print all declared variables
                  Not required from the specs. This is my own helper
                  thingy. Used in after DECLARE, ADD, SUBTRACT */
  kUnknown,  /*!< Unrecognized instruction (error case) */
};

/** @struct Statement
    @brief Represents a parsed instruction in the abstract syntax tree (AST).

    After parsing, a program becomes a tree of Statements where FOR loops
    can contain nested Statements. This tree structure makes it easy to traverse
    and execute the program recursively.
*/
struct Statement {
  Keyword keyword;               /*!< What kind of instruction this is */
  std::vector<std::string> args; /*!< Raw parsed arguments (not evaluated
                                      yet) */
  std::vector<Statement> nested; /*!< Child statements (only set for FOR
                                      loops) */
};

/** @brief Builds a randomly generated Statement for a synthetic workload.

    @param process_name Embedded in the text of generated PRINT statements
    @param max_depth Current FOR nesting depth. At depth 3 or greater, FOR is
           no longer generated, so the recursion always terminates.

    @return A randomly generated Statement, never UNKNOWN or DBG
*/
Statement GetRandomStatement(const std::string& process_name,
                             int max_depth = 0, int memStart = 0, 
                             int memEnd = 0);

}  // namespace prosched

#endif  // PROSCHED_COMMANDS_STATEMENT_H_
