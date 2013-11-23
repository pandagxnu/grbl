/*
  stepper.c - stepper motor driver: executes motion plans using stepper motors
  Part of Grbl

  Copyright (c) 2011-2013 Sungeun K. Jeon
  Copyright (c) 2009-2011 Simen Svale Skogsrud
  
  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <avr/interrupt.h>
#include "stepper.h"
#include "config.h"
#include "settings.h"
#include "planner.h"
#include "nuts_bolts.h"

// Some useful constants
#define TICKS_PER_MICROSECOND (F_CPU/1000000)

#define RAMP_NOOP_CRUISE 0
#define RAMP_ACCEL 1
#define RAMP_DECEL 2

#define LOAD_NOOP 0
#define LOAD_SEGMENT 1
#define LOAD_BLOCK 2

#define SEGMENT_NOOP 0
#define SEGMENT_END_OF_BLOCK bit(0)
#define RAMP_CHANGE_ACCEL bit(1)
#define RAMP_CHANGE_DECEL bit(2)

#define MINIMUM_STEPS_PER_SEGMENT 1 // Don't change

#define SEGMENT_BUFFER_SIZE 6

#define DT_SEGMENT ACCELERATION_TICKS_PER_SECOND/ISR_TICKS_PER_SECOND

// Stepper state variable. Contains running data and trapezoid variables.
typedef struct {
  // Used by the bresenham line algorithm
  int32_t counter_x,        // Counter variables for the bresenham line tracer
          counter_y, 
          counter_z;

  // Used by inverse time algorithm to track step rate
  int32_t counter_dist;    // Inverse time distance traveled since last step event
  
  uint8_t step_count;  // Steps remaining in line segment motion
  uint8_t phase_count; // Phase ticks remaining after line segment steps complete
  
  // Used by the stepper driver interrupt
  uint8_t execute_step;    // Flags step execution for each interrupt.
  uint8_t step_pulse_time; // Step pulse reset time after step rise
  uint8_t out_bits;        // The next stepping-bits to be output
  uint8_t load_flag;
} stepper_t;
static stepper_t st;

// Stores stepper common data for executing steps in the segment buffer. Data can change mid-block when the
// planner updates the remaining block velocity profile with a more optimal plan or a feedrate override occurs.
// NOTE: Normally, this buffer is partially in-use, but, for the worst case scenario, it will never exceed
// the number of accessible stepper buffer segments (SEGMENT_BUFFER_SIZE-1).
typedef struct {  
  uint32_t dist_per_step;
  float step_events_remaining; // Tracks step event count for the executing planner block
  float accelerate_until;
  float decelerate_after;
  float current_rate;
  float maximum_rate;
  float exit_rate;
  
  float acceleration;
  float step_per_mm;
} st_data_t;
static st_data_t segment_data[SEGMENT_BUFFER_SIZE-1];

// Primary stepper segment ring buffer. Contains small, short line segments for the stepper algorithm to execute,
// which are "checked-out" incrementally from the first block in the planner buffer. Once "checked-out", the steps
// in the segments buffer cannot be modified by the planner, where the remaining planner block steps still can.
typedef struct {
  uint8_t n_step;         // Number of step events to be executed for this segment
  uint8_t n_phase_tick;
  uint32_t dist_per_tick;
  uint8_t st_data_index;  // Stepper buffer common data index. Uses this information to execute this segment.
  uint8_t flag;           // Stepper algorithm bit-flag for special execution conditions.
} st_segment_t;
static st_segment_t segment_buffer[SEGMENT_BUFFER_SIZE];

// Step segment ring buffer indices
static volatile uint8_t segment_buffer_tail;
static volatile uint8_t segment_buffer_head;
static uint8_t segment_next_head;

static volatile uint8_t busy;   // Used to avoid ISR nesting of the "Stepper Driver Interrupt". Should never occur though.
static plan_block_t *pl_current_block;  // A pointer to the planner block currently being traced
static st_segment_t *st_current_segment;
static st_data_t *st_current_data;

// Pointers for the step segment being prepped from the planner buffer. Accessed only by the
// main program. Pointers may be planning segments or planner blocks ahead of what being executed.
static plan_block_t *pl_prep_block;   // Pointer to the planner block being prepped
static st_data_t *st_prep_data;       // Pointer to the stepper common data being prepped
static uint8_t pl_prep_index;         // Index of planner block being prepped
static uint8_t st_data_prep_index;    // Index of stepper common data block being prepped
static uint8_t pl_partial_block_flag; // Flag indicating the planner has modified the prepped planner block


/*        __________________________
         /|                        |\     _________________         ^
        / |                        | \   /|               |\        |
       /  |                        |  \ / |               | \       s
      /   |                        |   |  |               |  \      p
     /    |                        |   |  |               |   \     e
    +-----+------------------------+---+--+---------------+----+    e
    |               BLOCK 1            |      BLOCK 2          |    d

                            time ----->
  
   The trapezoid is the shape the speed curve over time. It starts at block->initial_rate, accelerates by block->rate_delta
   until reaching cruising speed block->nominal_rate, and/or until step_events_remaining reaches block->decelerate_after
   after which it decelerates until the block is completed. The driver uses constant acceleration, which is applied as
   +/- block->rate_delta velocity increments by the midpoint rule at each ACCELERATION_TICKS_PER_SECOND.
*/

