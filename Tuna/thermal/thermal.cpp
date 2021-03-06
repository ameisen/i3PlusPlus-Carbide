/**
 * Marlin 3D Printer Firmware
 * Copyright (C) 2016 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (C) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

 /**
  * temperature.cpp - temperature control
  */

#include <tuna.h>
#include "thermal/thermal.hpp"
#include "bi3_plus_lcd.h"
#include "planner.h"
#include "configuration_store.h"
#include "watchdog.h"

#define ENABLE_ERROR_1A 0
#define ENABLE_ERROR_1B 0
#define ENABLE_ERROR_2A 0
#define ENABLE_ERROR_2B 0
#define ENABLE_ERROR_3 1
#define ENABLE_ERROR_4 1
#define ENABLE_ERROR_5 1

#include "managers/simple.hpp"

using HeaterManager = Tuna::Thermal::Manager::Simple;

temp_t Temperature::min_extrude_temp = (typename temp_t::type)EXTRUDE_MINTEMP;

temp_t Temperature::current_temperature = 0_C,
Temperature::current_temperature_bed = 0;
temp_t Temperature::target_temperature = 0_C;
temp_t Temperature::target_temperature_bed = 0_C;

temp_t Temperature::watch_target_temp = 0_C;
millis_t Temperature::watch_heater_next_ms = 0_u16;

temp_t Temperature::watch_target_bed_temp = 0_C;
millis_t Temperature::watch_bed_next_ms = 0;

bool Temperature::allow_cold_extrude = false;

millis_t Temperature::next_bed_check_ms;

memory<uint8_t> Temperature::soft_pwm_amount = 0;
volatile_conditional_type<uint8, has_bed_thermal_management> Temperature::soft_pwm_amount_bed = 0_u8;
volatile_conditional_type<bool, !has_bed_thermal_management> Temperature::is_bed_heating = false;

namespace
{
  struct interrupt final : trait::ce_only
  {
    static memory<bool> ready_;
    static memory<uint16> raw_adc_hotend;
    static memory<uint16> raw_adc_bed;

    static inline void __forceinline __flatten set_adc (arg_type<uint16> hotend, arg_type<uint16> bed)
    {
      raw_adc_hotend.write_through(hotend);
      raw_adc_bed.write_through(bed);
      ready_.write_through(true);
    }

    static inline void __forceinline __flatten set_adc_hotend (arg_type<uint16> adc)
    {
      raw_adc_hotend.write_through(adc);
      ready_.write_through(true);
    }

    static inline void __forceinline __flatten set_adc_bed (arg_type<uint16> adc)
    {
      raw_adc_bed.write_through(adc);
      ready_.write_through(true);
    }

    static inline uint16 __forceinline __flatten __pure get_adc_hotend()
    {
      return raw_adc_hotend.read_through();
    }

    static inline uint16 __forceinline __flatten __pure get_adc_bed()
    {
      return raw_adc_bed.read_through();
    }

    static inline bool __forceinline __flatten __pure is_ready()
    {
      return ready_.read_through();
    }

    static inline void __forceinline __flatten set_ready(bool state)
    {
      ready_ = state;
    }
  };

  memory<bool> interrupt::ready_ = false;
  memory<uint16> interrupt::raw_adc_hotend = 0_u16;
  memory<uint16> interrupt::raw_adc_bed = 0_u16;
}

namespace
{
  class temp_trend final
  {
    static constexpr const auto MeanCount = make_uintsz<8>;
    static constexpr const auto PrinterMaxTempT = temp_t{ printer_max_temperature }.raw();
    using sum_t = uintsz<PrinterMaxTempT * MeanCount>;
    using tempt_t = typename temp_t::type;

    // positive hack because presently fixed-type doesn't support signed.
    bool m_Positive = true;
    sum_t m_MeanSum = 0;
  public:

    inline bool __forceinline __flatten is_positive () const __restrict
    {
      return m_Positive;
    }

    inline temp_t __forceinline __flatten getMean() const __restrict
    {
      return temp_t::from(m_MeanSum / MeanCount);
    }

