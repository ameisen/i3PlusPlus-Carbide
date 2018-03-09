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
 * planner.cpp
 *
 * Buffer movement commands and manage the acceleration profile plan
 *
 * Derived from Grbl
 * Copyright (c) 2009-2011 Simen Svale Skogsrud
 *
 * The ring buffer implementation gleaned from the wiring_serial library by David A. Mellis.
 *
 *
 * Reasoning behind the mathematics in this module (in the key of 'Mathematica'):
 *
 * s == speed, a == acceleration, t == time, d == distance
 *
 * Basic definitions:
 *   Speed[s_, a_, t_] := s + (a*t)
 *   Travel[s_, a_, t_] := Integrate[Speed[s, a, t], t]
 *
 * Distance to reach a specific speed with a constant acceleration:
 *   Solve[{Speed[s, a, t] == m, Travel[s, a, t] == d}, d, t]
 *   d -> (m^2 - s^2)/(2 a) --> estimate_acceleration_distance()
 *
 * Speed after a given distance of travel with constant acceleration:
 *   Solve[{Speed[s, a, t] == m, Travel[s, a, t] == d}, m, t]
 *   m -> Sqrt[2 a d + s^2]
 *
 * DestinationSpeed[s_, a_, d_] := Sqrt[2 a d + s^2]
 *
 * When to start braking (di) to reach a specified destination speed (s2) after accelerating
 * from initial speed s1 without ever stopping at a plateau:
 *   Solve[{DestinationSpeed[s1, a, di] == DestinationSpeed[s2, a, d - di]}, di]
 *   di -> (2 a d - s1^2 + s2^2)/(4 a) --> intersection_distance()
 *
 * IntersectionDistance[s1_, s2_, a_, d_] := (2 a d - s1^2 + s2^2)/(4 a)
 *
 */

#include <tuna.h>

#include "planner.h"
#include "bi3_plus_lcd.h"
#include "stepper.h"
#include "thermal/thermal.hpp"
#include "language.h"
#include "gcode.h"

#if ENABLED(MESH_BED_LEVELING)
  #include "mesh_bed_leveling.h"
#endif

Planner planner;

  // public:

/**
 * A ring buffer of moves described in steps
 */
block_t Planner::block_buffer[BLOCK_BUFFER_SIZE];
volatile uint8_t Planner::block_buffer_head = 0,           // Index of the next block to be pushed
                 Planner::block_buffer_tail = 0;

float Planner::max_feedrate_mm_s[XYZE_N], // Max speeds in mm per second
      Planner::axis_steps_per_mm[XYZE_N],
      Planner::steps_to_mm[XYZE_N];

//
// i3++
//
Planner::temperature_preset Planner::preheat_presets[3];


#if ENABLED(DISTINCT_E_FACTORS)
  uint8_t Planner::last_extruder = 0;     // Respond to extruder change
#endif

uint32 Planner::max_acceleration_steps_per_s2[XYZE_N],
         Planner::max_acceleration_mm_per_s2[XYZE_N]; // Use M201 to override by software

millis_t Planner::min_segment_time;
float Planner::min_feedrate_mm_s,
      Planner::acceleration,         // Normal acceleration mm/s^2  DEFAULT ACCELERATION for all printing moves. M204 SXXXX
      Planner::retract_acceleration, // Retract acceleration mm/s^2 filament pull-back and push-forward while standing still in the other axes M204 TXXXX
      Planner::travel_acceleration,  // Travel acceleration mm/s^2  DEFAULT ACCELERATION for all NON printing moves. M204 MXXXX
      Planner::max_jerk[XYZE],       // The largest speed change requiring no acceleration
      Planner::min_travel_feedrate_mm_s;

#if HAS_ABL
  bool Planner::abl_enabled = false; // Flag that auto bed leveling is enabled
#endif

#if ABL_PLANAR
  matrix_3x3 Planner::bed_level_matrix; // Transform to compensate for bed level
#endif

#if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
  float Planner::z_fade_height,
        Planner::inverse_z_fade_height;
#endif

#if ENABLED(AUTOTEMP)
  float Planner::autotemp_max = 250,
        Planner::autotemp_min = 210,
        Planner::autotemp_factor = 0.1;
  bool Planner::autotemp_enabled = false;
#endif

// private:

uint24 Planner::position[NUM_AXIS] = { 0 };

uint32 Planner::cutoff_long;

float Planner::previous_speed[NUM_AXIS],
      Planner::previous_nominal_speed;

#if ENABLED(DISABLE_INACTIVE_EXTRUDER)
  uint8_t Planner::g_uc_extruder_last_move[EXTRUDERS] = { 0 };
#endif

#ifdef XY_FREQUENCY_LIMIT
  // Old direction bits. Used for speed calculations
  unsigned char Planner::old_direction_bits = 0;
  // Segment times (in µs). Used for speed calculations
  long Planner::axis_segment_time[2][3] = { {MAX_FREQ_TIME + 1, 0, 0}, {MAX_FREQ_TIME + 1, 0, 0} };
#endif

#if ENABLED(LIN_ADVANCE)
  float Planner::extruder_advance_k = LIN_ADVANCE_K,
        Planner::advance_ed_ratio = LIN_ADVANCE_E_D_RATIO,
        Planner::position_float[NUM_AXIS] = { 0 };
#endif

#if ENABLED(ULTRA_LCD)
  volatile uint32 Planner::block_buffer_runtime_us = 0;
#endif

/**
 * Class and Instance Methods
 */

Planner::Planner() { init(); }

void Planner::init() {
  block_buffer_head = block_buffer_tail = 0;
  ZERO(position);
  #if ENABLED(LIN_ADVANCE)
    ZERO(position_float);
  #endif
  ZERO(previous_speed);
  previous_nominal_speed = 0.0;
  #if ABL_PLANAR
    bed_level_matrix.set_to_identity();
  #endif
}

#define MINIMAL_STEP_RATE 120

/**
 * Calculate trapezoid parameters, multiplying the entry- and exit-speeds
 * by the provided factors.
 */
void __forceinline __flatten Planner::calculate_trapezoid_for_block(block_t * __restrict const block, const float & __restrict entry_speed, const float & __restrict next_entry_speed) {
  //float nominal_recip = 1.0f / nominal_speed;
  uint32 initial_rate = CEIL(entry_speed),
           final_rate = CEIL(next_entry_speed); // (steps per second)

  // Limit minimal step rate (Otherwise the timer will overflow.)
  NOLESS(initial_rate, MINIMAL_STEP_RATE);
  NOLESS(final_rate, MINIMAL_STEP_RATE);

  int32 accel = block->acceleration_steps_per_s2;
  int32 accelerate_steps = CEIL(estimate_acceleration_distance(initial_rate, block->nominal_rate, accel));
  int32 decelerate_steps = FLOOR(estimate_acceleration_distance(block->nominal_rate, final_rate, -accel));
  int32 plateau_steps = block->step_event_count - accelerate_steps - decelerate_steps;

  // Is the Plateau of Nominal Rate smaller than nothing? That means no cruising, and we will
  // have to use intersection_distance() to calculate when to abort accel and start braking
  // in order to reach the final_rate exactly at the end of this block.
  if (plateau_steps < 0) {
    accelerate_steps = CEIL(intersection_distance(initial_rate, final_rate, accel, block->step_event_count));
    NOLESS(accelerate_steps, 0); // Check limits due to numerical round-off
    accelerate_steps = min(uint32(accelerate_steps), uint32(block->step_event_count));//(We can cast here to unsigned, because the above line ensures that we are above zero)
    plateau_steps = 0;
  }

  // Fill variables used by the stepper in a critical section
  {
    Tuna::critical_section _critsec;
    if (!TEST(block->flag, BLOCK_BIT_BUSY)) { // Don't update variables if block is busy.
      block_t * __restrict out = as<block_t * __restrict>(block);
      out->accelerate_until = accelerate_steps;
      out->decelerate_after = accelerate_steps + plateau_steps;
      out->initial_rate = initial_rate;
      out->final_rate = final_rate;
      out->acceleration_rate = int24(accel * 16777216.0 / ((F_CPU) * 0.125)); // * 8.388608

      /*
      float initial_component = float(out->accelerate_until) / float(out->step_event_count);
      float final_component = float(out->step_event_count - out->decelerate_after) / float(out->step_event_count);

      if (initial_component + final_component >= 0.99)
      {
        out->plateau_rate = out->nominal_rate;
      }
      else
      {
        out->plateau_rate = uint24((out->nominal_rate - (float(out->final_rate * final_component)) - (float(out->initial_rate) * initial_component)) + 0.5);
      }
      */
    }
  }
}

