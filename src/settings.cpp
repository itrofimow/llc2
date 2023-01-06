#include "settings.hpp"

#include <getopt.h>

#include <limits>
#include <memory>

namespace llc2 {

namespace {

std::unique_ptr<LLC2Settings> settings;

constexpr std::size_t kPageSize = 4096;

}  // namespace

std::size_t LLC2Settings::GetRealStackSize() const {
  // TODO : recheck
  return GetMmapSize() - kPageSize;
}

std::size_t LLC2Settings::GetMmapSize() const {
  const std::size_t pages = (stack_size + kPageSize - 1) / kPageSize;
  // add one page at bottom that will be used as guard-page
  return (pages + 1) * kPageSize;
}

LLC2Settings* GetSettings() { return settings.get(); }

void ParseSettings(char** cmd) {
  static struct option opts[] = {
      {"stack_size", required_argument, nullptr, 's'},
      {"context_implementation", required_argument, nullptr, 'c'},
      {"with_magic", required_argument, nullptr, 'm'},
      {"filter_by", optional_argument, nullptr, 'f'},
      {"truncate_at", optional_argument, nullptr, 't'},
      {nullptr, 0, nullptr, 0}};

  settings.reset();

  int argc = 1;
  for (char** p = cmd; p != nullptr && *p != nullptr; p++) argc++;

  char* args[argc];
  // Make this look like a command line, we need a valid element at index 0
  // for getopt_long to use in its error messages.
  char name[] = "llc2";
  args[0] = name;
  for (int i = 0; i < argc - 1; i++) args[i + 1] = cmd[i];

  LLC2Settings parsed_settings{};
  bool invalid = false;

  // Reset getopts.
  optind = 0;
  opterr = 1;
  do {
    // NOLINTNEXTLINE
    int arg = getopt_long(argc, args, "s:c:mf:t:", opts, nullptr);
    if (arg == -1) break;

    // NOLINTNEXTLINE
    switch (arg) {
      case 's': {
        std::size_t stack_size = std::strtoul(optarg, nullptr, 10);
        parsed_settings.stack_size = stack_size;
      } break;
      case 'c': {
        std::string context_implementation{optarg};
        if (context_implementation == "ucontext") {
          parsed_settings.context_implementation =
              ContextImplementation::kUcontext;
        } else if (context_implementation == "fcontext") {
          parsed_settings.context_implementation =
              ContextImplementation::kFcontext;
        } else {
          invalid = true;
        }
      } break;
      case 'm': {
        parsed_settings.with_magic = true;
      } break;
      case 'f': {
        std::string filter_by{optarg};
        parsed_settings.filter_by.emplace(std::move(filter_by));
      } break;
      case 't': {
        std::string truncate_at{optarg};
        parsed_settings.truncate_at.emplace(std::move(truncate_at));
      } break;
      default:
        continue;
    }
  } while (true);

  if (parsed_settings.stack_size == 0 ||
      parsed_settings.stack_size == std::numeric_limits<std::size_t>::max() ||
      parsed_settings.stack_size < 16 * 1024) {
    invalid = true;
  }

  if (!invalid) {
    settings = std::make_unique<LLC2Settings>(std::move(parsed_settings));
  }
}

}  // namespace llc2