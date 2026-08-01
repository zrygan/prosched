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
*/
constexpr std::array<Keyword, 8> kGeneratableKeywords = {
    Keyword::kPrint, Keyword::kAdd,   Keyword::kSubtract, Keyword::kDeclare,
    Keyword::kFor,   Keyword::kSleep, Keyword::kRead,     Keyword::kWrite,
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
std::string FormatAddress(int page_index) {
  std::ostringstream address;
  address << "0x" << std::hex << (page_index * 256);
  return address.str();
}

}  // namespace

Statement GetRandomStatement(const std::string& process_name, int max_depth, int memStart, int memEnd) {
  std::mt19937& gen = Rng();

  std::vector<Keyword> available(kGeneratableKeywords.begin(),
                                 kGeneratableKeywords.end());
  if (max_depth >= kMaxNestingDepth) {
    available.erase(
        std::remove(available.begin(), available.end(), Keyword::kFor),
        available.end());
  }

  std::uniform_int_distribution<std::size_t> keyword_dist(0,
                                                          available.size() - 1);
  const Keyword chosen = available[keyword_dist(gen)];

  std::uniform_int_distribution<int> uint16_dist(0, 65535);
  std::uniform_int_distribution<int> variable_dist(0, kVariableNameCount - 1);
  const auto random_variable_name = [&]() {
    return "var" + std::to_string(variable_dist(gen));
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
      std::ostringstream addr; 
      if (memEnd > memStart) {
        std::uniform_int_distribution<uint32_t> addrDist(memStart, memEnd - 2);
        uint32_t rawAddr = addrDist(gen);
        rawAddr = (rawAddr / 2) * 2;
        addr << "0x" << std::hex << rawAddr;
      } else {
          addr << "0x0";
      }
      stmt.args = { random_variable_name(), addr.str() };
      break;
    }

    case Keyword::kWrite: {
      std::ostringstream addr; 
      if (memEnd > memStart) {
        std::uniform_int_distribution<uint32_t> addrDist(memStart, memEnd - 2);
        uint32_t rawAddr = addrDist(gen);
        rawAddr = (rawAddr / 2) * 2;
        addr << "0x" << std::hex << rawAddr;
      } else {
          addr << "0x0";
      }
      stmt.args = { addr.str(), std::to_string(uint16_dist(gen)) };
      break;
    }

    case Keyword::kDebug:
    case Keyword::kUnknown:
      break;
  }

  return stmt;
}

}  // namespace prosched
