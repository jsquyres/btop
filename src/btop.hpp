#include <span>
#include <string_view>

[[nodiscard]] auto btop_main(std::string_view executable, std::span<const std::string_view> args) -> int;