// Stepper state initialization. Cycle should only start if the st.cycle_start flag is
// enabled. Startup init and limits call this function but shouldn't start the cycle.
void st_wake_up() 
{
  // Enable steppers by resetting the stepper disable port
  if (bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE)) { 
    STEPPERS_DISABLE_PORT |= (1<<STEPPERS_DISABLE_BIT); 
  } else { 
    STEPPERS_DISABLE_PORT &= ~(1<<STEPPERS_DISABLE_BIT);
  }
  if (sys.state == STATE_CYCLE) {
    // Initialize stepper output bits
    st.out_bits = settings.invert_mask; 
    // Initialize step pulse timing from settings.
    st.step_pulse_time = -(((settings.pulse_microseconds-2)*TICKS_PER_MICROSECOND) >> 3);
    // Enable stepper driver interrupt
    st.execute_step = false;
    st.load_flag = LOAD_BLOCK;
    
    TCNT2 = 0; // Clear Timer2
    TIMSK2 |= (1<<OCIE2A); // Enable Timer2 Compare Match A interrupt
    TCCR2B = (1<<CS21); // Begin Timer2. Full speed, 1/8 prescaler
  }
}


// Stepper shutdown
void st_go_idle() 
{
  // Disable stepper driver interrupt. Allow Timer0 to finish. It will disable itself.
  TIMSK2 &= ~(1<<OCIE2A); // Disable Timer2 interrupt
  TCCR2B = 0; // Disable Timer2
  busy = false;

  // Disable steppers only upon system alarm activated or by user setting to not be kept enabled.
  if ((settings.stepper_idle_lock_time != 0xff) || bit_istrue(sys.execute,EXEC_ALARM)) {
    // Force stepper dwell to lock axes for a defined amount of time to ensure the axes come to a complete
    // stop and not drift from residual inertial forces at the end of the last movement.
    delay_ms(settings.stepper_idle_lock_time);
    if (bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE)) { 
      STEPPERS_DISABLE_PORT &= ~(1<<STEPPERS_DISABLE_BIT); 
    } else { 
      STEPPERS_DISABLE_PORT |= (1<<STEPPERS_DISABLE_BIT); 
    }   
  }
}


