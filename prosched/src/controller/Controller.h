#include "Context.h"
#include "memory/PagingManager.h"
#include "src/scheduler/Scheduler.h"
#include "view/View.h"

#ifndef CONTROLLER_H
#define CONTROLLER_H

enum class CLI_COMMAND {
  CLI_EXIT,
  CLI_CLEAR,
  CLI_SCREEN_LS,
  CLI_SCREEN_S,
  CLI_SCREEN_R,
  CLI_SCREEN_C,
  CLI_SCHEDULER_START,
  CLI_SCHEDULER_STOP,
  CLI_REPORT_UTIL,
  CLI_VMSTAT,
  CLI_PROCESS_SMI,
  UNKNOWN,
};

struct Command {
  CLI_COMMAND cliCommand;

  // speicfically for screen -s
  std::string processName;
  long memorySize = 0;

  // specifically for screen -c: the quoted instruction list, with the
  // surrounding quotes stripped. Empty when none was supplied.
  std::string instructions;
};

class Controller {

private:
  View view;
  AlgoContext ctx;
  prosched::Scheduler *scheduler = nullptr;
  prosched::PagingManager *pagingManager = nullptr;
  // Read before it is ever assigned otherwise: Controller is default
  // constructed in main(), so an uninitialised bool takes whatever the stack
  // held. Reading true sends "initialize" to the post-init handler, which
  // answers "Command unknown" and leaves scheduler null - the program never
  // starts. It only appeared to work because a -O0 stack happened to be zero.
  bool isInitialized = false;

public:
  ~Controller();
  AlgoContext initialize();
  void run();
  void HandleView();
  bool HandlePreInit(const std::string &input);
  void HandlePostInit(const std::string &input);

  Command GetParsedInput(const std::string &input);
  void ExecuteCommand(const Command &command);
  CLI_COMMAND IdentifyCommand(const std::vector<std::string> &command);
  static long ParseMemorySize(const std::string &token);
  static bool IsValidMemoryAllocation(long bytes);

  /**
   * @brief Extracts the quoted instruction list from a raw "screen -c" input.
   *
   * Takes everything between the first and last double quote, so quotes that
   * belong to the instructions themselves are preserved. Backslash-escaped
   * quotes are unescaped, which lets a program be pasted in verbatim.
   *
   * @param input The whole command line
   * @return The instruction list, or an empty string when unquoted
   */
  static std::string ExtractQuotedInstructions(const std::string &input);

  /**
   * @brief Builds the "screen -r" notice for a process killed by a memory
   * access violation.
   *
   * Only ever called for a TERMINATED process, which by construction always
   * carries both a violation time and a violation address (Process.h sets all
   * three together).
   */
  static std::string FormatAccessViolationNotice(const std::string &name,
                                                 const std::string &clockTime,
                                                 uint32_t address);
  void ExitOS();
  void EnterProcessScreen(prosched::Process *p);
  void PrintReportUtil();
  void PrintVmStat();
  void PrintProcessSmi();
};

#endif
