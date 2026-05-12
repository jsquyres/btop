/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include "btop.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <clocale>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <span>
#include <string_view>
#ifdef __FreeBSD__
	#include <pthread_np.h>
#endif
#include <numeric>
#include <cstdlib>
#include <random>
#include <ranges>
#include <unistd.h>
#include <cmath>
#include <iostream>
#include <exception>
#include <tuple>
#include <regex>
#include <chrono>
#include <utility>
#include <semaphore>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <type_traits>

#ifdef __APPLE__
	#include <CoreFoundation/CoreFoundation.h>
	#include <mach-o/dyld.h>
	#include <limits.h>
#endif

#ifdef __NetBSD__
	#include <sys/param.h>
	#include <sys/sysctl.h>
	#include <unistd.h>
#endif

#include <fmt/core.h>
#include <fmt/ostream.h>

#include "btop_cli.hpp"
#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_input.hpp"
#include "btop_log.hpp"
#include "btop_menu.hpp"
#include "btop_shared.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"

using std::atomic;
using std::cout;
using std::flush;
using std::min;
using std::string;
using std::string_view;
using std::to_string;
using std::vector;

namespace fs = std::filesystem;

using namespace Tools;
using namespace std::chrono_literals;
using namespace std::literals;

namespace Global {
	const vector<array<string, 2>> Banner_src = {
		{"#E62525", "██████╗ ████████╗ ██████╗ ██████╗"},
		{"#CD2121", "██╔══██╗╚══██╔══╝██╔═══██╗██╔══██╗   ██╗    ██╗"},
		{"#B31D1D", "██████╔╝   ██║   ██║   ██║██████╔╝ ██████╗██████╗"},
		{"#9A1919", "██╔══██╗   ██║   ██║   ██║██╔═══╝  ╚═██╔═╝╚═██╔═╝"},
		{"#801414", "██████╔╝   ██║   ╚██████╔╝██║        ╚═╝    ╚═╝"},
		{"#000000", "╚═════╝    ╚═╝    ╚═════╝ ╚═╝"},
	};
	const string Version = "1.4.7";

	int coreCount;
	string overlay;
	string clock;

	string bg_black = "\x1b[0;40m";
	string fg_white = "\x1b[1;97m";
	string fg_green = "\x1b[1;92m";
	string fg_red = "\x1b[0;91m";

	uid_t real_uid, set_uid;

	fs::path self_path;

	string exit_error_msg;
	atomic<bool> thread_exception (false);

	bool debug{};

	uint64_t start_time;

	atomic<bool> resized (false);
	atomic<bool> quitting (false);
	atomic<bool> should_quit (false);
	atomic<bool> should_sleep (false);
	atomic<bool> _runner_started (false);
	atomic<bool> init_conf (false);
	atomic<bool> reload_conf (false);
	string executable_path;
}

namespace Runner {
	static pthread_t runner_id;
} // namespace Runner

namespace Cpu { extern cpu_info current_cpu; }
namespace Mem { extern mem_info current_mem; }
namespace Net { extern std::unordered_map<string, uint64_t> graph_max; extern std::unordered_map<string, array<int, 2>> max_count; extern uint64_t timestamp; }
namespace Proc { extern vector<proc_info> current_procs; }
#if defined(GPU_SUPPORT)
namespace Gpu { extern vector<gpu_info> gpus; }
#endif