    inline void __forceinline __flatten appendValue(arg_type<temp_t> value, bool positive) __restrict
    {
      const auto raw_value = value.raw();
      m_MeanSum -= tempt_t(m_MeanSum / MeanCount);
      if (positive == m_Positive)
      {
        m_MeanSum += raw_value;
      }
      else
      {
        if (raw_value > m_MeanSum)
        {
          m_MeanSum = raw_value - m_MeanSum;
          m_Positive = !m_Positive;
        }
        else
        {
          m_MeanSum -= raw_value;
        }
      }
    }
  };
  temp_trend temperatureTrendCalculator;
}

Temperature::Trend __forceinline __flatten Temperature::get_temperature_trend()
{
  return temperatureTrendCalculator.is_positive() ? Trend::Up : Trend::Down;
}

void Temperature::PID_autotune(arg_type<temp_t> temp, arg_type<int> ncycles, bool set_result/*=false*/) {
  HeaterManager::calibrate(temp);
}

void Temperature::updatePID() {}

template <Temperature::Manager manager_type>
uint8 __forceinline __flatten Temperature::getHeaterPower() {
	if constexpr (manager_type == Manager::Hotend)
	{
		return soft_pwm_amount;
	}
	else
	{
    if constexpr(has_bed_thermal_management)
    {
      return soft_pwm_amount_bed;
    }
    else
    {
      return is_bed_heating ? 0xFF : 0;
    }
	}
}
template uint8 Temperature::getHeaterPower<Temperature::Manager::Hotend>();
template uint8 Temperature::getHeaterPower<Temperature::Manager::Bed>();

//
// Temperature Error Handlers
//
template <Temperature::Manager manager_type>
void Temperature::_temp_error(const char * __restrict const serial_msg, const char * __restrict const lcd_msg) {
	static bool killed = false;
	lcd::show_page(lcd::Page::Thermal_Runaway);
	if (__likely(is_running())) {
		SERIAL_ERROR_START();
		serialprintPGM(serial_msg);
		SERIAL_ERRORPGM(MSG_STOPPED_HEATER);
		if constexpr (manager_type == Temperature::Manager::Hotend)
		{
			SERIAL_ERRORLN(0);
		}
		else
		{
			SERIAL_ERRORLNPGM(MSG_HEATER_BED);
		}
	}
#if DISABLED(BOGUS_TEMPERATURE_FAILSAFE_OVERRIDE)
	if (!killed)
  {
		Running = false;
		killed = true;
		kill("ohno i broke");
	}
  else
  {
    disable_all_heaters(); // paranoia
  }
#endif
}

template <Temperature::Manager manager_type>
void Temperature::max_temp_error() {
	_temp_error<manager_type>(PSTR(MSG_T_MAXTEMP), (manager_type == Manager::Hotend) ? PSTR(MSG_ERR_MAXTEMP) : PSTR(MSG_ERR_MAXTEMP_BED));
}

template <Temperature::Manager manager_type>
void Temperature::min_temp_error() {
	_temp_error<manager_type>(PSTR(MSG_T_MINTEMP), (manager_type == Manager::Hotend) ? PSTR(MSG_ERR_MINTEMP) : PSTR(MSG_ERR_MINTEMP_BED));
}

/**
 * Manage heating activities for extruder hot-ends and a heated bed
 *  - Acquire updated temperature readings
 *    - Also resets the watchdog timer
 *  - Invoke thermal runaway protection
 *  - Manage extruder auto-fan
 *  - Apply filament width to the extrusion rate (may move)
 *  - Update the heated bed PID output value
 */
