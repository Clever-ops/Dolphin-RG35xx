// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <algorithm>
#include <map>
#include <sstream>

#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/SDL/SDL.h"

#ifdef _WIN32
#pragma comment(lib, "SDL2.lib")
#endif

namespace ciface
{
namespace SDL
{

static std::string GetJoystickName(int index)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return SDL_JoystickNameForIndex(index);
#else
	return SDL_JoystickName(index);
#endif
}

void Init( std::vector<Core::Device*>& devices )
{
	// this is used to number the joysticks
	// multiple joysticks with the same name shall get unique ids starting at 0
	std::map<std::string, int> name_counts;

	if (SDL_Init( SDL_INIT_FLAGS ) >= 0)
	{
		// joysticks
		for (int i = 0; i < SDL_NumJoysticks(); ++i)
		{
			SDL_Joystick* dev = SDL_JoystickOpen(i);
			if (dev)
			{
				Joystick* js = new Joystick(dev, i, name_counts[GetJoystickName(i)]++);
				// only add if it has some inputs/outputs
				if (js->Inputs().size() || js->Outputs().size())
					devices.push_back( js );
				else
					delete js;
			}
		}
	}
}

Joystick::Joystick(SDL_Joystick* const joystick, const int sdl_index, const unsigned int index)
	: m_joystick(joystick)
	, m_sdl_index(sdl_index)
	, m_index(index)
{
	// really bad HACKS:
	// to not use SDL for an XInput device
	// too many people on the forums pick the SDL device and ask:
	// "why don't my 360 gamepad triggers/rumble work correctly"
#ifdef _WIN32
	// checking the name is probably good (and hacky) enough
	// but I'll double check with the num of buttons/axes
	std::string lcasename = GetName();
	std::transform(lcasename.begin(), lcasename.end(), lcasename.begin(), tolower);

	if ((std::string::npos != lcasename.find("xbox 360")) &&
	    (10 == SDL_JoystickNumButtons(joystick)) &&
	    (5 == SDL_JoystickNumAxes(joystick)) &&
	    (1 == SDL_JoystickNumHats(joystick)) &&
	    (0 == SDL_JoystickNumBalls(joystick)))
	{
		// this device won't be used
		return;
	}
#endif

	if (SDL_JoystickNumButtons(joystick) > 255 ||
	    SDL_JoystickNumAxes(joystick) > 255 ||
	    SDL_JoystickNumHats(joystick) > 255 ||
	    SDL_JoystickNumBalls(joystick) > 255)
	{
		// This device is invalid, don't use it
		// Some crazy devices(HP webcam 2100) end up as HID devices
		// SDL tries parsing these as joysticks
		return;
	}

	// get buttons
	for (u8 i = 0; i != SDL_JoystickNumButtons(m_joystick); ++i)
		AddInput(new Button(i, m_joystick));

	// get hats
	for (u8 i = 0; i != SDL_JoystickNumHats(m_joystick); ++i)
	{
		// each hat gets 4 input instances associated with it, (up down left right)
		for (u8 d = 0; d != 4; ++d)
			AddInput(new Hat(i, m_joystick, d));
	}

	// get axes
	for (u8 i = 0; i != SDL_JoystickNumAxes(m_joystick); ++i)
	{
		// each axis gets a negative and a positive input instance associated with it
		AddAnalogInputs(new Axis(i, m_joystick, -32768),
			new Axis(i, m_joystick, 32767));
	}

#ifdef USE_SDL_HAPTIC
	// try to get supported ff effects
	m_haptic = SDL_HapticOpenFromJoystick( m_joystick );
	if (m_haptic)
	{
		//SDL_HapticSetGain( m_haptic, 1000 );
		//SDL_HapticSetAutocenter( m_haptic, 0 );

		const unsigned int supported_effects = SDL_HapticQuery( m_haptic );

		// constant effect
		if (supported_effects & SDL_HAPTIC_CONSTANT)
			AddOutput(new ConstantEffect(m_haptic));

		// ramp effect
		if (supported_effects & SDL_HAPTIC_RAMP)
			AddOutput(new RampEffect(m_haptic));

		// sine effect
		if (supported_effects & SDL_HAPTIC_SINE)
			AddOutput(new SineEffect(m_haptic));

		// leftright effect
		if (supported_effects & SDL_HAPTIC_LEFTRIGHT)
		{
			// some controllers have two rumble motors, SDL allows to control them seperatly through the LeftRight
			// haptic effect. We should allow the user to use them seperatly, because there is no other use for it
			// yet and both motors are different, usually a big stronger and slower one and a small weaker and faster one.
			AddOutput(new LeftRightSmallEffect(m_haptic));
			AddOutput(new LeftRightLargeEffect(m_haptic));
		}

		// triangle effect
		if (supported_effects & SDL_HAPTIC_TRIANGLE)
			AddOutput(new TriangleEffect(m_haptic));
	}
#endif

}

Joystick::~Joystick()
{
#ifdef USE_SDL_HAPTIC
	if (m_haptic)
	{
		// stop/destroy all effects
		SDL_HapticStopAll(m_haptic);
		// close haptic first
		SDL_HapticClose(m_haptic);
	}
#endif

	// close joystick
	SDL_JoystickClose(m_joystick);
}

#ifdef USE_SDL_HAPTIC
void Joystick::HapticEffect::Update()
{
	if (m_id == -1 && m_effect.type > 0)
	{
		m_id = SDL_HapticNewEffect(m_haptic, &m_effect);
		if (m_id > -1)
			SDL_HapticRunEffect(m_haptic, m_id, 1);
	}
	else if (m_id > -1 && m_effect.type == 0)
	{
		SDL_HapticStopEffect(m_haptic, m_id);
		SDL_HapticDestroyEffect(m_haptic, m_id);
		m_id = -1;
	}
	else if (m_id > -1)
	{
		SDL_HapticUpdateEffect(m_haptic, m_id, &m_effect);
	}
}

std::string Joystick::ConstantEffect::GetName() const
{
	return "Constant";
}

std::string Joystick::RampEffect::GetName() const
{
	return "Ramp";
}

std::string Joystick::SineEffect::GetName() const
{
	return "Sine";
}

std::string Joystick::LeftRightSmallEffect::GetName() const
{
	return "SmallOnly";
}

std::string Joystick::LeftRightLargeEffect::GetName() const
{
	return "LargeOnly";
}

std::string Joystick::TriangleEffect::GetName() const
{
	return "Triangle";
}

void Joystick::ConstantEffect::SetState(ControlState state)
{
	if (state)
	{
		m_effect.type = SDL_HAPTIC_CONSTANT;
		m_effect.constant.length = SDL_HAPTIC_INFINITY;
	}
	else
	{
		m_effect.type = 0;
	}

	m_effect.constant.level = (Sint16)(state * 0x7FFF);
	Update();
}

void Joystick::RampEffect::SetState(ControlState state)
{
	if (state)
	{
		m_effect.type = SDL_HAPTIC_RAMP;
		m_effect.ramp.length = SDL_HAPTIC_INFINITY;
	}
	else
	{
		m_effect.type = 0;
	}

	m_effect.ramp.start = (Sint16)(state * 0x7FFF);
	Update();
}

void Joystick::SineEffect::SetState(ControlState state)
{
	if (state)
	{
		m_effect.type = SDL_HAPTIC_SINE;
		// 200 seems too weak, somebody with a lot of time could try out other values
		m_effect.periodic.length = 250;
	}
	else
	{
		m_effect.type = 0;
	}

	m_effect.periodic.magnitude = (Sint16)(state * 0x7FFF);
	Update();
}

void Joystick::LeftRightLargeEffect::SetState(ControlState state)
{
	if (state)
	{
		m_effect.type = SDL_HAPTIC_LEFTRIGHT;
		// 200 seems too weak, somebody with a lot of time could try out other values
		m_effect.leftright.length = 250;
	}
	else
	{
		m_effect.type = 0;
	}

	m_effect.leftright.large_magnitude = (Sint16)(state * 0x7FFF);
	Update();
}

void Joystick::LeftRightSmallEffect::SetState(ControlState state)
{
	if (state)
	{
		m_effect.type = SDL_HAPTIC_LEFTRIGHT;
		// 200 seems too weak, somebody with a lot of time could try out other values
		m_effect.leftright.length = 250;
	}
	else
	{
		m_effect.type = 0;
	}

	m_effect.leftright.small_magnitude = (Sint16)(state * 0x7FFF);
	Update();
}

void Joystick::TriangleEffect::SetState(ControlState state)
{
	if (state)
	{
		m_effect.type = SDL_HAPTIC_TRIANGLE;
		// 200 seems too weak, somebody with a lot of time could try out other values
		m_effect.periodic.length = 250;
	}
	else
	{
		m_effect.type = 0;
	}

	m_effect.periodic.magnitude = (Sint16)(state * 0x7FFF);
	Update();
}
#endif

void Joystick::UpdateInput()
{
	// each joystick is doin this, o well
	SDL_JoystickUpdate();
}

std::string Joystick::GetName() const
{
	return StripSpaces(GetJoystickName(m_sdl_index));
}

std::string Joystick::GetSource() const
{
	return "SDL";
}

int Joystick::GetId() const
{
	return m_index;
}

std::string Joystick::Button::GetName() const
{
	std::ostringstream ss;
	ss << "Button " << (int)m_index;
	return ss.str();
}

std::string Joystick::Axis::GetName() const
{
	std::ostringstream ss;
	ss << "Axis " << (int)m_index << (m_range<0 ? '-' : '+');
	return ss.str();
}

std::string Joystick::Hat::GetName() const
{
	static char tmpstr[] = "Hat . .";
	// I don't think more than 10 hats are supported
	tmpstr[4] = (char)('0' + m_index);
	tmpstr[6] = "NESW"[m_direction];
	return tmpstr;
}

ControlState Joystick::Button::GetState() const
{
	return SDL_JoystickGetButton(m_js, m_index);
}

ControlState Joystick::Axis::GetState() const
{
	return std::max(0.0, ControlState(SDL_JoystickGetAxis(m_js, m_index)) / m_range);
}

ControlState Joystick::Hat::GetState() const
{
	return (SDL_JoystickGetHat(m_js, m_index) & (1 << m_direction)) > 0;
}

}
}
