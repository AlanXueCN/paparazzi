/*
 * $Id$
 *  
 * Copyright (C) 2003-2005  Antoine Drouin
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 *
 */

#include <math.h>

#include "adc.h"
#include "pid.h"
#include "gps.h"
#include "infrared.h"
#include "ap_downlink.h"
#include "nav.h"
#include "autopilot.h"
#include "estimator.h"
#include "if_calib.h"
#include "cam.h"
#include "traffic_info.h"
#include "link_mcu.h"
#include "sys_time.h"
#include "flight_plan.h"

#ifdef LED
#include "led.h"
#endif

#ifdef TELEMETER
#include "srf08.h"
#endif

#if defined AHRS
#include "ahrs.h"
#endif // AHRS

#ifndef AUTO1_MAX_ROLL
#define AUTO1_MAX_ROLL 0.6
#endif

#ifndef AUTO1_MAX_PITCH
#define AUTO1_MAX_PITCH 0.5
#endif

#define LOW_BATTERY_DECIVOLT (LOW_BATTERY*10)


uint8_t  inflight_calib_mode = IF_CALIB_MODE_NONE;


/** Define minimal speed for takeoff in m/s */
#define MIN_SPEED_FOR_TAKEOFF 5.


uint8_t fatal_error_nb = 0;
static const uint16_t version = 1;

uint8_t pprz_mode = PPRZ_MODE_MANUAL;
uint8_t vertical_mode = VERTICAL_MODE_MANUAL;
uint8_t lateral_mode = LATERAL_MODE_MANUAL;

#ifdef AHRS
uint8_t ir_estim_mode = IR_ESTIM_MODE_OFF;
#else
uint8_t ir_estim_mode = IR_ESTIM_MODE_ON;
#endif // AHRS

bool_t auto_pitch = FALSE;

bool_t rc_event_1, rc_event_2;

uint8_t vsupply;

static uint8_t  mcu1_status, mcu1_ppm_cpt;

static bool_t low_battery = FALSE;

float slider_1_val, slider_2_val;

bool_t launch = FALSE;

float energy; /** Fuel consumption */


#define Min(x, y) (x < y ? x : y)
#define Max(x, y) (x > y ? x : y)


/** \brief Update paparazzi mode
 */
static inline uint8_t pprz_mode_update( void ) {
  /** We remain in home mode until explicit reset from the RC */
  if ((pprz_mode != PPRZ_MODE_HOME &&
       pprz_mode != PPRZ_MODE_GPS_OUT_OF_ORDER)
      || CheckEvent(rc_event_1)) {
    ModeUpdate(pprz_mode, PPRZ_MODE_OF_PULSE(from_fbw.from_fbw.channels[RADIO_MODE], from_fbw.from_fbw.status));
  } else
    return FALSE;
}

#ifdef RADIO_LLS
/** \fn inline uint8_t ir_estim_mode_update( void )
 *  \brief update ir estimation if RADIO_LLS is true \n
 */
inline uint8_t ir_estim_mode_update( void ) {
  ModeUpdate(ir_estim_mode, IR_ESTIM_MODE_OF_PULSE(from_fbw.from_fbw.channels[RADIO_LLS]));
}
#endif


static inline uint8_t mcu1_status_update( void ) {
  uint8_t new_mode = from_fbw.from_fbw.status;
  if (mcu1_status != new_mode) {
    bool_t changed = ((mcu1_status&MASK_FBW_CHANGED) != (new_mode&MASK_FBW_CHANGED));
    mcu1_status = new_mode;
    return changed;
  }
  return FALSE;
}

/** Delay between @@@@@ A FIXER @@@@@ */
#define EVENT_DELAY 20

/** \def EventUpdate(_cpt, _cond, _event)
 *  @@@@@ A FIXER @@@@@
 */
#define EventUpdate(_cpt, _cond, _event) \
  if (_cond) { \
    if (_cpt < EVENT_DELAY) { \
      _cpt++; \
      if (_cpt == EVENT_DELAY) \
        _event = TRUE; \
    } \
  } else { \
    _cpt = 0; \
  }