// "Junction jerk" in this context is the immediate change in speed at the junction of two blocks.
// This method will calculate the junction jerk as the euclidean distance between the nominal
// velocities of the respective blocks.
//inline float junction_jerk(block_t *before, block_t *after) {
//  return SQRT(
//    POW((before->speed_x-after->speed_x), 2)+POW((before->speed_y-after->speed_y), 2));
//}


// The kernel called by recalculate() when scanning the plan from last to first entry.
void __forceinline __flatten Planner::reverse_pass_kernel(block_t * __restrict const current, const block_t * __restrict next) {
  if (!current || !next) return;
  // If entry speed is already at the maximum entry speed, no need to recheck. Block is cruising.
  // If not, block in state of acceleration or deceleration. Reset entry speed to maximum and
  // check for maximum allowable speed reductions to ensure maximum possible planned speed.
  float max_entry_speed = current->max_entry_speed;
  if (__unlikely(current->entry_speed != max_entry_speed)) {
    // If nominal length true, max junction speed is guaranteed to be reached. Only compute
    // for max allowable speed if block is decelerating and nominal length is false.
    current->entry_speed = (TEST(current->flag, BLOCK_BIT_NOMINAL_LENGTH) || max_entry_speed <= next->entry_speed)
      ? max_entry_speed
      : min(max_entry_speed, max_allowable_speed(-current->acceleration, next->entry_speed, current->millimeters));
    SBI(current->flag, BLOCK_BIT_RECALCULATE);
  }
}

/**
 * recalculate() needs to go over the current plan twice.
 * Once in reverse and once forward. This implements the reverse pass.
 */
void __forceinline __flatten Planner::reverse_pass() {

  if (movesplanned() > 3) {
    const uint8_t endnr = BLOCK_MOD(block_buffer_tail + 2); // tail is running. tail+1 shouldn't be altered because it's connected to the running block.
                                                            // tail+2 because the index is not yet advanced when checked
    uint8_t blocknr = prev_block_index(block_buffer_head);
    block_t* __restrict current = &block_buffer[blocknr];

    do {
      const block_t * __restrict const next = current;
      blocknr = prev_block_index(blocknr);
      current = &block_buffer[blocknr];
      if (TEST(current->flag, BLOCK_BIT_START_FROM_FULL_HALT)) // Up to this every block is already optimized.
        break;
      reverse_pass_kernel(current, next);
    } while (blocknr != endnr);
  }
}

// The kernel called by recalculate() when scanning the plan from first to last entry.
void __forceinline __flatten Planner::forward_pass_kernel(const block_t * __restrict previous, block_t * __restrict const current) {
  if (!previous) return;

  // If the previous block is an acceleration block, but it is not long enough to complete the
  // full speed change within the block, we need to adjust the entry speed accordingly. Entry
  // speeds have already been reset, maximized, and reverse planned by reverse planner.
  // If nominal length is true, max junction speed is guaranteed to be reached. No need to recheck.
  if (!TEST(previous->flag, BLOCK_BIT_NOMINAL_LENGTH)) {
    if (previous->entry_speed < current->entry_speed) {
      float entry_speed = min(current->entry_speed,
                               max_allowable_speed(-previous->acceleration, previous->entry_speed, previous->millimeters));
      // Check for junction speed change
      if (current->entry_speed != entry_speed) {
        current->entry_speed = entry_speed;
        SBI(current->flag, BLOCK_BIT_RECALCULATE);
      }
    }
  }
}

/**
 * recalculate() needs to go over the current plan twice.
 * Once in reverse and once forward. This implements the forward pass.
 */
void __forceinline __flatten Planner::forward_pass() {
  block_t * __restrict block[3] = { nullptr, nullptr, nullptr };

  for (uint8_t b = block_buffer_tail; b != block_buffer_head; b = next_block_index(b)) {
    block[0] = block[1];
    block[1] = block[2];
    block[2] = as<block_t * __restrict>(&block_buffer[b]);
    forward_pass_kernel(block[0], block[1]);
  }
  forward_pass_kernel(block[1], block[2]);
}

/**
 * Recalculate the trapezoid speed profiles for all blocks in the plan
 * according to the entry_factor for each junction. Must be called by
 * recalculate() after updating the blocks.
 */
void __forceinline __flatten Planner::recalculate_trapezoids() {
  uint8 block_index = block_buffer_tail;
  block_t * __restrict next = nullptr;

  while (block_index != block_buffer_head) {
    block_t * __restrict current = next;
    next = as<block_t * __restrict>(&block_buffer[block_index]);
    if (current) {
      // Recalculate if current block entry or exit junction speed has changed.
      if (TEST(current->flag, BLOCK_BIT_RECALCULATE) || TEST(next->flag, BLOCK_BIT_RECALCULATE)) {
        const float old_entry_speed = next->entry_speed;
        const uint8 direction_bits_delta = next->direction_bits ^ current->direction_bits;

        constexpr const bool NEW_SPEEDER = false;
        constexpr const bool ALLOW_INVERSE_DIRECTION_SMOOTHING = true;
        constexpr const bool SCALE_AXIAL_VELOCITIES = true;

        if constexpr(NEW_SPEEDER)
        {
          if (ALLOW_INVERSE_DIRECTION_SMOOTHING || !direction_bits_delta)
          {
            const float velocity_scalar = next->nominal_rate / current->nominal_rate;

            const float current_adjusted_steps[3] = {
              current->steps[0] * steps_to_mm[X_AXIS],
              current->steps[1] * steps_to_mm[Y_AXIS],
              current->steps[2] * steps_to_mm[Z_AXIS],
            };
            // Treat them as a vector, and normalize them. We then use the components as multipliers to break down
            // the velocity.
            const float current_vector_length = sqrt(square(current_adjusted_steps[0]) + square(current_adjusted_steps[1]) + square(current_adjusted_steps[2]));
            float current_vector_length_recip = 0.0f;
            if (current_vector_length)
            {
              current_vector_length_recip = 1.0f / current_vector_length;
            }
            const float current_frac[3] = {
              current_adjusted_steps[0] * current_vector_length_recip,
              current_adjusted_steps[1] * current_vector_length_recip,
              current_adjusted_steps[2] * current_vector_length_recip
            };
            // We should recalculate entry_speed for next as well while we are here.
            const float current_axis_velocity[3] = {
              (current->nominal_speed * current_frac[0]),
              (current->nominal_speed * current_frac[1]),
              (current->nominal_speed * current_frac[2])
            };


            const float current_mult[3] = {
              (SCALE_AXIAL_VELOCITIES) ? (current_frac[0] * velocity_scalar) : (current_frac[0] == 0.0f) ? 0.0f : 1.0f,
              (SCALE_AXIAL_VELOCITIES) ? (current_frac[1] * velocity_scalar) : (current_frac[1] == 0.0f) ? 0.0f : 1.0f,
              (SCALE_AXIAL_VELOCITIES) ? (current_frac[2] * velocity_scalar) : (current_frac[2] == 0.0f) ? 0.0f : 1.0f
            };



            const float next_adjusted_steps[3] = {
              (next->steps[0] * steps_to_mm[X_AXIS]) * current_mult[0],
              (next->steps[1] * steps_to_mm[Y_AXIS]) * current_mult[1],
              (next->steps[2] * steps_to_mm[Z_AXIS]) * current_mult[2],
            };
            // Treat them as a vector, and normalize them. We then use the components as multipliers to break down
            // the velocity.
            const float next_vector_length = sqrt(square(next_adjusted_steps[0]) + square(next_adjusted_steps[1]) + square(next_adjusted_steps[2]));
            float next_vector_length_recip = 0.0f;
            if (next_vector_length)
            {
              next_vector_length_recip = 1.0f / next_vector_length;
            }
            const float next_frac[3] = {
              next_adjusted_steps[0] * next_vector_length_recip,
              next_adjusted_steps[1] * next_vector_length_recip,
              next_adjusted_steps[2] * next_vector_length_recip
            };
            // We should recalculate entry_speed for next as well while we are here.
            const float next_axis_velocity[3] = {
              TEST(direction_bits_delta, X_AXIS) ? 0 : (next->nominal_speed * next_frac[0]),
              TEST(direction_bits_delta, Y_AXIS) ? 0 : (next->nominal_speed * next_frac[1]),
              TEST(direction_bits_delta, Z_AXIS) ? 0 : (next->nominal_speed * next_frac[2])
            };

            const float new_velocity = sqrt(square(next_axis_velocity[0]) + square(next_axis_velocity[1]) + square(next_axis_velocity[2]));
            next->entry_speed = max(0.0f, new_velocity);
          }
          else
          {
            next->entry_speed = 0.0f;
          }
        }

        calculate_trapezoid_for_block(current, current->entry_speed, next->entry_speed);
        CBI(current->flag, BLOCK_BIT_RECALCULATE); // Reset current only to ensure next trapezoid is computed
      }
    }
    block_index = next_block_index(block_index);
  }
  // Last/newest block in buffer. Exit speed is set with MINIMUM_PLANNER_SPEED. Always recalculated.
  if (next) {
    calculate_trapezoid_for_block(next, next->entry_speed, 0.0f);
    CBI(next->flag, BLOCK_BIT_RECALCULATE);
  }
}

