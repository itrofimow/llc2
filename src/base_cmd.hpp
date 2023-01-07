#pragma once

#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBDebugger.h>

namespace llc2 {

class CmdBase : public lldb::SBCommandPluginInterface {
 protected:
  bool DoExecute(lldb::SBDebugger debugger, char** command,
                 lldb::SBCommandReturnObject& result) final;

  virtual bool RealExecute(lldb::SBDebugger debugger, char** command,
                           lldb::SBCommandReturnObject& result) = 0;
};

}  // namespace llc2
