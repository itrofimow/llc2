#include "llc2_bt_cmd.hpp"
#include "llc2_init_cmd.hpp"

namespace lldb {
bool PluginInitialize(lldb::SBDebugger debugger) {
  lldb::SBCommandInterpreter interpreter = debugger.GetCommandInterpreter();
  lldb::SBCommand foo = interpreter.AddMultiwordCommand("llc2", nullptr);
  foo.AddCommand("init", new llc2::InitCmd{}, "init");
  foo.AddCommand("bt", new llc2::BacktraceCmd{}, "bt");
  return true;
}
}  // namespace lldb
