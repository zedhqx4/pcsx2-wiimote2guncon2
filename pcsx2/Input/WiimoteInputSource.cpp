// SPDX-FileCopyrightText: 2026 PCSX2 Wiimote integration
// SPDX-License-Identifier: GPL-3.0+

#include "Input/WiimoteInputSource.h"

#include "wiimote/math.h"

#include "Input/InputManager.h"
#include "IconsPromptFont.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace std::chrono_literals;

	constexpr std::string_view DEVICE_NAME = "Wiimote-0";
	constexpr unsigned int SCAN_INTERVAL_MS = 1500;
	constexpr unsigned int READ_TIMEOUT_MS = 20;
	constexpr unsigned int HID_REPORT_SIZE = wiimote::kWiiInputBytes;

	static constexpr const char* s_button_names[] = {
		"Left", "Right", "Down", "Up", "Plus", "Two", "One", "B", "A", "Minus", "Home", "NunchukZ", "NunchukC"};

	static constexpr const char* s_button_icons[] = {
		ICON_PF_DPAD_LEFT,
		ICON_PF_DPAD_RIGHT,
		ICON_PF_DPAD_DOWN,
		ICON_PF_DPAD_UP,
		ICON_PF_JOYCON_DPAD_RIGHT,
		ICON_PF_BUTTON_SQUARE,
		ICON_PF_BUTTON_TRIANGLE,
		ICON_PF_RIGHT_TRIGGER_R2,
		ICON_PF_BUTTON_CROSS,
		ICON_PF_JOYCON_DPAD_LEFT,
		ICON_PF_HOME_MENU,
		ICON_PF_RIGHT_SHOULDER_RB,
		ICON_PF_BUTTON_CIRCLE,
	};

	static constexpr GenericInputBinding s_generic_button_mapping[] = {
		GenericInputBinding::DPadLeft,   // Left
		GenericInputBinding::DPadRight,  // Right
		GenericInputBinding::DPadDown,   // Down
		GenericInputBinding::DPadUp,     // Up
		GenericInputBinding::Start,      // Plus
		GenericInputBinding::Square,      // Two
		GenericInputBinding::Triangle,   // One
		GenericInputBinding::R2,         // B: native GunCon 2 trigger
		GenericInputBinding::Cross,      // A
		GenericInputBinding::Select,     // Minus
		GenericInputBinding::System,     // Home
		GenericInputBinding::R1,         // Nunchuk Z: native GunCon 2 offscreen shot
		GenericInputBinding::Circle,     // Nunchuk C
	};

	std::optional<u32> ParseDeviceIndex(const std::string_view device)
	{
		if (!device.starts_with("Wiimote-"))
			return std::nullopt;

		const std::optional<s32> index = StringUtil::FromChars<s32>(device.substr(8));
		if (!index || *index != 0)
			return std::nullopt;

		return 0u;
	}

	bool SendReport(SDL_hid_device* device, std::span<const u8> report, bool rumble)
	{
		if (!device || report.empty() || report.size() > HID_REPORT_SIZE)
			return false;

		// HIDAPI expects the report ID as byte zero. Windows Bluetooth HID
		// devices require the complete output-report size, so pad short reports.
		std::array<u8, HID_REPORT_SIZE> output{};
		std::copy(report.begin(), report.end(), output.begin());
		if (report.size() > 1)
		{
			output[1] &= ~u8(0x01);
			if (rumble)
				output[1] |= 0x01;
		}

		return SDL_hid_write(device, output.data(), output.size()) == static_cast<int>(output.size());
	}

	SDL_hid_device* OpenFirstRemote()
	{
		SDL_hid_device_info* const devices = SDL_hid_enumerate(wiimote::kWiimoteVendor, 0);
		SDL_hid_device* opened = nullptr;

		for (SDL_hid_device_info* current = devices; current; current = current->next)
		{
			if (current->product_id != wiimote::kWiimoteProduct &&
				current->product_id != wiimote::kWiimotePlusProduct)
			{
				continue;
			}

			if (!current->path || !current->path[0])
				continue;

			opened = SDL_hid_open_path(current->path);
			if (opened)
				break;
		}

		SDL_hid_free_enumeration(devices);
		return opened;
	}

	std::optional<GenericInputBinding> GetGenericButtonBinding(u32 button)
	{
		if (button >= std::size(s_generic_button_mapping))
			return std::nullopt;

		return s_generic_button_mapping[button];
	}
} // namespace

