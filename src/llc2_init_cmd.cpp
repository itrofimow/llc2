#include "llc2_init_cmd.hpp"

#include <getopt.h>
#include <cstdint>

namespace llc2 {

namespace {

struct Options final {
  std::size_t stack_size;
};

Options ParseOptions(char** cmd) {
  static struct option opts[] = {
      {"stack_size", required_argument, nullptr, 's'},
      {nullptr, 0, nullptr, 0}};

  int argc = 1;
  for (char** p = cmd; p != nullptr && *p != nullptr; p++) argc++;

  char* args[argc];
  // Make this look like a command line, we need a valid element at index 0
  // for getopt_long to use in its error messages.
  char name[] = "llc2";
  args[0] = name;
  for (int i = 0; i < argc - 1; i++) args[i + 1] = cmd[i];

  Options options{};

  // Reset getopts.
  optind = 0;
  opterr = 1;
  do {
    // NOLINTNEXTLINE
    int arg = getopt_long(argc, args, "s:", opts, nullptr);
    if (arg == -1) break;

    // NOLINTNEXTLINE
    switch (arg) {
      case 's': {
        std::size_t stack_size = std::strtoul(optarg, nullptr, 10);
        options.stack_size = stack_size;
      } break;
      default:
        continue;
    }
  } while (true);

  return options;
}

}  // namespace

bool InitCmd::DoExecute(lldb::SBDebugger, char** cmd,
                        lldb::SBCommandReturnObject& result) {
  result.Printf("Init called\n");
  const auto options = ParseOptions(cmd);
  result.Printf("stack_size: %lu\n", options.stack_size);
  return true;
}

}  // namespace llc2