/* "The Stepper Driver Interrupt" - This timer interrupt is the workhorse of Grbl. It is based
   on an inverse time stepper algorithm, where a timer ticks at a constant frequency and uses 
   time-distance counters to track when its the approximate time for a step event. For reference,
   a similar inverse-time algorithm by Pramod Ranade is susceptible to numerical round-off, as
   described, meaning that some axes steps may not execute correctly for a given multi-axis motion.
     Grbl's algorithm differs by using a single inverse time-distance counter to manage a
   Bresenham line algorithm for multi-axis step events, which ensures the number of steps for
   each axis are executed exactly. In other words, Grbl uses a Bresenham within a Bresenham 
   algorithm, where one tracks time for step events and the other steps for multi-axis moves.
   Grbl specifically uses the Bresenham algorithm due to its innate mathematical exactness and
   low computational overhead, requiring simple integer +,- counters only.
     This interrupt pops blocks from the step segment buffer and executes them by pulsing the 
   stepper pins appropriately. It is supported by The Stepper Port Reset Interrupt which it uses 
   to reset the stepper port after each pulse. The bresenham line tracer algorithm controls all 
   three stepper outputs simultaneously with these two interrupts.
*/
/* TODO: 
   - Measure time in ISR. Typical and worst-case. Should be virtually identical to last algorithm.
     There are no major changes to the base operations of this ISR with the new segment buffer.
   - Write how the acceleration counters work and why they are set at half via mid-point rule.
   - Determine if placing the position counters elsewhere (or change them to 8-bit variables that
     are added to the system position counters at the end of a segment) frees up cycles.
   - Write a blurb about how the acceleration should be handled within the ISR. All of the 
     time/step/ramp counters accurately keep track of the remainders and phasing of the variables
     with time. This means we do not have to compute them via expensive floating point beforehand.
   - Need to do an analysis to determine if these counters are really that much cheaper. At least
     find out when it isn't anymore. Particularly when the ISR is at a very high frequency.
   - Create NOTE: to describe that the total time in this ISR must be less than the ISR frequency
     in its worst case scenario.
*/
ISR(TIMER2_COMPA_vect)
{
// SPINDLE_ENABLE_PORT ^= 1<<SPINDLE_ENABLE_BIT; // Debug: Used to time ISR
  if (busy) { return; } // The busy-flag is used to avoid reentering this interrupt
  
  // Pulse stepper port pins, if flagged. New block dir will always be set one timer tick 
  // before any step pulse due to algorithm design.
  if (st.execute_step) {
    st.execute_step = false;
    STEPPING_PORT = ( STEPPING_PORT & ~(DIRECTION_MASK | STEP_MASK) ) | st.out_bits;
    TCNT0 = st.step_pulse_time; // Reload Timer0 counter.
    TCCR0B = (1<<CS21); // Begin Timer0. Full speed, 1/8 prescaler
  }
  
  busy = true;
  sei(); // Re-enable interrupts to allow Stepper Port Reset Interrupt to fire on-time. 
         // NOTE: The remaining code in this ISR will finish before returning to main program.
    
  // If there is no step segment, attempt to pop one from the stepper buffer
  if (st.load_flag != LOAD_NOOP) {
    
    // Anything in the buffer? If so, load and initialize next step segment.
    if (segment_buffer_head != segment_buffer_tail) {
      
      // Initialize new step segment and load number of steps to execute
      st_current_segment = &segment_buffer[segment_buffer_tail];
      st.step_count = st_current_segment->n_step;
    
      // If the new segment starts a new planner block, initialize stepper variables and counters.
      // NOTE: For new segments only, the step counters are not updated to ensure step phasing is continuous.
      if (st.load_flag == LOAD_BLOCK) {
        pl_current_block = plan_get_current_block(); // Should always be there. Stepper buffer handles this.
        st_current_data = &segment_data[segment_buffer[segment_buffer_tail].st_data_index];
        
        // Initialize direction bits for block. Set execute flag to set directions bits upon next ISR tick.
        st.out_bits = pl_current_block->direction_bits ^ settings.invert_mask;
        st.execute_step = true;
        
        // Initialize Bresenham line counters
        st.counter_x = (pl_current_block->step_event_count >> 1);
        st.counter_y = st.counter_x;
        st.counter_z = st.counter_x;
        
        // Initialize inverse time, step rate data, and acceleration ramp counters
        st.counter_dist = st_current_data->dist_per_step;  // dist_per_step always greater than dist_per_tick.
      }
      
      st.load_flag = LOAD_NOOP; // Segment motion loaded. Set no-operation flag to skip during execution.

    } else {
      // Can't discard planner block here if a feed hold stops in middle of block.
      st_go_idle();
      bit_true(sys.execute,EXEC_CYCLE_STOP); // Flag main program for cycle end
      return; // Nothing to do but exit.
    }  
    
  } 
    
  // Iterate inverse time counter. Triggers each Bresenham step event.
  st.counter_dist -= st_current_segment->dist_per_tick; 
  
  // Execute Bresenham step event, when it's time to do so.
  if (st.counter_dist < 0) {
    if (st.step_count > 0) {  // Block phase correction from executing step. 
      st.counter_dist += st_current_data->dist_per_step; // Reload inverse time counter
  
      st.out_bits = pl_current_block->direction_bits; // Reset out_bits and reload direction bits
      st.execute_step = true;
      
      // Execute step displacement profile by Bresenham line algorithm
      st.counter_x -= pl_current_block->steps[X_AXIS];
      if (st.counter_x < 0) {
        st.out_bits |= (1<<X_STEP_BIT);
        st.counter_x += pl_current_block->step_event_count;
        if (st.out_bits & (1<<X_DIRECTION_BIT)) { sys.position[X_AXIS]--; }
        else { sys.position[X_AXIS]++; }
      }
      st.counter_y -= pl_current_block->steps[Y_AXIS];
      if (st.counter_y < 0) {
        st.out_bits |= (1<<Y_STEP_BIT);
        st.counter_y += pl_current_block->step_event_count;
        if (st.out_bits & (1<<Y_DIRECTION_BIT)) { sys.position[Y_AXIS]--; }
        else { sys.position[Y_AXIS]++; }
      }
      st.counter_z -= pl_current_block->steps[Z_AXIS];
      if (st.counter_z < 0) {
        st.out_bits |= (1<<Z_STEP_BIT);
        st.counter_z += pl_current_block->step_event_count;
        if (st.out_bits & (1<<Z_DIRECTION_BIT)) { sys.position[Z_AXIS]--; }
        else { sys.position[Z_AXIS]++; }
      }
  
      // Check step events for trapezoid change or end of block.
      st.step_count--; // Decrement step events count    
  
      st.out_bits ^= settings.invert_mask;  // Apply step port invert mask    
    }
  }
  
  if (st.step_count == 0) {
    if (st.phase_count == 0) {
      // Line move is complete, set load line flag to check for new move.
      // Check if last line move in planner block. Discard if so.
      if (st_current_segment->flag & SEGMENT_END_OF_BLOCK) {
        plan_discard_current_block();
        st.load_flag = LOAD_BLOCK;
      } else {
        st.load_flag = LOAD_SEGMENT;
      }
      
      // Discard current segment by advancing buffer tail index
      if ( ++segment_buffer_tail == SEGMENT_BUFFER_SIZE) { segment_buffer_tail = 0; }
    }
    st.phase_count--;
  }
  
  
  busy = false;
// SPINDLE_ENABLE_PORT ^= 1<<SPINDLE_ENABLE_BIT;  
}