WiimoteInputSource::~WiimoteInputSource()
{
	Shutdown();
}

bool WiimoteInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	(void)si;
	(void)settings_lock;

	if (m_initialized.load())
		return true;

	// Wii Remotes are not necessarily classified as game controllers by SDL.
	// HIDAPI therefore needs to enumerate non-controller HID devices too.
	SDL_SetHint("SDL_HIDAPI_ENUMERATE_ONLY_CONTROLLERS", "0");

	if (SDL_hid_init() < 0)
	{
		Console.Error("(WiimoteInputSource) SDL HIDAPI initialization failed: %s", SDL_GetError());
		return false;
	}

	m_stop.store(false);
	m_initialized.store(true);
	m_device_list_changed.store(true);
	m_thread = std::thread(&WiimoteInputSource::ThreadMain, this);
	return true;
}

void WiimoteInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	(void)si;
	(void)settings_lock;
}

bool WiimoteInputSource::ReloadDevices()
{
	return m_device_list_changed.exchange(false);
}

void WiimoteInputSource::Shutdown()
{
	if (!m_initialized.exchange(false))
		return;

	m_stop.store(true);
	if (m_thread.joinable())
		m_thread.join();

	m_connected.store(false);
	m_rumble.store(false);
	{
		std::lock_guard lock(m_state_mutex);
		m_state = wiimote::WiiState{};
	}
	SDL_hid_exit();
}

bool WiimoteInputSource::IsInitialized()
{
	return m_initialized.load();
}