bool Temperature::manage_heater() {

  if (__likely(!updateTemperaturesFromRawValues()))
  {
    return false;
  }

	millis_t ms = millis();

	// Check for thermal runaway
#if ENABLE_ERROR_2A
	thermal_runaway_protection<Manager::Hotend>(thermal_runaway_state_machine, thermal_runaway_timer, current_temperature, target_temperature, THERMAL_PROTECTION_PERIOD, THERMAL_PROTECTION_HYSTERESIS);
#endif

	// Make sure temperature is increasing
	if (watch_heater_next_ms && __unlikely(ELAPSED(ms, watch_heater_next_ms))) { // Time to check this extruder?
#if ENABLE_ERROR_1A
		if (degHotend() < watch_target_temp)                             // Failed to increase enough?
			_temp_error<Manager::Hotend>(PSTR(MSG_T_HEATING_FAILED), PSTR(MSG_HEATING_FAILED_LCD));
		else                                                                 // Start again if the target is still far off
#endif
			start_watching_heater();
	}

	// Make sure temperature is increasing
	if (watch_bed_next_ms && __unlikely(ELAPSED(ms, watch_bed_next_ms))) {        // Time to check the bed?
#if ENABLE_ERROR_1B
		if (degBed() < watch_target_bed_temp)                           // Failed to increase enough?
			_temp_error<Manager::Bed>(PSTR(MSG_T_HEATING_FAILED), PSTR(MSG_HEATING_FAILED_LCD));
		else                                                            // Start again if the target is still far off
#endif
			start_watching_bed();
	}

	//if (PENDING(ms, next_bed_check_ms)) return true;
	//next_bed_check_ms = ms + BED_CHECK_INTERVAL;

#if ENABLE_ERROR_2B
	thermal_runaway_protection<Manager::Bed>(thermal_runaway_bed_state_machine, thermal_runaway_bed_timer, current_temperature_bed, target_temperature_bed, THERMAL_PROTECTION_BED_PERIOD, THERMAL_PROTECTION_BED_HYSTERESIS);
#endif

  // Failsafe to make sure fubar'd PID settings don't force the heater always on.
  if (__unlikely(target_temperature == 0_C))
  {
    soft_pwm_amount.write_through(0);
    WRITE_HEATER_0(LOW);
  }
  else if (__unlikely((current_temperature <= Hotend::min_temperature::Temperature || is_preheating()) || current_temperature >= Hotend::max_temperature::Temperature))
  {
    soft_pwm_amount.write_through(0);
  }
  else
  {
    soft_pwm_amount.write_through(HeaterManager::get_power(current_temperature, target_temperature));
  }

  // Failsafe to make sure fubar'd PID settings don't force the heater always on.
  if (__unlikely(target_temperature_bed == 0_C))
  {
    if constexpr(has_bed_thermal_management)
    {
      soft_pwm_amount_bed = 0;
    }
    else
    {
      is_bed_heating = false;
    }
    WRITE_HEATER_BED(LOW);
  }

	// Check if temperature is within the correct range
	if (__likely(WITHIN(current_temperature_bed, temp_t(Bed::min_temperature::Temperature), temp_t(Bed::max_temperature::Temperature))))
  {
    if constexpr(has_bed_thermal_management)
    {
      soft_pwm_amount_bed = current_temperature_bed < target_temperature_bed ? MAX_BED_POWER >> 1 : 0;
    }
    else
    {
      is_bed_heating = current_temperature_bed < target_temperature_bed;
    }
	}
	else
  {
    if constexpr(has_bed_thermal_management)
    {
      soft_pwm_amount_bed = 0;
    }
    else
    {
      is_bed_heating = false;
    }
		WRITE_HEATER_BED(LOW);
	}

  return true;
}

temp_t Temperature::adc_to_temperature(arg_type<uint16> raw)
{
	return Thermistor::adc_to_temperature(raw);
}

/**
 * Get the raw values into the actual temperatures.
 * The raw values are created in interrupt context,
 * and this function is called from normal context
 * as it would block the stepper routine.
 */
