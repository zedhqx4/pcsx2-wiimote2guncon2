// SPDX-FileCopyrightText: 2026 PCSX2 Wiimote integration
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Input/InputSource.h"
#include "wiimote/protocol.h"

#include <array>
#include <deque>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

class WiimoteInputSource final : public InputSource
{
public:
	WiimoteInputSource() = default;
	~WiimoteInputSource() override;

	bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	bool ReloadDevices() override;
	void Shutdown() override;
	bool IsInitialized() override;

	void PollEvents() override;
	std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
	std::vector<InputBindingKey> EnumerateMotors() override;
	bool GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping) override;
	InputLayout GetControllerLayout(u32 index) override;
	void UpdateMotorState(InputBindingKey key, float intensity) override;
	void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity) override;

	std::optional<InputBindingKey> ParseKeyString(const std::string_view device, const std::string_view binding) override;
	TinyString ConvertKeyToString(InputBindingKey key, bool display = false, bool migration = false) override;
	TinyString ConvertKeyToIcon(InputBindingKey key) override;

	// Returns the latest normalized IR aim position (-1..1), without routing it
	// through PCSX2's host-pointer or mouse event path.
	std::optional<std::array<float, 2>> GetIRPosition() const;

private:
	void ThreadMain();

	std::atomic<bool> m_initialized{false};
	std::atomic<bool> m_stop{false};
	std::atomic<bool> m_connected{false};
	std::atomic<bool> m_device_list_changed{false};
	std::atomic<bool> m_rumble{false};

	mutable std::mutex m_state_mutex;
	wiimote::WiiState m_state{};

	std::thread m_thread;

	struct ButtonEvent
	{
		u32 button = 0;
		bool down = false;
	};
	std::deque<ButtonEvent> m_button_events;

	// These values are only accessed by PollEvents() on the input/CPU thread.
	u32 m_previous_buttons = 0;
	std::array<float, 2> m_previous_stick{};
	bool m_notified_connected = false;
};