void WiimoteInputSource::ThreadMain()
{
	SDL_hid_device* device = nullptr;
	auto next_scan = std::chrono::steady_clock::now();
	bool extension_connected = false;
	bool received_first_status = false;
	bool sent_rumble = false;
	u32 previous_buttons = 0;
	auto last_report_time = std::chrono::steady_clock::now();
	auto last_ir_diagnostic = last_report_time;
	bool previous_ir_tracking = false;

	const auto disconnect = [&](const char* reason) {
		if (device)
			SDL_hid_close(device);
		device = nullptr;
		extension_connected = false;
		received_first_status = false;
		previous_buttons = 0;
		previous_ir_tracking = false;
		m_connected.store(false);
		m_device_list_changed.store(true);
		next_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(SCAN_INTERVAL_MS);
		{
			std::lock_guard lock(m_state_mutex);
			m_state = wiimote::WiiState{};
			m_button_events.clear();
		}
		Console.Warning(fmt::format("(WiimoteInputSource) {}. Retrying device discovery.", reason));
	};

	while (!m_stop.load())
	{
		if (!device)
		{
			if (std::chrono::steady_clock::now() < next_scan)
			{
				std::this_thread::sleep_for(50ms);
				continue;
			}

			next_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(SCAN_INTERVAL_MS);
			device = OpenFirstRemote();
			if (!device)
				continue;

			bool setup_ok = true;
			for (const wiimote::WiiStep& step : wiimote::wii_init_sequence())
			{
				if (!SendReport(device, step.report, m_rumble.load()))
				{
					setup_ok = false;
					break;
				}

				if (step.delay_ms)
					std::this_thread::sleep_for(std::chrono::milliseconds(step.delay_ms));
			}

			const auto status_request = wiimote::wii_status_request();
			setup_ok = setup_ok && SendReport(device, status_request, m_rumble.load());
			if (!setup_ok)
			{
				SDL_hid_close(device);
				device = nullptr;
				continue;
			}

			extension_connected = false;
			received_first_status = false;
			sent_rumble = m_rumble.load();
			{
				std::lock_guard lock(m_state_mutex);
				m_state = wiimote::WiiState{};
				m_button_events.clear();
				previous_buttons = 0;
			}
			last_report_time = std::chrono::steady_clock::now();
			last_ir_diagnostic = last_report_time;
			previous_ir_tracking = false;
			m_connected.store(true);
			m_device_list_changed.store(true);
			Console.WriteLn("(WiimoteInputSource) Wii Remote connected; waiting for IR points.");
		}

		const bool rumble = m_rumble.load();
		if (rumble != sent_rumble)
		{
			const std::array<u8, 2> motor_report = {0x10, 0x00};
			if (!SendReport(device, motor_report, rumble))
			{
				disconnect("HID write failed while changing rumble");
				continue;
			}
			sent_rumble = rumble;
		}

		std::array<u8, 64> input{};
		const int bytes_read = SDL_hid_read_timeout(device, input.data(), input.size(), READ_TIMEOUT_MS);
		if (bytes_read < 0)
		{
			disconnect("HID read failed or the remote was unplugged");
			continue;
		}

		if (bytes_read == 0)
		{
			// Some HID backends report an unplugged device as repeated timeouts
			// instead of returning an error. A Wii Remote in continuous report
			// mode should keep sending packets, even when no buttons are pressed.
			if (std::chrono::steady_clock::now() - last_report_time >= 1500ms)
				disconnect("No HID reports received for 1.5 seconds");
			continue;
		}

		last_report_time = std::chrono::steady_clock::now();

		const std::span<const u8> report(input.data(), static_cast<size_t>(bytes_read));
		if (const std::optional<wiimote::WiiStatus> status = wiimote::wii_parse_status(report))
		{
			// Connecting or disconnecting an extension stops the current data
			// reporting stream. Re-enable mode 0x37 after either transition.
			if (received_first_status && status->extension != extension_connected)
			{
				bool extension_setup_ok = true;

				if (status->extension)
				{
					// A newly connected Nunchuk must be initialized before its
					// extension bytes can be decoded as plain data.
					for (const wiimote::WiiStep& step : wiimote::wii_extension_steps())
					{
						if (!SendReport(device, step.report, m_rumble.load()))
						{
							extension_setup_ok = false;
							break;
						}
						if (step.delay_ms)
							std::this_thread::sleep_for(std::chrono::milliseconds(step.delay_ms));
					}
				}

				// The Wiimote disables continuous reports on both extension
				// insertion and removal, so always restore the selected mode.
				const auto mode = wiimote::wii_reporting_report(0x37);
				extension_setup_ok = extension_setup_ok && SendReport(device, mode, m_rumble.load());
				if (!extension_setup_ok)
				{
					disconnect("Failed to reconfigure reports after an extension change");
					continue;
				}
			}

			extension_connected = status->extension;
			received_first_status = true;
			continue;
		}

		wiimote::WiiState state;
		if (wiimote::wii_parse_report(report, state))
		{
			const auto aim = wiimote::wii_ir_pointer(state.ir);
			const u32 visible_dots = static_cast<u32>(std::count_if(state.ir.begin(), state.ir.end(),
				[](const wiimote::WiiIrDot& dot) { return dot.valid; }));
			const auto now = std::chrono::steady_clock::now();
			if (aim.has_value() != previous_ir_tracking)
			{
				if (aim.has_value())
					Console.WriteLn(fmt::format("(WiimoteInputSource) IR tracking acquired at ({:.3f}, {:.3f}) with {} visible dots.",
						aim->at(0), aim->at(1), visible_dots));
				else
					Console.Warning(fmt::format("(WiimoteInputSource) IR tracking lost; {} of 4 dots remain visible.", visible_dots));
				previous_ir_tracking = aim.has_value();
			}
			if (!aim.has_value() && now - last_ir_diagnostic >= 1500ms)
			{
				const auto dot_text = [](const wiimote::WiiIrDot& dot) {
					return dot.valid ? fmt::format("({}, {})", dot.x, dot.y) : std::string("missing");
				};
				Console.Warning(fmt::format("(WiimoteInputSource) IR unavailable: {}/4 points; {}, {}, {}, {}.",
					visible_dots, dot_text(state.ir[0]), dot_text(state.ir[1]), dot_text(state.ir[2]), dot_text(state.ir[3])));
				last_ir_diagnostic = now;
			}

			if (!extension_connected)
			{
				state.has_nunchuk = false;
				state.stick = {128, 128};
				state.buttons &= ~((1u << static_cast<u32>(wiimote::WiiButton::nunchuk_z)) |
					(1u << static_cast<u32>(wiimote::WiiButton::nunchuk_c)));
			}

			const u32 changed_buttons = state.buttons ^ previous_buttons;
			{
				std::lock_guard lock(m_state_mutex);
				for (u32 button = 0; button < static_cast<u32>(wiimote::WiiButton::count); button++)
				{
					const u32 mask = 1u << button;
					if (!(changed_buttons & mask))
						continue;

					if (m_button_events.size() >= 256)
						m_button_events.pop_front();

					m_button_events.push_back({button, (state.buttons & mask) != 0});
				}
				m_state = state;
			}
			previous_buttons = state.buttons;
		}
	}

	if (device)
	{
		if (m_rumble.load())
		{
			const std::array<u8, 2> motor_report = {0x10, 0x00};
			SendReport(device, motor_report, false);
		}
		SDL_hid_close(device);
	}

	if (m_connected.exchange(false))
		m_device_list_changed.store(true);

	// Shutdown can happen while buttons are held. Clear the queued transitions
	// as well as the snapshot so a later poll cannot replay stale input.
	{
		std::lock_guard lock(m_state_mutex);
		m_state = wiimote::WiiState{};
		m_button_events.clear();
	}
}