bool Temperature::updateTemperaturesFromRawValues() {

	if (__unlikely(interrupt::is_ready()))
	{
    HeaterManager::debug_dump();

		uint16 temperature_raw;
		uint16 temperature_bed_raw;
		{
			Tuna::critical_section_not_isr _critsec;
      temperature_raw = interrupt::get_adc_hotend();
			temperature_bed_raw = interrupt::get_adc_bed();
      interrupt::set_ready(false);
		}

#if ENABLE_ERROR_3
		if constexpr (HEATER_0_RAW_LO_TEMP < HEATER_0_RAW_HI_TEMP)
		{
			if (__unlikely((temperature_raw >= Hotend::max_temperature::Adc) & (target_temperature > 0_C))) { max_temp_error<Manager::Hotend>(); }
			if (__unlikely((Hotend::min_temperature::Adc >= temperature_raw) & (!is_preheating()) & (target_temperature > 0_C))) { min_temp_error<Manager::Hotend>(); }
		}
		else
		{
			if (__unlikely((temperature_raw <= Hotend::max_temperature::Adc) & (target_temperature > 0_C))) { max_temp_error<Manager::Hotend>(); }
			if (__unlikely((Hotend::min_temperature::Adc <= temperature_raw) & (!is_preheating()) & (target_temperature > 0_C))) { min_temp_error<Manager::Hotend>(); }
		}
#endif

#if ENABLE_ERROR_5
		if constexpr (HEATER_BED_RAW_LO_TEMP < HEATER_BED_RAW_HI_TEMP)
		{
			if (__unlikely((temperature_bed_raw >= Bed::max_temperature::Adc) & (target_temperature_bed > 0_C))) { max_temp_error<Manager::Bed>(); }
			if (__unlikely((Bed::min_temperature::Adc >= temperature_bed_raw) & (target_temperature_bed > 0_C))) { min_temp_error<Manager::Bed>(); }
		}
		else
		{
			if (__unlikely((temperature_bed_raw <= Bed::max_temperature::Adc) & (target_temperature_bed > 0_C))) { max_temp_error<Manager::Bed>(); }
			if (__unlikely((Bed::min_temperature::Adc <= temperature_bed_raw) & (target_temperature_bed > 0_C))) { min_temp_error<Manager::Bed>(); }
		}
#endif

    // reading from constexpr here.
    temperature_raw = Thermistor::clamp_adc(temperature_raw);
    temperature_bed_raw = Thermistor::clamp_adc(temperature_bed_raw);

    const auto previous_temperature = current_temperature;

		current_temperature = Temperature::adc_to_temperature(temperature_raw);
		current_temperature_bed = Temperature::adc_to_temperature(temperature_bed_raw);

    if (current_temperature >= previous_temperature)
    {
      const temp_t temperatureDiff = current_temperature - previous_temperature;
      temperatureTrendCalculator.appendValue(temperatureDiff, true);
    }
    else
    {
      const temp_t temperatureDiff = previous_temperature - current_temperature;
      temperatureTrendCalculator.appendValue(temperatureDiff, false);
    }

		// Reset the watchdog after we know we have a temperature measurement.
		Tuna::intrinsic::wdr();
		return true;
	}
	return false;
}

/**
 * Initialize the temperature manager
 * The manager is implemented by periodic calls to manage_heater()
 */
void Temperature::init()
{
	SET_OUTPUT(HEATER_0_PIN);
	SET_OUTPUT(HEATER_BED_PIN);

	SET_OUTPUT(FAN_PIN);

#define ANALOG_SELECT(pin) do{ SBI(DIDR0, pin); }while(0)

	// Set analog inputs
	ADCSRA = _BV(ADEN) | _BV(ADSC) | _BV(ADIF) | 0x07;
	DIDR0 = 0;
	ANALOG_SELECT(TEMP_0_PIN);
	ANALOG_SELECT(TEMP_BED_PIN);

	// Use timer0 for temperature measurement
	// Interleave temperature interrupt with millies interrupt
	OCR0B = 128;
	SBI(TIMSK0, OCIE0B);

	// Wait for temperature measurement to settle
	delay(250_u8);
}

/**
 * Start Heating Sanity Check for hotends that are below
 * their target temperature by a configurable margin.
 * This is called when the temperature is set. (M104, M109)
 */
void Temperature::start_watching_heater() {
	if (degHotend() < degTargetHotend() - uint8(WATCH_TEMP_INCREASE + TEMP_HYSTERESIS + 1)) {
		watch_target_temp = degHotend() + uint8(WATCH_TEMP_INCREASE);
		watch_heater_next_ms = millis() + (WATCH_TEMP_PERIOD) * 1000UL;
	}
	else
		watch_heater_next_ms = 0;
}

/**
 * Start Heating Sanity Check for hotends that are below
 * their target temperature by a configurable margin.
 * This is called when the temperature is set. (M140, M190)
 */
