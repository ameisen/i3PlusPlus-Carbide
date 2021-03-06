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
  * temperature.hpp - temperature controller
  */

#pragma once

#include "MarlinConfig.h"

namespace Tuna
{
  constexpr const auto printer_max_temperature = make_uintsz<300>;

  // generate a fixed-precision type that can hold up to at least the maximum temperature,
  // and has at least 4 bits of decimal precision.
  using temp_t = Tuna::fixedsz<printer_max_temperature, 4>;

  constexpr temp_t operator "" _C(long double temperature)
  {
    return { float(temperature) };
  }

  constexpr temp_t operator "" _C(unsigned long long int temperature)
  {
    return { (typename temp_t::type)(temperature) };
  }
}

#include "thermistors/thermistortables.h"

namespace Tuna
{
  // TODO move this to a better place
  static constexpr const bool has_bed_thermal_management = false;

  class Temperature final : trait::ce_only
  {
  public:
    // Minimum number of Temperature::ISR loops between sensor readings.
    // Multiplied by 16 (OVERSAMPLENR) to obtain the total time to
    // get all oversampled sensor readings
    static constexpr const uint8 ACTUAL_ADC_SAMPLES = 10;

	  enum class Manager : uint8
	  {
		  Hotend = 0,
		  Bed = 1
	  };

    enum class Trend : bool
    {
      Up = true,
      Down = false,
    };

	  static constexpr uint8 num_hotends = 1;
	  static constexpr uint8 num_beds = 1;

    static temp_t min_extrude_temp;

	  template <uint16 Celcius>
	  struct TemperatureValueConverter final : trait::ce_only
	  {
      static constexpr const temp_t Temperature = temp_t{Celcius};
		  static constexpr const auto Adc = make_uintsz<Thermistor::ce_convert_temp_to_adc<Temperature.raw()>()>;
	  };

	  struct Hotend final : trait::ce_only
	  {
		  using max_temperature = TemperatureValueConverter<HEATER_0_MAXTEMP>;
		  using min_temperature = TemperatureValueConverter<HEATER_0_MINTEMP>;
      static_assert(max_temperature::Adc != min_temperature::Adc, "these values should never be equal.");
      static_assert(max_temperature::Temperature != min_temperature::Temperature, "these values should never be equal.");
	  };

    struct Bed final : trait::ce_only
	  {
		  using max_temperature = TemperatureValueConverter<BED_MAXTEMP>;
		  using min_temperature = TemperatureValueConverter<BED_MINTEMP>;
      static_assert(max_temperature::Adc != min_temperature::Adc, "these values should never be equal.");
      static_assert(max_temperature::Temperature != min_temperature::Temperature, "these values should never be equal.");
	  };

    static temp_t current_temperature;
		static temp_t current_temperature_bed;
	  static temp_t target_temperature;
	  static temp_t target_temperature_bed;

	  static volatile bool in_temp_isr;

    static memory<uint8_t> soft_pwm_amount;
    static volatile_conditional_type<uint8, has_bed_thermal_management> soft_pwm_amount_bed;
    static volatile_conditional_type<bool, !has_bed_thermal_management> is_bed_heating;

	  static temp_t watch_target_temp;
	  static millis_t watch_heater_next_ms;

	  static temp_t watch_target_bed_temp;
	  static millis_t watch_bed_next_ms;

	  static bool allow_cold_extrude;
	  static __pure bool is_coldextrude() {
		  return allow_cold_extrude ? false : degHotend() < min_extrude_temp;
	  }

  private:
	  static millis_t next_bed_check_ms;

  public:
	  /**
	   * Instance Methods
	   */

	  static void init();

    static Trend __forceinline __flatten get_temperature_trend();

	  /**
	   * Static (class) methods
	   */
	  static temp_t adc_to_temperature(arg_type<uint16> raw);

	  /**
	   * Called from the Temperature ISR
	   */
	  static void isr();

	  /**
	   * Call periodically to manage heaters
	   */
	  static bool manage_heater();

	  /**
	   * Preheating hotends
	   */
	  static __const constexpr bool is_preheating() { return false; }

	   //high level conversion routines, for use outside of temperature.cpp
	   //inline so that there is no performance decrease.
	   //deg=degreeCelsius

	  static __pure const temp_t & degHotend() { return current_temperature; }
	  static __pure const temp_t & degBed() { return current_temperature_bed; }

	  static __pure const temp_t & degTargetHotend() { return target_temperature; }

	  static __pure const temp_t & degTargetBed() { return target_temperature_bed; }

	  static void start_watching_heater();

	  static void start_watching_bed();

	  static void setTargetHotend(arg_type<temp_t> celsius) {
		  target_temperature = celsius;
		  start_watching_heater();
	  }

	  static void setTargetBed(arg_type<temp_t> celsius) {
		  target_temperature_bed = min(celsius, temp_t(BED_MAXTEMP));
		  start_watching_bed();
	  }

	  static __pure bool isHeatingHotend() {
		  return current_temperature <= target_temperature;
	  }
	  static __pure bool isHeatingBed() { return current_temperature_bed <= target_temperature_bed; }

	  static __pure bool isCoolingHotend() {
		  return current_temperature > target_temperature;
	  }
	  static __pure bool isCoolingBed() { return current_temperature_bed > target_temperature_bed; }

	  /**
	   * The software PWM power for a heater
	   */
	  template <Manager manager_type>
	  static __pure uint8 __forceinline __flatten getHeaterPower();

	  /**
	   * Switch off all heaters, set all target temperatures to 0
	   */
	  static void disable_all_heaters();

	  /**
	   * Perform auto-tuning for hotend or bed in response to M303
	   */
	  static void PID_autotune(arg_type<temp_t> temp, arg_type<int> ncycles, bool set_result = false);

	  /**
	   * Update the temp manager when PID values change
	   */
	  static void updatePID();

  private:
	  static bool updateTemperaturesFromRawValues();

	  static void checkExtruderAutoFans();

	  template <Manager manager_type>
	  static void _temp_error(const char * __restrict const serial_msg, const char * __restrict const lcd_msg);
	  template <Manager manager_type>
	  static void min_temp_error();
	  template <Manager manager_type>
	  static void max_temp_error();

	  typedef enum TRState { TRInactive, TRFirstHeating, TRStable, TRRunaway } TRstate;

	  template <Manager manager_type>
	  static void thermal_runaway_protection(TRState & __restrict state, millis_t & __restrict timer, arg_type<temp_t> temperature, arg_type<temp_t> target_temperature, arg_type<int> period_seconds, arg_type<int> hysteresis_degc);

	  static TRState thermal_runaway_state_machine;
	  static millis_t thermal_runaway_timer;

	  static TRState thermal_runaway_bed_state_machine;
	  static millis_t thermal_runaway_bed_timer;

  };

}
