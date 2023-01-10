#include "llc2_bt_cmd.hpp"

#include "settings.hpp"

#include <sys/ucontext.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
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
    "engine::impl::TaskContext::Sleep(";
constexpr std::string_view kUserverWrappedCallImplMark =
    "utils::impl::WrappedCallImpl<";
constexpr std::string_view kTaskContextPointerTypeMark =
    "engine::impl::TaskContext *";

constexpr std::string_view kLotsOfDashes =
    "--------------------------------------------------------------------------"
    "--------------------------------------------------------------------------"
    "---------------------------------------------------";

constexpr std::string_view GetDashesSw(std::size_t size) {
  return kLotsOfDashes.substr(0, std::min(kLotsOfDashes.size(), size));
}

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

std::chrono::steady_clock::time_point Now() {
  return std::chrono::steady_clock::now();
}

void PrintDuration(lldb::SBCommandReturnObject& result, const std::string& name,
                   std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point finish) {
  result.Printf(
      "%s duration: %lu"
      "ms\n",
      name.data(),
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

std::string GetFullWidth(std::string_view what, bool center) {
  if (what.size() + 2 > terminal_width) {
    return std::string{what};
  }

  const auto num_dashes = (terminal_width - (what.size() + 2)) / 2;
  std::string result;
  result.reserve(terminal_width);

  if (center) {
    result.append(GetDashesSw(num_dashes))
        .append(" ")
        .append(what)
        .append(" ")
        .append(GetDashesSw(num_dashes));
  } else {
    result.append(what).append(" ").append(GetDashesSw(num_dashes * 2));
  }

  return result;
}

bool EndsWith(std::string_view source, std::string_view what) {
  const auto pos = source.find(what);
  return pos != std::string_view::npos && pos + what.size() == source.size();
}

// https://bugs.llvm.org/show_bug.cgi?id=24202

// Clang is not emitting debug information for std::string because it was told
// that libstdc++ provides it. This is a debug size optimization that GCC
// apparently doesn't perform.
//
// So we read strings by hand to not depend on libstdc++ debug info presence.
std::optional<std::string> ReadStdString(lldb::SBProcess process,
                                         std::size_t address,
                                         lldb::SBCommandReturnObject& result) {
  // let's hope for the best
  constexpr std::size_t kBufferSize = sizeof(std::string);
  constexpr std::size_t kMaxLen = 100;

  if (address == 0) {
    return std::nullopt;
  }

  char buffer[kBufferSize]{};
  lldb::SBError error{};
  process.ReadMemory(address, buffer, kBufferSize, error);
  if (!error.Success()) {
    result.Printf("Failed to read std::string from process memory: %s\n",
                  error.GetCString());
    return std::nullopt;
  }

  const auto* fake_str_ptr = reinterpret_cast<std::string*>(buffer);

  const auto* data = fake_str_ptr->data();
  const auto size = fake_str_ptr->size();
  if (size > kMaxLen) {
    return std::nullopt;
  }

  std::string result_string{};
  result_string.resize(size);

  process.ReadMemory(reinterpret_cast<std::uintptr_t>(data),
                     result_string.data(), size, error);
  if (!error.Success()) {
    result.Printf("Failed to read std::string from process memory: %s\n",
                  error.GetCString());
    return std::nullopt;
  }

  return {std::move(result_string)};
}

void BacktraceCoroutine(std::uintptr_t stack_address,
                        lldb::SBThread& current_thread,
                        lldb::SBCommandReturnObject& result, bool full) {
  const auto num_frames = current_thread.GetNumFrames();

  bool has_sleep = false;
  int wrapped_call_frame = num_frames;

  std::vector<lldb::SBStream> frame_descriptions(num_frames);

  struct SpanInfo final {
    std::string name;
    std::string span_id;
    std::string trace_id;
  };
  std::optional<SpanInfo> span_info{};

  for (int i = 0; i < num_frames; ++i) {
    auto frame = current_thread.GetFrameAtIndex(i);
    frame.GetDescription(frame_descriptions[i]);

    const auto* display_name = frame_descriptions[i].GetData();
    if (display_name == nullptr) {
      continue;
    }

    const std::string_view display_name_sw{display_name};
    // TODO : this doesn't always work for reasons i don't quite understand
    if (display_name_sw.find(kUserverSleepMark) != std::string_view::npos) {
      if (i == 0) {
        // we are somewhere in the process of coroutine going to sleep, this
        // means it's running right now and is visible in just 'bt', without
        // llc2. We also might have some things not yet set up, so we skip it.
        break;
      }
      has_sleep = true;

      auto maybe_context_ptr = frame.FindVariable("this");
      const auto* display_type_name = maybe_context_ptr.GetDisplayTypeName();
      if (display_type_name != nullptr &&
          EndsWith(display_type_name, kTaskContextPointerTypeMark) &&
          !span_info.has_value()) {
        auto task_context = maybe_context_ptr.Dereference();
        auto span_ptr = task_context.GetChildMemberWithName("parent_span_");

        if (span_ptr.GetValueAsUnsigned() != 0) {
          auto span_impl = span_ptr.Dereference()
                               .GetChildMemberWithName("pimpl_")
                               .Dereference();

          auto lldb_name = span_impl.GetChildMemberWithName("name_");
          auto lldb_span_id = span_impl.GetChildMemberWithName("span_id_");
          auto lldb_trace_id = span_impl.GetChildMemberWithName("trace_id_");

          const auto name_opt =
              ReadStdString(current_thread.GetProcess(),
                            lldb_name.AddressOf().GetValueAsUnsigned(), result);
          const auto span_id_opt = ReadStdString(
              current_thread.GetProcess(),
              lldb_span_id.AddressOf().GetValueAsUnsigned(), result);
          const auto trace_id_opt = ReadStdString(
              current_thread.GetProcess(),
              lldb_trace_id.AddressOf().GetValueAsUnsigned(), result);

          const std::string empty_str{"(none)"};

          span_info.emplace(SpanInfo{name_opt.value_or(empty_str),
                                     span_id_opt.value_or(empty_str),
                                     trace_id_opt.value_or(empty_str)});
        }
      }
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
        const auto args_title = GetFullWidth("FRAME ARGUMENTS", false);
        stream.Printf("%s\n", args_title.data());
      } else if (locals) {
        const auto locals_title = GetFullWidth("FRAME LOCALS", false);
        stream.Printf("%s\n", locals_title.data());
      }
    }
    for (std::uint32_t j = 0; j < frame_variables.GetSize(); ++j) {
      auto variable = frame_variables.GetValueAtIndex(j);
      variable.GetDescription(stream);
    }
  };

  const auto found_coroutine_title =
      GetFullWidth("FOUND SLEEPING COROUTINE", true);
  result.AppendMessage(found_coroutine_title.data());
  auto printed = result.Printf("coro stack address: %p",
                               reinterpret_cast<void*>(stack_address));
  result.Printf("\n%s\n", std::string{GetDashesSw(printed)}.data());

  if (span_info.has_value()) {
    printed =
        result.Printf("Parent span (name, span_id, trace_id): %s | %s | %s",
                      span_info->name.data(), span_info->span_id.data(),
                      span_info->trace_id.data());
    result.Printf("\n%s\n", std::string{GetDashesSw(printed)}.data());
  }

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

std::int64_t UpdateRegisterValue(lldb::SBValue& general_purpose_registers,
                                 lldb::SBCommandReturnObject& result,
                                 const char* reg_name, std::int64_t reg_value) {
  auto reg_sb_value =
      general_purpose_registers.GetChildMemberWithName(reg_name);

  lldb::SBData data{};
  data.SetDataFromSInt64Array(&reg_value, 1);

  const auto prev = reg_sb_value.GetValueAsSigned();

  lldb::SBError error{};
  reg_sb_value.SetData(data, error);
  if (!error.Success()) {
    result.Printf("Failed to update '%s' register\n", reg_name);
  }

  return prev;
}

class CurrentFrameRegistersGuard final {
 public:
  CurrentFrameRegistersGuard(lldb::SBThread& thread,
                             lldb::SBCommandReturnObject& result)
      : thread_{thread}, result_{result} {}

  void ChangeRegisters(const UnwindRegisters& regs) {
    auto [frame, registers] = GetCurrentFrameRegisters();

    const auto old_regs = UpdateRegs(registers, regs);
    if (!old_registers_.has_value()) {
      old_registers_.emplace(old_regs);
    }
    frame.SetPC(regs.rip);
  }

  ~CurrentFrameRegistersGuard() {
    if (!old_registers_.has_value()) {
      return;
    }
    const auto& old_regs = *old_registers_;

    auto [frame, registers] = GetCurrentFrameRegisters();
    UpdateRegs(registers, old_regs);
    frame.SetPC(old_regs.rip);
  }

 private:
  struct FrameRegisters final {
    lldb::SBFrame frame;
    lldb::SBValue registers;
  };

  FrameRegisters GetCurrentFrameRegisters() {
    auto frame = thread_.GetSelectedFrame();
    auto registers =
        frame.GetRegisters().GetFirstValueByName("General Purpose Registers");

    return {std::move(frame), std::move(registers)};
  }

  UnwindRegisters UpdateRegs(lldb::SBValue& lldb_registers,
                             const UnwindRegisters& regs) {
    const auto old_rsp =
        UpdateRegisterValue(lldb_registers, result_, "rsp", regs.rsp);
    const auto old_rbp =
        UpdateRegisterValue(lldb_registers, result_, "rbp", regs.rbp);
    const auto old_rip =
        UpdateRegisterValue(lldb_registers, result_, "rip", regs.rip);

    return {old_rsp, old_rbp, old_rip};
  }

  lldb::SBThread& thread_;
  lldb::SBCommandReturnObject& result_;
  std::optional<UnwindRegisters> old_registers_;
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
  if (!thread.IsValid()) {
    result.Printf("No thread selected\n");
    return false;
  }

  ScopeTimer total{result, "llc2 bt"};

  const auto memory_regions = GetProcessMemoryRegions(process, result);
  CurrentFrameRegistersGuard regs_guard{thread, result};
  for (const auto& memory_region : memory_regions) {
    const auto length = memory_region.end - memory_region.begin;
    if (length != settings_ptr->GetRealStackSize()) {
      continue;
    }

    // this doesn't directly relate to neither stack bottom nor stack top
    const auto stack_address = memory_region.begin;

    if (bt_settings.stack_address.value_or(stack_address) != stack_address) {
      continue;
    }

    auto unwind_registers_ptr =
        TryFindCoroRegisters(process, result, memory_region);
    if (unwind_registers_ptr != nullptr) {
      ScopeTimer coro{result, "coro backtrace"};
      const auto& regs = *unwind_registers_ptr;
      regs_guard.ChangeRegisters(regs);
      BacktraceCoroutine(stack_address, thread, result, bt_settings.full);
    }
  }

  return true;
}

}  // namespace llc2