void WiimoteInputSource::PollEvents()
{
	const bool connected = m_connected.load();
	if (connected != m_notified_connected)
	{
		if (connected)
		{
			InputManager::OnInputDeviceConnected(DEVICE_NAME, "Nintendo Wii Remote");
		}
		else
		{
			InputBindingKey key{};
			key.source_type = InputSourceType::Wiimote;
			InputManager::OnInputDeviceDisconnected(key, DEVICE_NAME);
		}
		m_notified_connected = connected;
	}

	wiimote::WiiState state{};
	std::deque<ButtonEvent> button_events;
	{
		std::lock_guard lock(m_state_mutex);
		if (connected)
			state = m_state;
		button_events.swap(m_button_events);
	}

	const auto emit_button = [this](u32 button, bool down) {
		const InputBindingKey key =
			MakeGenericControllerButtonKey(InputSourceType::Wiimote, 0, static_cast<s32>(button));
		InputManager::InvokeEvents(key, down ? 1.0f : 0.0f,
			GetGenericButtonBinding(button).value_or(GenericInputBinding::Unknown));
		const u32 mask = 1u << button;
		if (down)
			m_previous_buttons |= mask;
		else
			m_previous_buttons &= ~mask;
	};

	// Preserve short presses that begin and end between two PCSX2 input polls.
	for (const ButtonEvent& event : button_events)
		emit_button(event.button, event.down);

	// Reconcile the queue with the latest report, and release everything on disconnect.
	const u32 current_buttons = connected ? state.buttons : 0;
	const u32 changed_buttons = current_buttons ^ m_previous_buttons;
	for (u32 button = 0; button < static_cast<u32>(wiimote::WiiButton::count); button++)
	{
		const u32 mask = 1u << button;
		if (changed_buttons & mask)
			emit_button(button, (current_buttons & mask) != 0);
	}

	const std::array<float, 2> stick = (connected && state.has_nunchuk) ?
		wiimote::wii_stick(state.stick, 0.15f) : std::array<float, 2>{0.0f, 0.0f};

	if (std::abs(stick[0] - m_previous_stick[0]) > 0.001f)
	{
		const InputBindingKey key = MakeGenericControllerAxisKey(InputSourceType::Wiimote, 0, 0);
		InputManager::InvokeEvents(key, stick[0], GenericInputBinding::Unknown,
			GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight);
	}
	if (std::abs(stick[1] - m_previous_stick[1]) > 0.001f)
	{
		const InputBindingKey key = MakeGenericControllerAxisKey(InputSourceType::Wiimote, 0, 1);
		// wii_stick() uses SDL's axis convention: negative Y is up, positive Y is down.
		InputManager::InvokeEvents(key, stick[1], GenericInputBinding::Unknown,
			GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown);
	}
	m_previous_stick = stick;
}

std::vector<std::pair<std::string, std::string>> WiimoteInputSource::EnumerateDevices()
{
	if (!m_connected.load())
		return {};

	return {{std::string(DEVICE_NAME), "Nintendo Wii Remote"}};
}

std::vector<InputBindingKey> WiimoteInputSource::EnumerateMotors()
{
	if (!m_connected.load())
		return {};

	return {MakeGenericControllerMotorKey(InputSourceType::Wiimote, 0, 0)};
}