/*
 * Recalculate the motion plan according to the following algorithm:
 *
 *   1. Go over every block in reverse order...
 *
 *      Calculate a junction speed reduction (block_t.entry_factor) so:
 *
 *      a. The junction jerk is within the set limit, and
 *
 *      b. No speed reduction within one block requires faster
 *         deceleration than the one, true constant acceleration.
 *
 *   2. Go over every block in chronological order...
 *
 *      Dial down junction speed reduction values if:
 *      a. The speed increase within one block would require faster
 *         acceleration than the one, true constant acceleration.
 *
 * After that, all blocks will have an entry_factor allowing all speed changes to
 * be performed using only the one, true constant acceleration, and where no junction
 * jerk is jerkier than the set limit, Jerky. Finally it will:
 *
 *   3. Recalculate "trapezoids" for all blocks.
 */
void __forceinline __flatten Planner::recalculate() {
  reverse_pass();
  forward_pass();
  recalculate_trapezoids();
}


#if ENABLED(AUTOTEMP)

  void Planner::getHighESpeed() {
    static float oldt = 0;

    if (__likely(!autotemp_enabled)) return;
    if (float(Temperature::degTargetHotend() + 2) < autotemp_min) return; // probably temperature set to zero.

    float high = 0.0;
    for (uint8_t b = block_buffer_tail; b != block_buffer_head; b = next_block_index(b)) {
      const block_t & __restrict block = as<const block_t & __restrict>(block_buffer[b]);
      if (block.steps[X_AXIS] || block.steps[Y_AXIS] || block.steps[Z_AXIS]) {
        float se = (float)block.steps[E_AXIS] / block.step_event_count * block.nominal_speed; // mm/sec;
        NOLESS(high, se);
      }
    }

    float t = autotemp_min + high * autotemp_factor;
    t = clamp(t, autotemp_min, autotemp_max);
    if (t < oldt) t = t * (1 - (AUTOTEMP_OLDWEIGHT)) + oldt * (AUTOTEMP_OLDWEIGHT);
    oldt = t;
    Temperature::setTargetHotend(t);
  }

#endif // AUTOTEMP

/**
 * Maintain fans, paste extruder pressure,
 */
void __forceinline __flatten Planner::check_axes_activity() {
  unsigned char axis_active[NUM_AXIS] = { 0 },
                tail_fan_speed[FAN_COUNT];

  #if FAN_COUNT > 0
    for (uint8_t i = 0; i < FAN_COUNT; i++) tail_fan_speed[i] = fanSpeeds[i];
  #endif

  #if ENABLED(BARICUDA)
    #if HAS_HEATER_1
      unsigned char tail_valve_pressure = baricuda_valve_pressure;
    #endif
    #if HAS_HEATER_2
      unsigned char tail_e_to_p_pressure = baricuda_e_to_p_pressure;
    #endif
  #endif

  if (blocks_queued()) {

    #if FAN_COUNT > 0
      for (uint8_t i = 0; i < FAN_COUNT; i++) tail_fan_speed[i] = as<const block_t & __restrict>(block_buffer[block_buffer_tail]).fan_speed[i];
    #endif

    #if ENABLED(BARICUDA)
      block = &block_buffer[block_buffer_tail];
      #if HAS_HEATER_1
        tail_valve_pressure = block->valve_pressure;
      #endif
      #if HAS_HEATER_2
        tail_e_to_p_pressure = block->e_to_p_pressure;
      #endif
    #endif

    for (uint8_t b = block_buffer_tail; b != block_buffer_head; b = next_block_index(b)) {
      const block_t & __restrict block = as<const block_t & __restrict>(block_buffer[b]);
      LOOP_XYZE(i) if (block.steps[i]) axis_active[i]++;
    }
  }
  #if ENABLED(DISABLE_X)
    if (!axis_active[X_AXIS]) disable_X();
  #endif
  #if ENABLED(DISABLE_Y)
    if (!axis_active[Y_AXIS]) disable_Y();
  #endif
  #if ENABLED(DISABLE_Z)
    if (!axis_active[Z_AXIS]) disable_Z();
  #endif
  #if ENABLED(DISABLE_E)
    if (!axis_active[E_AXIS]) disable_e_steppers();
  #endif

  #if FAN_COUNT > 0

    #ifdef FAN_MIN_PWM
      #define CALC_FAN_SPEED(f) (tail_fan_speed[f] ? ( FAN_MIN_PWM + (tail_fan_speed[f] * (255 - FAN_MIN_PWM)) / 255 ) : 0)
    #else
      #define CALC_FAN_SPEED(f) tail_fan_speed[f]
    #endif

    #ifdef FAN_KICKSTART_TIME

      static millis_t fan_kick_end[FAN_COUNT] = { 0 };

      #define KICKSTART_FAN(f) \
        if (tail_fan_speed[f]) { \
          millis_t ms = millis(); \
          if (fan_kick_end[f] == 0) { \
            fan_kick_end[f] = ms + FAN_KICKSTART_TIME; \
            tail_fan_speed[f] = 255; \
          } else if (PENDING(ms, fan_kick_end[f])) \
            tail_fan_speed[f] = 255; \
        } else fan_kick_end[f] = 0

      #if HAS_FAN0
        KICKSTART_FAN(0);
      #endif
      #if HAS_FAN1
        KICKSTART_FAN(1);
      #endif
      #if HAS_FAN2
        KICKSTART_FAN(2);
      #endif

    #endif // FAN_KICKSTART_TIME

    #if ENABLED(FAN_SOFT_PWM)
      #if HAS_FAN0
        thermalManager.soft_pwm_amount_fan[0] = CALC_FAN_SPEED(0);
      #endif
      #if HAS_FAN1
        thermalManager.soft_pwm_amount_fan[1] = CALC_FAN_SPEED(1);
      #endif
      #if HAS_FAN2
        thermalManager.soft_pwm_amount_fan[2] = CALC_FAN_SPEED(2);
      #endif
    #else
      #if HAS_FAN0
        analogWrite(FAN_PIN, CALC_FAN_SPEED(0));
      #endif
      #if HAS_FAN1
        analogWrite(FAN1_PIN, CALC_FAN_SPEED(1));
      #endif
      #if HAS_FAN2
        analogWrite(FAN2_PIN, CALC_FAN_SPEED(2));
      #endif
    #endif

  #endif // FAN_COUNT > 0

  #if ENABLED(AUTOTEMP)
    getHighESpeed();
  #endif
}

