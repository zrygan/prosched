#include "Menu.h"
#include <iostream>

/**
 * @brief Sets the terminal text color using ANSI escape sequences.
 *
 * @param color The color code to apply
 *  (e.g., 10 = green, 14 = yellow, 7 = default).
 */
static void SetColor(int color) {
  switch (color) {
  case 10:
    std::cout << "\033[32m";
    break;
  case 14:
    std::cout << "\033[33m";
    break;
  default:
    std::cout << "\033[0m";
    break;
  }
}

/**
 * @brief Prints the CSOPESY ASCII art banner followed by the initial
 *  commandline instructions. Color-codes key lines using SetColor().
 */
void Welcome() {
  std::cout << R"(

  ______   _____   ____   _____   ______   _____  __     __
 / ____/  / ___/  / __ \ |  __ \ |  ____| / ____| \ \   / /
| |      | (___  | |  | || |__) || |__   | (___    \ \_/ /
| |       \___ \ | |  | ||  ___/ |  __|   \___ \    \   /
| |____   ____) || |__| || |     | |____  ____) |    | |
 \_____| |_____/  \____/ |_|     |______||_____/     |_|

)" << std::endl;
  SetColor(10);
  std::cout << "Hello, welcome to the CSOPESY commandline!" << std::endl;
  SetColor(14);
  std::cout << "Type 'exit' to quit, 'clear' to clear the screen." << std::endl;
  std::cout << "\n ** IMPORTANT: Type 'initialize' to load config and start "
               "system ** \n"
            << std::endl;
  SetColor(7);
}

void CommandSet() {
  std::cout << "Initialized successfully.\n\n";
  std::cout << "List of accessible commands:\n"
            << "- exit\n"
            << "- screen -s <process name> <process memory size>\n"
            << "- screen -r <process name>\n"
            << "- screen -c <process name> <process memory size> "
               "\"<instructions>\"\n"
            << "- screen -ls\n"
            << "- scheduler-start (or scheduler-test)\n"
            << "- scheduler-stop\n"
            << "- report-util\n"
            << "- vmstat\n"
            << "- process-smi\n\n";
}