//* Handler for SIGWINCH and general resizing events, does nothing if terminal hasn't been resized unless force=true
void term_resize(bool force) {
	static atomic<bool> resizing (false);
	if (Input::polling) {
		Global::resized = true;
		Input::interrupt();
		return;
	}
	atomic_lock lck(resizing, true);
	if (auto refreshed = Term::refresh(true); refreshed or force) {
		if (force and refreshed) force = false;
	}
	else return;
#ifdef GPU_SUPPORT
	static const array<string, 10> all_boxes = {"gpu5", "cpu", "mem", "net", "proc", "gpu0", "gpu1", "gpu2", "gpu3", "gpu4"};
#else
	static const array<string, 5> all_boxes = {"", "cpu", "mem", "net", "proc"};
#endif
	Global::resized = true;
	if (Runner::active) Runner::stop();
	Term::refresh();
	Config::unlock();

	auto boxes = Config::getS("shown_boxes");
	auto min_size = Term::get_min_size(boxes);
	auto minWidth = min_size.at(0), minHeight = min_size.at(1);

	while (not force or (Term::width < minWidth or Term::height < minHeight)) {
		sleep_ms(100);
		if (Term::width < minWidth or Term::height < minHeight) {
			int width = Term::width, height = Term::height;
			cout << fmt::format("{clear}{bg_black}{fg_white}"
					"{mv1}Terminal size too small:"
					"{mv2} Width = {fg_width}{width} {fg_white}Height = {fg_height}{height}"
					"{mv3}{fg_white}Needed for current config:"
					"{mv4}Width = {minWidth} Height = {minHeight}",
					"clear"_a = Term::clear, "bg_black"_a = Global::bg_black, "fg_white"_a = Global::fg_white,
					"mv1"_a = Mv::to((height / 2) - 2, (width / 2) - 11),
					"mv2"_a = Mv::to((height / 2) - 1, (width / 2) - 10),
						"fg_width"_a = (width < minWidth ? Global::fg_red : Global::fg_green),
						"width"_a = width,
						"fg_height"_a = (height < minHeight ? Global::fg_red : Global::fg_green),
						"height"_a = height,
					"mv3"_a = Mv::to((height / 2) + 1, (width / 2) - 12),
					"mv4"_a = Mv::to((height / 2) + 2, (width / 2) - 10),
						"minWidth"_a = minWidth,
						"minHeight"_a = minHeight
			) << std::flush;

			bool got_key = false;
			for (; not Term::refresh() and not got_key; got_key = Input::poll(10));
			if (got_key) {
				auto key = Input::get();
				if (key == "q")
					clean_quit(0);
				else if (key.size() == 1 and isint(key)) {
					auto intKey = stoi(key);
				#ifdef GPU_SUPPORT
					if ((intKey == 0 and Gpu::count >= 5) or (intKey >= 5 and intKey - 4 <= Gpu::count)) {
				#else
					if (intKey > 0 and intKey < 5) {
				#endif
						const auto& box = all_boxes.at(intKey);
						Config::current_preset.reset();
						Config::toggle_box(box);
						boxes = Config::getS("shown_boxes");
					}
				}
			}
			min_size = Term::get_min_size(boxes);
			minWidth = min_size.at(0);
			minHeight = min_size.at(1);
		}
		else if (not Term::refresh()) break;
	}

	Input::interrupt();
}

//* Exit handler; stops threads, restores terminal and saves config changes
void clean_quit(int sig) {
	if (Global::quitting) return;
	Global::quitting = true;
	Runner::stop();
	if (Global::_runner_started) {
	#if defined __APPLE__ || defined __OpenBSD__ || defined __NetBSD__
		if (pthread_join(Runner::runner_id, nullptr) != 0) {
			Logger::warning("Failed to join _runner thread on exit!");
			pthread_cancel(Runner::runner_id);
		}
	#else
		constexpr struct timespec ts { .tv_sec = 5, .tv_nsec = 0 };
		if (pthread_timedjoin_np(Runner::runner_id, nullptr, &ts) != 0) {
			Logger::warning("Failed to join _runner thread on exit!");
			pthread_cancel(Runner::runner_id);
		}
	#endif
	}

#ifdef GPU_SUPPORT
	Gpu::Nvml::shutdown();
	Gpu::Rsmi::shutdown();
	#ifdef __APPLE__
	Gpu::AppleSilicon::shutdown();
	#endif
#endif


	if (Config::getB("save_config_on_exit")) {
		Config::write();
	}

	if (Term::initialized) {
		Input::clear();
		Term::restore();
	}

	if (not Global::exit_error_msg.empty()) {
		sig = 1;
		Logger::error("{}", Global::exit_error_msg);
		fmt::println(std::cerr, "{}ERROR: {}{}{}", Global::fg_red, Global::fg_white, Global::exit_error_msg, Fx::reset);
	}
	Logger::info("Quitting! Runtime: {}", sec_to_dhms(time_s() - Global::start_time));

	const auto excode = (sig != -1 ? sig : 0);

#if defined __APPLE__ || defined __OpenBSD__ || defined __NetBSD__
	_Exit(excode);
#else
	quick_exit(excode);
#endif
}

//* Handler for SIGTSTP; stops threads, restores terminal and sends SIGSTOP
static void _sleep() {
	Runner::stop();
	Term::restore();
	std::raise(SIGSTOP);
}

//* Handler for SIGCONT; re-initialize terminal and force a resize event
static void _resume() {
	Term::init();
	term_resize(true);
}

static void _exit_handler() {
	clean_quit(-1);
}

static void _crash_handler(const int sig) {
	// Restore terminal before crashing
	if (Term::initialized) {
		Term::restore();
	}
	// Re-raise the signal to get default behavior (core dump)
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

static void _signal_handler(const int sig) {
	switch (sig) {
		case SIGINT:
			if (Runner::active) {
				Global::should_quit = true;
				Runner::stopping = true;
				Input::interrupt();
			}
			else {
				clean_quit(0);
			}
			break;
		case SIGTSTP:
			if (Runner::active) {
				Global::should_sleep = true;
				Runner::stopping = true;
				Input::interrupt();
			}
			else {
				_sleep();
			}
			break;
		case SIGCONT:
			_resume();
			break;
		case SIGWINCH:
			term_resize();
			break;
		case SIGUSR1:
			// Input::poll interrupt
			break;
		case SIGUSR2:
			Global::reload_conf = true;
			Input::interrupt();
			break;
	}
}

//* Config init
void init_config(bool low_color, std::optional<std::string>& filter) {
	atomic_lock lck(Global::init_conf);
	vector<string> load_warnings;
	Config::load(Config::conf_file, load_warnings);
	Config::set("lowcolor", (low_color ? true : not Config::getB("truecolor")));

	static bool first_init = true;

	if (Global::debug and first_init) {
		Logger::set_log_level(Logger::Level::DEBUG);
		Logger::debug("Running in DEBUG mode!");
	}
	else Logger::set_log_level(Config::getS("log_level"));

	if (filter.has_value()) {
		Config::set("proc_filter", filter.value());
	}

	static string log_level;
	if (const string current_level = Config::getS("log_level"); log_level != current_level) {
		log_level = current_level;
		Logger::info("Logger set to {}", (Global::debug ? "DEBUG" : log_level));
	}

	for (const auto& err_str : load_warnings) Logger::warning("{}", err_str);
	first_init = false;
}

//* Manages secondary thread for collection and drawing of boxes
namespace Runner {
	atomic<bool> active (false);
	atomic<bool> stopping (false);
	atomic<bool> waiting (false);
	atomic<bool> redraw (false);
	atomic<bool> coreNum_reset (false);

	static inline auto set_active(bool value) noexcept {
		active.store(value, std::memory_order_relaxed);
		active.notify_all();
	}

	//* Setup semaphore for triggering thread to do work
	// TODO: This can be made a local without too much effort.
	std::binary_semaphore do_work { 0 };
	inline void thread_wait() { do_work.acquire(); }
	inline void thread_trigger() { do_work.release(); }

	//* Wrapper for raising privileges when using SUID bit
	class gain_priv {
		int status = -1;
	public:
		gain_priv() {
			if (Global::real_uid != Global::set_uid)
				this->status = seteuid(Global::set_uid);
		}
		~gain_priv() noexcept {
			if (status == 0)
				status = seteuid(Global::real_uid);
		}
		gain_priv(const gain_priv& other) = delete;
		gain_priv& operator=(const gain_priv& other) = delete;
		gain_priv(gain_priv&& other) = delete;
		gain_priv& operator=(gain_priv&& other) = delete;
	};

	string output;
	string empty_bg;
	bool pause_output{};
	sigset_t mask;
	std::mutex mtx;

	enum debug_actions {
		collect_begin,
		collect_done,
		draw_begin,
		draw_begin_only,
		draw_done
	};

	enum debug_array {
		collect,
		draw
	};

	string debug_bg;
	std::unordered_map<string, array<uint64_t, 2>> debug_times;

	class MyNumPunct : public std::numpunct<char>
	{
	protected:
		virtual char do_thousands_sep() const override { return '\''; }
		virtual std::string do_grouping() const override { return "\03"; }
	};


	struct runner_conf {
		vector<string> boxes;
		bool no_update;
		bool force_redraw;
		bool background_update;
		string overlay;
		string clock;
	};

	struct runner_conf current_conf;

	struct collector_sample {
		std::optional<Cpu::cpu_info> cpu;
#if defined(GPU_SUPPORT)
		std::optional<vector<Gpu::gpu_info>> gpu;
		std::unordered_map<string, deque<long long>> shared_gpu_percent;
#endif
		bool cpu_redraw = false;
		bool cpu_topology_reset = false;
		std::optional<Mem::mem_info> mem;
		bool mem_redraw = false;
		std::optional<Net::net_info> net;
		string net_iface;
		vector<string> net_interfaces;
		std::unordered_map<string, Net::net_info> net_current;
		std::unordered_map<string, uint64_t> net_graph_max;
		std::unordered_map<string, array<int, 2>> net_max_count;
		uint64_t net_timestamp = 0;
		std::optional<vector<Proc::proc_info>> proc;
	};

	collector_sample current_sample;

	struct ipc_writer {
		string buf;

		template <typename T>
		void pod(const T& value) {
			static_assert(std::is_trivially_copyable_v<T>);
			buf.append(reinterpret_cast<const char*>(&value), sizeof(value));
		}

		void boolean(bool value) {
			pod<uint8_t>(value ? 1 : 0);
		}

		void str(const string& value) {
			pod<uint64_t>(value.size());
			buf.append(value);
		}

		void path(const fs::path& value) {
			str(value.string());
		}
	};

	struct ipc_reader {
		std::string_view buf;
		size_t pos = 0;

		template <typename T>
		T pod() {
			static_assert(std::is_trivially_copyable_v<T>);
			if (pos + sizeof(T) > buf.size()) throw std::runtime_error("short ipc read");
			T value;
			std::memcpy(&value, buf.data() + pos, sizeof(value));
			pos += sizeof(value);
			return value;
		}

		bool boolean() {
			return pod<uint8_t>() != 0;
		}

		string str() {
			const auto size = pod<uint64_t>();
			if (size > buf.size() - pos) throw std::runtime_error("short ipc string");
			string value{buf.substr(pos, size)};
			pos += size;
			return value;
		}

		fs::path path() {
			return fs::path{str()};
		}
	};

	static void write_deque(ipc_writer& w, const deque<long long>& values) {
		w.pod<uint64_t>(values.size());
		for (const auto value : values) w.pod(value);
	}

	static auto read_deque(ipc_reader& r) -> deque<long long> {
		deque<long long> values;
		const auto size = r.pod<uint64_t>();
		for (uint64_t i = 0; i < size; ++i) values.push_back(r.pod<long long>());
		return values;
	}

	template <typename T, typename WriteValue>
	static void write_map(ipc_writer& w, const std::unordered_map<string, T>& values, WriteValue write_value) {
		w.pod<uint64_t>(values.size());
		for (const auto& [key, value] : values) {
			w.str(key);
			write_value(value);
		}
	}

	template <typename T, typename ReadValue>
	static auto read_map(ipc_reader& r, ReadValue read_value) -> std::unordered_map<string, T> {
		std::unordered_map<string, T> values;
		const auto size = r.pod<uint64_t>();
		for (uint64_t i = 0; i < size; ++i) {
			auto key = r.str();
			auto value = read_value();
			values.emplace(std::move(key), std::move(value));
		}
		return values;
	}

	static void write_string_vector(ipc_writer& w, const vector<string>& values) {
		w.pod<uint64_t>(values.size());
		for (const auto& value : values) w.str(value);
	}

	static auto read_string_vector(ipc_reader& r) -> vector<string> {
		vector<string> values;
		const auto size = r.pod<uint64_t>();
		values.reserve(size);
		for (uint64_t i = 0; i < size; ++i) values.push_back(r.str());
		return values;
	}

	static void write_deque_vector(ipc_writer& w, const vector<deque<long long>>& values) {
		w.pod<uint64_t>(values.size());
		for (const auto& value : values) write_deque(w, value);
	}

	static auto read_deque_vector(ipc_reader& r) -> vector<deque<long long>> {
		vector<deque<long long>> values;
		const auto size = r.pod<uint64_t>();
		values.reserve(size);
		for (uint64_t i = 0; i < size; ++i) values.push_back(read_deque(r));
		return values;
	}

	static void write_cpu_info(ipc_writer& w, const Cpu::cpu_info& cpu) {
		write_map<deque<long long>>(w, cpu.cpu_percent, [&](const auto& value) { write_deque(w, value); });
		write_deque_vector(w, cpu.core_percent);
		write_deque_vector(w, cpu.temp);
		w.pod(cpu.temp_max);
		for (const auto value : cpu.load_avg) w.pod(value);
		w.pod(cpu.usage_watts);
		w.boolean(cpu.active_cpus.has_value());
		if (cpu.active_cpus.has_value()) {
			w.pod<uint64_t>(cpu.active_cpus->size());
			for (const auto value : *cpu.active_cpus) w.pod(value);
		}
	}

	static auto read_cpu_info(ipc_reader& r) -> Cpu::cpu_info {
		Cpu::cpu_info cpu;
		cpu.cpu_percent = read_map<deque<long long>>(r, [&] { return read_deque(r); });
		cpu.core_percent = read_deque_vector(r);
		cpu.temp = read_deque_vector(r);
		cpu.temp_max = r.pod<long long>();
		for (auto& value : cpu.load_avg) value = r.pod<double>();
		cpu.usage_watts = r.pod<float>();
		if (r.boolean()) {
			vector<int32_t> active_cpus;
			const auto size = r.pod<uint64_t>();
			active_cpus.reserve(size);
			for (uint64_t i = 0; i < size; ++i) active_cpus.push_back(r.pod<int32_t>());
			cpu.active_cpus = std::move(active_cpus);
		}
		else cpu.active_cpus.reset();
		return cpu;
	}

	static void write_disk_info(ipc_writer& w, const Mem::disk_info& disk) {
		w.path(disk.dev);
		w.str(disk.name);
		w.str(disk.fstype);
		w.path(disk.stat);
		w.pod(disk.total);
		w.pod(disk.used);
		w.pod(disk.free);
		w.pod(disk.used_percent);
		w.pod(disk.free_percent);
		for (const auto value : disk.old_io) w.pod(value);
		write_deque(w, disk.io_read);
		write_deque(w, disk.io_write);
		write_deque(w, disk.io_activity);
	}

	static auto read_disk_info(ipc_reader& r) -> Mem::disk_info {
		Mem::disk_info disk;
		disk.dev = r.path();
		disk.name = r.str();
		disk.fstype = r.str();
		disk.stat = r.path();
		disk.total = r.pod<int64_t>();
		disk.used = r.pod<int64_t>();
		disk.free = r.pod<int64_t>();
		disk.used_percent = r.pod<int>();
		disk.free_percent = r.pod<int>();
		for (auto& value : disk.old_io) value = r.pod<int64_t>();
		disk.io_read = read_deque(r);
		disk.io_write = read_deque(r);
		disk.io_activity = read_deque(r);
		return disk;
	}

#if defined(GPU_SUPPORT)
	static void write_gpu_supported(ipc_writer& w, const Gpu::gpu_info_supported& supported) {
		w.boolean(supported.gpu_utilization);
		w.boolean(supported.mem_utilization);
		w.boolean(supported.gpu_clock);
		w.boolean(supported.mem_clock);
		w.boolean(supported.pwr_usage);
		w.boolean(supported.pwr_state);
		w.boolean(supported.temp_info);
		w.boolean(supported.mem_total);
		w.boolean(supported.mem_used);
		w.boolean(supported.pcie_txrx);
		w.boolean(supported.encoder_utilization);
		w.boolean(supported.decoder_utilization);
	}

	static auto read_gpu_supported(ipc_reader& r) -> Gpu::gpu_info_supported {
		Gpu::gpu_info_supported supported;
		supported.gpu_utilization = r.boolean();
		supported.mem_utilization = r.boolean();
		supported.gpu_clock = r.boolean();
		supported.mem_clock = r.boolean();
		supported.pwr_usage = r.boolean();
		supported.pwr_state = r.boolean();
		supported.temp_info = r.boolean();
		supported.mem_total = r.boolean();
		supported.mem_used = r.boolean();
		supported.pcie_txrx = r.boolean();
		supported.encoder_utilization = r.boolean();
		supported.decoder_utilization = r.boolean();
		return supported;
	}

	static void write_gpu_info(ipc_writer& w, const Gpu::gpu_info& gpu) {
		write_map<deque<long long>>(w, gpu.gpu_percent, [&](const auto& value) { write_deque(w, value); });
		w.pod(gpu.gpu_clock_speed);
		w.pod(gpu.pwr_usage);
		w.pod(gpu.pwr_max_usage);
		w.pod(gpu.pwr_state);
		write_deque(w, gpu.temp);
		w.pod(gpu.temp_max);
		w.pod(gpu.mem_total);
		w.pod(gpu.mem_used);
		write_deque(w, gpu.mem_utilization_percent);
		w.pod(gpu.mem_clock_speed);
		w.pod(gpu.pcie_tx);
		w.pod(gpu.pcie_rx);
		w.pod(gpu.encoder_utilization);
		w.pod(gpu.decoder_utilization);
		write_gpu_supported(w, gpu.supported_functions);
	}

	static auto read_gpu_info(ipc_reader& r) -> Gpu::gpu_info {
		Gpu::gpu_info gpu;
		gpu.gpu_percent = read_map<deque<long long>>(r, [&] { return read_deque(r); });
		gpu.gpu_clock_speed = r.pod<unsigned int>();
		gpu.pwr_usage = r.pod<long long>();
		gpu.pwr_max_usage = r.pod<long long>();
		gpu.pwr_state = r.pod<long long>();
		gpu.temp = read_deque(r);
		gpu.temp_max = r.pod<long long>();
		gpu.mem_total = r.pod<long long>();
		gpu.mem_used = r.pod<long long>();
		gpu.mem_utilization_percent = read_deque(r);
		gpu.mem_clock_speed = r.pod<long long>();
		gpu.pcie_tx = r.pod<long long>();
		gpu.pcie_rx = r.pod<long long>();
		gpu.encoder_utilization = r.pod<long long>();
		gpu.decoder_utilization = r.pod<long long>();
		gpu.supported_functions = read_gpu_supported(r);
		return gpu;
	}

	static void write_gpu_vector(ipc_writer& w, const vector<Gpu::gpu_info>& gpus) {
		w.pod<uint64_t>(gpus.size());
		for (const auto& gpu : gpus) write_gpu_info(w, gpu);
	}

	static auto read_gpu_vector(ipc_reader& r) -> vector<Gpu::gpu_info> {
		vector<Gpu::gpu_info> gpus;
		const auto size = r.pod<uint64_t>();
		gpus.reserve(size);
		for (uint64_t i = 0; i < size; ++i) gpus.push_back(read_gpu_info(r));
		return gpus;
	}
#endif

	static void write_mem_info(ipc_writer& w, const Mem::mem_info& mem) {
		write_map<uint64_t>(w, mem.stats, [&](const auto value) { w.pod(value); });
		write_map<deque<long long>>(w, mem.percent, [&](const auto& value) { write_deque(w, value); });
		write_map<Mem::disk_info>(w, mem.disks, [&](const auto& value) { write_disk_info(w, value); });
		write_string_vector(w, mem.disks_order);
	}

	static auto read_mem_info(ipc_reader& r) -> Mem::mem_info {
		Mem::mem_info mem;
		mem.stats = read_map<uint64_t>(r, [&] { return r.pod<uint64_t>(); });
		mem.percent = read_map<deque<long long>>(r, [&] { return read_deque(r); });
		mem.disks = read_map<Mem::disk_info>(r, [&] { return read_disk_info(r); });
		mem.disks_order = read_string_vector(r);
		return mem;
	}

	static void write_net_stat(ipc_writer& w, const Net::net_stat& stat) {
		w.pod(stat.speed);
		w.pod(stat.top);
		w.pod(stat.total);
		w.pod(stat.last);
		w.pod(stat.offset);
		w.pod(stat.rollover);
	}

	static auto read_net_stat(ipc_reader& r) -> Net::net_stat {
		Net::net_stat stat;
		stat.speed = r.pod<uint64_t>();
		stat.top = r.pod<uint64_t>();
		stat.total = r.pod<uint64_t>();
		stat.last = r.pod<uint64_t>();
		stat.offset = r.pod<uint64_t>();
		stat.rollover = r.pod<uint64_t>();
		return stat;
	}

	static void write_net_info(ipc_writer& w, const Net::net_info& net) {
		write_map<deque<long long>>(w, net.bandwidth, [&](const auto& value) { write_deque(w, value); });
		write_map<Net::net_stat>(w, net.stat, [&](const auto& value) { write_net_stat(w, value); });
		w.str(net.ipv4);
		w.str(net.ipv6);
		w.boolean(net.connected);
	}

	static auto read_net_info(ipc_reader& r) -> Net::net_info {
		Net::net_info net;
		net.bandwidth = read_map<deque<long long>>(r, [&] { return read_deque(r); });
		net.stat = read_map<Net::net_stat>(r, [&] { return read_net_stat(r); });
		net.ipv4 = r.str();
		net.ipv6 = r.str();
		net.connected = r.boolean();
		return net;
	}

	static void write_proc_info(ipc_writer& w, const Proc::proc_info& proc) {
		w.pod(proc.pid);
		w.str(proc.name);
		w.str(proc.cmd);
		w.str(proc.short_cmd);
		w.pod(proc.threads);
		w.pod(proc.name_offset);
		w.str(proc.user);
		w.pod(proc.mem);
		w.pod(proc.cpu_p);
		w.pod(proc.cpu_c);
		w.pod(proc.state);
		w.pod(proc.p_nice);
		w.pod(proc.ppid);
		w.pod(proc.cpu_s);
		w.pod(proc.cpu_t);
		w.pod(proc.death_time);
		w.str(proc.prefix);
		w.pod(proc.depth);
		w.pod(proc.tree_index);
		w.boolean(proc.collapsed);
		w.boolean(proc.filtered);
	}

	static auto read_proc_info(ipc_reader& r) -> Proc::proc_info {
		Proc::proc_info proc;
		proc.pid = r.pod<size_t>();
		proc.name = r.str();
		proc.cmd = r.str();
		proc.short_cmd = r.str();
		proc.threads = r.pod<size_t>();
		proc.name_offset = r.pod<int>();
		proc.user = r.str();
		proc.mem = r.pod<uint64_t>();
		proc.cpu_p = r.pod<double>();
		proc.cpu_c = r.pod<double>();
		proc.state = r.pod<char>();
		proc.p_nice = r.pod<int64_t>();
		proc.ppid = r.pod<uint64_t>();
		proc.cpu_s = r.pod<uint64_t>();
		proc.cpu_t = r.pod<uint64_t>();
		proc.death_time = r.pod<uint64_t>();
		proc.prefix = r.str();
		proc.depth = r.pod<size_t>();
		proc.tree_index = r.pod<size_t>();
		proc.collapsed = r.boolean();
		proc.filtered = r.boolean();
		return proc;
	}

	static void write_proc_vector(ipc_writer& w, const vector<Proc::proc_info>& procs) {
		w.pod<uint64_t>(procs.size());
		for (const auto& proc : procs) write_proc_info(w, proc);
	}

	static auto read_proc_vector(ipc_reader& r) -> vector<Proc::proc_info> {
		vector<Proc::proc_info> procs;
		const auto size = r.pod<uint64_t>();
		procs.reserve(size);
		for (uint64_t i = 0; i < size; ++i) procs.push_back(read_proc_info(r));
		return procs;
	}

	static void write_sample(ipc_writer& w, const collector_sample& sample) {
		w.boolean(sample.cpu.has_value());
		if (sample.cpu.has_value()) write_cpu_info(w, *sample.cpu);
#if defined(GPU_SUPPORT)
		w.boolean(sample.gpu.has_value());
		if (sample.gpu.has_value()) {
			write_gpu_vector(w, *sample.gpu);
			write_map<deque<long long>>(w, sample.shared_gpu_percent, [&](const auto& value) { write_deque(w, value); });
		}
#endif
		w.boolean(sample.cpu_redraw);
		w.boolean(sample.cpu_topology_reset);
		w.boolean(sample.mem.has_value());
		if (sample.mem.has_value()) write_mem_info(w, *sample.mem);
		w.boolean(sample.mem_redraw);
		w.boolean(sample.net.has_value());
		if (sample.net.has_value()) {
			w.str(sample.net_iface);
			write_string_vector(w, sample.net_interfaces);
			write_map<Net::net_info>(w, sample.net_current, [&](const auto& value) { write_net_info(w, value); });
			write_net_info(w, *sample.net);
			write_map<uint64_t>(w, sample.net_graph_max, [&](const auto& value) { w.pod(value); });
			write_map<array<int, 2>>(w, sample.net_max_count, [&](const auto& value) { w.pod(value); });
			w.pod(sample.net_timestamp);
		}
		w.boolean(sample.proc.has_value());
		if (sample.proc.has_value()) write_proc_vector(w, *sample.proc);
	}

	static auto read_sample(ipc_reader& r) -> collector_sample {
		collector_sample sample;
		if (r.boolean()) sample.cpu = read_cpu_info(r);
#if defined(GPU_SUPPORT)
		if (r.boolean()) {
			sample.gpu = read_gpu_vector(r);
			sample.shared_gpu_percent = read_map<deque<long long>>(r, [&] { return read_deque(r); });
		}
#endif
		sample.cpu_redraw = r.boolean();
		sample.cpu_topology_reset = r.boolean();
		if (r.boolean()) sample.mem = read_mem_info(r);
		sample.mem_redraw = r.boolean();
		if (r.boolean()) {
			sample.net_iface = r.str();
			sample.net_interfaces = read_string_vector(r);
			sample.net_current = read_map<Net::net_info>(r, [&] { return read_net_info(r); });
			sample.net = read_net_info(r);
			sample.net_graph_max = read_map<uint64_t>(r, [&] { return r.pod<uint64_t>(); });
			sample.net_max_count = read_map<array<int, 2>>(r, [&] { return r.pod<array<int, 2>>(); });
			sample.net_timestamp = r.pod<uint64_t>();
		}
		if (r.boolean()) sample.proc = read_proc_vector(r);
		return sample;
	}

	static auto framed_payload(const string& payload) -> string {
		string framed;
		const uint64_t size = payload.size();
		framed.append(reinterpret_cast<const char*>(&size), sizeof(size));
		framed.append(payload);
		return framed;
	}

	static auto parse_framed_payload(const string& framed) -> string {
		if (framed.size() < sizeof(uint64_t)) throw std::runtime_error("short ipc frame");
		uint64_t size = 0;
		std::memcpy(&size, framed.data(), sizeof(size));
		if (size > framed.size() - sizeof(uint64_t)) throw std::runtime_error("truncated ipc frame");
		return framed.substr(sizeof(uint64_t), size);
	}

#ifdef BTOP_TESTING
	namespace Test {
		bool ipc_sample_roundtrip_ok() {
			collector_sample sample;
			sample.cpu = Cpu::cpu_info{};
			sample.cpu->cpu_percent.at("total") = {11, 22, 33};
			sample.cpu->core_percent = {{44, 55}};
			sample.cpu->temp = {{66, 77}};
			sample.cpu->temp_max = 88;
			sample.cpu->load_avg = {1.0, 2.0, 3.0};
			sample.cpu->usage_watts = 12.5;
			sample.cpu->active_cpus = std::vector<std::int32_t>{0, 2};
			sample.cpu_redraw = true;
			sample.cpu_topology_reset = true;
			sample.mem = Mem::mem_info{};
			sample.mem->stats["used"] = 1234;
			sample.mem->percent["used"] = {10, 20};
			Mem::disk_info disk;
			disk.dev = "/dev/disk1";
			disk.name = "disk1";
			disk.fstype = "apfs";
			disk.stat = "/tmp/stat";
			disk.total = 1000;
			disk.used = 400;
			disk.free = 600;
			disk.used_percent = 40;
			disk.free_percent = 60;
			disk.old_io = {1, 2, 3};
			disk.io_read = {4, 5};
			disk.io_write = {6, 7};
			disk.io_activity = {8, 9};
			sample.mem->disks.emplace("disk1", disk);
			sample.mem->disks_order = {"disk1"};
			sample.mem_redraw = true;
			sample.net = Net::net_info{};
			sample.net->bandwidth.at("download") = {111, 222};
			sample.net->stat.at("download").speed = 333;
			sample.net->ipv4 = "127.0.0.1";
			sample.net->ipv6 = "::1";
			sample.net->connected = true;
			sample.net_iface = "lo0";
			sample.net_interfaces = {"en0", "lo0"};
			sample.net_current = {{"lo0", *sample.net}};
			Net::net_info en0;
			en0.bandwidth.at("download") = {333, 444};
			en0.stat.at("download").speed = 555;
			en0.ipv4 = "192.0.2.1";
			en0.connected = true;
			sample.net_current.emplace("en0", en0);
			sample.net_graph_max = {{"download", 444}, {"upload", 555}};
			sample.net_max_count = {{"download", {1, 2}}, {"upload", {3, 4}}};
			sample.net_timestamp = 666;
#if defined(GPU_SUPPORT)
			sample.gpu = vector<Gpu::gpu_info>{};
			Gpu::gpu_info gpu;
			gpu.gpu_percent.at("gpu-totals") = {12, 34};
			gpu.gpu_percent.at("gpu-vram-totals") = {56, 78};
			gpu.gpu_percent.at("gpu-pwr-totals") = {90, 91};
			gpu.gpu_clock_speed = 123;
			gpu.pwr_usage = 456;
			gpu.pwr_max_usage = 789;
			gpu.pwr_state = 2;
			gpu.temp = {45, 46};
			gpu.temp_max = 100;
			gpu.mem_total = 123456;
			gpu.mem_used = 65432;
			gpu.mem_utilization_percent = {23, 24};
			gpu.mem_clock_speed = 321;
			gpu.pcie_tx = 11;
			gpu.pcie_rx = 22;
			gpu.encoder_utilization = 33;
			gpu.decoder_utilization = 44;
			gpu.supported_functions.decoder_utilization = false;
			sample.gpu->push_back(gpu);
			sample.shared_gpu_percent = {{"gpu-totals", {1, 2}}, {"gpu-vram-totals", {3, 4}}, {"gpu-pwr-totals", {5, 6}}};
#endif
			sample.proc = vector<Proc::proc_info>{};
			Proc::proc_info proc;
			proc.pid = 42;
			proc.name = "proc";
			proc.cmd = "proc --flag";
			proc.short_cmd = "proc";
			proc.threads = 3;
			proc.user = "user";
			proc.mem = 777;
			proc.cpu_p = 1.5;
			proc.cpu_c = 2.5;
			proc.state = 'R';
			proc.prefix = "|-";
			proc.collapsed = true;
			proc.filtered = true;
			sample.proc->push_back(proc);

			ipc_writer writer;
			write_sample(writer, sample);
			ipc_reader reader{writer.buf};
			const auto actual = read_sample(reader);

			return actual.cpu.has_value()
				and actual.cpu->cpu_percent.at("total") == sample.cpu->cpu_percent.at("total")
				and actual.cpu->core_percent == sample.cpu->core_percent
				and actual.cpu->temp == sample.cpu->temp
				and actual.cpu->active_cpus == sample.cpu->active_cpus
				and actual.cpu_redraw == sample.cpu_redraw
				and actual.cpu_topology_reset == sample.cpu_topology_reset
				and actual.mem.has_value()
				and actual.mem->stats.at("used") == sample.mem->stats.at("used")
				and actual.mem->percent.at("used") == sample.mem->percent.at("used")
				and actual.mem->disks.at("disk1").io_activity == disk.io_activity
				and actual.mem->disks_order == sample.mem->disks_order
				and actual.mem_redraw == sample.mem_redraw
				and actual.net.has_value()
				and actual.net->bandwidth.at("download") == sample.net->bandwidth.at("download")
				and actual.net->stat.at("download").speed == sample.net->stat.at("download").speed
				and actual.net->ipv4 == sample.net->ipv4
				and actual.net->ipv6 == sample.net->ipv6
				and actual.net->connected == sample.net->connected
				and actual.net_iface == sample.net_iface
				and actual.net_interfaces == sample.net_interfaces
				and actual.net_current.at("lo0").bandwidth.at("download") == sample.net_current.at("lo0").bandwidth.at("download")
				and actual.net_current.at("en0").stat.at("download").speed == sample.net_current.at("en0").stat.at("download").speed
				and actual.net_current.at("en0").ipv4 == sample.net_current.at("en0").ipv4
				and actual.net_graph_max == sample.net_graph_max
				and actual.net_max_count == sample.net_max_count
				and actual.net_timestamp == sample.net_timestamp
#if defined(GPU_SUPPORT)
				and actual.gpu.has_value()
				and actual.gpu->size() == 1
				and actual.gpu->front().gpu_percent.at("gpu-totals") == sample.gpu->front().gpu_percent.at("gpu-totals")
				and actual.gpu->front().temp == sample.gpu->front().temp
				and actual.gpu->front().mem_utilization_percent == sample.gpu->front().mem_utilization_percent
				and actual.gpu->front().decoder_utilization == sample.gpu->front().decoder_utilization
				and actual.gpu->front().supported_functions.decoder_utilization == sample.gpu->front().supported_functions.decoder_utilization
				and actual.shared_gpu_percent == sample.shared_gpu_percent
#endif
				and actual.proc.has_value()
				and actual.proc->size() == 1
				and actual.proc->front().pid == proc.pid
				and actual.proc->front().cmd == proc.cmd
				and actual.proc->front().collapsed == proc.collapsed
				and actual.proc->front().filtered == proc.filtered;
		}

		bool ipc_frame_errors_ok() {
			try { parse_framed_payload("short"); return false; }
			catch (const std::runtime_error&) {}
			string truncated;
			const uint64_t size = 10;
			truncated.append(reinterpret_cast<const char*>(&size), sizeof(size));
			truncated.append("abc");
			try { parse_framed_payload(truncated); return false; }
			catch (const std::runtime_error&) {}
			return parse_framed_payload(framed_payload("payload")) == "payload";
		}
	}
#endif

	static void debug_timer(const char* name, const int action) {
		switch (action) {
			case collect_begin:
				debug_times[name].at(collect) = time_micros();
				return;
			case collect_done:
				debug_times[name].at(collect) = time_micros() - debug_times[name].at(collect);
				debug_times["total"].at(collect) += debug_times[name].at(collect);
				return;
			case draw_begin_only:
				debug_times[name].at(draw) = time_micros();
				return;
			case draw_begin:
				debug_times[name].at(draw) = time_micros();
				debug_times[name].at(collect) = debug_times[name].at(draw) - debug_times[name].at(collect);
				debug_times["total"].at(collect) += debug_times[name].at(collect);
				return;
			case draw_done:
				debug_times[name].at(draw) = time_micros() - debug_times[name].at(draw);
				debug_times["total"].at(draw) += debug_times[name].at(draw);
				return;
		}
	}

	static auto resolve_path_from_env(const std::string_view argv0) -> string {
		const char* path_env = std::getenv("PATH");
		if (path_env == nullptr) return {};

		for (const auto& entry : ssplit(path_env, ':')) {
			if (entry.empty()) continue;
			fs::path dir{entry};
			if (not dir.is_absolute()) continue;
			std::error_code ec;
			const auto candidate = fs::canonical(dir / argv0, ec);
			if (ec or not candidate.is_absolute() or not fs::is_regular_file(candidate, ec)) continue;
			if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
		}
		return {};
	}

	static auto resolve_executable_path(const std::string_view argv0) -> string {
		std::error_code ec;
#if defined(__linux__)
		const auto self_path = fs::read_symlink("/proc/self/exe", ec);
		if (not ec and self_path.is_absolute()) return self_path.string();
#elif defined(__APPLE__)
		char buf[PATH_MAX];
		uint32_t bufsize = PATH_MAX;
		if (_NSGetExecutablePath(buf, &bufsize) == 0) {
			const auto self_path = fs::absolute(fs::path{buf}, ec);
			if (not ec and self_path.is_absolute()) return self_path.string();
		}
#elif defined(__NetBSD__)
		int mib[4] {CTL_KERN, KERN_PROC_ARGS, getpid(), KERN_PROC_PATHNAME};
		char buf[PATH_MAX];
		size_t bufsize = sizeof buf;
		if (sysctl(mib, 4, buf, &bufsize, NULL, 0) == 0) {
			const auto self_path = fs::absolute(fs::path{buf}, ec);
			if (not ec and self_path.is_absolute()) return self_path.string();
		}
#endif
		const fs::path path {argv0};
		if (path.has_parent_path()) {
			const auto absolute = fs::absolute(path, ec);
			if (not ec and absolute.is_absolute()) return absolute.string();
		}

		// Resolve bare argv0 in the parent so helper re-exec can keep using execv() instead of child-side PATH search.
		return resolve_path_from_env(argv0);
	}

	static auto select_parent_net_iface(const collector_sample& update) -> string {
		if (update.net_interfaces.empty()) return {};
		if (not Net::selected_iface.empty() and v_contains(update.net_interfaces, Net::selected_iface)) return Net::selected_iface;
		auto& config_iface = Config::getS("net_iface");
		if (not config_iface.empty() and v_contains(update.net_interfaces, config_iface)) return config_iface;

		auto sorted_interfaces = update.net_interfaces;
		std::ranges::sort(sorted_interfaces, [&](const auto& a, const auto& b) {
			const auto a_total = update.net_current.contains(a) ? update.net_current.at(a).stat.at("download").total + update.net_current.at(a).stat.at("upload").total : 0;
			const auto b_total = update.net_current.contains(b) ? update.net_current.at(b).stat.at("download").total + update.net_current.at(b).stat.at("upload").total : 0;
			return a_total > b_total;
		});
		for (const auto& iface : sorted_interfaces) {
			if (update.net_current.contains(iface) and update.net_current.at(iface).connected) return iface;
		}
		return sorted_interfaces.front();
	}

	static void seed_collectors(const collector_sample& sample) {
		if (sample.cpu.has_value()) Cpu::current_cpu = *sample.cpu;
#if defined(GPU_SUPPORT)
		if (sample.gpu.has_value()) Gpu::gpus = *sample.gpu;
		if (not sample.shared_gpu_percent.empty()) Gpu::shared_gpu_percent = sample.shared_gpu_percent;
#endif
		if (sample.cpu_redraw) Cpu::redraw = true;
		if (sample.cpu_topology_reset) {
			Cpu::core_mapping = Cpu::get_core_mapping();
			Global::resized = true;
			Input::interrupt();
		}
		if (sample.mem.has_value()) Mem::current_mem = *sample.mem;
		if (sample.mem_redraw) Mem::redraw = true;
		if (sample.net.has_value()) {
			Net::interfaces = sample.net_interfaces;
			Net::current_net = sample.net_current;
			if (not sample.net_iface.empty()) Net::selected_iface = sample.net_iface;
			if (sample.net.has_value() and not sample.net_iface.empty() and not Net::current_net.contains(sample.net_iface)) Net::current_net[sample.net_iface] = *sample.net;
			Net::graph_max = sample.net_graph_max;
			Net::max_count = sample.net_max_count;
			Net::timestamp = sample.net_timestamp;
		}
		if (sample.proc.has_value()) Proc::current_procs = *sample.proc;
	}

	static void collect_and_draw(const runner_conf& conf, const collector_sample& sample, string& rendered) {
		try {
			seed_collectors(sample);
			if (v_contains(conf.boxes, "cpu") and sample.cpu.has_value()) {
				try {
					if (Global::debug) debug_timer("cpu", draw_begin);
					if (not pause_output) rendered += Cpu::draw(*sample.cpu,
#if defined(GPU_SUPPORT)
						sample.gpu.value_or(vector<Gpu::gpu_info>{}),
#endif
						conf.force_redraw, conf.no_update);
					if (Global::debug) debug_timer("cpu", draw_done);
				}
				catch (const std::exception& e) { throw std::runtime_error("Cpu:: -> " + string{e.what()}); }
			}
#if defined(GPU_SUPPORT)
			if (sample.gpu.has_value()) {
				vector<unsigned int> gpu_panels;
				for (const auto& box : conf.boxes)
					if (box.starts_with("gpu")) gpu_panels.push_back(box.back() - '0');
				if (not gpu_panels.empty()) {
					try {
						if (Global::debug) debug_timer("gpu", draw_begin_only);
						if (not pause_output)
							for (unsigned long i = 0; i < gpu_panels.size(); ++i) {
								if (gpu_panels[i] >= sample.gpu->size()) continue;
								rendered += Gpu::draw(sample.gpu->at(gpu_panels[i]), i, conf.force_redraw, conf.no_update);
							}
						if (Global::debug) debug_timer("gpu", draw_done);
					}
					catch (const std::exception& e) { throw std::runtime_error("Gpu:: -> " + string{e.what()}); }
				}
			}
#endif
			if (v_contains(conf.boxes, "mem") and sample.mem.has_value()) {
				try { if (Global::debug) debug_timer("mem", draw_begin); if (not pause_output) rendered += Mem::draw(*sample.mem, conf.force_redraw, conf.no_update); if (Global::debug) debug_timer("mem", draw_done); }
				catch (const std::exception& e) { throw std::runtime_error("Mem:: -> " + string{e.what()}); }
			}
			if (v_contains(conf.boxes, "net") and sample.net.has_value()) {
				try {
					if (Global::debug) debug_timer("net", draw_begin);
					if (not pause_output) {
						const auto& net = (not Net::selected_iface.empty() and sample.net_current.contains(Net::selected_iface) ? sample.net_current.at(Net::selected_iface) : *sample.net);
						rendered += Net::draw(net, conf.force_redraw, conf.no_update);
					}
					if (Global::debug) debug_timer("net", draw_done);
				}
				catch (const std::exception& e) { throw std::runtime_error("Net:: -> " + string{e.what()}); }
			}
			if (v_contains(conf.boxes, "proc") and sample.proc.has_value()) {
				try { if (Global::debug) debug_timer("proc", draw_begin); if (not pause_output) rendered += Proc::draw(*sample.proc, conf.force_redraw, conf.no_update); if (Global::debug) debug_timer("proc", draw_done); }
				catch (const std::exception& e) { throw std::runtime_error("Proc:: -> " + string{e.what()}); }
			}
		}
		catch (const std::exception& e) { throw std::runtime_error(fmt::format("Exception in runner thread -> {}", e.what())); }
	}

	static auto collect_sample(const runner_conf& conf) -> collector_sample {
		collector_sample sample;
		try {
			if (v_contains(conf.boxes, "cpu")) {
				try {
					if (Global::debug) debug_timer("cpu", collect_begin);
					sample.cpu = Cpu::collect(conf.no_update);
					sample.cpu_redraw = Cpu::redraw;
					if (coreNum_reset) { coreNum_reset = false; sample.cpu_topology_reset = true; return sample; }
					if (Global::debug) debug_timer("cpu", collect_done);
				}
				catch (const std::exception& e) { throw std::runtime_error("Cpu:: -> " + string{e.what()}); }
			}
#if defined(GPU_SUPPORT)
			const bool gpu_in_cpu_panel = Gpu::gpu_names.size() > 0 and (
				Config::getS("cpu_graph_lower").starts_with("gpu-")
				or (Config::getS("cpu_graph_lower") == "Auto")
				or Config::getS("cpu_graph_upper").starts_with("gpu-")
				or (Gpu::shown == 0 and Config::getS("show_gpu_info") != "Off")
			);
			const bool gpu_panel_shown = std::any_of(conf.boxes.begin(), conf.boxes.end(), [](const string& box) { return box.starts_with("gpu"); });
			if (gpu_in_cpu_panel or gpu_panel_shown) {
				try {
					if (Global::debug) debug_timer("gpu", collect_begin);
					sample.gpu = Gpu::collect(conf.no_update);
					sample.shared_gpu_percent = Gpu::shared_gpu_percent;
					if (Global::debug) debug_timer("gpu", collect_done);
				}
				catch (const std::exception& e) { throw std::runtime_error("Gpu:: -> " + string{e.what()}); }
			}
#endif
			if (v_contains(conf.boxes, "mem")) {
				try { if (Global::debug) debug_timer("mem", collect_begin); sample.mem = Mem::collect(conf.no_update); sample.mem_redraw = Mem::redraw; if (Global::debug) debug_timer("mem", collect_done); }
				catch (const std::exception& e) { throw std::runtime_error("Mem:: -> " + string{e.what()}); }
			}
			if (v_contains(conf.boxes, "net")) {
				try { if (Global::debug) debug_timer("net", collect_begin); sample.net = Net::collect(false); sample.net_iface = Net::selected_iface; sample.net_interfaces = Net::interfaces; sample.net_current = Net::current_net; sample.net_graph_max = Net::graph_max; sample.net_max_count = Net::max_count; sample.net_timestamp = Net::timestamp; if (Global::debug) debug_timer("net", collect_done); }
				catch (const std::exception& e) { throw std::runtime_error("Net:: -> " + string{e.what()}); }
			}
			if (v_contains(conf.boxes, "proc")) {
				try { if (Global::debug) debug_timer("proc", collect_begin); sample.proc = Proc::collect(conf.no_update); if (Global::debug) debug_timer("proc", collect_done); }
				catch (const std::exception& e) { throw std::runtime_error("Proc:: -> " + string{e.what()}); }
			}
		}
		catch (const std::exception& e) { throw std::runtime_error(fmt::format("Exception in collector helper -> {}", e.what())); }
		return sample;
	}

	static auto collect_and_serialize(const runner_conf& conf, std::string_view previous_state) -> string {
		ipc_reader reader{previous_state};
		seed_collectors(read_sample(reader));
		ipc_writer writer;
		write_sample(writer, collect_sample(conf));
		return writer.buf;
	}

	static void merge_sample(collector_sample& sample, collector_sample update) {
		if (update.cpu.has_value()) sample.cpu = std::move(update.cpu);
#if defined(GPU_SUPPORT)
		if (update.gpu.has_value()) {
			sample.gpu = std::move(update.gpu);
			sample.shared_gpu_percent = std::move(update.shared_gpu_percent);
		}
#endif
		sample.cpu_redraw = update.cpu_redraw;
		sample.cpu_topology_reset = update.cpu_topology_reset;
		if (update.mem.has_value()) sample.mem = std::move(update.mem);
		sample.mem_redraw = update.mem_redraw;
		if (update.net.has_value()) {
			const auto net_iface = select_parent_net_iface(update);
			sample.net_interfaces = std::move(update.net_interfaces);
			sample.net_current = std::move(update.net_current);
			sample.net_graph_max = std::move(update.net_graph_max);
			sample.net_max_count = std::move(update.net_max_count);
			sample.net_timestamp = update.net_timestamp;
			sample.net_iface = net_iface;
			if (not sample.net_iface.empty() and sample.net_current.contains(sample.net_iface)) sample.net = sample.net_current.at(sample.net_iface);
			else sample.net = std::move(update.net);
			Net::selected_iface = sample.net_iface;
			Net::interfaces = sample.net_interfaces;
			Net::current_net = sample.net_current;
		}
		if (update.proc.has_value()) sample.proc = std::move(update.proc);
	}

	static auto draw_sample(const runner_conf& conf, const collector_sample& sample) -> string {
		string rendered;
		collect_and_draw(conf, sample, rendered);
		return rendered;
	}

	static auto helper_token() -> string {
		std::random_device rd;
		uint64_t values[2] { (static_cast<uint64_t>(rd()) << 32) ^ rd(), (static_cast<uint64_t>(rd()) << 32) ^ rd() };
		return fmt::format("{:016x}{:016x}", values[0], values[1]);
	}

	static auto run_collector_subprocess(const runner_conf& conf, collector_sample& sample) -> bool {
		if (Global::executable_path.empty()) return false;
		int in_pipe[2] {-1, -1}; int out_pipe[2] {-1, -1}; int err_pipe[2] {-1, -1}; int auth_pipe[2] {-1, -1};
		if (pipe(in_pipe) != 0) return false;
		if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return false; }
		if (pipe(err_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]); return false; }
		if (pipe(auth_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]); return false; }
		auto close_fd = [](int& fd) { if (fd >= 0) { close(fd); fd = -1; } };
		auto set_nonblock = [](int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0; };
		ipc_writer request_writer;
		if (v_contains(conf.boxes, "net")) {
			sample.net_iface = Net::selected_iface;
			sample.net_interfaces = Net::interfaces;
			sample.net_current = Net::current_net;
		}
		write_sample(request_writer, sample);
		const auto request = framed_payload(request_writer.buf);
		string width = to_string(Term::width.load()); string height = to_string(Term::height.load()); string boxes;
		for (const auto& box : conf.boxes) boxes += (boxes.empty() ? "" : " ") + box;
		string no_update = conf.no_update ? "1" : "0"; string force_redraw = conf.force_redraw ? "1" : "0"; string config = Config::conf_file.string(); string token = helper_token(); string token_fd = to_string(auth_pipe[0]);
		const char* argv[] = {Global::executable_path.c_str(), "--btop-collector-helper", "--token-fd", token_fd.c_str(), "--token", token.c_str(), "--width", width.c_str(), "--height", height.c_str(), "--boxes", boxes.c_str(), "--no-update", no_update.c_str(), "--force-redraw", force_redraw.c_str(), "--config", config.c_str(), nullptr};
		const pid_t pid = fork();
		if (pid == -1) { close_fd(in_pipe[0]); close_fd(in_pipe[1]); close_fd(out_pipe[0]); close_fd(out_pipe[1]); close_fd(err_pipe[0]); close_fd(err_pipe[1]); close_fd(auth_pipe[0]); close_fd(auth_pipe[1]); return false; }
		if (pid == 0) {
			if (Global::real_uid != Global::set_uid) seteuid(Global::real_uid);
			dup2(in_pipe[0], STDIN_FILENO); dup2(out_pipe[1], STDOUT_FILENO); dup2(err_pipe[1], STDERR_FILENO);
			close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]); close(auth_pipe[1]);
			execv(Global::executable_path.c_str(), const_cast<char* const*>(argv));
			_exit(127);
		}
		close_fd(in_pipe[0]); close_fd(out_pipe[1]); close_fd(err_pipe[1]); close_fd(auth_pipe[0]); set_nonblock(in_pipe[1]); set_nonblock(out_pipe[0]); set_nonblock(err_pipe[0]);
		if (write(auth_pipe[1], token.data(), token.size()) != static_cast<ssize_t>(token.size())) { close_fd(in_pipe[1]); close_fd(out_pipe[0]); close_fd(err_pipe[0]); close_fd(auth_pipe[1]); return false; }
		close_fd(auth_pipe[1]);
		string child_out, child_err; int status = 0; bool exited = false, timed_out = false, saw_response = false, response_stalled = false, shutting_down = false;
		size_t written = 0;
		const auto collect_deadline = std::chrono::steady_clock::now() + 5s;
		auto response_deadline = collect_deadline;
		while (true) {
			if (Global::quitting or stopping) {
				shutting_down = true;
				break;
			}
			struct pollfd fds[3] {{in_pipe[1], static_cast<short>(written < request.size() ? POLLOUT : 0), 0}, {out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
			const auto now = std::chrono::steady_clock::now();
			const auto deadline = saw_response ? response_deadline : collect_deadline;
			if (now >= deadline) {
				if (saw_response) response_stalled = true;
				else timed_out = true;
				break;
			}
			const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
			const int timeout_ms = static_cast<int>(std::min<int64_t>(wait_ms, 100));
			if (poll(fds, 3, timeout_ms) < 0 and errno != EINTR) break;

			if (in_pipe[1] >= 0 and (fds[0].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
				while (written < request.size()) {
					const ssize_t n = write(in_pipe[1], request.data() + written, request.size() - written);
					if (n > 0) written += static_cast<size_t>(n);
					else if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) break;
					else { close_fd(in_pipe[1]); break; }
				}
				if (written == request.size()) close_fd(in_pipe[1]);
			}

			char buf[4096];
			for (size_t i = 1; i < 3; ++i) {
				auto& fd = fds[i];
				while (fd.fd >= 0 and (fd.revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
					const ssize_t n = read(fd.fd, buf, sizeof(buf));
					if (n > 0) {
						if (fd.fd == out_pipe[0]) {
							child_out.append(buf, n);
							saw_response = true;
							response_deadline = std::chrono::steady_clock::now() + 1s;
						}
						else {
							child_err.append(buf, n);
							if (saw_response) response_deadline = std::chrono::steady_clock::now() + 1s;
						}
					}
					else if (n == 0) { if (fd.fd == out_pipe[0]) close_fd(out_pipe[0]); else close_fd(err_pipe[0]); break; }
					else if (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR) break;
					else { if (fd.fd == out_pipe[0]) close_fd(out_pipe[0]); else close_fd(err_pipe[0]); break; }
				}
			}
			if (not exited and waitpid(pid, &status, WNOHANG) == pid) {
				exited = true;
				if (saw_response) response_deadline = std::chrono::steady_clock::now() + 1s;
			}
			if (exited and out_pipe[0] < 0 and err_pipe[0] < 0) break;
		}
		if (not exited) {
			kill(pid, SIGTERM);
			for (int i = 0; i < 20; ++i) { if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; } sleep_ms(10); }
			if (not exited) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
			if (timed_out) Logger::warning("Collector helper timed out after 5 seconds");
			else if (response_stalled) Logger::warning("Collector helper response stalled");
			else if (not shutting_down) Logger::warning("Collector helper stopped responding");
			close_fd(in_pipe[1]); close_fd(out_pipe[0]); close_fd(err_pipe[0]); return false;
		}
		close_fd(out_pipe[0]); close_fd(err_pipe[0]);
		if (not WIFEXITED(status) or WEXITSTATUS(status) != 0) { Logger::warning("Collector helper failed with status {}{}{}", status, child_err.empty() ? "" : ": ", child_err); return false; }
		try {
			const auto payload = parse_framed_payload(child_out);
			ipc_reader reader{payload};
			merge_sample(sample, read_sample(reader));
		}
		catch (const std::exception& e) {
			Logger::warning("Collector helper returned invalid payload: {}", e.what());
			return false;
		}
		return true;
	}

	//? ------------------------------- Secondary thread: async launcher and drawing ----------------------------------
	static void * _runner(void *) {
		//? Block some signals in this thread to avoid deadlock from any signal handlers trying to stop this thread
		sigemptyset(&mask);
		// sigaddset(&mask, SIGINT);
		// sigaddset(&mask, SIGTSTP);
		sigaddset(&mask, SIGWINCH);
		sigaddset(&mask, SIGTERM);
		pthread_sigmask(SIG_BLOCK, &mask, nullptr);

		// TODO: On first glance it looks redudant with `Runner::active`.
		std::lock_guard lock {mtx};

		//* ----------------------------------------------- THREAD LOOP -----------------------------------------------
		while (not Global::quitting) {
			thread_wait();
			atomic_wait_for(active, true, 5000);
			if (active) {
				Global::exit_error_msg = "Runner thread failed to get active lock!";
				Global::thread_exception = true;
				Input::interrupt();
				stopping = true;
			}
			if (stopping or Global::resized) {
				sleep_ms(1);
				continue;
			}

			//? Atomic lock used for blocking non thread-safe actions in main thread
			atomic_lock lck(active);

			//? Set effective user if SUID bit is set
			gain_priv powers{};

			auto& conf = current_conf;

			//! DEBUG stats
			if (Global::debug) {
                if (debug_bg.empty() or redraw)
                    Runner::debug_bg = Draw::createBox(2, 2, 33,
					#ifdef GPU_SUPPORT
						9,
					#else
						8,
					#endif
					"", true, "μs");

				debug_times.clear();
				debug_times["total"] = {0, 0};
			}

			try {
				if (run_collector_subprocess(conf, current_sample)) {
					output = draw_sample(conf, current_sample);
				}
				else {
					Logger::warning("Collector helper failed; leaving previous output on screen");
					continue;
				}
			}
			catch (const std::exception& e) {
				Global::exit_error_msg = e.what();
				Global::thread_exception = true;
				Input::interrupt();
				stopping = true;
			}

			if (stopping) {
				continue;
			}

			if (redraw or conf.force_redraw) {
				empty_bg.clear();
				redraw = false;
			}

			if (not pause_output) output += conf.clock;
			if (not conf.overlay.empty() and not conf.background_update) pause_output = true;
			if (output.empty() and not pause_output) {
				if (empty_bg.empty()) {
					const int x = Term::width / 2 - 10, y = Term::height / 2 - 10;
					output += Term::clear;
					empty_bg = fmt::format(
						"{banner}"
						"{mv1}{titleFg}{b}No boxes shown!"
						"{mv2}{hiFg}1 {mainFg}| Show CPU box"
						"{mv3}{hiFg}2 {mainFg}| Show MEM box"
						"{mv4}{hiFg}3 {mainFg}| Show NET box"
						"{mv5}{hiFg}4 {mainFg}| Show PROC box"
						"{mv6}{hiFg}5-0 {mainFg}| Show GPU boxes"
						"{mv7}{hiFg}esc {mainFg}| Show menu"
						"{mv8}{hiFg}q {mainFg}| Quit",
						"banner"_a = Draw::banner_gen(y, 0, true),
						"titleFg"_a = Theme::c("title"), "b"_a = Fx::b, "hiFg"_a = Theme::c("hi_fg"), "mainFg"_a = Theme::c("main_fg"),
						"mv1"_a = Mv::to(y+6, x),
						"mv2"_a = Mv::to(y+8, x),
						"mv3"_a = Mv::to(y+9, x),
						"mv4"_a = Mv::to(y+10, x),
						"mv5"_a = Mv::to(y+11, x),
						"mv6"_a = Mv::to(y+12, x-2),
						"mv7"_a = Mv::to(y+13, x-2),
						"mv8"_a = Mv::to(y+14, x)
					);
				}
				output += empty_bg;
			}

			//! DEBUG stats -->
			if (Global::debug and not Menu::active) {
				output += fmt::format("{pre}{box:5.5} {collect:>12.12} {draw:>12.12}{post}",
					"pre"_a = debug_bg + Theme::c("title") + Fx::b,
					"box"_a = "box", "collect"_a = "collect", "draw"_a = "draw",
					"post"_a = Theme::c("main_fg") + Fx::ub
				);
				static auto loc = std::locale(std::locale::classic(), new MyNumPunct);
			#ifdef GPU_SUPPORT
				for (const string name : {"cpu", "mem", "net", "proc", "gpu", "total"}) {
			#else
				for (const string name : {"cpu", "mem", "net", "proc", "total"}) {
			#endif
					if (not debug_times.contains(name)) debug_times[name] = {0,0};
					const auto& [time_collect, time_draw] = debug_times.at(name);
					if (name == "total") output += Fx::b;
					output += fmt::format(loc, "{mvLD}{name:5.5} {collect:12L} {draw:12L}",
						"mvLD"_a = Mv::l(31) + Mv::d(1),
						"name"_a = name,
						"collect"_a = time_collect,
						"draw"_a = time_draw
					);
				}
			}

			//? If overlay isn't empty, print output without color and then print overlay on top
			const bool term_sync = Config::getB("terminal_sync");
			cout << (term_sync ? Term::sync_start : "") << (conf.overlay.empty()
					? output
					: (output.empty() ? "" : Fx::ub + Theme::c("inactive_fg") + Fx::uncolor(output)) + conf.overlay)
				<< (term_sync ? Term::sync_end : "") << flush;
		}
		//* ----------------------------------------------- THREAD LOOP -----------------------------------------------
		return {};
	}
	//? ------------------------------------------ Secondary thread end -----------------------------------------------

	//* Runs collect and draw in a secondary thread, unlocks and locks config to update cached values
	void run(const string& box, bool no_update, bool force_redraw) {
		atomic_wait_for(active, true, 5000);
		if (active) {
			Logger::warning("Runner thread still active after 5 seconds; collector subprocess timeout should recover without restarting the thread");
			return;
		}
		if (stopping or Global::resized) return;

		if (box == "overlay") {
			const bool term_sync = Config::getB("terminal_sync");
			cout << (term_sync ? Term::sync_start : "") << Global::overlay << (term_sync ? Term::sync_end : "") << flush;
		}
		else if (box == "clock") {
			const bool term_sync = Config::getB("terminal_sync");
			cout << (term_sync ? Term::sync_start : "") << Global::clock << (term_sync ? Term::sync_end : "") << flush;
		}
		else {
			Config::unlock();
			Config::lock();

			current_conf = {
				(box == "all" ? Config::current_boxes : vector{box}),
				no_update, force_redraw,
				(not Config::getB("tty_mode") and Config::getB("background_update")),
				Global::overlay,
				Global::clock
			};

			if (Menu::active and not current_conf.background_update) Global::overlay.clear();

			thread_trigger();
			atomic_wait_for(active, false, 10);
		}


	}

	//* Stops any work being done in runner thread and checks for thread errors
	void stop() {
		stopping = true;
		auto lock = std::unique_lock {mtx, std::defer_lock};
		const auto is_runner_busy = !lock.try_lock();
		if (!is_runner_busy and not Global::quitting) {
			if (active) {
				set_active(false);
			}
			Global::exit_error_msg = "Runner thread died unexpectedly!";
			clean_quit(1);
		} else if (is_runner_busy) {
			if (Global::quitting) {
				set_active(false);
				thread_trigger();
				return;
			}
			atomic_wait_for(active, true, 5000);
			if (active) {
				set_active(false);
				if (Global::quitting) {
					return;
				}
				Logger::warning("Runner thread did not stop within 5 seconds; continuing cooperatively");
				thread_trigger();
				stopping = false;
				return;
			}
			thread_trigger();
			atomic_wait_for(active, false, 100);
			atomic_wait_for(active, true, 100);
		}
		stopping = false;
	}

}

static auto configure_tty_mode(std::optional<bool> force_tty) {
	if (force_tty.has_value()) {
		Config::set("tty_mode", force_tty.value());
		Logger::debug("TTY mode set via command line");
	}
	else if (Config::getB("force_tty")) {
		Config::set("tty_mode", true);
		Logger::debug("TTY mode set via config");
	}

#if !defined(__APPLE__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
	else if (Term::current_tty.starts_with("/dev/tty")) {
		Config::set("tty_mode", true);
		Logger::debug("Auto detect real TTY");
	}
#endif

	Logger::debug("TTY mode enabled: {}", Config::getB("tty_mode"));
}


//* --------------------------------------------- Main starts here! ---------------------------------------------------
static auto btop_collector_helper(const std::span<const std::string_view> args) -> int {
	int width = 0, height = 0, token_fd = -1; string boxes; bool no_update = false, force_redraw = false; std::optional<std::filesystem::path> config_file; string token;
	for (size_t i = 1; i + 1 < args.size(); i += 2) {
		if (args[i] == "--token") token = args[i + 1];
		else if (args[i] == "--token-fd") token_fd = std::stoi(string{args[i + 1]});
		else if (args[i] == "--width") width = std::stoi(string{args[i + 1]});
		else if (args[i] == "--height") height = std::stoi(string{args[i + 1]});
		else if (args[i] == "--boxes") boxes = args[i + 1];
		else if (args[i] == "--no-update") no_update = args[i + 1] == "1";
		else if (args[i] == "--force-redraw") force_redraw = args[i + 1] == "1";
		else if (args[i] == "--config") config_file = std::filesystem::path{args[i + 1]};
	}
	string expected_token(token.size(), '\0');
	if (token.empty() or token_fd < 0 or read(token_fd, expected_token.data(), expected_token.size()) != static_cast<ssize_t>(expected_token.size()) or token != expected_token) return 2;
	close(token_fd);
	if (width <= 0 or height <= 0 or boxes.empty()) return 2;
	Global::start_time = time_s();
	Global::real_uid = getuid();
	Global::set_uid = geteuid();
	if (Global::real_uid != Global::set_uid and seteuid(Global::real_uid) != 0) return 5;
	Global::real_uid = getuid();
	if (auto config_dir = Config::get_config_dir(); config_dir.has_value()) { Config::conf_dir = config_dir.value(); Config::conf_file = config_file.value_or(Config::conf_dir / "btop.conf"); if (auto log_file = Config::get_log_file(); log_file.has_value()) Logger::init(log_file.value()); Theme::user_theme_dir = Config::conf_dir / "themes"; }
	std::optional<std::string> filter; init_config(false, filter);
	Term::width = width; Term::height = height; Term::current_tty = "collector-helper";
	try {
		Shared::init(); Config::set("shown_boxes", boxes); if (not Config::set_boxes(boxes)) return 3; Theme::updateThemes(); Theme::setTheme(); Draw::calcSizes();
		string request;
		char buf[4096];
		while (true) {
			const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n > 0) request.append(buf, n);
			else break;
		}
		Runner::runner_conf conf {Config::current_boxes, no_update, force_redraw, false, {}, {}};
		Runner::gain_priv powers{};
		const auto response = Runner::collect_and_serialize(conf, Runner::parse_framed_payload(request));
		const auto framed = Runner::framed_payload(response);
		std::cout.write(framed.data(), static_cast<std::streamsize>(framed.size()));
		std::cout.flush();
	}
	catch (const std::exception& e) { fmt::println(std::cerr, "{}", e.what()); return 4; }
	return 0;
}