// The Stepper Port Reset Interrupt: Timer0 OVF interrupt handles the falling edge of the step
// pulse. This should always trigger before the next Timer2 COMPA interrupt and independently
// finish, if Timer2 is disabled after completing a move.
ISR(TIMER0_OVF_vect)
{
  STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | (settings.invert_mask & STEP_MASK); 
  TCCR0B = 0; // Disable timer until needed.
}


// Reset and clear stepper subsystem variables
void st_reset()
{
  memset(&st, 0, sizeof(st));
  
  st.load_flag = LOAD_BLOCK;
  busy = false;

  pl_current_block = NULL; // Planner block pointer used by stepper algorithm
  pl_prep_block = NULL;  // Planner block pointer used by segment buffer
  pl_prep_index =   0; // Planner buffer indices are also reset to zero.
  st_data_prep_index = 0;
  
  segment_buffer_tail = 0;
  segment_buffer_head = 0; // empty = tail
  segment_next_head = 1;
    
  pl_partial_block_flag = false;
}


// Initialize and start the stepper motor subsystem
void st_init()
{
  // Configure directions of interface pins
  STEPPING_DDR |= STEPPING_MASK;
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | settings.invert_mask;
  STEPPERS_DISABLE_DDR |= 1<<STEPPERS_DISABLE_BIT;
	
  // Configure Timer 2
  TIMSK2 &= ~(1<<OCIE2A); // Disable Timer2 interrupt while configuring it
  TCCR2B = 0; // Disable Timer2 until needed
  TCNT2 = 0; // Clear Timer2 counter
  TCCR2A = (1<<WGM21);  // Set CTC mode
  OCR2A = (F_CPU/ISR_TICKS_PER_SECOND)/8 - 1; // Set Timer2 CTC rate

  // Configure Timer 0
  TIMSK0 &= ~(1<<TOIE0);
  TCCR0A = 0; // Normal operation
  TCCR0B = 0; // Disable Timer0 until needed
  TIMSK0 |= (1<<TOIE0); // Enable overflow interrupt
  
  // Start in the idle state, but first wake up to check for keep steppers enabled option.
  st_wake_up();
  st_go_idle();
}


// Planner external interface to start stepper interrupt and execute the blocks in queue. Called
// by the main program functions: planner auto-start and run-time command execution.
void st_cycle_start() 
{
  if (sys.state == STATE_QUEUED) {
    sys.state = STATE_CYCLE;
    st_prep_buffer(); // Initialize step segment buffer before beginning cycle.
    st_wake_up();
  }
}


// Execute a feed hold with deceleration, only during cycle. Called by main program.
void st_feed_hold() 
{
  if (sys.state == STATE_CYCLE) {
    sys.state = STATE_HOLD;
    sys.auto_start = false; // Disable planner auto start upon feed hold.
  }
}


// Reinitializes the cycle plan and stepper system after a feed hold for a resume. Called by 
// runtime command execution in the main program, ensuring that the planner re-plans safely.
// NOTE: Bresenham algorithm variables are still maintained through both the planner and stepper
// cycle reinitializations. The stepper path should continue exactly as if nothing has happened.
// Only the planner de/ac-celerations profiles and stepper rates have been updated.
void st_cycle_reinitialize()
{
//   if (pl_current_block != NULL) {
    // Replan buffer from the feed hold stop location.
    
    // TODO: Need to add up all of the step events in the current planner block to give 
    // back to the planner. Should only need it for the current block. 
    // BUT! The planner block millimeters is all changed and may be changed into the next
    // planner block. The block millimeters would need to be recalculated via step counts
    // and the mm/step variable.
    // OR. Do we plan the feed hold itself down with the planner.
    
//     plan_cycle_reinitialize(st_current_data->step_events_remaining);
//     st.ramp_type = RAMP_ACCEL;
//     st.counter_ramp = ISR_TICKS_PER_ACCELERATION_TICK/2; 
//     st.ramp_rate = 0;
//     sys.state = STATE_QUEUED;
//   } else {
//     sys.state = STATE_IDLE;
//   }
    sys.state = STATE_IDLE;

}


