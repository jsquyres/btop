#include <span>
#include <string_view>

[[nodiscard]] auto btop_main(std::span<const std::string_view> args, std::string_view argv0) -> int;

#ifdef BTOP_TESTING
namespace Runner::Test {
	bool ipc_sample_roundtrip_ok();
	bool ipc_frame_errors_ok();
}
#endif
