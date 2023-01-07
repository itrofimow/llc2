#include "llc2_bt_cmd.hpp"
#include "llc2_init_cmd.hpp"

namespace lldb {
bool PluginInitialize(lldb::SBDebugger debugger) {
  lldb::SBCommandInterpreter interpreter = debugger.GetCommandInterpreter();
  lldb::SBCommand llc2 = interpreter.AddMultiwordCommand("llc2", nullptr);

  llc2.AddCommand(
      "init", new llc2::InitCmd{},
      "Initialize plugin settings\n"
      "-s              coroutine stack size\n"
      "-c              context implementation (ucontext|fcontext)\n"
      "-m              with coroutine signing magic\n"
      "-f              only show coroutines which have this in their trace\n"
      "-t              truncate coroutine stack when this is met\n");

  // TODO : add some options
  llc2.AddCommand("bt", new llc2::BacktraceCmd{},
                  "Print backtrace of all currently sleeping coroutines\n");
  return true;
}
}  // namespace lldb
