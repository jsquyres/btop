#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace Btop {
	struct restart_state {
		uint64_t first;
		uint64_t count;
	};

	[[nodiscard]] inline auto parse_restart_env(std::string_view value) -> std::optional<uint64_t> {
		uint64_t result{};
		const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
		if (ec != std::errc{} or ptr != value.data() + value.size()) return std::nullopt;
		return result;
	}

	[[nodiscard]] inline auto parse_restart_env(const char* value) -> std::optional<uint64_t> {
		return value == nullptr ? std::nullopt : parse_restart_env(std::string_view{value});
	}

	[[nodiscard]] inline auto next_restart_state(uint64_t now, std::optional<uint64_t> restart_first, std::optional<uint64_t> restart_count) -> restart_state {
		const auto in_window = restart_first.has_value() and now - restart_first.value() <= 60;
		const auto first = in_window ? restart_first.value() : now;
		const auto count = (in_window and restart_count.has_value()) ? restart_count.value() + 1 : 1;
		return {first, count};
	}
}

[[nodiscard]] auto btop_main(std::string_view executable, std::span<const std::string_view> args) -> int;
