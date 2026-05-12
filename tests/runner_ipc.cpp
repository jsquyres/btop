// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "btop.hpp"

TEST(runner_ipc, sample_roundtrip) {
	EXPECT_TRUE(Runner::Test::ipc_sample_roundtrip_ok());
}

TEST(runner_ipc, frame_errors) {
	EXPECT_TRUE(Runner::Test::ipc_frame_errors_ok());
}