#if defined RADIO_CALIB && defined RADIO_CONTROL_CALIB
static inline uint8_t inflight_calib_mode_update ( void ) {
  ModeUpdate(inflight_calib_mode, IF_CALIB_MODE_OF_PULSE(from_fbw.from_fbw.channels[RADIO_CALIB]));
}
#define EventPos(_cpt, _channel, _event) \
 EventUpdate(_cpt, (inflight_calib_mode==IF_CALIB_MODE_NONE && from_fbw.from_fbw.channels[_channel]>(int)(0.75*MAX_PPRZ)), _event)

#define EventNeg(_cpt, _channel, _event) \
 EventUpdate(_cpt, (inflight_calib_mode==IF_CALIB_MODE_NONE && from_fbw.from_fbw.channels[_channel]<(int)(-0.75*MAX_PPRZ)), _event)

#else //  RADIO_CALIB && defined RADIO_CONTROL_CALIB

#define EventPos(_cpt, _channel, _event) \
 EventUpdate(_cpt, (from_fbw.from_fbw.channels[_channel]>(int)(0.75*MAX_PPRZ)), _event)

#define EventNeg(_cpt, _channel, _event) \
 EventUpdate(_cpt, (from_fbw.from_fbw.channels[_channel]<(int)(-0.75*MAX_PPRZ)), _event)

#endif //  RADIO_CALIB && defined RADIO_CONTROL_CALIB




static inline void events_update( void ) {
  static uint16_t event1_cpt = 0;
  EventPos(event1_cpt, RADIO_GAIN1, rc_event_1);
  static uint16_t event2_cpt = 0;
  EventNeg(event2_cpt, RADIO_GAIN1, rc_event_2);
}  


/** \brief Send back uncontrolled channels (actually only rudder)
 */
static inline void copy_from_to_fbw ( void ) {
#ifdef COMMAND_YAW /* FIXME */
  from_ap.from_ap.channels[COMMAND_YAW] =from_fbw.from_fbw.channels[RADIO_YAW];
#endif
}



/* 
   called at 20Hz.
   sends a serie of initialisation messages followed by a stream of periodic ones
*/ 

/** Define number of message at initialisation */
#define INIT_MSG_NB 2

uint8_t ac_ident = AC_ID;

/** \brief Send a serie of initialisation messages followed by a stream of periodic ones
 *
 * Called at 10Hz.
 */
static inline void reporting_task( void ) {
  static uint8_t boot = TRUE;

  /** initialisation phase during boot */
  if (boot) {
    DOWNLINK_SEND_BOOT(&version);
    SEND_RAD_OF_IR();
    boot = FALSE;
  }
  /** then report periodicly */
  else {
//    IO1CLR = LED_2_BIT;
    PeriodicSendAp();
//    IO1SET = LED_2_BIT;
  }
}

/** \brief Function to be called when a command is available (usually comming from the radio command)
 */
