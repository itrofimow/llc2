#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace llc2 {

enum class ContextImplementation { kUcontext, kFcontext };

struct LLC2Settings final {
  std::size_t stack_size{};
  ContextImplementation context_implementation{};
  bool with_magic{false};
  std::optional<std::string> filter_by;
  std::optional<std::string> truncate_at;

  std::size_t GetRealStackSize() const;

  std::size_t GetMmapSize() const;
};

// Returns null if settings are invalid, pointer to settings otherwise.
LLC2Settings* GetSettings();

void ParseSettings(char** cmd);

}  // namespace llc2
