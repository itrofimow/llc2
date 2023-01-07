#include "llc2_bt_cmd.hpp"

#include "settings.hpp"

#include <sys/ucontext.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

struct RegionInfo final {
  std::uintptr_t begin{};
  std::uintptr_t end{};
};

std::vector<RegionInfo> GetProcessMemoryRegions(
    lldb::SBProcess& process, lldb::SBCommandReturnObject& result) {
  auto lldb_regions = process.GetMemoryRegions();

  std::vector<RegionInfo> regions(lldb_regions.GetSize());
  for (std::uint32_t i = 0; i < lldb_regions.GetSize(); ++i) {
    lldb::SBMemoryRegionInfo region_info{};
    if (!lldb_regions.GetMemoryRegionAtIndex(i, region_info)) {
      result.Printf("Failed to get memory region info at index %u\n", i);
      continue;
    }
    regions[i].begin = region_info.GetRegionBase();
    regions[i].end = region_info.GetRegionEnd();
  }

  std::sort(
      regions.begin(), regions.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.begin < rhs.begin; });
  return regions;
}

enum class state_t : unsigned int {
  none = 0,
  complete = 1 << 1,
  unwind = 1 << 2,
  destroy = 1 << 3
};

// This struct mimics that of boost.Coroutine2
struct CoroControlBlockWithMagic final {
  std::size_t magic{0};
  void* fiber{};
  void* other{};  // this is pull_coroutine
  state_t state{};
  void* except{};  // this is std::exception_ptr

  static constexpr std::size_t kMagic = 0x12345678;
};

// This struct mimics that of boost.Coroutine2
struct CoroControlBlock final {
  void* fiber{};
  void* other{};  // this is pull_coroutine
  state_t state{};
  void* except{};  // this is std::exception_ptr
};

