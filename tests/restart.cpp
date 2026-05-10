// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "btop.hpp"

TEST(restart, parse_env) {
	EXPECT_EQ(Btop::parse_restart_env(nullptr), std::nullopt);
	EXPECT_EQ(Btop::parse_restart_env(""), std::nullopt);
	EXPECT_EQ(Btop::parse_restart_env("abc"), std::nullopt);
	EXPECT_EQ(Btop::parse_restart_env("12x"), std::nullopt);
	EXPECT_EQ(Btop::parse_restart_env("42"), 42u);

	const auto too_large = std::to_string(std::numeric_limits<uint64_t>::max()) + "0";
	EXPECT_EQ(Btop::parse_restart_env(too_large.c_str()), std::nullopt);
}

TEST(restart, next_state) {
	EXPECT_EQ(Btop::next_restart_state(100, std::nullopt, std::nullopt).first, 100u);
	EXPECT_EQ(Btop::next_restart_state(100, std::nullopt, std::nullopt).count, 1u);

	EXPECT_EQ(Btop::next_restart_state(120, 100u, 2u).first, 100u);
	EXPECT_EQ(Btop::next_restart_state(120, 100u, 2u).count, 3u);

	EXPECT_EQ(Btop::next_restart_state(200, 100u, 2u).first, 200u);
	EXPECT_EQ(Btop::next_restart_state(200, 100u, 2u).count, 1u);

	EXPECT_EQ(Btop::next_restart_state(210, 200u, 1u).first, 200u);
	EXPECT_EQ(Btop::next_restart_state(210, 200u, 1u).count, 2u);
}
