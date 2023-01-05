#pragma once

#include "base_cmd.hpp"

namespace llc2 {

class BacktraceCmd final : public CmdBase {
 public:
  bool DoExecute(lldb::SBDebugger, char**,
                 lldb::SBCommandReturnObject& result) final;
};

}  // namespace llc2