#if PLANNER_LEVELING
  /**
   * lx, ly, lz - logical (cartesian, not delta) positions in mm
   */
  void Planner::apply_leveling(float &lx, float &ly, float &lz) {

    #if ENABLED(AUTO_BED_LEVELING_UBL)
      if (!ubl.state.active) return;
      #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
        // if z_fade_height enabled (nonzero) and raw_z above it, no leveling required
        if (planner.z_fade_height && planner.z_fade_height <= RAW_Z_POSITION(lz)) return;
        lz += ubl.state.z_offset + ubl.get_z_correction(lx, ly) * ubl.fade_scaling_factor_for_z(lz);
      #else // no fade
        lz += ubl.state.z_offset + ubl.get_z_correction(lx, ly);
      #endif // FADE
    #endif // UBL

    #if HAS_ABL
      if (!abl_enabled) return;
    #endif

    #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT) && DISABLED(AUTO_BED_LEVELING_UBL)
      static float z_fade_factor = 1.0, last_raw_lz = -999.0;
      if (z_fade_height) {
        const float raw_lz = RAW_Z_POSITION(lz);
        if (raw_lz >= z_fade_height) return;
        if (last_raw_lz != raw_lz) {
          last_raw_lz = raw_lz;
          z_fade_factor = 1.0 - raw_lz * inverse_z_fade_height;
        }
      }
      else
        z_fade_factor = 1.0;
    #endif

    #if ENABLED(MESH_BED_LEVELING)

      if (mbl.active())
        lz += mbl.get_z(RAW_X_POSITION(lx), RAW_Y_POSITION(ly)
          #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
            , z_fade_factor
          #endif
          );

    #elif ABL_PLANAR

      float dx = RAW_X_POSITION(lx) - (X_TILT_FULCRUM),
            dy = RAW_Y_POSITION(ly) - (Y_TILT_FULCRUM),
            dz = RAW_Z_POSITION(lz);

      apply_rotation_xyz(bed_level_matrix, dx, dy, dz);

      lx = LOGICAL_X_POSITION(dx + X_TILT_FULCRUM);
      ly = LOGICAL_Y_POSITION(dy + Y_TILT_FULCRUM);
      lz = LOGICAL_Z_POSITION(dz);

    #elif ENABLED(AUTO_BED_LEVELING_BILINEAR)

      float tmp[XYZ] = { lx, ly, 0 };
      lz += bilinear_z_offset(tmp)
        #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
          * z_fade_factor
        #endif
      ;

    #endif
  }

  void Planner::unapply_leveling(float logical[XYZ]) {

    #if ENABLED(AUTO_BED_LEVELING_UBL)

      if (ubl.state.active) {

        const float z_physical = RAW_Z_POSITION(logical[Z_AXIS]),
                    z_correct = ubl.get_z_correction(logical[X_AXIS], logical[Y_AXIS]),
                    z_virtual = z_physical - ubl.state.z_offset - z_correct;
              float z_logical = LOGICAL_Z_POSITION(z_virtual);

        #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)

          // for P=physical_z, L=logical_z, M=mesh_z, O=z_offset, H=fade_height,
          // Given P=L+O+M(1-L/H) (faded mesh correction formula for L<H)
          //  then L=P-O-M(1-L/H)
          //    so L=P-O-M+ML/H
          //    so L-ML/H=P-O-M
          //    so L(1-M/H)=P-O-M
          //    so L=(P-O-M)/(1-M/H) for L<H

          if (planner.z_fade_height) {
            if (z_logical >= planner.z_fade_height)
              z_logical = LOGICAL_Z_POSITION(z_physical - ubl.state.z_offset);
            else
              z_logical /= 1.0 - z_correct * planner.inverse_z_fade_height;
          }

        #endif // ENABLE_LEVELING_FADE_HEIGHT

        logical[Z_AXIS] = z_logical;
      }

      return; // don't fall thru to other ENABLE_LEVELING_FADE_HEIGHT logic

    #endif

    #if HAS_ABL
      if (!abl_enabled) return;
    #endif

    #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
      if (z_fade_height && RAW_Z_POSITION(logical[Z_AXIS]) >= z_fade_height) return;
    #endif

    #if ENABLED(MESH_BED_LEVELING)

      if (mbl.active()) {
        #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
          const float c = mbl.get_z(RAW_X_POSITION(logical[X_AXIS]), RAW_Y_POSITION(logical[Y_AXIS]), 1.0);
          logical[Z_AXIS] = (z_fade_height * (RAW_Z_POSITION(logical[Z_AXIS]) - c)) / (z_fade_height - c);
        #else
          logical[Z_AXIS] -= mbl.get_z(RAW_X_POSITION(logical[X_AXIS]), RAW_Y_POSITION(logical[Y_AXIS]));
        #endif
      }

    #elif ABL_PLANAR

      matrix_3x3 inverse = matrix_3x3::transpose(bed_level_matrix);

      float dx = RAW_X_POSITION(logical[X_AXIS]) - (X_TILT_FULCRUM),
            dy = RAW_Y_POSITION(logical[Y_AXIS]) - (Y_TILT_FULCRUM),
            dz = RAW_Z_POSITION(logical[Z_AXIS]);

      apply_rotation_xyz(inverse, dx, dy, dz);

      logical[X_AXIS] = LOGICAL_X_POSITION(dx + X_TILT_FULCRUM);
      logical[Y_AXIS] = LOGICAL_Y_POSITION(dy + Y_TILT_FULCRUM);
      logical[Z_AXIS] = LOGICAL_Z_POSITION(dz);

    #elif ENABLED(AUTO_BED_LEVELING_BILINEAR)

      #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
        const float c = bilinear_z_offset(logical);
        logical[Z_AXIS] = (z_fade_height * (RAW_Z_POSITION(logical[Z_AXIS]) - c)) / (z_fade_height - c);
      #else
        logical[Z_AXIS] -= bilinear_z_offset(logical);
      #endif

    #endif
  }

#endif // PLANNER_LEVELING

/**
* Add an arc movement to the buffer.
*/
void __forceinline Planner::_buffer_arc(
  const float(&target)[4],
  const float(&start_velocity)[4],
  const float(&end_velocity)[4]
)
{
}

/**
 * Planner::_buffer_line
 *
 * Add a new linear movement to the buffer.
 *
 * Leveling and kinematics should be applied ahead of calling this.
 *
 *  a,b,c,e     - target positions in mm or degrees
 *  fr_mm_s     - (target) speed of the move
 *  extruder    - target extruder
 */
