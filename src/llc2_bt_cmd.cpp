#include "llc2_bt_cmd.hpp"

#include <sys/ucontext.h>
#include <cstdint>
#include <memory>
#include <string_view>

#include <lldb/API/SBMemoryRegionInfo.h>
#include <lldb/API/SBMemoryRegionInfoList.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBStream.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBThread.h>

#include <lldb/Target/ExecutionContext.h>
#include <lldb/Target/Process.h>
#include <lldb/Target/RegisterContext.h>
#include <lldb/Target/StackFrame.h>
#include <lldb/Target/Thread.h>

namespace llc2 {

namespace {

constexpr std::size_t kStackSize = 262144;
constexpr std::size_t kFullMmapSize = kStackSize + 4096;

constexpr std::string_view kUserverSleepMark =
    "engine::impl::TaskContext::Sleep";
constexpr std::string_view kUserverWrappedCallImplMark =
    "utils::impl::WrappedCallImpl<";

constexpr std::string_view kFoundCoroutineMessage =
    "\n---------------------------- found sleeping coroutine "
    "----------------------------";

class LLC2Frame : public lldb::SBFrame {
 public:
  LLC2Frame(const lldb::SBFrame& other) : lldb::SBFrame{other} {}

  lldb::StackFrameSP GetNativeFrame() const { return GetFrameSP(); }

  lldb::ThreadSP GetNativeThread() const { return m_opaque_sp->GetThreadSP(); }
};

enum class state_t : unsigned int {
  none = 0,
  complete = 1 << 1,
  unwind = 1 << 2,
  destroy = 1 << 3
};

// This struct mimics that of boost.Coroutine2
struct CoroControlBlock final {
  std::size_t magic{0};
  void* fiber;
  void* other;
  state_t state;
  std::exception_ptr except;

  static constexpr std::size_t kMagic = 0x12345678;
};

template <std::size_t Size>
struct FailAssert final {
  static_assert(!Size);
};

std::unique_ptr<ucontext_t> TryFindCoroContext(
    lldb::SBProcess& process, lldb::SBCommandReturnObject& result,
    lldb::SBMemoryRegionInfo& region_info) {
  constexpr std::size_t func_alignment = 64;  // alignof( ControlBlock);
  constexpr std::size_t func_size = sizeof(CoroControlBlock);

  // reserve space on stack
  void* sp = reinterpret_cast<char*>(region_info.GetRegionEnd()) - func_size -
             func_alignment;
  // align sp pointer
  std::size_t space = func_size + func_alignment;
  sp = std::align(func_alignment, func_size, sp, space);

  const auto remaining_size =
      kFullMmapSize - (reinterpret_cast<char*>(region_info.GetRegionEnd()) -
                       static_cast<char*>(sp));
  const auto expected_magic = CoroControlBlock::kMagic ^
                              reinterpret_cast<std::uintptr_t>(sp) ^
                              remaining_size;

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

  if (control_block.magic != expected_magic) {
    result.Printf("Magic doesn't match: expected %lu, got %lu\n",
                  expected_magic, control_block.magic);
    return nullptr;
  }

  ucontext_t context{};
  // TODO : what's the deal with +8 here?
  process.ReadMemory(reinterpret_cast<std::uintptr_t>(control_block.fiber) + 8,
                     &context, sizeof(ucontext_t), error);
  if (!error.Success()) {
    result.Printf("Failed to read ucontext from process memory: %s\n",
                  error.GetCString());
    return nullptr;
  }

  return std::make_unique<ucontext_t>(std::move(context));
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

    // TODO : this doesn't work for some reason
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
        stream.Print("------------- frame arguments -------------\n");
      } else if (locals) {
        stream.Print("------------- frame locals -------------\n");
      }
    }
    for (std::uint32_t j = 0; j < frame_variables.GetSize(); ++j) {
      auto variable = frame_variables.GetValueAtIndex(j);
      variable.GetDescription(stream);
    }
  };

  result.AppendMessage(kFoundCoroutineMessage.data());
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
  result.Printf("bt called\n");

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
    if (length != kStackSize) {
      continue;
    }

    auto context_ptr = TryFindCoroContext(process, result, region_info);
    if (context_ptr != nullptr) {
      LLC2Frame hack{thread.GetSelectedFrame()};

      auto registers = hack.GetNativeFrame()->GetRegisterContext();
      const auto old_fp = registers->GetFP();
      const auto old_sp = registers->GetSP();
      const auto old_pc = registers->GetPC();

      const auto& gregs = context_ptr->uc_mcontext.gregs;
      registers->SetFP(gregs[REG_RBP]);
      registers->SetSP(gregs[REG_RSP]);
      registers->SetPC(gregs[REG_RIP]);

      // debugger.HandleCommand("bt");
      BacktraceCoroutine(thread, result);

      registers->SetFP(old_fp);
      registers->SetSP(old_sp);
      registers->SetPC(old_pc);
    }
  }

  return true;
}

}  // namespace llc2