void Temperature::start_watching_bed() {
	if (degBed() < degTargetBed() - uint8(WATCH_BED_TEMP_INCREASE + TEMP_BED_HYSTERESIS + 1)) {
		watch_target_bed_temp = degBed() + (WATCH_BED_TEMP_INCREASE);
		watch_bed_next_ms = millis() + uint8(WATCH_BED_TEMP_PERIOD) * 1000UL;
	}
	else
		watch_bed_next_ms = 0;
}

Temperature::TRState Temperature::thermal_runaway_state_machine = TRInactive;
millis_t Temperature::thermal_runaway_timer = 0;

Temperature::TRState Temperature::thermal_runaway_bed_state_machine = TRInactive;
millis_t Temperature::thermal_runaway_bed_timer;

template <Temperature::Manager manager_type> static temp_t tr_target_temperature = 0_C;

template <Temperature::Manager manager_type>
void Temperature::thermal_runaway_protection(Temperature::TRState & __restrict state, millis_t & __restrict timer, arg_type<temp_t> current, arg_type<temp_t> target, arg_type<int> period_seconds, arg_type<int> hysteresis_degc)
{
	/**
		SERIAL_ECHO_START();
		SERIAL_ECHOPGM("Thermal Thermal Runaway Running. Heater ID: ");
		if (heater_id < 0) SERIAL_ECHOPGM("bed"); else SERIAL_ECHO(heater_id);
		SERIAL_ECHOPAIR(" ;  State:", *state);
		SERIAL_ECHOPAIR(" ;  Timer:", *timer);
		SERIAL_ECHOPAIR(" ;  Temperature:", current);
		SERIAL_ECHOPAIR(" ;  Target Temp:", target);
		if (heater_id >= 0)
		  SERIAL_ECHOPAIR(" ;  Idle Timeout:", heater_idle_timeout_exceeded[heater_id]);
		else
		  SERIAL_ECHOPAIR(" ;  Idle Timeout:", bed_idle_timeout_exceeded);
		SERIAL_EOL();
	*/

	// If the target temperature changes, restart
	if (tr_target_temperature<manager_type> != target) {
		tr_target_temperature<manager_type> = target;
		state = target > 0_C ? TRFirstHeating : TRInactive;
	}

  __assume(state == TRInactive || state == TRFirstHeating || state == TRStable || state == TRRunaway);

	switch (state) {
		// Inactive state waits for a target temperature to be set
	case TRInactive: break;
		// When first heating, wait for the temperature to be reached then go to Stable state
	case TRFirstHeating:
    if (current < tr_target_temperature<manager_type>)
    {
      break;
    }
		state = TRStable;
		// While the temperature is stable watch for a bad temperature
	case TRStable:
		if (current >= tr_target_temperature<manager_type> -hysteresis_degc) {
			timer = millis() + period_seconds * 1000UL;
			break;
		}
    else if (PENDING(millis(), timer))
    {
      break;
    }
		state = TRRunaway;
	case TRRunaway:
    // This branch should get compiled away.
    if (__unlikely(state == TRRunaway))
    {
      _temp_error<manager_type>(PSTR(MSG_T_THERMAL_RUNAWAY), PSTR(MSG_THERMAL_RUNAWAY));
    }
	}
}

//template void Temperature::thermal_runaway_protection<Temperature::Manager::Hotend>(Temperature::TRState & __restrict state, millis_t & __restrict timer, arg_type<temp_t> current, arg_type<temp_t> target, arg_type<int> period_seconds, arg_type<int> hysteresis_degc);
//template void Temperature::thermal_runaway_protection<Temperature::Manager::Bed>(Temperature::TRState & __restrict state, millis_t & __restrict timer, arg_type<temp_t> current, arg_type<temp_t> target, arg_type<int> period_seconds, arg_type<int> hysteresis_degc);

void Temperature::disable_all_heaters() {

	planner.autotemp_enabled = false;

	setTargetHotend(0);
	setTargetBed(0);

	// If all heaters go down then for sure our print job has stopped
	print_job_timer.stop();

	setTargetHotend(0);
	soft_pwm_amount.write_through(0);
	WRITE_HEATER_0(LOW);

	target_temperature_bed = 0;
  if constexpr(has_bed_thermal_management)
  {
    soft_pwm_amount_bed = 0;
  }
  else
  {
    is_bed_heating = false;
  }
	WRITE_HEATER_BED(LOW);
}

