// SPDX-FileCopyrightText: 2026 PCSX2 Wiimote integration
// SPDX-License-Identifier: GPL-3.0+

#include "wiimote/math.h"
#include "wiimote/protocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace
{
	bool HasButton(const wiimote::WiiState& state, wiimote::WiiButton button)
	{
		return (state.buttons & (1u << static_cast<unsigned>(button))) != 0;
	}
} // namespace

TEST(WiimoteProtocol, ParsesBasicIrAndNunchukReport)
{
	std::array<std::uint8_t, wiimote::kWiiInputBytes> report{};

	// Report 0x37 contains buttons, accelerometer, basic IR, and extension data.
	report[0] = 0x37;
	report[1] = 0x01 | 0x10; // Left + Plus
	report[2] = 0x04;         // B

	// Two IR points at (400, 384) and (624, 384).
	// The middle byte packs the upper two bits of all four coordinates.
	report[6] = 0x90;
	report[7] = 0x80;
	report[8] = 0x56;
	report[9] = 0x70;
	report[10] = 0x80;

	// The second pair is not visible.
	for (std::size_t i = 11; i < 16; i++)
		report[i] = 0xFF;

	// Plain Nunchuk data: centred stick and both buttons pressed (active low).
	report[16] = 128;
	report[17] = 128;
	report[18] = 0x80;
	report[19] = 0x80;
	report[20] = 0x80;
	report[21] = 0xFC;

	wiimote::WiiState state{};
	ASSERT_TRUE(wiimote::wii_parse_report(report, state));

	EXPECT_TRUE(HasButton(state, wiimote::WiiButton::left));
	EXPECT_TRUE(HasButton(state, wiimote::WiiButton::plus));
	EXPECT_TRUE(HasButton(state, wiimote::WiiButton::b));
	EXPECT_TRUE(HasButton(state, wiimote::WiiButton::nunchuk_z));
	EXPECT_TRUE(HasButton(state, wiimote::WiiButton::nunchuk_c));

	ASSERT_TRUE(state.has_nunchuk);
	EXPECT_EQ(state.stick[0], 128);
	EXPECT_EQ(state.stick[1], 128);

	ASSERT_TRUE(state.ir[0].valid);
	ASSERT_TRUE(state.ir[1].valid);
	EXPECT_EQ(state.ir[0].x, 400);
	EXPECT_EQ(state.ir[0].y, 384);
	EXPECT_EQ(state.ir[1].x, 624);
	EXPECT_EQ(state.ir[1].y, 384);
	EXPECT_FALSE(state.ir[2].valid);
	EXPECT_FALSE(state.ir[3].valid);
}

TEST(WiimoteProtocol, RejectsUnknownAndTruncatedReportsWithoutChangingState)
{
	wiimote::WiiState state{};
	state.buttons = 0x1234;
	state.stick = {90, 170};

	const std::array<std::uint8_t, 2> unknown = {0x7F, 0x00};
	EXPECT_FALSE(wiimote::wii_parse_report(unknown, state));
	EXPECT_EQ(state.buttons, 0x1234u);
	EXPECT_EQ(state.stick[0], 90);
	EXPECT_EQ(state.stick[1], 170);

	const std::array<std::uint8_t, 5> truncated = {0x31, 0, 0, 0, 0};
	EXPECT_FALSE(wiimote::wii_parse_report(truncated, state));
	EXPECT_EQ(state.buttons, 0x1234u);
}

TEST(WiimoteProtocol, ParsesExtensionStatus)
{
	const std::array<std::uint8_t, 7> report = {0x20, 0, 0, 0x02, 0, 0, 0x80};
	const std::optional<wiimote::WiiStatus> status = wiimote::wii_parse_status(report);

	ASSERT_TRUE(status.has_value());
	EXPECT_TRUE(status->extension);
	EXPECT_EQ(status->battery, 0x80);
}

TEST(WiimoteMath, CentresAndMirrorsTheIrPointer)
{
	std::array<wiimote::WiiIrDot, 4> dots{};
	dots[0] = {400, 384, true};
	dots[1] = {624, 384, true};

	const std::optional<std::array<float, 2>> centred = wiimote::wii_ir_pointer(dots);
	ASSERT_TRUE(centred.has_value());
	EXPECT_NEAR((*centred)[0], 0.0f, 0.001f);
	EXPECT_NEAR((*centred)[1], 0.0f, 0.001f);

	// Moving the sensor bar left in the camera image means the remote aims right.
	dots[0].x = 288;
	dots[1].x = 512;
	const std::optional<std::array<float, 2>> shifted = wiimote::wii_ir_pointer(dots);
	ASSERT_TRUE(shifted.has_value());
	EXPECT_GT((*shifted)[0], 0.0f);
	EXPECT_NEAR((*shifted)[1], 0.0f, 0.001f);
}

TEST(WiimoteMath, RequiresTwoVisibleIrPoints)
{
	std::array<wiimote::WiiIrDot, 4> dots{};
	dots[0] = {512, 384, true};

	EXPECT_FALSE(wiimote::wii_ir_pointer(dots).has_value());
}

TEST(WiimoteMath, AppliesTheNunchukStickDeadZone)
{
	const std::array<float, 2> centred = wiimote::wii_stick({128, 128}, 0.15f);
	EXPECT_NEAR(centred[0], 0.0f, 0.001f);
	EXPECT_NEAR(centred[1], 0.0f, 0.001f);

	const std::array<float, 2> full_right = wiimote::wii_stick({223, 128}, 0.15f);
	EXPECT_NEAR(full_right[0], 1.0f, 0.001f);
	EXPECT_NEAR(full_right[1], 0.0f, 0.001f);

	const std::array<float, 2> full_down = wiimote::wii_stick({128, 223}, 0.15f);
	EXPECT_NEAR(full_down[0], 0.0f, 0.001f);
	EXPECT_NEAR(full_down[1], 1.0f, 0.001f);

	const std::array<float, 2> full_up = wiimote::wii_stick({128, 33}, 0.15f);
	EXPECT_NEAR(full_up[0], 0.0f, 0.001f);
	EXPECT_NEAR(full_up[1], -1.0f, 0.001f);
}