void __forceinline Planner::_buffer_line(const float & __restrict a, const float & __restrict b, const float & __restrict c, const float & __restrict e, float fr_mm_s, const uint8_t extruder) {

  // The target position of the tool in absolute steps
  // Calculate target position in absolute steps
  //this should be done after the wait, because otherwise a M92 code within the gcode disrupts this calculation somehow

  // TODO FIXME their code always presumes LROUND, which doesn't make sense
  // if you can have negative positions.
  // That being said, I'm just going to rework it somewhat as the target in our firmware is always > 0 anyways.

  __assume(a >= 0.0f);
  __assume(b >= 0.0f);
  __assume(c >= 0.0f);
  __assume(e >= 0.0f);

  const uint24 target[XYZE] = {
    round<uint24>(a * axis_steps_per_mm[X_AXIS]),
    round<uint24>(b * axis_steps_per_mm[Y_AXIS]),
    round<uint24>(c * axis_steps_per_mm[Z_AXIS]),
    round<uint24>(e * axis_steps_per_mm[E_AXIS_N])
  };

  // When changing extruders recalculate steps corresponding to the E position
  #if ENABLED(DISTINCT_E_FACTORS)
    if (last_extruder != extruder && axis_steps_per_mm[E_AXIS_N] != axis_steps_per_mm[E_AXIS + last_extruder]) {
      position[E_AXIS] = LROUND(position[E_AXIS] * axis_steps_per_mm[E_AXIS_N] * steps_to_mm[E_AXIS + last_extruder]);
      last_extruder = extruder;
    }
  #endif

  #if ENABLED(LIN_ADVANCE)
    const float mm_D_float = SQRT(sq(a - position_float[X_AXIS]) + sq(b - position_float[Y_AXIS]));
  #endif

  const int24 da = target[X_AXIS] - position[X_AXIS],
             db = target[Y_AXIS] - position[Y_AXIS],
             dc = target[Z_AXIS] - position[Z_AXIS];

  /*
  SERIAL_ECHOPAIR("  Planner FR:", fr_mm_s);
  SERIAL_CHAR(' ');
  #if IS_KINEMATIC
    SERIAL_ECHOPAIR("A:", a);
    SERIAL_ECHOPAIR(" (", da);
    SERIAL_ECHOPAIR(") B:", b);
  #else
    SERIAL_ECHOPAIR("X:", a);
    SERIAL_ECHOPAIR(" (", da);
    SERIAL_ECHOPAIR(") Y:", b);
  #endif
  SERIAL_ECHOPAIR(" (", db);
  #if ENABLED(DELTA)
    SERIAL_ECHOPAIR(") C:", c);
  #else
    SERIAL_ECHOPAIR(") Z:", c);
  #endif
  SERIAL_ECHOPAIR(" (", dc);
  SERIAL_CHAR(')');
  SERIAL_EOL();
  //*/

  // DRYRUN ignores all temperature constraints and assures that the extruder is instantly satisfied
  if (DEBUGGING(DRYRUN)) {
    position[E_AXIS] = target[E_AXIS];
    #if ENABLED(LIN_ADVANCE)
      position_float[E_AXIS] = e;
    #endif
  }

  int24 de = target[E_AXIS] - position[E_AXIS];

  #if ENABLED(LIN_ADVANCE)
    float de_float = e - position_float[E_AXIS];
  #endif

  #if ENABLED(PREVENT_COLD_EXTRUSION)
    if (de) {
      if (Temperature::is_coldextrude()) {
        position[E_AXIS] = target[E_AXIS]; // Behave as if the move really took place, but ignore E part
        de = 0; // no difference
        #if ENABLED(LIN_ADVANCE)
          position_float[E_AXIS] = e;
          de_float = 0;
        #endif
        SERIAL_ECHO_START();
        SERIAL_ECHOLNPGM(MSG_ERR_COLD_EXTRUDE_STOP);
      }
      #if ENABLED(PREVENT_LENGTHY_EXTRUDE)
        if (abs(de) > (int32)axis_steps_per_mm[E_AXIS_N] * (EXTRUDE_MAXLENGTH)) { // It's not important to get max. extrusion length in a precision < 1mm, so save some cycles and cast to int
          position[E_AXIS] = target[E_AXIS]; // Behave as if the move really took place, but ignore E part
          de = 0; // no difference
          #if ENABLED(LIN_ADVANCE)
            position_float[E_AXIS] = e;
            de_float = 0;
          #endif
          SERIAL_ECHO_START();
          SERIAL_ECHOLNPGM(MSG_ERR_LONG_EXTRUDE_STOP);
        }
      #endif
    }
  #endif

  // Compute direction bit-mask for this block
  uint8_t dm = 0;
  #if CORE_IS_XY
    if (da < 0) SBI(dm, X_HEAD);                // Save the real Extruder (head) direction in X Axis
    if (db < 0) SBI(dm, Y_HEAD);                // ...and Y
    if (dc < 0) SBI(dm, Z_AXIS);
    if (da + db < 0) SBI(dm, A_AXIS);           // Motor A direction
    if (CORESIGN(da - db) < 0) SBI(dm, B_AXIS); // Motor B direction
  #elif CORE_IS_XZ
    if (da < 0) SBI(dm, X_HEAD);                // Save the real Extruder (head) direction in X Axis
    if (db < 0) SBI(dm, Y_AXIS);
    if (dc < 0) SBI(dm, Z_HEAD);                // ...and Z
    if (da + dc < 0) SBI(dm, A_AXIS);           // Motor A direction
    if (CORESIGN(da - dc) < 0) SBI(dm, C_AXIS); // Motor C direction
  #elif CORE_IS_YZ
    if (da < 0) SBI(dm, X_AXIS);
    if (db < 0) SBI(dm, Y_HEAD);                // Save the real Extruder (head) direction in Y Axis
    if (dc < 0) SBI(dm, Z_HEAD);                // ...and Z
    if (db + dc < 0) SBI(dm, B_AXIS);           // Motor B direction
    if (CORESIGN(db - dc) < 0) SBI(dm, C_AXIS); // Motor C direction
  #else
    if (da < 0) SBI(dm, X_AXIS);
    if (db < 0) SBI(dm, Y_AXIS);
    if (dc < 0) SBI(dm, Z_AXIS);
  #endif
  if (de < 0) SBI(dm, E_AXIS);

  const float esteps_float = de * volumetric_multiplier[extruder] * flow_percentage[extruder] * 0.01;
  const uint24 esteps = uint24(abs(esteps_float) + 0.5);

  // Calculate the buffer head after we push this byte
  const uint8_t next_buffer_head = next_block_index(block_buffer_head);

  // If the buffer is full: good! That means we are well ahead of the robot.
  // Rest here until there is room in the buffer.
  while (block_buffer_tail == next_buffer_head) idle();

  // Prepare to set up new block
  block_t * __restrict block = as<block_t * __restrict>(&block_buffer[block_buffer_head]);

  // Clear all flags, including the "busy" bit
  block->flag = 0;

  // Set direction bits
  block->direction_bits = dm;

  // Number of steps for each axis
  // See http://www.corexy.com/theory.html
  #if CORE_IS_XY
    block->steps[A_AXIS] = uabs(da + db);
    block->steps[B_AXIS] = uabs(da - db);
    block->steps[Z_AXIS] = uabs(dc);
  #elif CORE_IS_XZ
    block->steps[A_AXIS] = uabs(da + dc);
    block->steps[Y_AXIS] = uabs(db);
    block->steps[C_AXIS] = uabs(da - dc);
  #elif CORE_IS_YZ
    block->steps[X_AXIS] = uabs(da);
    block->steps[B_AXIS] = uabs(db + dc);
    block->steps[C_AXIS] = uabs(db - dc);
  #else
    // default non-h-bot planning
    block->steps[X_AXIS] = uabs(da);
    block->steps[Y_AXIS] = uabs(db);
    block->steps[Z_AXIS] = uabs(dc);
  #endif

  block->steps[E_AXIS] = esteps;
  block->step_event_count = max(block->steps[X_AXIS], block->steps[Y_AXIS], block->steps[Z_AXIS], esteps);

  // Bail if this is a zero-length block
  if (__unlikely(block->step_event_count < MIN_STEPS_PER_SEGMENT)) return;

  __assume(block->step_event_count > 0);

  // For a mixing extruder, get a magnified step_event_count for each
  #if ENABLED(MIXING_EXTRUDER)
    for (uint8_t i = 0; i < MIXING_STEPPERS; i++)
      block->mix_event_count[i] = mixing_factor[i] * block->step_event_count;
  #endif

  #if FAN_COUNT > 0
    for (uint8_t i = 0; i < FAN_COUNT; i++) block->fan_speed[i] = fanSpeeds[i];
  #endif

  #if ENABLED(BARICUDA)
    block->valve_pressure = baricuda_valve_pressure;
    block->e_to_p_pressure = baricuda_e_to_p_pressure;
  #endif

#if EXTRUDERS > 1
  block->active_extruder = extruder;
#endif

  //enable active axes
  #if CORE_IS_XY
    if (block->steps[A_AXIS] || block->steps[B_AXIS]) {
      enable_X();
      enable_Y();
    }
    #if DISABLED(Z_LATE_ENABLE)
      if (block->steps[Z_AXIS]) enable_Z();
    #endif
  #elif CORE_IS_XZ
    if (block->steps[A_AXIS] || block->steps[C_AXIS]) {
      enable_X();
      enable_Z();
    }
    if (block->steps[Y_AXIS]) enable_Y();
  #elif CORE_IS_YZ
    if (block->steps[B_AXIS] || block->steps[C_AXIS]) {
      enable_Y();
      enable_Z();
    }
    if (block->steps[X_AXIS]) enable_X();
  #else
    if (block->steps[X_AXIS]) enable_X();
    if (block->steps[Y_AXIS]) enable_Y();
    #if DISABLED(Z_LATE_ENABLE)
      if (block->steps[Z_AXIS]) enable_Z();
    #endif
  #endif

  // Enable extruder(s)
  if (esteps) {

    #if ENABLED(DISABLE_INACTIVE_EXTRUDER) // Enable only the selected extruder

      #define DISABLE_IDLE_E(N) if (!g_uc_extruder_last_move[N]) disable_E##N();

      for (uint8_t i = 0; i < EXTRUDERS; i++)
        if (g_uc_extruder_last_move[i] > 0) g_uc_extruder_last_move[i]--;

      switch(extruder) {
        case 0:
          enable_E0();
          g_uc_extruder_last_move[0] = (BLOCK_BUFFER_SIZE) * 2;
          #if ENABLED(DUAL_X_CARRIAGE) || ENABLED(DUAL_NOZZLE_DUPLICATION_MODE)
            if (extruder_duplication_enabled) {
              enable_E1();
              g_uc_extruder_last_move[1] = (BLOCK_BUFFER_SIZE) * 2;
            }
          #endif
          #if EXTRUDERS > 1
            DISABLE_IDLE_E(1);
            #if EXTRUDERS > 2
              DISABLE_IDLE_E(2);
              #if EXTRUDERS > 3
                DISABLE_IDLE_E(3);
                #if EXTRUDERS > 4
                  DISABLE_IDLE_E(4);
                #endif // EXTRUDERS > 4
              #endif // EXTRUDERS > 3
            #endif // EXTRUDERS > 2
          #endif // EXTRUDERS > 1
        break;
        #if EXTRUDERS > 1
          case 1:
            enable_E1();
            g_uc_extruder_last_move[1] = (BLOCK_BUFFER_SIZE) * 2;
            DISABLE_IDLE_E(0);
            #if EXTRUDERS > 2
              DISABLE_IDLE_E(2);
              #if EXTRUDERS > 3
                DISABLE_IDLE_E(3);
                #if EXTRUDERS > 4
                  DISABLE_IDLE_E(4);
                #endif // EXTRUDERS > 4
              #endif // EXTRUDERS > 3
            #endif // EXTRUDERS > 2
          break;
          #if EXTRUDERS > 2
            case 2:
              enable_E2();
              g_uc_extruder_last_move[2] = (BLOCK_BUFFER_SIZE) * 2;
              DISABLE_IDLE_E(0);
              DISABLE_IDLE_E(1);
              #if EXTRUDERS > 3
                DISABLE_IDLE_E(3);
                #if EXTRUDERS > 4
                  DISABLE_IDLE_E(4);
                #endif
              #endif
            break;
            #if EXTRUDERS > 3
              case 3:
                enable_E3();
                g_uc_extruder_last_move[3] = (BLOCK_BUFFER_SIZE) * 2;
                DISABLE_IDLE_E(0);
                DISABLE_IDLE_E(1);
                DISABLE_IDLE_E(2);
                #if EXTRUDERS > 4
                  DISABLE_IDLE_E(4);
                #endif
              break;
              #if EXTRUDERS > 4
                case 4:
                  enable_E4();
                  g_uc_extruder_last_move[4] = (BLOCK_BUFFER_SIZE) * 2;
                  DISABLE_IDLE_E(0);
                  DISABLE_IDLE_E(1);
                  DISABLE_IDLE_E(2);
                  DISABLE_IDLE_E(3);
                break;
              #endif // EXTRUDERS > 4
            #endif // EXTRUDERS > 3
          #endif // EXTRUDERS > 2
        #endif // EXTRUDERS > 1
      }
    #else
      enable_E0();
      enable_E1();
      enable_E2();
      enable_E3();
      enable_E4();
    #endif
  }

  if (esteps && fr_mm_s < min_feedrate_mm_s)
  {
    fr_mm_s = min_feedrate_mm_s;
  }
  else if (!esteps && fr_mm_s < min_travel_feedrate_mm_s)
  {
    fr_mm_s = min_travel_feedrate_mm_s;
  }
  __assume(fr_mm_s > 0);
  /**
   * This part of the code calculates the total length of the movement.
   * For cartesian bots, the X_AXIS is the real X movement and same for Y_AXIS.
   * But for corexy bots, that is not true. The "X_AXIS" and "Y_AXIS" motors (that should be named to A_AXIS
   * and B_AXIS) cannot be used for X and Y length, because A=X+Y and B=X-Y.
   * So we need to create other 2 "AXIS", named X_HEAD and Y_HEAD, meaning the real displacement of the Head.
   * Having the real displacement of the head, we can calculate the total movement length and apply the desired speed.
   */
  #if IS_CORE
    float delta_mm[Z_HEAD + 1];
    #if CORE_IS_XY
      delta_mm[X_HEAD] = da * steps_to_mm[A_AXIS];
      delta_mm[Y_HEAD] = db * steps_to_mm[B_AXIS];
      delta_mm[Z_AXIS] = dc * steps_to_mm[Z_AXIS];
      delta_mm[A_AXIS] = (da + db) * steps_to_mm[A_AXIS];
      delta_mm[B_AXIS] = CORESIGN(da - db) * steps_to_mm[B_AXIS];
    #elif CORE_IS_XZ
      delta_mm[X_HEAD] = da * steps_to_mm[A_AXIS];
      delta_mm[Y_AXIS] = db * steps_to_mm[Y_AXIS];
      delta_mm[Z_HEAD] = dc * steps_to_mm[C_AXIS];
      delta_mm[A_AXIS] = (da + dc) * steps_to_mm[A_AXIS];
      delta_mm[C_AXIS] = CORESIGN(da - dc) * steps_to_mm[C_AXIS];
    #elif CORE_IS_YZ
      delta_mm[X_AXIS] = da * steps_to_mm[X_AXIS];
      delta_mm[Y_HEAD] = db * steps_to_mm[B_AXIS];
      delta_mm[Z_HEAD] = dc * steps_to_mm[C_AXIS];
      delta_mm[B_AXIS] = (db + dc) * steps_to_mm[B_AXIS];
      delta_mm[C_AXIS] = CORESIGN(db - dc) * steps_to_mm[C_AXIS];
    #endif
  #else
    float delta_mm[XYZE];
    delta_mm[X_AXIS] = da * steps_to_mm[X_AXIS];
    delta_mm[Y_AXIS] = db * steps_to_mm[Y_AXIS];
    delta_mm[Z_AXIS] = dc * steps_to_mm[Z_AXIS];
  #endif
  delta_mm[E_AXIS] = esteps_float * steps_to_mm[E_AXIS_N];

  if (block->steps[X_AXIS] < MIN_STEPS_PER_SEGMENT && block->steps[Y_AXIS] < MIN_STEPS_PER_SEGMENT && block->steps[Z_AXIS] < MIN_STEPS_PER_SEGMENT) {
    block->millimeters = FABS(delta_mm[E_AXIS]);
  }
  else {
    block->millimeters = SQRT(
      #if CORE_IS_XY
        sq(delta_mm[X_HEAD]) + sq(delta_mm[Y_HEAD]) + sq(delta_mm[Z_AXIS])
      #elif CORE_IS_XZ
        sq(delta_mm[X_HEAD]) + sq(delta_mm[Y_AXIS]) + sq(delta_mm[Z_HEAD])
      #elif CORE_IS_YZ
        sq(delta_mm[X_AXIS]) + sq(delta_mm[Y_HEAD]) + sq(delta_mm[Z_HEAD])
      #else
        sq(delta_mm[X_AXIS]) + sq(delta_mm[Y_AXIS]) + sq(delta_mm[Z_AXIS])
      #endif
    );
  }
  __assume(block->millimeters > 0);
  float inverse_millimeters = 1.0 / block->millimeters;  // Inverse millimeters to remove multiple divides

  // Calculate moves/second for this move. No divide by zero due to previous checks.
  float inverse_mm_s = fr_mm_s * inverse_millimeters;
  __assume(inverse_mm_s > 0);

  const uint8_t moves_queued = movesplanned();

  // Slow down when the buffer starts to empty, rather than wait at the corner for a buffer refill
  #if ENABLED(SLOWDOWN) || ENABLED(ULTRA_LCD) || defined(XY_FREQUENCY_LIMIT)
    // Segment time im micro seconds
    unsigned long segment_time = LROUND(1000000.0 / inverse_mm_s);
  #endif
  #if ENABLED(SLOWDOWN)
    if (WITHIN(moves_queued, 2, (BLOCK_BUFFER_SIZE) / 2 - 1)) {
      if (segment_time < min_segment_time) {
        // buffer is draining, add extra time.  The amount of time added increases if the buffer is still emptied more.
        inverse_mm_s = 1000000.0 / (segment_time + LROUND(2 * (min_segment_time - segment_time) / moves_queued));
        #if defined(XY_FREQUENCY_LIMIT) || ENABLED(ULTRA_LCD)
          segment_time = LROUND(1000000.0 / inverse_mm_s);
        #endif
      }
    }
  #endif

  #if ENABLED(ULTRA_LCD)
    CRITICAL_SECTION_START
      block_buffer_runtime_us += segment_time;
    CRITICAL_SECTION_END
#endif

  block->nominal_speed = block->millimeters * inverse_mm_s; // (mm/sec) Always > 0
  __assume(block->nominal_speed > 0.0);
  block->nominal_rate = CEIL(block->step_event_count * inverse_mm_s); // (step/sec) Always > 0
  __assume(block->nominal_rate > 0.0);
  //block->plateau_rate = block->nominal_rate;

  #if ENABLED(FILAMENT_WIDTH_SENSOR)
    static float filwidth_e_count = 0, filwidth_delay_dist = 0;

    //FMM update ring buffer used for delay with filament measurements
    if (extruder == FILAMENT_SENSOR_EXTRUDER_NUM && filwidth_delay_index[1] >= 0) {  //only for extruder with filament sensor and if ring buffer is initialized

      const int MMD_CM = MAX_MEASUREMENT_DELAY + 1, MMD_MM = MMD_CM * 10;

      // increment counters with next move in e axis
      filwidth_e_count += delta_mm[E_AXIS];
      filwidth_delay_dist += delta_mm[E_AXIS];

      // Only get new measurements on forward E movement
      if (filwidth_e_count > 0.0001) {

        // Loop the delay distance counter (modulus by the mm length)
        while (filwidth_delay_dist >= MMD_MM) filwidth_delay_dist -= MMD_MM;

        // Convert into an index into the measurement array
        filwidth_delay_index[0] = int8_t(filwidth_delay_dist * 0.1);

        // If the index has changed (must have gone forward)...
        if (filwidth_delay_index[0] != filwidth_delay_index[1]) {
          filwidth_e_count = 0; // Reset the E movement counter
          const uint8_t meas_sample = thermalManager.widthFil_to_size_ratio() - 100; // Subtract 100 to reduce magnitude - to store in a signed char
          do {
            filwidth_delay_index[1] = (filwidth_delay_index[1] + 1) % MMD_CM; // The next unused slot
            measurement_delay[filwidth_delay_index[1]] = meas_sample;         // Store the measurement
          } while (filwidth_delay_index[0] != filwidth_delay_index[1]);       // More slots to fill?
        }
      }
    }
  #endif

  // Calculate and limit speed in mm/sec for each axis
  float current_speed[NUM_AXIS], speed_factor = 1.0; // factor <1 decreases speed
  LOOP_XYZE(i) {
    const float cs = FABS(current_speed[i] = delta_mm[i] * inverse_mm_s);
    #if ENABLED(DISTINCT_E_FACTORS)
      if (i == E_AXIS) i += extruder;
    #endif
    if (cs > max_feedrate_mm_s[i]) NOMORE(speed_factor, max_feedrate_mm_s[i] / cs);
  }

  // Max segment time in µs.
  #ifdef XY_FREQUENCY_LIMIT

    // Check and limit the xy direction change frequency
    const unsigned char direction_change = block->direction_bits ^ old_direction_bits;
    old_direction_bits = block->direction_bits;
    segment_time = LROUND((float)segment_time / speed_factor);

    long xs0 = axis_segment_time[X_AXIS][0],
         xs1 = axis_segment_time[X_AXIS][1],
         xs2 = axis_segment_time[X_AXIS][2],
         ys0 = axis_segment_time[Y_AXIS][0],
         ys1 = axis_segment_time[Y_AXIS][1],
         ys2 = axis_segment_time[Y_AXIS][2];

    if (TEST(direction_change, X_AXIS)) {
      xs2 = axis_segment_time[X_AXIS][2] = xs1;
      xs1 = axis_segment_time[X_AXIS][1] = xs0;
      xs0 = 0;
    }
    xs0 = axis_segment_time[X_AXIS][0] = xs0 + segment_time;

    if (TEST(direction_change, Y_AXIS)) {
      ys2 = axis_segment_time[Y_AXIS][2] = axis_segment_time[Y_AXIS][1];
      ys1 = axis_segment_time[Y_AXIS][1] = axis_segment_time[Y_AXIS][0];
      ys0 = 0;
    }
    ys0 = axis_segment_time[Y_AXIS][0] = ys0 + segment_time;

    const long max_x_segment_time = MAX3(xs0, xs1, xs2),
               max_y_segment_time = MAX3(ys0, ys1, ys2),
               min_xy_segment_time = min(max_x_segment_time, max_y_segment_time);
    if (min_xy_segment_time < MAX_FREQ_TIME) {
      const float low_sf = speed_factor * min_xy_segment_time / (MAX_FREQ_TIME);
      NOMORE(speed_factor, low_sf);
    }
  #endif // XY_FREQUENCY_LIMIT

  // Correct the speed
  if (speed_factor < 1.0) {
    LOOP_XYZE(i) current_speed[i] *= speed_factor;
    block->nominal_speed *= speed_factor;
    block->nominal_rate *= speed_factor;
    //block->plateau_rate *= speed_factor;
  }

  // Compute and limit the acceleration rate for the trapezoid generator.
  const float steps_per_mm = block->step_event_count * inverse_millimeters;
  uint32 accel;
  if (!block->steps[X_AXIS] && !block->steps[Y_AXIS] && !block->steps[Z_AXIS]) {
    // convert to: acceleration steps/sec^2
    accel = CEIL(retract_acceleration * steps_per_mm);
  }
  else {
    #define LIMIT_ACCEL_LONG(AXIS,INDX) do{ \
      if (block->steps[AXIS] && max_acceleration_steps_per_s2[AXIS+INDX] < accel) { \
        const uint32 comp = max_acceleration_steps_per_s2[AXIS+INDX] * block->step_event_count; \
        if (accel * block->steps[AXIS] > comp) accel = comp / block->steps[AXIS]; \
      } \
    }while(0)

    #define LIMIT_ACCEL_FLOAT(AXIS,INDX) do{ \
      if (block->steps[AXIS] && max_acceleration_steps_per_s2[AXIS+INDX] < accel) { \
        const float comp = (float)max_acceleration_steps_per_s2[AXIS+INDX] * (float)block->step_event_count; \
        if ((float)accel * (float)block->steps[AXIS] > comp) accel = comp / (float)block->steps[AXIS]; \
      } \
    }while(0)

    // Start with print or travel acceleration
    accel = CEIL((esteps ? acceleration : travel_acceleration) * steps_per_mm);

    #if ENABLED(DISTINCT_E_FACTORS)
      #define ACCEL_IDX extruder
    #else
      #define ACCEL_IDX 0
    #endif

    // Limit acceleration per axis
    if (__likely(block->step_event_count <= cutoff_long)) {
      LIMIT_ACCEL_LONG(X_AXIS, 0);
      LIMIT_ACCEL_LONG(Y_AXIS, 0);
      LIMIT_ACCEL_LONG(Z_AXIS, 0);
      LIMIT_ACCEL_LONG(E_AXIS, ACCEL_IDX);
    }
    else {
      LIMIT_ACCEL_FLOAT(X_AXIS, 0);
      LIMIT_ACCEL_FLOAT(Y_AXIS, 0);
      LIMIT_ACCEL_FLOAT(Z_AXIS, 0);
      LIMIT_ACCEL_FLOAT(E_AXIS, ACCEL_IDX);
    }
  }
  block->acceleration_steps_per_s2 = accel;
  block->acceleration = accel / steps_per_mm;

  // Initial limit on the segment entry velocity
  float vmax_junction;

  /**
   * Adapted from Průša MKS firmware
   * https://github.com/prusa3d/Prusa-Firmware
   *
   * Start with a safe speed (from which the machine may halt to stop immediately).
   */

  // Exit speed limited by a jerk to full halt of a previous last segment
  static float previous_safe_speed;

  float safe_speed = block->nominal_speed;
  uint8_t limited = 0;
  LOOP_XYZE(i) {
    const float jerk = FABS(current_speed[i]), maxj = max_jerk[i];
    if (jerk > maxj) {
      if (limited) {
        const float mjerk = maxj * block->nominal_speed;
        if (jerk * safe_speed > mjerk) safe_speed = mjerk / jerk;
      }
      else {
        ++limited;
        safe_speed = maxj;
      }
    }
  }

  if (moves_queued > 1 && previous_nominal_speed > 0.0001) {
    // Estimate a maximum velocity allowed at a joint of two successive segments.
    // If this maximum velocity allowed is lower than the minimum of the entry / exit safe velocities,
    // then the machine is not coasting anymore and the safe entry / exit velocities shall be used.

    // The junction velocity will be shared between successive segments. Limit the junction velocity to their minimum.
    bool prev_speed_larger = previous_nominal_speed > block->nominal_speed;
    float smaller_speed_factor = prev_speed_larger ? (block->nominal_speed / previous_nominal_speed) : (previous_nominal_speed / block->nominal_speed);
    // Pick the smaller of the nominal speeds. Higher speed shall not be achieved at the junction during coasting.
    vmax_junction = prev_speed_larger ? block->nominal_speed : previous_nominal_speed;
    // Factor to multiply the previous / current nominal velocities to get componentwise limited velocities.
    float v_factor = 1.f;
    limited = 0;
    // Now limit the jerk in all axes.
    LOOP_XYZE(axis) {
      // Limit an axis. We have to differentiate: coasting, reversal of an axis, full stop.
      float v_exit = previous_speed[axis], v_entry = current_speed[axis];
      if (prev_speed_larger) v_exit *= smaller_speed_factor;
      if (limited) {
        v_exit *= v_factor;
        v_entry *= v_factor;
      }

      // Calculate jerk depending on whether the axis is coasting in the same direction or reversing.
      const float jerk = (v_exit > v_entry)
          ? //                                  coasting             axis reversal
            ( (v_entry > 0.f || v_exit < 0.f) ? (v_exit - v_entry) : max(v_exit, -v_entry) )
          : // v_exit <= v_entry                coasting             axis reversal
            ( (v_entry < 0.f || v_exit > 0.f) ? (v_entry - v_exit) : max(-v_exit, v_entry) );

      if (jerk > max_jerk[axis]) {
        v_factor *= max_jerk[axis] / jerk;
        ++limited;
      }
    }
    if (limited) vmax_junction *= v_factor;
    // Now the transition velocity is known, which maximizes the shared exit / entry velocity while
    // respecting the jerk factors, it may be possible, that applying separate safe exit / entry velocities will achieve faster prints.
    const float vmax_junction_threshold = vmax_junction * 0.99f;
    if (previous_safe_speed > vmax_junction_threshold && safe_speed > vmax_junction_threshold) {
      // Not coasting. The machine will stop and start the movements anyway,
      // better to start the segment from start.
      SBI(block->flag, BLOCK_BIT_START_FROM_FULL_HALT);
      vmax_junction = safe_speed;
    }
  }
  else {
    SBI(block->flag, BLOCK_BIT_START_FROM_FULL_HALT);
    vmax_junction = safe_speed;
  }

  // Max entry speed of this block equals the max exit speed of the previous block.
  block->max_entry_speed = vmax_junction;

  // Initialize block entry speed. Compute based on deceleration to user-defined MINIMUM_PLANNER_SPEED.
  const float v_allowable = max_allowable_speed(-block->acceleration, 0.0f, block->millimeters);
  // If stepper ISR is disabled, this indicates buffer_segment wants to add a split block.
  // In this case start with the max. allowed speed to avoid an interrupted first move.
  block->entry_speed = TEST(TIMSK1, OCIE1A) ? 0.0f : min(vmax_junction, v_allowable);

  // Initialize planner efficiency flags
  // Set flag if block will always reach maximum junction speed regardless of entry/exit speeds.
  // If a block can de/ac-celerate from nominal speed to zero within the length of the block, then
  // the current block and next block junction speeds are guaranteed to always be at their maximum
  // junction speeds in deceleration and acceleration, respectively. This is due to how the current
  // block nominal speed limits both the current and next maximum junction speeds. Hence, in both
  // the reverse and forward planners, the corresponding block junction speed will always be at the
  // the maximum junction speed and may always be ignored for any speed reduction checks.
  block->flag |= block->nominal_speed <= v_allowable ? BLOCK_FLAG_RECALCULATE | BLOCK_FLAG_NOMINAL_LENGTH : BLOCK_FLAG_RECALCULATE;

  // Update previous path unit_vector and nominal speed
  COPY(previous_speed, current_speed);
  previous_nominal_speed = block->nominal_speed;
  previous_safe_speed = safe_speed;

  #if ENABLED(LIN_ADVANCE)

    //
    // Use LIN_ADVANCE for blocks if all these are true:
    //
    // esteps                                          : We have E steps todo (a printing move)
    //
    // block->steps[X_AXIS] || block->steps[Y_AXIS]    : We have a movement in XY direction (i.e., not retract / prime).
    //
    // extruder_advance_k                              : There is an advance factor set.
    //
    // block->steps[E_AXIS] != block->step_event_count : A problem occurs if the move before a retract is too small.
    //                                                   In that case, the retract and move will be executed together.
    //                                                   This leads to too many advance steps due to a huge e_acceleration.
    //                                                   The math is good, but we must avoid retract moves with advance!
    // de_float > 0.0                                  : Extruder is running forward (e.g., for "Wipe while retracting" (Slic3r) or "Combing" (Cura) moves)
    //
    block->use_advance_lead =  esteps
                            && (block->steps[X_AXIS] || block->steps[Y_AXIS])
                            && extruder_advance_k
                            && (uint32)esteps != block->step_event_count
                            && de_float > 0.0;
    if (block->use_advance_lead)
      block->abs_adv_steps_multiplier8 = LROUND(
        extruder_advance_k
        * (UNEAR_ZERO(advance_ed_ratio) ? de_float / mm_D_float : advance_ed_ratio) // Use the fixed ratio, if set
        * (block->nominal_speed / (float)block->nominal_rate)
        * axis_steps_per_mm[E_AXIS_N] * 256.0
      );
  #endif // LIN_ADVANCE

  // Move buffer head
  block_buffer_head = next_buffer_head;

  // Update the position (only when a move was queued)
  COPY(position, target);
  #if ENABLED(LIN_ADVANCE)
    position_float[X_AXIS] = a;
    position_float[Y_AXIS] = b;
    position_float[Z_AXIS] = c;
    position_float[E_AXIS] = e;
  #endif

  recalculate();

  stepper.wake_up();

} // buffer_line()

