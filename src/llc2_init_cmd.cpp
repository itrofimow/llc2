#include "llc2_init_cmd.hpp"

#include "settings.hpp"

namespace llc2 {

bool InitCmd::DoExecute(lldb::SBDebugger, char** cmd,
                        lldb::SBCommandReturnObject& result) {
  ParseSettings(cmd);

  auto* settings_ptr = GetSettings();
  if (settings_ptr == nullptr) {
    result.Printf("Failed to parsed init options\n");
    return false;
  }

  const std::string none_opt{"(null)"};

  const auto& settings = *settings_ptr;
  result.Printf(
      "LLC2 plugin initialized. Settings:\n"
      "stack_size: %lu\ncontext implementation: %s\nwith magic: %s\n"
      "filter by: %s\ntruncate at: %s\n",
      settings.stack_size,
      settings.context_implementation == ContextImplementation::kUcontext
          ? "ucontext"
          : "fcontext",
      settings.with_magic ? "true" : "false",
      settings.filter_by.value_or(none_opt).data(),
      settings.truncate_at.value_or(none_opt).data());
  return true;
}

}  // namespace llc2