bool WiimoteInputSource::GetGenericBindingMapping(
	const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	if (device != DEVICE_NAME)
		return false;

	for (u32 button = 0; button < std::size(s_generic_button_mapping); button++)
	{
		if (s_generic_button_mapping[button] == GenericInputBinding::Unknown)
			continue;
		mapping->emplace_back(s_generic_button_mapping[button],
			fmt::format("{}/{}", DEVICE_NAME, s_button_names[button]));
	}

	mapping->emplace_back(GenericInputBinding::LeftStickLeft, fmt::format("{}/-NunchukX", DEVICE_NAME));
	mapping->emplace_back(GenericInputBinding::LeftStickRight, fmt::format("{}/+NunchukX", DEVICE_NAME));
	mapping->emplace_back(GenericInputBinding::LeftStickDown, fmt::format("{}/-NunchukY", DEVICE_NAME));
	mapping->emplace_back(GenericInputBinding::LeftStickUp, fmt::format("{}/+NunchukY", DEVICE_NAME));
	mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("{}/Rumble", DEVICE_NAME));
	mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("{}/Rumble", DEVICE_NAME));
	return true;
}

InputLayout WiimoteInputSource::GetControllerLayout(u32 index)
{
	return (index == 0) ? InputLayout::Nintendo : InputLayout::Unknown;
}

void WiimoteInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	if (key.source_subtype == InputSubclass::ControllerMotor)
		m_rumble.store(intensity >= 0.01f);
}

void WiimoteInputSource::UpdateMotorState(
	InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
	if (large_key.source_type == InputSourceType::Wiimote && small_key.source_type == InputSourceType::Wiimote)
		m_rumble.store(std::max(large_intensity, small_intensity) >= 0.01f);
}

std::optional<InputBindingKey> WiimoteInputSource::ParseKeyString(
	const std::string_view device, const std::string_view binding)
{
	if (!ParseDeviceIndex(device) || binding.empty())
		return std::nullopt;

	for (u32 button = 0; button < std::size(s_button_names); button++)
	{
		if (binding == s_button_names[button])
			return MakeGenericControllerButtonKey(InputSourceType::Wiimote, 0, static_cast<s32>(button));
	}

	for (u32 axis = 0; axis < 2; axis++)
	{
		const std::string_view axis_name = axis == 0 ? "NunchukX" : "NunchukY";
		InputModifier modifier = InputModifier::None;
		if (binding == std::string("+") + std::string(axis_name))
			modifier = InputModifier::None;
		else if (binding == std::string("-") + std::string(axis_name))
			modifier = InputModifier::Negate;
		else if (binding == std::string("Full") + std::string(axis_name))
			modifier = InputModifier::FullAxis;
		else
			continue;

		InputBindingKey key = MakeGenericControllerAxisKey(InputSourceType::Wiimote, 0, static_cast<s32>(axis));
		key.modifier = modifier;
		return key;
	}

	if (binding == "Rumble")
		return MakeGenericControllerMotorKey(InputSourceType::Wiimote, 0, 0);

	return std::nullopt;
}

TinyString WiimoteInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	(void)migration;

	TinyString ret;
	if (key.source_type != InputSourceType::Wiimote || key.source_index != 0)
		return ret;

	if (key.source_subtype == InputSubclass::ControllerButton && key.data < std::size(s_button_names))
	{
		if (display)
			ret.format("Wiimote 0 {}", s_button_names[key.data]);
		else
			ret.format("{}/{}", DEVICE_NAME, s_button_names[key.data]);
	}
	else if (key.source_subtype == InputSubclass::ControllerAxis && key.data < 2)
	{
		const char* modifier = (key.modifier == InputModifier::Negate) ? "-" :
			(key.modifier == InputModifier::FullAxis) ? "Full" : "+";
		const char* axis_name = (key.data == 0) ? "NunchukX" : "NunchukY";
		if (display)
			ret.format("Wiimote 0 {}{}", modifier, axis_name);
		else
			ret.format("{}/{}{}", DEVICE_NAME, modifier, axis_name);
	}
	else if (key.source_subtype == InputSubclass::ControllerMotor)
	{
		if (display)
			ret.format("Wiimote 0 Rumble");
		else
			ret.format("{}/Rumble", DEVICE_NAME);
	}

	return ret;
}

TinyString WiimoteInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	TinyString ret;
	if (key.source_type == InputSourceType::Wiimote &&
		key.source_subtype == InputSubclass::ControllerButton &&
		key.data < std::size(s_button_icons))
	{
		ret.format("{}", s_button_icons[key.data]);
	}
	return ret;
}

std::optional<std::array<float, 2>> WiimoteInputSource::GetIRPosition() const
{
	if (!m_connected.load())
		return std::nullopt;

	wiimote::WiiState state;
	{
		std::lock_guard lock(m_state_mutex);
		state = m_state;
	}

	return wiimote::wii_ir_pointer(state.ir);
}