/**
* States for ADC reading in the ISR
*/
namespace
{
  enum class sensor_state : uint8
  {
    initialize_hotend = 0,
    read_hotend,
    initialize_bed,
    read_bed,
    ready,
  };

  inline sensor_state __forceinline __flatten operator ++ (sensor_state & __restrict val, int)
  {
    using underlying_t = underlying_type<sensor_state>;

    const sensor_state ret = val;
    val = sensor_state(underlying_t(ret) + 1_u8);
    __assume(underlying_t(val) >= underlying_t(sensor_state::initialize_hotend));
    __assume(underlying_t(val) <= underlying_t(sensor_state::ready));
    return ret;
  }
}

template <uint8 pin>
inline void __forceinline __flatten start_adc()
{
  if constexpr (pin > 7)
    ADCSRB = _BV(MUX5);
  else
    ADCSRB = 0_u8;

  ADMUX = _BV(REFS0) | (pin & 0x07_u8);
  SBI(ADCSRA, ADSC);
}

template <uint8 pin, bool superscalar = false>
inline void __forceinline __flatten set_pin(bool state)
{
  // TODO figure out why their original code uses critical sections when the port address >= 0x100.

  volatile uint8 * __restrict port_ptr;
  uint8 port_pin_mask;

  switch (pin)
  {
  case 4: { // HEATER_0_PIN
    port_ptr = &DIO4_WPORT;
    port_pin_mask = 1_u8 << uint8(DIO4_PIN);
  } break;
  case 3: { // HEATER_BED_PIN
    port_ptr = &DIO3_WPORT;
    port_pin_mask = 1_u8 << uint8(DIO3_PIN);
  } break;
  }

  volatile uint8 & __restrict port = *port_ptr;

  // For some reason, the superscalar version generates better code.
  // https://graphics.stanford.edu/~seander/bithacks.html#ConditionalSetOrClearBitsWithoutBranching
  // However, it can only be used in contexts where ISRs are disabled,
  // as it is a load-store, rather than a direct alteration.

  if constexpr(superscalar)
  {
    port = (port & ~port_pin_mask) | ((state ? 0xFF_u8 : 0x00_u8) & port_pin_mask);
  }
  else
  {
    if (state) // high
    {
      port |= port_pin_mask;
    }
    else // low
    {
      port &= ~port_pin_mask;
    }
  }
}

/**
* Timer 0 is shared with millies so don't change the prescaler.
*
* This ISR uses the compare method so it runs at the base
* frequency (16 MHz / 64 / 256 = 976.5625 Hz), but at the TCNT0 set
* in OCR0B above (128 or halfway between OVFs).
*
*  - Manage PWM to all the heaters and fan
*  - Prepare or Measure one of the raw ADC sensor values
*  - Check new temperature values for MIN/MAX errors (kill on error)
*  - Step the babysteps value for each axis towards 0
*  - For PINS_DEBUGGING, monitor and report endstop pins
*  - For ENDSTOP_INTERRUPTS_FEATURE check endstops if flagged
*/
__signal(TIMER0_COMPB) { Temperature::isr(); }

template <typename T, uint8 count>
class running_average final
{
  static constexpr const uint64 max_value = uint64(type_trait<T>::max) * count;
  using mul_t = uintsz<max_value>;

  mul_t value_ = 0;

public:
  constexpr running_average(arg_type<T> def_value) : value_(def_value) {}
  constexpr running_average() = default;

  operator T () const __restrict
  {
    __assume(value_ <= max_value);
    return T(value_ / count);
  }

  running_average & operator += (arg_type<T> new_value) __restrict
  {
    __assume(value_ <= max_value);
    mul_t cur_value = value_;
    cur_value -= T(*this);
    cur_value += new_value;
    value_ = cur_value;

    return *this;
  }
};