inline void telecommand_task( void ) {
  uint8_t mode_changed = FALSE;
  copy_from_to_fbw();
  if ((bit_is_set(from_fbw.from_fbw.status, RADIO_REALLY_LOST) && (pprz_mode == PPRZ_MODE_AUTO1 || pprz_mode == PPRZ_MODE_MANUAL)) || too_far_from_home) {
    pprz_mode = PPRZ_MODE_HOME;
    mode_changed = TRUE;
  }
  if (bit_is_set(from_fbw.from_fbw.status, AVERAGED_CHANNELS_SENT)) {
    bool_t pprz_mode_changed = pprz_mode_update();
    mode_changed |= pprz_mode_changed;
#ifdef RADIO_LLS
    mode_changed |= ir_estim_mode_update();
#endif
#if defined RADIO_CALIB && defined RADIO_CONTROL_CALIB
    bool_t calib_mode_changed = inflight_calib_mode_update();
    inflight_calib(calib_mode_changed || pprz_mode_changed);
    mode_changed |= calib_mode_changed;
#endif
  }
  mode_changed |= mcu1_status_update();
  if ( mode_changed )
    PERIODIC_SEND_PPRZ_MODE();
  
  /** If Auto1 mode, compute \a desired_roll and \a desired_pitch from 
   * \a RADIO_ROLL and \a RADIO_PITCH \n
   * Else asynchronously set by \a course_pid_run
   */
  if (pprz_mode == PPRZ_MODE_AUTO1) {
    /** In Auto1 mode, roll is bounded between [-AUTO1_MAX_ROLL;AUTO1_MAX_ROLL] */
    desired_roll = FLOAT_OF_PPRZ(from_fbw.from_fbw.channels[RADIO_ROLL], 0., -AUTO1_MAX_ROLL);
    
    /** In Auto1 mode, pitch is bounded between [-AUTO1_MAX_PITCH;AUTO1_MAX_PITCH] */
    desired_pitch = FLOAT_OF_PPRZ(from_fbw.from_fbw.channels[RADIO_PITCH], 0., AUTO1_MAX_PITCH);
  }
  if (pprz_mode == PPRZ_MODE_MANUAL || pprz_mode == PPRZ_MODE_AUTO1) {
    desired_gaz = from_fbw.from_fbw.channels[RADIO_THROTTLE];
  }
  /** else asynchronously set by climb_pid_run(); */
  
  mcu1_ppm_cpt = from_fbw.from_fbw.ppm_cpt;
  vsupply = from_fbw.from_fbw.vsupply;
  
  events_update();
  
  if (!estimator_flight_time) {
#ifdef INFRARED
    ground_calibrate(STICK_PUSHED(from_fbw.from_fbw.channels[RADIO_ROLL]));
#endif
    if (pprz_mode == PPRZ_MODE_AUTO2 && from_fbw.from_fbw.channels[RADIO_THROTTLE] > GAZ_THRESHOLD_TAKEOFF) {
      launch = TRUE;
    }
  }
}

/** \fn void navigation_task( void )
 *  \brief Compute desired_course
 */
static void navigation_task( void ) {
#if defined GPS && defined FAILSAFE_DELAY_WITHOUT_GPS
  /** This section is used for the failsafe of GPS */
  static uint8_t last_pprz_mode;
  static bool_t has_lost_gps = FALSE;
	
  /** Test if we lost the GPS */
  if (!GPS_FIX_VALID(gps_mode) ||
      (cpu_time - last_gps_msg_t > FAILSAFE_DELAY_WITHOUT_GPS)) {
    /** If aircraft is launch and is in autonomus mode, go into
	PPRZ_MODE_GPS_OUT_OF_ORDER mode (Failsafe). */
    if (launch && (pprz_mode == PPRZ_MODE_AUTO2 ||
		   pprz_mode == PPRZ_MODE_HOME)) {
      last_pprz_mode = pprz_mode;
      pprz_mode = PPRZ_MODE_GPS_OUT_OF_ORDER;
      PERIODIC_SEND_PPRZ_MODE();
      has_lost_gps = TRUE;
    }
  }
  /** If aircraft was in failsafe mode, come back in previous mode */
  else if (has_lost_gps) {
    pprz_mode = last_pprz_mode;
    has_lost_gps = FALSE;
    PERIODIC_SEND_PPRZ_MODE();
  }
#endif /* GPS && FAILSAFE_DELAY_WITHOUT_GPS */
  
  /** Default to keep compatibility with previous behaviour */
  lateral_mode = LATERAL_MODE_COURSE;
  if (pprz_mode == PPRZ_MODE_HOME)
    nav_home();
  else if (pprz_mode == PPRZ_MODE_GPS_OUT_OF_ORDER)
    nav_without_gps();
  else
    nav_update();
  
  SEND_NAVIGATION();

  SEND_CAM();
  
  
  if (pprz_mode == PPRZ_MODE_AUTO2 || pprz_mode == PPRZ_MODE_HOME
			|| pprz_mode == PPRZ_MODE_GPS_OUT_OF_ORDER) {
    if (lateral_mode >=LATERAL_MODE_COURSE)
      course_pid_run(); /* aka compute nav_desired_roll */
    desired_roll = nav_desired_roll;
    if (vertical_mode == VERTICAL_MODE_AUTO_ALT)
      altitude_pid_run();
    if (vertical_mode >= VERTICAL_MODE_AUTO_CLIMB)
      climb_pid_run();
    if (vertical_mode == VERTICAL_MODE_AUTO_GAZ)
      desired_gaz = nav_desired_gaz;
    desired_pitch = nav_pitch;
    if (low_battery || (!estimator_flight_time && !launch))
      desired_gaz = 0;
  }  
  energy += (float)desired_gaz * (MILLIAMP_PER_PERCENT / MAX_PPRZ * 0.25);
}