/* Prepares step segment buffer. Continuously called from main program. 

   The segment buffer is an intermediary buffer interface between the execution of steps
   by the stepper algorithm and the velocity profiles generated by the planner. The stepper
   algorithm only executes steps within the segment buffer and is filled by the main program
   when steps are "checked-out" from the first block in the planner buffer. This keeps the
   step execution and planning optimization processes atomic and protected from each other.
   The number of steps "checked-out" from the planner buffer and the number of segments in
   the segment buffer is sized and computed such that no operation in the main program takes
   longer than the time it takes the stepper algorithm to empty it before refilling it. 
   Currently, the segment buffer conservatively holds roughly up to 40-60 msec of steps. 

   NOTE: The segment buffer executes a set number of steps over an approximate time period.
   If we try to execute over a fixed time period, it is difficult to guarantee or predict 
   how many steps will execute over it, especially when the step pulse phasing between the
   neighboring segments must also be kept consistent. Meaning that, if the last segment step 
   pulses right before a segment end, the next segment must delay its first pulse so that the
   step pulses are consistently spaced apart over time to keep the step pulse train nice and
   smooth. Keeping track of phasing and ensuring that the exact number of steps are executed
   as defined by the planner block, the related computational overhead can get quickly and
   prohibitively expensive, especially in real-time.
     Since the stepper algorithm automatically takes care of the step pulse phasing with
   its ramp and inverse time counters by retaining the count remainders, we don't have to
   explicitly and expensively track and synchronize the exact number of steps, time, and
   phasing of steps. All we need to do is approximate the number of steps in each segment 
   such that the segment buffer has enough execution time for the main program to do what 
   it needs to do and refill it when it comes back. In other words, we just need to compute
   a cheap approximation of the current velocity and the number of steps over it. 
*/

