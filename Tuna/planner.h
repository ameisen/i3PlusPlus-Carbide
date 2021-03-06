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
 * planner.h
 *
 * Buffer movement commands and manage the acceleration profile plan
 *
 * Derived from Grbl
 * Copyright (c) 2009-2011 Simen Svale Skogsrud
 */

#ifndef PLANNER_H
#define PLANNER_H

#include "types.h"
#include "enum.h"

#if HAS_ABL
  #include "vector_3.h"
#endif

enum BlockFlagBit : uint8_t {
  // Recalculate trapezoids on entry junction. For optimization.
  BLOCK_BIT_RECALCULATE,

  // Nominal speed always reached.
  // i.e., The segment is long enough, so the nominal speed is reachable if accelerating
  // from a safe speed (in consideration of jerking from zero speed).
  BLOCK_BIT_NOMINAL_LENGTH,

  // Start from a halt at the start of this block, respecting the maximum allowed jerk.
  BLOCK_BIT_START_FROM_FULL_HALT,

  // The block is busy
  BLOCK_BIT_BUSY,

  // The Block is an arc block
  BLOCK_BIT_ARC,
};

enum BlockFlag : uint8_t {
  BLOCK_FLAG_RECALCULATE          = _BV(BLOCK_BIT_RECALCULATE),
  BLOCK_FLAG_NOMINAL_LENGTH       = _BV(BLOCK_BIT_NOMINAL_LENGTH),
  BLOCK_FLAG_START_FROM_FULL_HALT = _BV(BLOCK_BIT_START_FROM_FULL_HALT),
  BLOCK_FLAG_BUSY                 = _BV(BLOCK_BIT_BUSY),
  BLOCK_FLAG_ARC                  = _BV(BLOCK_BIT_ARC)
};

/**
 * struct block_t
 *
 * A single entry in the planner buffer.
 * Tracks linear movement over multiple axes.
 *
 * The "nominal" values are as-specified by gcode, and
 * may never actually be reached due to acceleration limits.
 */
struct block_t final
{

  uint8 flag;                             // Block flags (See BlockFlag enum above)

  uint8 active_extruder;            // The extruder to move (if E move)

  // Fields used by the Bresenham algorithm for tracing the line
  uint24 steps[NUM_AXIS];                 // Step count along each axis
  uint24 step_event_count;                // The number of step events required to complete this block

  #if ENABLED(MIXING_EXTRUDER)
    uint32 mix_event_count[MIXING_STEPPERS]; // Scaled step_event_count for the mixing steppers
  #endif

  uint24 accelerate_until,                 // The index of the step event on which to stop acceleration
          decelerate_after,                 // The index of the step event on which to start decelerating
          acceleration_rate;                // The acceleration rate used for acceleration calculation

  uint8 direction_bits;                   // The direction bit set for this block (refers to *_DIRECTION_BIT in config.h)

  // Advance extrusion
  #if ENABLED(LIN_ADVANCE)
    bool use_advance_lead;
    uint32 abs_adv_steps_multiplier8; // Factorised by 2^8 to avoid float
  #endif

  // Fields used by the motion planner to manage acceleration
  float nominal_speed,                      // The nominal speed for this block in mm/sec
        entry_speed,                        // Entry speed at previous-current junction in mm/sec
        max_entry_speed,                    // Maximum allowable junction entry speed in mm/sec
        millimeters,                        // The total travel of this block in mm
        acceleration;                       // acceleration mm/sec^2

  // Settings for the trapezoid generator
  uint24 nominal_rate,                    // The nominal step rate for this block in step_events/sec
           initial_rate,                    // The jerk-adjusted step rate at start of block
           final_rate,                      // The minimal rate at exit
           acceleration_steps_per_s2;       // acceleration steps/sec^2

  //uint24 plateau_rate;

  #if FAN_COUNT > 0
    uint8 fan_speed[FAN_COUNT];
  #endif

  #if ENABLED(BARICUDA)
    uint32 valve_pressure, e_to_p_pressure;
  #endif

  uint32 segment_time;

};

#define BLOCK_MOD(n) ((n)&(BLOCK_BUFFER_SIZE-1))

class Planner final {

  public:

    /**
     * A ring buffer of moves described in steps
     */
    static block_t block_buffer[BLOCK_BUFFER_SIZE];
    static volatile uint8_t block_buffer_head,  // Index of the next block to be pushed
                            block_buffer_tail;

    #if ENABLED(DISTINCT_E_FACTORS)
      static uint8_t last_extruder;             // Respond to extruder change
    #endif

