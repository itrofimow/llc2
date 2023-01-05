#pragma once

#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBDebugger.h>

namespace llc2 {

class CmdBase : public lldb::SBCommandPluginInterface {};

}  // namespace llc2