/* 
   TODO: Figure out how to enforce a deceleration when a feedrate override is reduced. 
     The problem is that when an override is reduced, the planner may not plan back to 
     the current rate. Meaning that the velocity profiles for certain conditions no longer
     are trapezoidal or triangular. For example, if the current block is cruising at a
     nominal rate and the feedrate override is reduced, the new nominal rate will now be
     lower. The velocity profile must first decelerate to the new nominal rate and then 
     follow on the new plan. So the remaining velocity profile will have a decelerate, 
     cruise, and another decelerate.
        Another issue is whether or not a feedrate override reduction causes a deceleration
     that acts over several planner blocks. For example, say that the plan is already 
     heavily decelerating throughout it, reducing the feedrate will not do much to it. So,
     how do we determine when to resume the new plan? How many blocks do we have to wait 
     until the new plan intersects with the deceleration curve? One plus though, the 
     deceleration will never be more than the number of blocks in the entire planner buffer,
     but it theoretically can be equal to it when all planner blocks are decelerating already.
*/
void st_prep_buffer()
{
  if (sys.state == STATE_QUEUED) { return; } // Block until a motion state is issued
  while (segment_buffer_tail != segment_next_head) { // Check if we need to fill the buffer.
    
    // Initialize new segment
    st_segment_t *prep_segment = &segment_buffer[segment_buffer_head];
    prep_segment->flag = SEGMENT_NOOP;
    
    // -----------------------------------------------------------------------------------
    // Determine if we need to load a new planner block. If so, prepare step data.
    if (pl_prep_block == NULL) {
      pl_prep_block = plan_get_block_by_index(pl_prep_index); // Query planner for a queued block
      if (pl_prep_block == NULL) { return; } // No planner blocks. Exit.
  SPINDLE_ENABLE_PORT ^= 1<<SPINDLE_ENABLE_BIT;  
        
      // Increment stepper common data index 
      if ( ++st_data_prep_index == (SEGMENT_BUFFER_SIZE-1) ) { st_data_prep_index = 0; }
        
      // Check if the planner has re-computed this block mid-execution. If so, push the previous 
      // segment data. Otherwise, prepare a new segment data for the new planner block.
      if (pl_partial_block_flag) {
        
        // Prepare new shared segment block data and copy the relevant last segment block data.
        st_data_t *last_st_prep_data;
        last_st_prep_data = st_prep_data;
        st_prep_data = &segment_data[st_data_prep_index];
            
        st_prep_data->step_events_remaining = last_st_prep_data->step_events_remaining;
        st_prep_data->dist_per_step = last_st_prep_data->dist_per_step;
        st_prep_data->step_per_mm = last_st_prep_data->step_per_mm;
        st_prep_data->acceleration = last_st_prep_data->acceleration;

        pl_partial_block_flag = false; // Reset flag
        
      } else {
      
        // Prepare commonly shared planner block data for the ensuing segment buffer moves ad-hoc, since 
        // the planner buffer can dynamically change the velocity profile data as blocks are added.
        st_prep_data = &segment_data[st_data_prep_index];
      
        // Initialize planner block step data
        st_prep_data->step_events_remaining = pl_prep_block->step_event_count;
        st_prep_data->step_per_mm = pl_prep_block->step_event_count/pl_prep_block->millimeters;        
        st_prep_data->dist_per_step = ceil(INV_TIME_MULTIPLIER/st_prep_data->step_per_mm); // (mult*mm/step)
        st_prep_data->acceleration = st_prep_data->step_per_mm*pl_prep_block->acceleration;
        
      }
      
      // Convert planner entry speed to stepper initial rate. 
      st_prep_data->current_rate = st_prep_data->step_per_mm*sqrt(pl_prep_block->entry_speed_sqr);
  
      // Determine current block exit speed
      plan_block_t *pl_next_block = plan_get_block_by_index(plan_next_block_index(pl_prep_index));
      float exit_speed_sqr;  
      if (pl_next_block != NULL) { 
        exit_speed_sqr = pl_next_block->entry_speed_sqr; 
        st_prep_data->exit_rate = st_prep_data->step_per_mm*sqrt(exit_speed_sqr);
      } else {
        exit_speed_sqr = 0.0; // End of planner buffer. Zero speed.
        st_prep_data->exit_rate = 0.0;
      }
  
      // Determine velocity profile based on the 7 possible types: Cruise-only, cruise-deceleration,
      // acceleration-cruise, acceleration-only, deceleration-only, trapezoid, and triangle.
      st_prep_data->accelerate_until = pl_prep_block->millimeters;      
      if (pl_prep_block->entry_speed_sqr == pl_prep_block->nominal_speed_sqr) {
        st_prep_data->maximum_rate = sqrt(pl_prep_block->nominal_speed_sqr);
        st_prep_data->accelerate_until = pl_prep_block->millimeters;
        if (exit_speed_sqr == pl_prep_block->nominal_speed_sqr) { // Cruise-only type
          st_prep_data->decelerate_after = 0.0;
        } else { // Cruise-deceleration type
          st_prep_data->decelerate_after = (pl_prep_block->nominal_speed_sqr-exit_speed_sqr)/(2*pl_prep_block->acceleration);
        }
      } else if (exit_speed_sqr == pl_prep_block->nominal_speed_sqr) { 
        // Acceleration-cruise type
        st_prep_data->maximum_rate = sqrt(pl_prep_block->nominal_speed_sqr);
        st_prep_data->decelerate_after = 0.0;
        st_prep_data->accelerate_until -= (pl_prep_block->nominal_speed_sqr-pl_prep_block->entry_speed_sqr)/(2*pl_prep_block->acceleration);
      } else {
        float intersection_dist = 0.5*( pl_prep_block->millimeters + (pl_prep_block->entry_speed_sqr
                                  - exit_speed_sqr)/(2*pl_prep_block->acceleration) );
        if (intersection_dist > 0.0) {
          if (intersection_dist < pl_prep_block->millimeters) { // Either trapezoid or triangle types
            st_prep_data->decelerate_after = (pl_prep_block->nominal_speed_sqr-exit_speed_sqr)/(2*pl_prep_block->acceleration);
            if (st_prep_data->decelerate_after < intersection_dist) { // Trapezoid type
              st_prep_data->maximum_rate = sqrt(pl_prep_block->nominal_speed_sqr);
              st_prep_data->accelerate_until -= (pl_prep_block->nominal_speed_sqr-pl_prep_block->entry_speed_sqr)/(2*pl_prep_block->acceleration);
            } else { // Triangle type
              st_prep_data->decelerate_after = intersection_dist;
              st_prep_data->maximum_rate = sqrt(2*pl_prep_block->acceleration*st_prep_data->decelerate_after+exit_speed_sqr);
              st_prep_data->accelerate_until -= st_prep_data->decelerate_after;
            }          
          } else { // Deceleration-only type
            st_prep_data->maximum_rate = sqrt(pl_prep_block->entry_speed_sqr);
            st_prep_data->decelerate_after = pl_prep_block->millimeters;
          }
        } else { // Acceleration-only type
          st_prep_data->maximum_rate = sqrt(exit_speed_sqr);
          st_prep_data->decelerate_after = 0.0;
          st_prep_data->accelerate_until = 0.0;
        }
      }
      
      // Determine block velocity profile parameters
//       st_prep_data->accelerate_until = (pl_prep_block->nominal_speed_sqr-pl_prep_block->entry_speed_sqr)/(2*pl_prep_block->acceleration);
//       st_prep_data->decelerate_after = (pl_prep_block->nominal_speed_sqr-exit_speed_sqr)/(2*pl_prep_block->acceleration);
//          
//       // Determine if velocity profile is a triangle or trapezoid.
//       if (pl_prep_block->millimeters < st_prep_data->accelerate_until+st_prep_data->decelerate_after) {  
//         st_prep_data->decelerate_after = 0.5*( pl_prep_block->millimeters + (pl_prep_block->entry_speed_sqr
//                                           - exit_speed_sqr)/(2*pl_prep_block->acceleration) );
//         st_prep_data->accelerate_until = pl_prep_block->millimeters-st_prep_data->decelerate_after;
//         st_prep_data->maximum_speed = sqrt(2*pl_prep_block->acceleration*st_prep_data->decelerate_after+exit_speed_sqr);
//       } else {
//         st_prep_data->accelerate_until = pl_prep_block->millimeters-st_prep_data->accelerate_until;
//         st_prep_data->maximum_speed = sqrt(pl_prep_block->nominal_speed_sqr);
//       }

      // Convert velocity profile parameters in terms of steps.
      st_prep_data->maximum_rate *= st_prep_data->step_per_mm;
      st_prep_data->accelerate_until *= st_prep_data->step_per_mm;
      st_prep_data->decelerate_after *= st_prep_data->step_per_mm;

    }

    // Set new segment to point to the current segment data block.
    prep_segment->st_data_index = st_data_prep_index;

    // -----------------------------------------------------------------------------------
    // Initialize segment execute distance. Attempt to create a full segment over DT_SEGMENT.
    // NOTE: Computed in terms of steps and seconds to prevent numerical round-off issues.

    float steps_remaining = st_prep_data->step_events_remaining;
    float dt = DT_SEGMENT;           
    if (steps_remaining > st_prep_data->accelerate_until) { // Acceleration ramp
      steps_remaining -= st_prep_data->current_rate*DT_SEGMENT
                         + st_prep_data->acceleration*(0.5*DT_SEGMENT*DT_SEGMENT);
      if (steps_remaining < st_prep_data->accelerate_until) { // **Incomplete** Acceleration ramp end.
        // Acceleration-cruise, acceleration-deceleration ramp junction, or end of block
        steps_remaining = st_prep_data->accelerate_until;
        dt = 2*(st_prep_data->step_events_remaining-steps_remaining)/
             (st_prep_data->current_rate+st_prep_data->maximum_rate);
        st_prep_data->current_rate = st_prep_data->maximum_rate;
      } else { // **Complete** Acceleration only. 
        st_prep_data->current_rate += st_prep_data->acceleration*DT_SEGMENT;
      }
    } else if (steps_remaining <= st_prep_data->decelerate_after) { // Deceleration ramp
      steps_remaining -= st_prep_data->current_rate*DT_SEGMENT
                         - st_prep_data->acceleration*(0.5*DT_SEGMENT*DT_SEGMENT);
      if (steps_remaining > 0) { // **Complete** Deceleration only.
        st_prep_data->current_rate -= st_prep_data->acceleration*DT_SEGMENT;
      } else { // **Complete* End of block.
        dt = 2*st_prep_data->step_events_remaining/(st_prep_data->current_rate+st_prep_data->exit_rate);
        steps_remaining = 0;
        // st_prep_data->current_speed = st_prep_data->exit_speed;
      }
    } else { // Cruising profile
      steps_remaining -= st_prep_data->maximum_rate*DT_SEGMENT;
      if (steps_remaining < st_prep_data->decelerate_after) { // **Incomplete** End of cruise. 
        steps_remaining = st_prep_data->decelerate_after;
        dt = (st_prep_data->step_events_remaining-steps_remaining)/st_prep_data->maximum_rate;
      } // Otherwise **Complete** Cruising only.
    }
    
    // -----------------------------------------------------------------------------------
    // If segment is incomplete, attempt to fill the remainder.
    // NOTE: Segment remainder always spans a cruise and/or a deceleration ramp.
    
    if (dt < DT_SEGMENT) {
      if (steps_remaining > 0) { // Skip if end of block.
        float last_steps_remaining;
        
        // Fill incomplete segment with an acceleration junction. 
        if (steps_remaining > st_prep_data->decelerate_after) { // Cruising profile
          last_steps_remaining = steps_remaining;
          steps_remaining -= st_prep_data->current_rate*(DT_SEGMENT-dt);
          if (steps_remaining < st_prep_data->decelerate_after) { // **Incomplete**
            steps_remaining = st_prep_data->decelerate_after;
            dt += (last_steps_remaining-steps_remaining)/st_prep_data->maximum_rate;
            // current_speed = maximum_speed;
          } else { // **Complete** Segment filled. 
            dt = DT_SEGMENT;
          }
        }
        
        // Fill incomplete segment with a deceleration junction.
        if (steps_remaining > 0) {
          if (steps_remaining <= st_prep_data->decelerate_after) { // Deceleration ramp
            last_steps_remaining = steps_remaining;
            float dt_remainder = DT_SEGMENT-dt;
            steps_remaining -= dt_remainder*(st_prep_data->current_rate 
                               - 0.5*st_prep_data->acceleration*dt_remainder);
            if (steps_remaining > 0) { // **Complete** Segment filled.
              st_prep_data->current_rate -= st_prep_data->acceleration*dt_remainder;
              dt = DT_SEGMENT;
            } else { // **Complete** End of block.
              steps_remaining = 0;
              dt += (2*last_steps_remaining/(st_prep_data->current_rate+st_prep_data->exit_rate));
              // st_prep_data->current_speed = st_prep_data->exit_speed;
            }
          }
        }
        
      }
    }
 
    // -----------------------------------------------------------------------------------
    // Compute segment step rate, steps to execute, and step phase correction parameters.
    // NOTE: 
    
    // !!! PROBLEM. Step events remaining in floating point can limit the number of steps
    // we can accurately track, since floats have 8 significant digits. However, this only
    // becomes a problem if there are more than 10,000,000, which translates to a CNC machine
    // with 800 step/mm and 10 meters of axis travel.
    
    prep_segment->dist_per_tick = ceil((st_prep_data->step_events_remaining-steps_remaining)
                                  /dt*(INV_TIME_MULTIPLIER/ISR_TICKS_PER_SECOND)); // (mult*mm/isr_tic)
      
    if (steps_remaining > 0) { 

      // Compute number of steps to execute and segment step phase correction. 
      prep_segment->n_step = ceil(st_prep_data->step_events_remaining)-ceil(steps_remaining);
      prep_segment->n_phase_tick = ceil((ceil(steps_remaining)-steps_remaining)*st_prep_data->dist_per_step);
          
    } else { // End of block. Finish it out.
      
      // Set to execute the remaining steps and no phase correction upon finishing the block. 
      prep_segment->n_step = ceil(st_prep_data->step_events_remaining);
      prep_segment->n_phase_tick = 0;
 
      // Move planner pointer to next block and flag to load a new block for the next segment.
      pl_prep_index = plan_next_block_index(pl_prep_index);
      pl_prep_block = NULL;
      prep_segment->flag |= SEGMENT_END_OF_BLOCK;
    }
  
    // Update step execution variables
    st_prep_data->step_events_remaining = steps_remaining;

   // Ensure the initial step rate exceeds the MINIMUM_STEP_RATE.
   // TODO: Use config.h error checking to do this. Otherwise, counters get screwy.

    // New step segment initialization completed. Increment segment buffer indices.
    segment_buffer_head = segment_next_head;
    if ( ++segment_next_head == SEGMENT_BUFFER_SIZE ) { segment_next_head = 0; }
  SPINDLE_ENABLE_PORT ^= 1<<SPINDLE_ENABLE_BIT;  
  } 
}      