#define PERIOD (256. * 1024. / CLOCK / 1000000.)

/** Maximum time allowed for low battery level */
#define LOW_BATTERY_DELAY 5

/** \fn inline void periodic_task( void )
 *  \brief Do periodic tasks at 61 Hz
 */
/**There are four @@@@@ boucles @@@@@:
 * - 20 Hz:
 *   - lets use \a reporting_task at 10 Hz
 *   - updates ir with \a ir_update
 *   - updates estimator of ir with \a estimator_update_state_infrared
 *   - set \a desired_aileron and \a desired_elevator with \a roll_pitch_pid_run
 *   - sends to \a fbw \a desired_gaz, \a desired_aileron and
 *     \a desired_elevator \note \a desired_gaz is set upon GPS
 *     message reception
 * - 10 Hz: to get a \a stage_time_ds
 * - 4 Hz:
 *   - calls \a estimator_propagate_state
 *   - do navigation with \a navigation_task
 *
 */
inline void periodic_task( void ) {
  static uint8_t _20Hz   = 0;
  static uint8_t _10Hz   = 0;
  static uint8_t _4Hz   = 0;
  static uint8_t _1Hz   = 0;

  estimator_t += PERIOD;

  _20Hz++;
  if (_20Hz>=3) _20Hz=0;
  _10Hz++;
  if (_10Hz>=6) _10Hz=0;
  _4Hz++;
  if (_4Hz>=15) _4Hz=0;
  _1Hz++;
  if (_1Hz>=61) _1Hz=0;
	  
 
  
  if (!_10Hz) {
    stage_time_ds = stage_time_ds + .1;
  }
  if (!_1Hz) {
    if (estimator_flight_time) estimator_flight_time++;
    cpu_time++;
    stage_time_ds = (int16_t)(stage_time_ds+.5);
    stage_time++;
    block_time++;

    static uint8_t t = 0;
    if (vsupply < LOW_BATTERY_DECIVOLT) t++; else t = 0;
    low_battery |= (t >= LOW_BATTERY_DELAY);
  }
  switch (_1Hz) {
#ifdef TELEMETER
  case 1:
    srf08_initiate_ranging();
    break;
  case 5:
    /** 65ms since initiate_ranging() (the spec ask for 65ms) */
    srf08_receive();
    break;
  case 10:
    SEND_RAD_OF_IR();
    break;
#endif
  }
  switch(_4Hz) {
  case 0:
    estimator_propagate_state();
    navigation_task();
    break;
  case 1:
    if (!estimator_flight_time && 
	estimator_hspeed_mod > MIN_SPEED_FOR_TAKEOFF) {
      estimator_flight_time = 1;
      launch = TRUE; /* Not set in non auto launch */
      DOWNLINK_SEND_TAKEOFF(&cpu_time);
    }
    break;
    /*  default: */
  }
#if defined AHRS
  ahrs_update();//<8,6 ms called at 60hz
#endif
  switch (_20Hz) {
  case 0:
    break;
  case 1: {
    static uint8_t odd;
    odd++;
    if (odd & 0x01)
      reporting_task();
    break;
  }
  case 2:
    
#if defined IMU_3DMG
  //estimator_update_state_3DMG( );

#elif defined IMU_ANALOG && defined AHRS
   //ahrs_update();show up (it's called at 60/4 Hz)
   estimator_update_state_ANALOG( );
#elif defined INFRARED /*NO IMU*/
    ir_update();
    estimator_update_state_infrared();
#endif
    roll_pitch_pid_run(); /* Set  desired_aileron & desired_elevator */
    from_ap.from_ap.channels[COMMAND_THROTTLE] = desired_gaz; /* desired_gaz is set upon GPS message reception */
    from_ap.from_ap.channels[COMMAND_ROLL] = desired_aileron;
    from_ap.from_ap.channels[COMMAND_PITCH] = desired_elevator;
    
#if defined MCU_SPI_LINK && !defined SITL
    link_fbw_send();
#endif
    break;
  default:
    fatal_error_nb++;
  }
}
