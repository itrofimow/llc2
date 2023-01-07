#pragma once

#include "base_cmd.hpp"

namespace llc2 {
class InitCmd final : public CmdBase {
 public:
  bool RealExecute(lldb::SBDebugger, char**,
                   lldb::SBCommandReturnObject&) final;
};
}  // namespace llc2