    static float max_feedrate_mm_s[XYZE_N],     // Max speeds in mm per second
                 axis_steps_per_mm[XYZE_N],
                 steps_to_mm[XYZE_N];

    //
    // i3++
    //
    struct temperature_preset final
    {
      uint16 hotend;
      uint8 bed;
    } static preheat_presets[3];
    
    static uint32 max_acceleration_steps_per_s2[XYZE_N],
                    max_acceleration_mm_per_s2[XYZE_N]; // Use M201 to override by software

    static millis_t min_segment_time;
    static float min_feedrate_mm_s,
                 acceleration,         // Normal acceleration mm/s^2  DEFAULT ACCELERATION for all printing moves. M204 SXXXX
                 retract_acceleration, // Retract acceleration mm/s^2 filament pull-back and push-forward while standing still in the other axes M204 TXXXX
                 travel_acceleration,  // Travel acceleration mm/s^2  DEFAULT ACCELERATION for all NON printing moves. M204 MXXXX
                 max_jerk[XYZE],       // The largest speed change requiring no acceleration
                 min_travel_feedrate_mm_s;

    #if HAS_ABL
      static bool abl_enabled;              // Flag that bed leveling is enabled
      #if ABL_PLANAR
        static matrix_3x3 bed_level_matrix; // Transform to compensate for bed level
      #endif
    #endif

    #if ENABLED(ENABLE_LEVELING_FADE_HEIGHT)
      static float z_fade_height, inverse_z_fade_height;
    #endif

    #if ENABLED(LIN_ADVANCE)
      static float extruder_advance_k, advance_ed_ratio;
    #endif

  private:

    /**
     * The current position of the tool in absolute steps
     * Recalculated if any axis_steps_per_mm are changed by gcode
     */
    static uint24 position[NUM_AXIS];

    /**
     * Speed of previous path line segment
     */
    static float previous_speed[NUM_AXIS];

    /**
     * Nominal speed of previous path line segment
     */
    static float previous_nominal_speed;

    /**
     * Limit where 64bit math is necessary for acceleration calculation
     */
    static uint32 cutoff_long;

    #if ENABLED(DISABLE_INACTIVE_EXTRUDER)
      /**
       * Counters to manage disabling inactive extruders
       */
      static uint8_t g_uc_extruder_last_move[EXTRUDERS];
    #endif // DISABLE_INACTIVE_EXTRUDER

    #ifdef XY_FREQUENCY_LIMIT
      // Used for the frequency limit
      #define MAX_FREQ_TIME long(1000000.0/XY_FREQUENCY_LIMIT)
      // Old direction bits. Used for speed calculations
      static unsigned char old_direction_bits;
      // Segment times (in µs). Used for speed calculations
      static long axis_segment_time[2][3];
    #endif

    #if ENABLED(LIN_ADVANCE)
      static float position_float[NUM_AXIS];
    #endif

    #if ENABLED(ULTRA_LCD)
      volatile static uint32 block_buffer_runtime_us; //Theoretical block buffer runtime in µs
    #endif

  public:

    /**
     * Instance Methods
     */

    Planner();

    void init();

    /**
     * Static (class) Methods
     */

    static __forceinline __flatten void reset_acceleration_rates();
    static __forceinline void refresh_positioning();

    // Manage fans, paste pressure, etc.
    static __forceinline __flatten void check_axes_activity();

    /**
     * Number of moves currently in the planner
     */
    static __forceinline __flatten uint8_t movesplanned() { return BLOCK_MOD(block_buffer_head - block_buffer_tail + BLOCK_BUFFER_SIZE); }

    static __forceinline __flatten bool is_full() { return (block_buffer_tail == BLOCK_MOD(block_buffer_head + 1)); }

    #if PLANNER_LEVELING

      #define ARG_X float lx
      #define ARG_Y float ly
      #define ARG_Z float lz

      /**
       * Apply leveling to transform a cartesian position
       * as it will be given to the planner and steppers.
       */
      static void apply_leveling(float &lx, float &ly, float &lz);
      static void apply_leveling(float logical[XYZ]) { apply_leveling(logical[X_AXIS], logical[Y_AXIS], logical[Z_AXIS]); }
      static void unapply_leveling(float logical[XYZ]);

    #else

      #define ARG_X const float & __restrict lx
      #define ARG_Y const float & __restrict ly
      #define ARG_Z const float & __restrict lz

    #endif