uint8_t st_get_prep_block_index() 
{
// Returns only the index but doesn't state if the block has been partially executed. How do we simply check for this?
  return(pl_prep_index);
}


void st_fetch_partial_block_parameters(uint8_t block_index, float *millimeters_remaining, uint8_t *is_decelerating)
{
 // if called, can we assume that this always changes and needs to be updated? if so, then
 // we can perform all of the segment buffer setup tasks here to make sure the next time
 // the segments are loaded, the st_data buffer is updated correctly.
 // !!! Make sure that this is always pointing to the correct st_prep_data block. 
 
 // When a mid-block acceleration occurs, we have to make sure the ramp counters are updated
 // correctly, much in the same fashion as the deceleration counters. Need to think about this 
 // make sure this is right, but i'm pretty sure it is.
  
  // TODO: NULL means that the segment buffer has just completed a planner block. Clean up!
  if (pl_prep_block != NULL) {
    *millimeters_remaining = st_prep_data->step_events_remaining/st_prep_data->step_per_mm;
    if (st_prep_data->step_events_remaining < st_prep_data->decelerate_after) { *is_decelerating = true; } 
    else { *is_decelerating = false; }

    // Flag for new prep_block when st_prep_buffer() is called after the planner recomputes.
    pl_partial_block_flag = true;
    pl_prep_block = NULL;
  }
  return;
}