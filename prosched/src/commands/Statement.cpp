#include "commands/Statement.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ios>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace prosched {
namespace {

/** @brief Keywords eligible for random generation.

    DBG and UNKNOWN are excluded: the former is a debugging aid, the latter is
    an error state.

    FOR is deliberately last: past the nesting limit the generator draws from
    the array minus its final entry, which costs an integer instead of the
    copy-and-erase of a temporary vector. The generator runs once per generated
    instruction - thousands of times per second per process - so per-call heap
    traffic here shows up directly in the scheduler's tick rate.
*/
constexpr std::array<Keyword, 8> kGeneratableKeywords = {
    Keyword::kPrint, Keyword::kAdd,   Keyword::kSubtract, Keyword::kDeclare,
    Keyword::kSleep, Keyword::kRead,  Keyword::kWrite,    Keyword::kFor,
};

/** @brief Nesting depth at which FOR stops being generated, bounding
           recursion. */
constexpr int kMaxNestingDepth = 3;

/** @brief Number of distinct variable names the generator draws from. */
constexpr int kVariableNameCount = 10;

/** @brief The generator's random source.

    One generator per thread: a std::mt19937 carries mutable state, and both
    the scheduler's generator thread and the CLI thread ("screen -s") reach the
    statement generator, so a single shared instance would be advanced by two
    threads at once.

    @return This thread's Mersenne Twister, seeded on first use
*/
std::mt19937& Rng() {
  static thread_local std::mt19937 gen(std::random_device{}());
  return gen;
}

/** @brief Formats a page index as a page-aligned hexadecimal address literal.

    @param page_index The page number to format
    @return A string such as "0x1f00", suitable as a READ/WRITE argument
*/
std::string FormatHexAddress(uint32_t address) {
  static constexpr char kDigits[] = "0123456789abcdef";
  char buffer[11];  // "0x" + 8 nibbles + NUL
  int end = sizeof(buffer) - 1;
  buffer[end] = '\0';
  if (address == 0) {
    buffer[--end] = '0';
  }
  while (address != 0) {
    buffer[--end] = kDigits[address & 0xF];
    address >>= 4;
  }
  buffer[--end] = 'x';
  buffer[--end] = '0';
  return std::string(buffer + end);
}

std::string FormatAddress(int page_index) {
  return FormatHexAddress(static_cast<uint32_t>(page_index) * 256);
}

/** @brief The variable names the generator draws from, built once.

    "var" + std::to_string(n) allocated a fresh string on every draw, and every
    ADD or SUBTRACT draws twice.
*/
/** @brief Draws a word-aligned address inside a process's address space.

    READ and WRITE move a uint16, so the address has to be even; an empty or
    inverted range yields 0.

    @param gen The generator to draw from
    @param memStart First address the process owns
    @param memEnd One past the last address the process owns
    @return An even address in [memStart, memEnd)
*/
uint32_t RandomWordAddress(std::mt19937& gen, int memStart, int memEnd) {
  if (memEnd <= memStart) {
    return 0;
  }
  std::uniform_int_distribution<uint32_t> address_dist(memStart, memEnd - 2);
  return (address_dist(gen) / 2) * 2;
}

const std::array<std::string, kVariableNameCount>& VariableNames() {
  static const std::array<std::string, kVariableNameCount> names = [] {
    std::array<std::string, kVariableNameCount> built;
    for (int i = 0; i < kVariableNameCount; ++i) {
      built[i] = "var" + std::to_string(i);
    }
    return built;
  }();
  return names;
}

}  // namespace

Statement GetRandomStatement(const std::string& process_name, int max_depth, int memStart, int memEnd) {
  std::mt19937& gen = Rng();

  // Past the nesting limit FOR drops out; it is the last entry, so that is one
  // fewer candidate rather than a rebuilt list.
  const std::size_t candidates =
      kGeneratableKeywords.size() - (max_depth >= kMaxNestingDepth ? 1 : 0);
  std::uniform_int_distribution<std::size_t> keyword_dist(0, candidates - 1);
  const Keyword chosen = kGeneratableKeywords[keyword_dist(gen)];

  std::uniform_int_distribution<int> uint16_dist(0, 65535);
  std::uniform_int_distribution<int> variable_dist(0, kVariableNameCount - 1);
  const auto random_variable_name = [&]() -> const std::string& {
    return VariableNames()[variable_dist(gen)];
  };

  Statement stmt;
  stmt.keyword = chosen;

  switch (chosen) {
    case Keyword::kPrint:
      stmt.args = {"\"Hello world from " + process_name + "!\""};
      break;

    case Keyword::kDeclare:
      stmt.args = {random_variable_name(), std::to_string(uint16_dist(gen))};
      break;

    case Keyword::kAdd:
    case Keyword::kSubtract:
      stmt.args = {random_variable_name(), random_variable_name(),
                   std::to_string(uint16_dist(gen))};
      break;

    case Keyword::kSleep: {
      std::uniform_int_distribution<int> uint8_dist(0, 255);
      stmt.args = {std::to_string(uint8_dist(gen))};
      break;
    }

    case Keyword::kFor: {
      std::uniform_int_distribution<int> repeat_dist(1, 5);
      std::uniform_int_distribution<int> body_size_dist(1, 3);

      const int repeats = repeat_dist(gen);
      const int body_size = body_size_dist(gen);

      stmt.args = {"", std::to_string(repeats)};
      for (int i = 0; i < body_size; ++i) {
        stmt.nested.push_back(GetRandomStatement(process_name, max_depth + 1, memStart, memEnd));
      }
      break;
    }

    case Keyword::kRead: {
      stmt.args = {random_variable_name(),
                   FormatHexAddress(RandomWordAddress(gen, memStart, memEnd))};
      break;
    }

    case Keyword::kWrite: {
      stmt.args = {FormatHexAddress(RandomWordAddress(gen, memStart, memEnd)),
                   std::to_string(uint16_dist(gen))};
      break;
    }

    case Keyword::kDebug:
    case Keyword::kUnknown:
      break;
  }

  return stmt;
}

}  // namespace prosched