    /**
     * Planner::_buffer_line
     *
     * Add a new direct linear movement to the buffer.
     *
     * Leveling and kinematics should be applied ahead of this.
     *
     *  a,b,c,e   - target position in mm or degrees
     *  fr_mm_s   - (target) speed of the move (mm/s)
     *  extruder  - target extruder
     */
    // TODO validate I actually want this to be forceinline. This makes the binary waaaaay bigger.
    static void __forceinline _buffer_line(const float & __restrict a, const float & __restrict b, const float & __restrict c, const float & __restrict e, float fr_mm_s, const uint8_t extruder);

    static void __forceinline _buffer_arc(
      const float(&target)[4],
      const float(&start_velocity)[4],
      const float(&end_velocity)[4]
    );

    static void __forceinline _set_position_mm(const float & __restrict a, const float & __restrict b, const float & __restrict c, const float & __restrict e);

    /**
     * Add a new linear movement to the buffer.
     * The target is NOT translated to delta/scara
     *
     * Leveling will be applied to input on cartesians.
     * Kinematic machines should call buffer_line_kinematic (for leveled moves).
     * (Cartesians may also call buffer_line_kinematic.)
     *
     *  lx,ly,lz,e   - target position in mm or degrees
     *  fr_mm_s      - (target) speed of the move (mm/s)
     *  extruder     - target extruder
     */
    static void __forceinline __flatten buffer_line(ARG_X, ARG_Y, ARG_Z, const float & __restrict e, const float & __restrict fr_mm_s, const uint8_t extruder) {
      #if PLANNER_LEVELING && IS_CARTESIAN
        apply_leveling(lx, ly, lz);
      #endif
      _buffer_line(lx, ly, lz, e, fr_mm_s, extruder);
    }

    static void __forceinline buffer_arc(
      const float(&target)[4],
      const float(&start_velocity)[4],
      const float(&end_velocity)[4]
    )
    {
      _buffer_arc(target, start_velocity, end_velocity);
    }

    /**
     * Add a new linear movement to the buffer.
     * The target is cartesian, it's translated to delta/scara if
     * needed.
     *
     *  ltarget  - x,y,z,e CARTESIAN target in mm
     *  fr_mm_s  - (target) speed of the move (mm/s)
     *  extruder - target extruder
     */
    static void __forceinline __flatten buffer_line_kinematic(const float ltarget[XYZE], const float & __restrict fr_mm_s, const uint8_t extruder) {
      #if PLANNER_LEVELING
        float lpos[XYZ] = { ltarget[X_AXIS], ltarget[Y_AXIS], ltarget[Z_AXIS] };
        apply_leveling(lpos);
      #else
        const float * __restrict const lpos = ltarget;
      #endif
      #if IS_KINEMATIC
        inverse_kinematics(lpos);
        _buffer_line(delta[A_AXIS], delta[B_AXIS], delta[C_AXIS], ltarget[E_AXIS], fr_mm_s, extruder);
      #else
        _buffer_line(lpos[motion::as_index(AxisEnum::X_AXIS)], lpos[motion::as_index(AxisEnum::Y_AXIS)], lpos[motion::as_index(AxisEnum::Z_AXIS)], lpos[motion::as_index(AxisEnum::E_AXIS)], fr_mm_s, extruder);
      #endif
    }

    /**
     * Set the planner.position and individual stepper positions.
     * Used by G92, G28, G29, and other procedures.
     *
     * Multiplies by axis_steps_per_mm[] and does necessary conversion
     * for COREXY / COREXZ / COREYZ to set the corresponding stepper positions.
     *
     * Clears previous speed values.
     */
    static void __forceinline __flatten set_position_mm(ARG_X, ARG_Y, ARG_Z, const float & __restrict e) {
      #if PLANNER_LEVELING && IS_CARTESIAN
        apply_leveling(lx, ly, lz);
      #endif
      _set_position_mm(lx, ly, lz, e);
    }
    static void __forceinline __flatten set_position_mm_kinematic(const float position[NUM_AXIS]);
    static void __forceinline __flatten set_position_mm(const AxisEnum axis, const float & __restrict v);
    static void __forceinline __flatten set_z_position_mm(const float & __restrict z) { set_position_mm(AxisEnum::Z_AXIS, z); }
    static void __forceinline __flatten set_e_position_mm(const float & __restrict e) { set_position_mm(AxisEnum::E_AXIS, e); }

    /**
     * Sync from the stepper positions. (e.g., after an interrupted move)
     */
    static __forceinline __flatten void sync_from_steppers();

