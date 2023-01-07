#pragma once

#include "base_cmd.hpp"

namespace llc2 {

class BacktraceCmd final : public CmdBase {
 public:
  bool RealExecute(lldb::SBDebugger, char**,
                   lldb::SBCommandReturnObject&) final;
};

}  // namespace llc2