[[nodiscard]] auto btop_main(const std::span<const std::string_view> args, const std::string_view argv0) -> int {
	Global::executable_path = Runner::resolve_executable_path(argv0);

	//? ------------------------------------------------ INIT ---------------------------------------------------------

	if (not args.empty() and args.front() == "--btop-collector-helper") return btop_collector_helper(args);

	Global::start_time = time_s();

	//? Save real and effective userid's and drop privileges until needed if running with SUID bit set
	Global::real_uid = getuid();
	Global::set_uid = geteuid();
	if (Global::real_uid != Global::set_uid) {
		if (seteuid(Global::real_uid) != 0) {
			Global::real_uid = Global::set_uid;
			Global::exit_error_msg = "Failed to change effective user ID. Unset btop SUID bit to ensure security on this system. Quitting!";
			clean_quit(1);
		}
	}
	Cli::Cli cli;
	{
		// Get the cli options or return with an exit code
		auto result = Cli::parse(args);
		if (result.has_value()) {
			cli = result.value();
		} else {
			auto error = result.error();
			if (error != 0) {
				Cli::usage();
				Cli::help_hint();
			}
			return error;
		}
	}

	Global::debug = cli.debug;

	{
		const auto config_dir = Config::get_config_dir();
		if (config_dir.has_value()) {
			Config::conf_dir = config_dir.value();
			if (cli.config_file.has_value()) {
				Config::conf_file = cli.config_file.value();
			} else {
				Config::conf_file = Config::conf_dir / "btop.conf";
			}

			auto log_file = Config::get_log_file();
			if (log_file.has_value()) {
				Logger::init(log_file.value());
			}

			Theme::user_theme_dir = Config::conf_dir / "themes";

			// If necessary create the user theme directory
			std::error_code error;
			if (not fs::exists(Theme::user_theme_dir, error) and not fs::create_directories(Theme::user_theme_dir, error)) {
				Theme::user_theme_dir.clear();
				Logger::warning("Failed to create user theme directory: {}", error.message());
			}
		}
	}

	//? Try to find global btop theme path relative to binary path
#ifdef __linux__
	{ 	std::error_code ec;
		Global::self_path = fs::read_symlink("/proc/self/exe", ec).remove_filename();
	}
#elif __APPLE__
	{
		char buf [PATH_MAX];
		uint32_t bufsize = PATH_MAX;
		if(!_NSGetExecutablePath(buf, &bufsize))
			Global::self_path = fs::path(buf).remove_filename();
	}
#elif __NetBSD__
	{
		int mib[4];
		char buf[PATH_MAX];
		size_t bufsize = sizeof buf;

		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC_ARGS;
		mib[2] = getpid();
		mib[3] = KERN_PROC_PATHNAME;
		if (sysctl(mib, 4, buf, &bufsize, NULL, 0) == 0)
			Global::self_path = fs::path(buf).remove_filename();
	}
#endif
	if (std::error_code ec; not Global::self_path.empty()) {
		Theme::theme_dir = fs::canonical(Global::self_path / "../share/btop/themes", ec);
		if (ec or not fs::is_directory(Theme::theme_dir) or access(Theme::theme_dir.c_str(), R_OK) == -1) Theme::theme_dir.clear();
	}
	//? If relative path failed, check two most common absolute paths
	if (Theme::theme_dir.empty()) {
		for (auto theme_path : {"/usr/local/share/btop/themes", "/usr/share/btop/themes"}) {
			if (fs::is_directory(fs::path(theme_path)) and access(theme_path, R_OK) != -1) {
				Theme::theme_dir = fs::path(theme_path);
				break;
			}
		}
	}

	//? Set custom themes directory from command line if provided
	if (cli.themes_dir.has_value()) {
		Theme::custom_theme_dir = cli.themes_dir.value();
		Logger::info("Using custom themes directory: {}", Theme::custom_theme_dir.string());
	}

	//? Config init
	init_config(cli.low_color, cli.filter);

	//? Try to find and set a UTF-8 locale
	if (std::setlocale(LC_ALL, "") != nullptr and not std::string_view { std::setlocale(LC_ALL, "") }.contains(";")
	and str_to_upper(s_replace((string)std::setlocale(LC_ALL, ""), "-", "")).ends_with("UTF8")) {
		Logger::debug("Using locale {}", std::locale().name());
	}
	else {
		string found;
		bool set_failure{};
		for (const auto loc_env : array{"LANG", "LC_ALL", "LC_CTYPE"}) {
			if (std::getenv(loc_env) != nullptr and str_to_upper(s_replace((string)std::getenv(loc_env), "-", "")).ends_with("UTF8")) {
				found = std::getenv(loc_env);
				if (std::setlocale(LC_ALL, found.c_str()) == nullptr) {
					set_failure = true;
					Logger::warning("Failed to set locale {} continuing anyway.", found);
				}
			}
		}
		if (found.empty()) {
			if (setenv("LC_ALL", "", 1) == 0 and setenv("LANG", "", 1) == 0) {
				try {
					if (const auto loc = std::locale("").name(); not loc.empty() and loc != "*") {
						for (auto& l : ssplit(loc, ';')) {
							if (str_to_upper(s_replace(l, "-", "")).ends_with("UTF8")) {
								found = l.substr(l.find('=') + 1);
								if (std::setlocale(LC_ALL, found.c_str()) != nullptr) {
									break;
								}
							}
						}
					}
				}
				catch (...) { found.clear(); }
			}
		}
	//
	#ifdef __APPLE__
		if (found.empty()) {
			CFLocaleRef cflocale = CFLocaleCopyCurrent();
			CFStringRef id_value = (CFStringRef)CFLocaleGetValue(cflocale, kCFLocaleIdentifier);
			auto loc_id = CFStringGetCStringPtr(id_value, kCFStringEncodingUTF8);
			CFRelease(cflocale);
			std::string cur_locale = (loc_id != nullptr ? loc_id : "");
			if (cur_locale.empty()) {
				Logger::warning("No UTF-8 locale detected! Some symbols might not display correctly.");
			}
			else if (std::setlocale(LC_ALL, string(cur_locale + ".UTF-8").c_str()) != nullptr) {
				Logger::debug("Setting LC_ALL={}.UTF-8", cur_locale);
			}
			else if(std::setlocale(LC_ALL, "en_US.UTF-8") != nullptr) {
				Logger::debug("Setting LC_ALL=en_US.UTF-8");
			}
			else {
				Logger::warning("Failed to set macos locale, continuing anyway.");
			}
		}
	#else
		if (found.empty() and cli.force_utf) {
			Logger::warning("No UTF-8 locale detected! Forcing start with --force-utf argument.");
		} else if (found.empty()) {
			Global::exit_error_msg = "No UTF-8 locale detected!\nUse --force-utf argument to force start if you're sure your terminal can handle it.";
			clean_quit(1);
		}
	#endif
		else if (not set_failure) {
			Logger::debug("Setting LC_ALL={}", found);
		}
	}

	//? Initialize terminal and set options
	if (not Term::init()) {
		Global::exit_error_msg = "No tty detected!\nbtop++ needs an interactive shell to run.";
		clean_quit(1);
	}

	if (Term::current_tty != "unknown") {
		Logger::info("Running on {}", Term::current_tty);
	}

	configure_tty_mode(cli.force_tty);

	//? Check for valid terminal dimensions
	{
		int t_count = 0;
		while (Term::width <= 0 or Term::width > 10000 or Term::height <= 0 or Term::height > 10000) {
			sleep_ms(10);
			Term::refresh();
			if (++t_count == 100) {
				Global::exit_error_msg = "Failed to get size of terminal!";
				clean_quit(1);
			}
		}
	}

	//? Platform dependent init and error check
	try {
		Shared::init();
	}
	catch (const std::exception& e) {
		Global::exit_error_msg = fmt::format("Exception in Shared::init() -> {}", e.what());
		clean_quit(1);
	}

	if (not Config::set_boxes(Config::getS("shown_boxes"))) {
		Config::set_boxes("cpu mem net proc");
		Config::set("shown_boxes", "cpu mem net proc"s);
	}

	//? Update list of available themes and generate the selected theme
	Theme::updateThemes();
	Theme::setTheme();

	//? Setup signal handlers for CTRL-C, CTRL-Z, resume and terminal resize
	std::atexit(_exit_handler);
	std::signal(SIGINT, _signal_handler);
	std::signal(SIGTSTP, _signal_handler);
	std::signal(SIGCONT, _signal_handler);
	std::signal(SIGWINCH, _signal_handler);
	std::signal(SIGPIPE, SIG_IGN);
	std::signal(SIGUSR1, _signal_handler);
	std::signal(SIGUSR2, _signal_handler);
	// Add crash handlers to restore terminal on crash
	std::signal(SIGSEGV, _crash_handler);
	std::signal(SIGABRT, _crash_handler);
	std::signal(SIGTRAP, _crash_handler);
	std::signal(SIGBUS, _crash_handler);
	std::signal(SIGILL, _crash_handler);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &mask, &Input::signal_mask);

	if (pthread_create(&Runner::runner_id, nullptr, &Runner::_runner, nullptr) != 0) {
		Global::exit_error_msg = "Failed to create _runner thread!";
		clean_quit(1);
	}
	else {
		Global::_runner_started = true;
	}

	//? Calculate sizes of all boxes
	Config::presetsValid(Config::getS("presets"));
	if (cli.preset.has_value()) {
		Config::current_preset = min(static_cast<std::int32_t>(cli.preset.value()), static_cast<std::int32_t>(Config::preset_list.size() - 1));
		Config::apply_preset(Config::preset_list.at(Config::current_preset.value()));
	}

	{
		const auto [x, y] = Term::get_min_size(Config::getS("shown_boxes"));
		if (Term::height < y or Term::width < x) {
			pthread_sigmask(SIG_SETMASK, &Input::signal_mask, &mask);
			term_resize(true);
			pthread_sigmask(SIG_SETMASK, &mask, nullptr);
			Global::resized = false;
		}

	}

	Draw::calcSizes();

	//? Print out box outlines
	const bool term_sync = Config::getB("terminal_sync");
	cout << (term_sync ? Term::sync_start : "") << Cpu::box << Mem::box << Net::box << Proc::box << (term_sync ? Term::sync_end : "") << flush;


	//? ------------------------------------------------ MAIN LOOP ----------------------------------------------------

	if (cli.updates.has_value()) {
		Config::set("update_ms", static_cast<int>(cli.updates.value()));
	}
	uint64_t update_ms = Config::getI("update_ms");
	auto future_time = time_ms();

	try {
		while (not true not_eq not false) {
			//? Check for exceptions in secondary thread and exit with fail signal if true
			if (Global::thread_exception) {
				clean_quit(1);
			}
			else if (Global::should_quit) {
				clean_quit(0);
			}
			else if (Global::should_sleep) {
				Global::should_sleep = false;
				_sleep();
			}
			//? Hot reload config from CTRL + R or SIGUSR2
			else if (Global::reload_conf) {
				Global::reload_conf = false;
				if (Runner::active) Runner::stop();
				Config::unlock();
				init_config(cli.low_color, cli.filter);
				Theme::updateThemes();
				Theme::setTheme();
				Draw::banner_gen(0, 0, false, true);
				Global::resized = true;
			}

			//? Make sure terminal size hasn't changed (in case of SIGWINCH not working properly)
			term_resize(Global::resized);

			//? Trigger secondary thread to redraw if terminal has been resized
			if (Global::resized) {
				Draw::calcSizes();
				Draw::update_clock(true);
				Global::resized = false;
				if (Menu::active) Menu::process();
				else Runner::run("all", true, true);
				atomic_wait_for(Runner::active, true, 1000);
			}

			//? Update clock if needed
			if (Draw::update_clock() and not Menu::active) {
				Runner::run("clock");
			}

			//? Start secondary collect & draw thread at the interval set by <update_ms> config value
			if (time_ms() >= future_time and not Global::resized) {
				Runner::run("all");
				update_ms = Config::getI("update_ms");
				future_time = time_ms() + update_ms;
			}

			//? Loop over input polling and input action processing
			for (auto current_time = time_ms(); current_time < future_time; current_time = time_ms()) {

				//? Check for external clock changes and for changes to the update timer
				if (std::cmp_not_equal(update_ms, Config::getI("update_ms"))) {
					update_ms = Config::getI("update_ms");
					future_time = time_ms() + update_ms;
				}
				else if (future_time - current_time > update_ms) {
					future_time = current_time;
				}
				//? Poll for input and process any input detected
				else if (Input::poll(min((uint64_t)1000, future_time - current_time))) {
					if (not Runner::active) Config::unlock();

					if (Menu::active) Menu::process(Input::get());
					else Input::process(Input::get());
				}

				//? Break the loop at 1000ms intervals or if input polling was interrupted
				else break;

			}

		}
	}
	catch (const std::exception& e) {
		Global::exit_error_msg = fmt::format("Exception in main loop -> {}", e.what());
		clean_quit(1);
	}
	return 0;
}