void __forceinline __flatten Temperature::isr()
{
	static uint8 oversample_count = 0;
  static sensor_state adc_sensor_state = sensor_state::initialize_hotend;

  static constexpr const uint8 temp_avg_count = 32;

  // ADC read/handle
  {
    /**
    * One sensor is sampled on every other call of the ISR.
    * Each sensor is read 16 (OVERSAMPLENR) times, taking the average.
    *
    * On each Prepare pass, ADC is started for a sensor pin.
    * On the next pass, the ADC value is read and accumulated.
    *
    * This gives each ADC 0.9765ms to charge up.
    */

    // TODO add a max value expected.
    static running_average<uint16, 32> local_raw_adc_hotend;
    static running_average<uint16, 32> local_raw_adc_bed;

    switch (adc_sensor_state++)
    {
    case sensor_state::initialize_hotend:
    {
      start_adc<TEMP_0_PIN>();
    } break;
    case sensor_state::read_hotend:
    {
      local_raw_adc_hotend += (uint16(ADC) * OVERSAMPLENR);
    } break;
    case sensor_state::initialize_bed:
    {
      start_adc<TEMP_BED_PIN>();
    } break;
    case sensor_state::read_bed:
    {
      local_raw_adc_bed += (uint16(ADC) * OVERSAMPLENR);

      {
        // Update the raw values.
        interrupt::set_adc(uint16(local_raw_adc_hotend), uint16(local_raw_adc_bed));
      }

      adc_sensor_state = sensor_state::initialize_hotend;
    }
    }
  }

  static constexpr const uint8 skip_mask = 8; // Only run every Nth times this ISR is hit.
  static uint8 skip_counter = 0;
  if ((++skip_counter % skip_mask) != 0)
  {
    return;
  }

  const uint8_t extruder_pwm = soft_pwm_amount.read_through();
  const uint8_t bed_pwm = []() -> uint8 {
    if constexpr(has_bed_thermal_management)
    {
      return soft_pwm_amount_bed;
    }
    else
    {
      return is_bed_heating ? 0xFF : 0;
    }
  }();

  // If 'false', ISR PWM sequences will look like:
  // 111111111111000000000011111111111100000000
  // If 'true':
  // 101010101010101010101010101010101010101010
  constexpr const bool uniform_distributed_pwm = false;

  if constexpr (uniform_distributed_pwm)
  {
    static uint16 pwm_counter = 0;

    static uint16 extruder_intercept = 0;
    static uint16 bed_intercept = 0;

    static bool extruder_on = false;
    static bool bed_on = false;

    if (__likely(extruder_pwm > 0))
    {
      const bool intercepted = __unlikely(pwm_counter == extruder_intercept);
      if (__unlikely(!extruder_on) || intercepted)
      {
        const uint16 pwm_iter = type_trait<uint16>::max / uint16(uint16(extruder_pwm) << 8);
        extruder_intercept = pwm_counter + pwm_iter;
      }

      set_pin<HEATER_0_PIN>(__unlikely(intercepted));
      extruder_on = true;
    }
    else
    {
      set_pin<HEATER_0_PIN>(false);
      extruder_on = false;
    }

    if (__likely(bed_pwm > 0))
    {
      const bool intercepted = __unlikely(pwm_counter == bed_intercept);
      if (__unlikely(!extruder_on) || intercepted)
      {
        const uint16 pwm_iter = type_trait<uint16>::max / uint16(uint16(bed_pwm) << 8);
        bed_intercept = pwm_counter + pwm_iter;
      }

      set_pin<HEATER_BED_PIN>(__unlikely(intercepted));

      bed_on = true;
    }
    else
    {
      set_pin<HEATER_BED_PIN>(false);
      bed_on = false;
    }

    ++pwm_counter;
  }
  else
  {
    static uint8 pwm_counter = 0;

    // TODO : better heater state tracking.

    const bool new_extruder_state = (pwm_counter <= extruder_pwm && __likely(extruder_pwm > 0));
    const bool new_bed_state = (pwm_counter <= bed_pwm && __likely(bed_pwm > 0));

    ++pwm_counter;

    set_pin<HEATER_0_PIN>(new_extruder_state);
    set_pin<HEATER_BED_PIN>(new_bed_state);
  }
}