/**
 * Directly set the planner XYZ position (and stepper positions)
 * converting mm (or angles for SCARA) into steps.
 *
 * On CORE machines stepper ABC will be translated from the given XYZ.
 */

void __forceinline Planner::_set_position_mm(const float & __restrict a, const float & __restrict b, const float & __restrict c, const float & __restrict e) {
  #if ENABLED(DISTINCT_E_FACTORS)
    #define _EINDEX (E_AXIS + active_extruder)
    last_extruder = active_extruder;
  #else
    #define _EINDEX E_AXIS
  #endif
  int24 na = position[X_AXIS] = LROUND(a * axis_steps_per_mm[X_AXIS]),
       nb = position[Y_AXIS] = LROUND(b * axis_steps_per_mm[Y_AXIS]),
       nc = position[Z_AXIS] = LROUND(c * axis_steps_per_mm[Z_AXIS]),
       ne = position[E_AXIS] = LROUND(e * axis_steps_per_mm[_EINDEX]);
  #if ENABLED(LIN_ADVANCE)
    position_float[X_AXIS] = a;
    position_float[Y_AXIS] = b;
    position_float[Z_AXIS] = c;
    position_float[E_AXIS] = e;
  #endif
  stepper.set_position(na, nb, nc, ne);
  previous_nominal_speed = 0.0; // Resets planner junction speeds. Assumes start from rest.
  ZERO(previous_speed);
}

