#include "llc2_bt_cmd.hpp"

#include "settings.hpp"

#include <sys/ucontext.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <lldb/API/SBMemoryRegionInfo.h>
#include <lldb/API/SBMemoryRegionInfoList.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBStream.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBThread.h>

namespace llc2 {

namespace {

std::uint32_t terminal_width;

constexpr std::string_view kUserverSleepMark =
    "engine::impl::TaskContext::Sleep";
constexpr std::string_view kUserverWrappedCallImplMark =
    "utils::impl::WrappedCallImpl<";

enum class state_t : unsigned int {
  none = 0,
  complete = 1 << 1,
  unwind = 1 << 2,
  destroy = 1 << 3
};

// This struct mimics that of boost.Coroutine2
struct CoroControlBlockWithMagic final {
  std::size_t magic{0};
  void* fiber;
  void* other;
  state_t state;
  std::exception_ptr except;

  static constexpr std::size_t kMagic = 0x12345678;
};

// This struct mimics that of boost.Coroutine2
struct CoroControlBlock final {
  void* fiber;
  void* other;
  state_t state;
  std::exception_ptr except;
};

void* GetFiberPointer(lldb::SBProcess& process,
                      lldb::SBCommandReturnObject& result,
                      lldb::SBMemoryRegionInfo& region_info, void* sp,
                      const LLC2Settings& settings) {
  if (settings.with_magic) {
    const auto remaining_size =
        GetSettings()->GetMmapSize() -
        (reinterpret_cast<char*>(region_info.GetRegionEnd()) -
         static_cast<char*>(sp));
    const auto expected_magic = CoroControlBlockWithMagic::kMagic ^
                                reinterpret_cast<std::uintptr_t>(sp) ^
                                remaining_size;

    CoroControlBlockWithMagic control_block{};
    lldb::SBError error{};
    process.ReadMemory(reinterpret_cast<std::uintptr_t>(sp), &control_block,
                       sizeof(CoroControlBlockWithMagic), error);
    if (!error.Success()) {
      result.Printf(
          "Failed to read Coro::control_block from process memory: %s\n",
          error.GetCString());
      return nullptr;
    }

    if (control_block.magic != expected_magic) {
      result.Printf("Magic doesn't match: expected %lu, got %lu\n",
                    expected_magic, control_block.magic);
      return nullptr;
    }

    return control_block.fiber;
  } else {
    CoroControlBlock control_block{};
    lldb::SBError error{};
    process.ReadMemory(reinterpret_cast<std::uintptr_t>(sp), &control_block,
                       sizeof(CoroControlBlock), error);
    if (!error.Success()) {
      result.Printf(
          "Failed to read Coro::control_block from process memory: %s\n",
          error.GetCString());
      return nullptr;
    }

    return control_block.fiber;
  }
}

std::unique_ptr<ucontext_t> TryFindCoroContext(
    lldb::SBProcess& process, lldb::SBCommandReturnObject& result,
    lldb::SBMemoryRegionInfo& region_info) {
  // We validated that settings aren't null
  const auto& settings = *GetSettings();

  constexpr std::size_t func_alignment = 64;  // alignof( ControlBlock);
  const std::size_t func_size = settings.with_magic
                                    ? sizeof(CoroControlBlockWithMagic)
                                    : sizeof(CoroControlBlock);

  // reserve space on stack
  void* sp = reinterpret_cast<char*>(region_info.GetRegionEnd()) - func_size -
             func_alignment;
  // align sp pointer
  std::size_t space = func_size + func_alignment;
  sp = std::align(func_alignment, func_size, sp, space);

  void* fiber_ptr = GetFiberPointer(process, result, region_info, sp, settings);
  if (fiber_ptr == nullptr) {
    return nullptr;
  }

  lldb::SBError error{};
  ucontext_t context{};
  // TODO : what's the deal with +8 here?
  process.ReadMemory(reinterpret_cast<std::uintptr_t>(fiber_ptr) + 8, &context,
                     sizeof(ucontext_t), error);
  if (!error.Success()) {
    result.Printf("Failed to read ucontext from process memory: %s\n",
                  error.GetCString());
    return nullptr;
  }

  return std::make_unique<ucontext_t>(std::move(context));
}

std::string GetFullWidth(std::string_view what) {
  if (what.size() + 2 > terminal_width) {
    return std::string{what};
  }

  const auto num_dashes = (terminal_width - (what.size() + 2)) / 2;
  std::string dashes(num_dashes, '-');

  return dashes + " " + what.data() + " " + dashes;
}

void BacktraceCoroutine(lldb::SBThread& current_thread,
                        lldb::SBCommandReturnObject& result,
                        bool full = false) {
  const auto num_frames = current_thread.GetNumFrames();

  bool has_sleep = false;
  int wrapped_call_frame = 0;
  for (int i = 0; i < num_frames; ++i) {
    auto frame = current_thread.GetFrameAtIndex(i);
    const auto* display_name = frame.GetDisplayFunctionName();
    if (display_name == nullptr) {
      continue;
    }

    const std::string_view display_name_sw{display_name};

    if (display_name_sw.find(kUserverSleepMark) != std::string_view::npos) {
      has_sleep = true;
    }

    // TODO : this doesn't work that well with inlined frames
    if (display_name_sw.find(kUserverWrappedCallImplMark) !=
        std::string_view::npos) {
      wrapped_call_frame = i;
      break;
    }
  }
  if (!has_sleep) return;

  const auto dump_variables = [](lldb::SBFrame& frame, lldb::SBStream& stream,
                                 bool arguments, bool locals) {
    auto frame_variables = frame.GetVariables(
        arguments, locals, false /* statics */, true /* in_scope_only */);
    if (frame_variables.GetSize() > 0) {
      if (arguments) {
        const auto args_title = GetFullWidth("FRAME ARGUMENTS");
        stream.Printf("%s\n", args_title.data());
      } else if (locals) {
        const auto locals_title = GetFullWidth("FRAME LOCALS");
        stream.Printf("%s\n", locals_title.data());
      }
    }
    for (std::uint32_t j = 0; j < frame_variables.GetSize(); ++j) {
      auto variable = frame_variables.GetValueAtIndex(j);
      variable.GetDescription(stream);
    }
  };

  const auto found_coroutine_title = GetFullWidth("found sleeping coroutine");
  result.AppendMessage(found_coroutine_title.data());
  for (int i = 0; i < wrapped_call_frame; ++i) {
    auto frame = current_thread.GetFrameAtIndex(i);
    lldb::SBStream stream{};
    frame.GetDescription(stream);

    if (full) {
      dump_variables(frame, stream, true, false);
      dump_variables(frame, stream, false, true);
    }
    result.Printf("%s", stream.GetData());
  }
}

}  // namespace

bool BacktraceCmd::DoExecute(lldb::SBDebugger debugger, char**,
                             lldb::SBCommandReturnObject& result) {
  try {
    const auto* settings_ptr = GetSettings();
    if (settings_ptr == nullptr) {
      result.Printf("LLC2 plugin is not initialized\n");
      return false;
    }
    terminal_width = debugger.GetTerminalWidth();

    auto target = debugger.GetSelectedTarget();
    if (!target.IsValid()) {
      result.Printf("No target selected\n");
      return false;
    }
    auto process = target.GetProcess();
    if (!process.IsValid()) {
      result.Printf("No process launched\n");
      return false;
    }
    auto thread = process.GetSelectedThread();

    auto memory_regions = target.GetProcess().GetMemoryRegions();
    for (std::uint32_t i = 0; i < memory_regions.GetSize(); ++i) {
      lldb::SBMemoryRegionInfo region_info{};
      if (!memory_regions.GetMemoryRegionAtIndex(i, region_info)) {
        result.Printf("Failed to get memory region info at index %u\n", i);
        return false;
      }

      const auto length =
          region_info.GetRegionEnd() - region_info.GetRegionBase();
      // TODO : settings.stack_size
      if (length != settings_ptr->GetRealStackSize()) {
        continue;
      }

      auto context_ptr = TryFindCoroContext(process, result, region_info);
      if (context_ptr != nullptr) {
        auto frame = thread.GetSelectedFrame();
        auto registers = frame.GetRegisters();
        auto gpr_registers =
            registers.GetFirstValueByName("General Purpose Registers");

        const auto upd_register = [&gpr_registers, &result](
                                      const char* reg_name,
                                      std::int64_t reg_value) {
          auto reg_sb_value = gpr_registers.GetChildMemberWithName(reg_name);

          lldb::SBData data{};
          data.SetDataFromSInt64Array(&reg_value, 1);

          const auto prev = reg_sb_value.GetValueAsSigned();

          lldb::SBError error{};
          reg_sb_value.SetData(data, error);
          if (!error.Success()) {
            result.Printf("Failed to update '%s' register\n", reg_name);
          }

          return prev;
        };

        const auto& gregs = context_ptr->uc_mcontext.gregs;
        const auto old_rbp = upd_register("rbp", gregs[REG_RBP]);
        const auto old_rsp = upd_register("rsp", gregs[REG_RSP]);
        const auto old_rip = upd_register("rip", gregs[REG_RIP]);

        frame.SetPC(gregs[REG_RIP]);
        BacktraceCoroutine(thread, result);

        frame = thread.GetSelectedFrame();
        registers = frame.GetRegisters();
        gpr_registers =
            registers.GetFirstValueByName("General Purpose Registers");
        upd_register("rbp", old_rbp);
        upd_register("rsp", old_rsp);
        upd_register("rip", old_rip);
        frame.SetPC(old_rip);
      }
    }

    return true;
  } catch (const std::exception& ex) {
    result.Printf("something went wrong: %s\n", ex.what());
    return false;
  }
}

}  // namespace llc2
