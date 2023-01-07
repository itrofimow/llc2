#include "base_cmd.hpp"

namespace llc2 {

bool CmdBase::DoExecute(lldb::SBDebugger debugger, char** command,
                        lldb::SBCommandReturnObject& result) {
  try {
    return RealExecute(debugger, command, result);
  } catch (const std::exception& ex) {
    result.Printf("Something went terribly wrong: %s\n", ex.what());
    return false;
  }
}

}  // namespace llc2