void __forceinline __flatten Planner::set_position_mm_kinematic(const float position[NUM_AXIS]) {
  #if PLANNER_LEVELING
    float lpos[XYZ] = { position[X_AXIS], position[Y_AXIS], position[Z_AXIS] };
    apply_leveling(lpos);
  #else
    const float * __restrict const lpos = position;
  #endif
  #if IS_KINEMATIC
    inverse_kinematics(lpos);
    _set_position_mm(delta[A_AXIS], delta[B_AXIS], delta[C_AXIS], position[E_AXIS]);
  #else
    _set_position_mm(lpos[X_AXIS], lpos[Y_AXIS], lpos[Z_AXIS], position[E_AXIS]);
  #endif
}

/**
 * Sync from the stepper positions. (e.g., after an interrupted move)
 */
void __forceinline __flatten Planner::sync_from_steppers() {
  LOOP_XYZE(i) {
    position[i] = stepper.position((AxisEnum)i);
    #if ENABLED(LIN_ADVANCE)
      position_float[i] = position[i] * steps_to_mm[i
        #if ENABLED(DISTINCT_E_FACTORS)
          + (i == E_AXIS ? active_extruder : 0)
        #endif
      ];
    #endif
  }
}

/**
 * Setters for planner position (also setting stepper position).
 */