    /**
     * Does the buffer have any blocks queued?
     */
    static inline bool __forceinline __flatten blocks_queued() { return (block_buffer_head != block_buffer_tail); }

    /**
     * "Discards" the block and "releases" the memory.
     * Called when the current block is no longer needed.
     */
    static __forceinline __flatten void discard_current_block() {
      if (blocks_queued())
        block_buffer_tail = BLOCK_MOD(block_buffer_tail + 1);
    }

    /**
     * The current block. nullptr if the buffer is empty.
     * This also marks the block as busy.
     */
    static __forceinline __flatten block_t * __restrict get_current_block() {
      if (blocks_queued()) {
        block_t * __restrict block = &block_buffer[block_buffer_tail];

        // If the trapezoid of this block has to be recalculated, it's not save to execute it.
        if (movesplanned() > 1) {
          block_t * next = &block_buffer[next_block_index(block_buffer_tail)];
          if (TEST(block->flag, BLOCK_BIT_RECALCULATE) || TEST(next->flag, BLOCK_BIT_RECALCULATE))
            return nullptr;
        }
        else if (TEST(block->flag, BLOCK_BIT_RECALCULATE))
          return nullptr;

        #if ENABLED(ULTRA_LCD)
          block_buffer_runtime_us -= block->segment_time; //We can't be sure how long an active block will take, so don't count it.
        #endif
        SBI(block->flag, BLOCK_BIT_BUSY);
        return block;
      }
      else {
        #if ENABLED(ULTRA_LCD)
          clear_block_buffer_runtime(); // paranoia. Buffer is empty now - so reset accumulated time to zero.
        #endif
        return nullptr;
      }
    }

    #if ENABLED(AUTOTEMP)
      static float autotemp_min, autotemp_max, autotemp_factor;
      static bool autotemp_enabled;
      static void getHighESpeed();
      static void autotemp_M104_M109();
    #endif

  private:

    /**
     * Get the index of the next / previous block in the ring buffer
     */
    static __forceinline __flatten int8_t next_block_index(int8_t block_index) { return BLOCK_MOD(block_index + 1); }
    static __forceinline __flatten int8_t prev_block_index(int8_t block_index) { return BLOCK_MOD(block_index - 1); }

    /**
     * Calculate the distance (not time) it takes to accelerate
     * from initial_rate to target_rate using the given acceleration:
     */
    static __forceinline __flatten float estimate_acceleration_distance(const float & __restrict initial_rate, const float & __restrict target_rate, const float & __restrict accel) {
      if (accel == 0) return 0; // accel was 0, set acceleration distance to 0
      return (sq(target_rate) - sq(initial_rate)) / (accel * 2);
    }

    /**
     * Return the point at which you must start braking (at the rate of -'acceleration') if
     * you start at 'initial_rate', accelerate (until reaching the point), and want to end at
     * 'final_rate' after traveling 'distance'.
     *
     * This is used to compute the intersection point between acceleration and deceleration
     * in cases where the "trapezoid" has no plateau (i.e., never reaches maximum speed)
     */
    static __forceinline __flatten float intersection_distance(const float & __restrict initial_rate, const float & __restrict final_rate, const float & __restrict accel, const float & __restrict distance) {
      if (accel == 0) return 0; // accel was 0, set intersection distance to 0
      return (accel * 2 * distance - sq(initial_rate) + sq(final_rate)) / (accel * 4);
    }

    /**
     * Calculate the maximum allowable speed at this point, in order
     * to reach 'target_velocity' using 'acceleration' within a given
     * 'distance'.
     */
    static __forceinline __flatten float max_allowable_speed(const float & __restrict accel, const float & __restrict target_velocity, const float & __restrict distance) {
      return SQRT(sq(target_velocity) - 2 * accel * distance);
    }

    static void __forceinline __flatten calculate_trapezoid_for_block(block_t * __restrict const block, const float & __restrict entry_speed, const float & __restrict next_entry_speed);

    static void __forceinline __flatten reverse_pass_kernel(block_t * __restrict const current, const block_t * __restrict next);
    static void __forceinline __flatten forward_pass_kernel(const block_t * __restrict previous, block_t * __restrict const current);

    static void __forceinline __flatten reverse_pass();
    static void __forceinline __flatten forward_pass();

    static void __forceinline __flatten recalculate_trapezoids();

    static void __forceinline __flatten recalculate();

};

#define PLANNER_XY_FEEDRATE() (min(planner.max_feedrate_mm_s[X_AXIS], planner.max_feedrate_mm_s[Y_AXIS]))

extern Planner planner;

#endif // PLANNER_H