void* GetFiberPointer(lldb::SBProcess& process,
                      lldb::SBCommandReturnObject& result,
                      const RegionInfo& region_info, void* sp,
                      const LLC2Settings& settings) {
  if (settings.with_magic) {
    const auto remaining_size =
        GetSettings()->GetMmapSize() -
        (reinterpret_cast<char*>(region_info.end) - static_cast<char*>(sp));
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

// We only need 3 registers to unwind: rsp, rbp and rip.
// This is all x86_64 ofc.
struct UnwindRegisters final {
  std::int64_t rsp{};  // stack pointer
  std::int64_t rbp{};  // frame pointer
  std::int64_t rip{};  // instruction pointer

  UnwindRegisters(std::int64_t rsp, std::int64_t rbp, std::int64_t rip)
      : rsp{rsp}, rbp{rbp}, rip{rip} {}

  UnwindRegisters(const ucontext_t& ucontext)
      : rsp{ucontext.uc_mcontext.gregs[REG_RSP]},
        rbp{ucontext.uc_mcontext.gregs[REG_RBP]},
        rip{ucontext.uc_mcontext.gregs[REG_RIP]} {}
};

std::unique_ptr<UnwindRegisters> TryGetRegistersFromUcontext(
    lldb::SBProcess& process, lldb::SBCommandReturnObject& result,
    void* fiber_ptr) {
  lldb::SBError error{};
  ucontext_t context{};
  // fiber_ptr points to fiber_activation_record, which has ucontext_t as the
  // first field. We hope that compiler doesn't reorder fields in the struct.
  // TODO : what's the deal with +8 here?
  process.ReadMemory(reinterpret_cast<std::uintptr_t>(fiber_ptr) + 8, &context,
                     sizeof(ucontext_t), error);
  if (!error.Success()) {
    result.Printf("Failed to read ucontext from process memory: %s\n",
                  error.GetCString());
    return nullptr;
  }

  return std::make_unique<UnwindRegisters>(context);
}

// clang-format off
/****************************************************************************************
 *                                                                                      *
 *  ----------------------------------------------------------------------------------  *
 *  |    0    |    1    |    2    |    3    |    4     |    5    |    6    |    7    |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x0   |   0x4   |   0x8   |   0xc   |   0x10   |   0x14  |   0x18  |   0x1c  |  *
 *  ----------------------------------------------------------------------------------  *
 *  | fc_mxcsr|fc_x87_cw|        R12        |         R13        |        R14        |  *
 *  ----------------------------------------------------------------------------------  *
 *  ----------------------------------------------------------------------------------  *
 *  |    8    |    9    |   10    |   11    |    12    |    13   |    14   |    15   |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x20  |   0x24  |   0x28  |  0x2c   |   0x30   |   0x34  |   0x38  |   0x3c  |  *
 *  ----------------------------------------------------------------------------------  *
 *  |        R15        |        RBX        |         RBP        |        RIP        |  *
 *  ----------------------------------------------------------------------------------  *
 *                                                                                      *
 ****************************************************************************************/
// clang-format on
std::unique_ptr<UnwindRegisters> TryGetRegistersFromFcontext(
    lldb::SBProcess& process, lldb::SBCommandReturnObject& result,
    void* fiber_ptr) {
  constexpr std::size_t kContextDataSize = 0x40;

  char context_data[kContextDataSize];
  lldb::SBError error{};
  // so fiber_ptr is a detail::fcontext_t, which in turn is just a void*.
  process.ReadMemory(reinterpret_cast<std::uintptr_t>(fiber_ptr), &context_data,
                     kContextDataSize, error);
  if (!error.Success()) {
    result.Printf("Failed to read fcontext from process memory: %s\n",
                  error.GetCString());
    return nullptr;
  }

  const auto read_with_offset = [&context_data](std::size_t offset) {
    return *reinterpret_cast<std::int64_t*>(&context_data[offset]);
  };

  // with fcontext fiber_ptr is a pointer to context data,
  // detail::jump_fcontext populate registers from it and sets rsp to it + 0x40
  const auto rsp = reinterpret_cast<std::int64_t>(fiber_ptr) + kContextDataSize;
  const auto rbp = read_with_offset(0x30);
  const auto rip = read_with_offset(0x38);

  return std::make_unique<UnwindRegisters>(rsp, rbp, rip);
}

std::unique_ptr<UnwindRegisters> TryFindCoroRegisters(
    lldb::SBProcess& process, lldb::SBCommandReturnObject& result,
    const RegionInfo& region_info) {
  // We already validated that settings aren't null
  const auto& settings = *GetSettings();

  constexpr std::size_t func_alignment = 64;  // alignof( ControlBlock);
  const std::size_t func_size = settings.with_magic
                                    ? sizeof(CoroControlBlockWithMagic)
                                    : sizeof(CoroControlBlock);

  // reserve space on stack
  void* sp =
      reinterpret_cast<char*>(region_info.end) - func_size - func_alignment;
  // align sp pointer
  std::size_t space = func_size + func_alignment;
  sp = std::align(func_alignment, func_size, sp, space);
  // sp is where coroutine::control_block is allocated on stack

  void* fiber_ptr = GetFiberPointer(process, result, region_info, sp, settings);
  if (fiber_ptr == nullptr) {
    return nullptr;
  }

  if (settings.context_implementation == ContextImplementation::kUcontext) {
    return TryGetRegistersFromUcontext(process, result, fiber_ptr);
  } else {
    return TryGetRegistersFromFcontext(process, result, fiber_ptr);
  }
}

std::string GetFullWidth(std::string_view what) {
  if (what.size() + 2 > terminal_width) {
    return std::string{what};
  }

  const auto num_dashes = (terminal_width - (what.size() + 2)) / 2;
  std::string dashes(num_dashes, '-');

  return dashes + " " + what.data() + " " + dashes;
}

void BacktraceCoroutine(std::uintptr_t stack_address,
                        lldb::SBThread& current_thread,
                        lldb::SBCommandReturnObject& result, bool full) {
  const auto num_frames = current_thread.GetNumFrames();

  bool has_sleep = false;
  int wrapped_call_frame = num_frames;

  std::vector<lldb::SBStream> frame_descriptions(num_frames);
  for (int i = 0; i < num_frames; ++i) {
    auto frame = current_thread.GetFrameAtIndex(i);
    frame.GetDescription(frame_descriptions[i]);

    const auto* display_name = frame_descriptions[i].GetData();
    if (display_name == nullptr) {
      continue;
    }

    const std::string_view display_name_sw{display_name};
    // TODO : this doesn't work reliably for reasons i don't quite understand
    if (display_name_sw.find(kUserverSleepMark) != std::string_view::npos) {
      has_sleep = true;
    }

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
  result.Printf("coro stack address: %p\n",
                reinterpret_cast<void*>(stack_address));

  lldb::SBStream result_stream{};
  for (int i = 0; i < wrapped_call_frame; ++i) {
    auto frame = current_thread.GetFrameAtIndex(i);
    result_stream.Print(frame_descriptions[i].GetData());
    if (full) {
      dump_variables(frame, result_stream, true, false);
      dump_variables(frame, result_stream, false, true);
    }
  }
  result.Printf("%s", result_stream.GetData());
}

struct BtSettings final {
  bool full{false};
  std::optional<std::uintptr_t> stack_address;
};

BtSettings ParseBtSettings(char** cmd) {
  BtSettings result{};
  for (auto** p = cmd; p != nullptr && *p != nullptr; ++p) {
    const auto* s = *p;
    if (std::strcmp(s, "-f") == 0) {
      result.full = true;
      continue;
    }
    if (std::strcmp(s, "-s") == 0) {
      if ((p + 1) != nullptr && *(p + 1) != nullptr) {
        const std::string_view v{*(p + 1)};
        char* end = nullptr;
        result.stack_address.emplace(std::strtoul(v.data(), &end, 16));
        if (end != v.data() + v.size()) {
          result.stack_address.reset();
        }
      }
    }
  }

  return result;
}

std::chrono::steady_clock::time_point Now() {
  return std::chrono::steady_clock::now();
}

void PrintDuration(lldb::SBCommandReturnObject& result, const std::string& name,
                   std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point finish) {
  result.Printf(
      "%s duration: %lu ms\n", name.data(),
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count());
}

struct ScopeTimer final {
  std::string name;
  std::chrono::steady_clock::time_point start;
  lldb::SBCommandReturnObject& result;

  ScopeTimer(lldb::SBCommandReturnObject& result, std::string name)
      : name{std::move(name)}, start{Now()}, result{result} {}

  ~ScopeTimer() { PrintDuration(result, name, start, Now()); }
};

}  // namespace

bool BacktraceCmd::RealExecute(lldb::SBDebugger debugger, char** cmd,
                               lldb::SBCommandReturnObject& result) {
  const auto bt_settings = ParseBtSettings(cmd);

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

  ScopeTimer total{result, "llc2 bt"};
  const auto memory_regions = GetProcessMemoryRegions(process, result);
  for (const auto& memory_region : memory_regions) {
    const auto length = memory_region.end - memory_region.begin;
    if (length != settings_ptr->GetRealStackSize()) {
      continue;
    }

    // this doesn't directly relate to neither stack bottom nor stack top,
    // not the best name, probably.
    const auto stack_address = memory_region.begin;

    if (bt_settings.stack_address.value_or(stack_address) != stack_address) {
      continue;
    }

    auto unwind_registers_ptr =
        TryFindCoroRegisters(process, result, memory_region);
    if (unwind_registers_ptr != nullptr) {
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

      const auto& regs = *unwind_registers_ptr;
      const auto old_rsp = upd_register("rsp", regs.rsp);
      const auto old_rbp = upd_register("rbp", regs.rbp);
      const auto old_rip = upd_register("rip", regs.rip);

      frame.SetPC(regs.rip);
      BacktraceCoroutine(stack_address, thread, result, bt_settings.full);

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
}

}  // namespace llc2