void __forceinline __flatten Planner::set_position_mm(const AxisEnum axis, const float & __restrict v) {
  #if ENABLED(DISTINCT_E_FACTORS)
    const uint8_t axis_index = axis + (axis == E_AXIS ? active_extruder : 0);
    last_extruder = active_extruder;
  #else
    const uint8_t axis_index = axis;
  #endif
  position[axis] = LROUND(v * axis_steps_per_mm[axis_index]);
  #if ENABLED(LIN_ADVANCE)
    position_float[axis] = v;
  #endif
  stepper.set_position(axis, v);
  previous_speed[axis] = 0.0;
}

// Recalculate the steps/s^2 acceleration rates, based on the mm/s^2
void __forceinline __flatten Planner::reset_acceleration_rates() {
  #if ENABLED(DISTINCT_E_FACTORS)
    #define HIGHEST_CONDITION (i < E_AXIS || i == E_AXIS + active_extruder)
  #else
    #define HIGHEST_CONDITION true
  #endif
  uint32 highest_rate = 1;
  LOOP_XYZE_N(i) {
    max_acceleration_steps_per_s2[i] = max_acceleration_mm_per_s2[i] * axis_steps_per_mm[i];
    if (HIGHEST_CONDITION) NOLESS(highest_rate, max_acceleration_steps_per_s2[i]);
  }
  cutoff_long = 4294967295UL / highest_rate;
}

// Recalculate position, steps_to_mm if axis_steps_per_mm changes!
void __forceinline Planner::refresh_positioning() {
  LOOP_XYZE_N(i) steps_to_mm[i] = 1.0 / axis_steps_per_mm[i];
  set_position_mm_kinematic(current_position);
  reset_acceleration_rates();
}

#if ENABLED(AUTOTEMP)

  void Planner::autotemp_M104_M109() {
    autotemp_enabled = parser.seen('F');
    if (autotemp_enabled) autotemp_factor = parser.value_celsius_diff();
    if (parser.seen('S')) autotemp_min = parser.value_celsius();
    if (parser.seen('B')) autotemp_max = parser.value_celsius();
  }

#endif
