/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2013--2016
*/

/*
 Simple schedule support for TRV.
 */


#include <util/atomic.h>

#include <Arduino.h>

#include "OTV0P2BASE_SimpleValveSchedule.h"

#include "OTV0P2BASE_EEPROM.h"
#include "OTV0P2BASE_RTC.h"


namespace OTV0P2BASE
{


//#if defined(UNIT_TESTS)
//// Support for unit tests to force particular apparent schedule state.
//// Current override state; 0 (default) means no override.
//static _TEST_schedule_override _soUT_override;
//// Set the override value (or remove the override).
//void _TEST_set_schedule_override(const _TEST_schedule_override override)
//  { _soUT_override = override; }
//#endif

// Maximum mins-after-midnight compacted value in one byte.
static const uint8_t MAX_COMPRESSED_MINS_AFTER_MIDNIGHT = ((OTV0P2BASE::MINS_PER_DAY / SimpleValveScheduleBase::SIMPLE_SCHEDULE_GRANULARITY_MINS) - 1);

// Get the simple/primary schedule on time, as minutes after midnight [0,1439]; invalid (eg ~0) if none set.
// Will usually include a pre-warm time before the actual time set.
// Note that unprogrammed EEPROM value will result in invalid time, ie schedule not set.
//   * which  schedule number, counting from 0
uint_least16_t SimpleValveScheduleBase::getSimpleScheduleOn(const uint8_t which)
  {
  if(which >= MAX_SIMPLE_SCHEDULES) { return(~0); } // Invalid schedule number.
  uint8_t startMM;
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    { startMM = eeprom_read_byte((uint8_t*)(V0P2BASE_EE_START_SIMPLE_SCHEDULE0_ON + which)); }
  if(startMM > MAX_COMPRESSED_MINS_AFTER_MIDNIGHT) { return(~0); } // No schedule set.
  // Compute start time from stored schedule value.
  uint_least16_t startTime = SIMPLE_SCHEDULE_GRANULARITY_MINS * startMM;
// If LEARN_BUTTON_AVAILABLE then in the absence of anything better SUPPORT_SINGLETON_SCHEDULE should be supported.
//#ifdef LEARN_BUTTON_AVAILABLE
  const uint8_t windBackM = PREWARM_MINS; // Wind back start time to allow room to get to temp on time.
  if(windBackM > startTime) { startTime += OTV0P2BASE::MINS_PER_DAY; } // Allow for wrap-around at midnight.
  startTime -= windBackM;
//#endif
  return(startTime);
  }

// Get the simple/primary schedule off time, as minutes after midnight [0,1439]; invalid (eg ~0) if none set.
// This is based on specified start time and some element of the current eco/comfort bias.
//   * which  schedule number, counting from 0
uint_least16_t SimpleValveScheduleBase::getSimpleScheduleOff(const uint8_t which)
  {
  const uint_least16_t startMins = getSimpleScheduleOn(which);
  if(startMins == (uint_least16_t)~0) { return(~0); }
  // Compute end from start, allowing for wrap-around at midnight.
  uint_least16_t endTime = startMins + PREWARM_MINS + onTime();
  if(endTime >= OTV0P2BASE::MINS_PER_DAY) { endTime -= OTV0P2BASE::MINS_PER_DAY; } // Allow for wrap-around at midnight.
  return(endTime);
  }

// Set the simple/primary simple on time.
//   * startMinutesSinceMidnightLT  is start/on time in minutes after midnight [0,1439]
//   * which  schedule number, counting from 0
// Invalid parameters will be ignored and false returned,
// else this will return true and isSimpleScheduleSet() will return true after this.
// NOTE: over-use of this routine may prematurely wear out the EEPROM.
bool SimpleValveScheduleBase::setSimpleSchedule(const uint_least16_t startMinutesSinceMidnightLT, const uint8_t which)
  {
  if(which >= MAX_SIMPLE_SCHEDULES) { return(false); } // Invalid schedule number.
  if(startMinutesSinceMidnightLT >= OTV0P2BASE::MINS_PER_DAY) { return(false); } // Invalid time.

  // Set the schedule, minimising wear.
  const uint8_t startMM = startMinutesSinceMidnightLT / SIMPLE_SCHEDULE_GRANULARITY_MINS; // Round down...
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    { OTV0P2BASE::eeprom_smart_update_byte((uint8_t*)(V0P2BASE_EE_START_SIMPLE_SCHEDULE0_ON + which), startMM); }
  return(true); // Assume EEPROM programmed OK...
  }

// Clear a simple schedule.
// There will be neither on nor off events from the selected simple schedule once this is called.
//   * which  schedule number, counting from 0
void SimpleValveScheduleBase::clearSimpleSchedule(const uint8_t which)
  {
  if(which >= MAX_SIMPLE_SCHEDULES) { return; } // Invalid schedule number.
  // Clear the schedule back to 'unprogrammed' values, minimising wear.
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    { OTV0P2BASE::eeprom_smart_erase_byte((uint8_t*)(V0P2BASE_EE_START_SIMPLE_SCHEDULE0_ON + which)); }
  }

// Returns true if any simple schedule is set, false otherwise.
// This implementation just checks for any valid schedule 'on' time.
// In unit-test override mode is true for soon/now, false for off.
bool SimpleValveScheduleBase::isAnySimpleScheduleSet()
  {
//#if defined(UNIT_TESTS)
//  // Special behaviour for unit tests.
//  switch(_soUT_override)
//    {
//    case _soUT_off: return(false);
//    case _soUT_soon: return(true);
//    case _soUT_now: return(true);
//    }
//#endif

  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    for(uint8_t which = 0; which < MAX_SIMPLE_SCHEDULES; ++which)
      {
      if(eeprom_read_byte((uint8_t*)(V0P2BASE_EE_START_SIMPLE_SCHEDULE0_ON + which)) <= MAX_COMPRESSED_MINS_AFTER_MIDNIGHT)
        { return(true); }
      }
    }
  return(false);
  }


// True iff any schedule is currently 'on'/'WARM' even when schedules overlap.
// May be relatively slow/expensive.
// Can be used to suppress all 'off' activity except for the final one.
// Can be used to suppress set-backs during on times.
// In unit-test override mode is true for now, false for soon/off.
bool SimpleValveScheduleBase::isAnyScheduleOnWARMNow()
  {
//#if defined(UNIT_TESTS)
//  // Special behaviour for unit tests.
//  switch(_soUT_override)
//    {
//    case _soUT_off: return(false);
//    case _soUT_soon: return(false);
//    case _soUT_now: return(true);
//    }
//#endif

  const uint_least16_t mm = OTV0P2BASE::getMinutesSinceMidnightLT();

  for(uint8_t which = 0; which < MAX_SIMPLE_SCHEDULES; ++which)
    {
    const uint_least16_t s = getSimpleScheduleOn(which);
    if(mm < s) { continue; } // Also deals with case where this schedule is not set at all (s == ~0);
    uint_least16_t e = getSimpleScheduleOff(which);
    if(e < s) { e += OTV0P2BASE::MINS_PER_DAY; } // Cope with schedule wrap around midnight.
    if(mm < e) { return(true); }
    }

  return(false);
  }


// True iff any schedule is due 'on'/'WARM' soon even when schedules overlap.
// May be relatively slow/expensive.
// Can be used to allow room to be brought up to at least a set-back temperature
// if very cold when a WARM period is due soon (to help ensure that WARM target is met on time).
// In unit-test override mode is true for soon, false for now/off.
bool SimpleValveScheduleBase::isAnyScheduleOnWARMSoon()
  {
#if defined(UNIT_TESTS)
  // Special behaviour for unit tests.
  switch(_soUT_override)
    {
    case _soUT_off: return(false);
    case _soUT_soon: return(true);
    case _soUT_now: return(false);
    }
#endif

  const uint_least16_t mm0 = OTV0P2BASE::getMinutesSinceMidnightLT() + PREPREWARM_MINS; // Look forward...
  const uint_least16_t mm = (mm0 >= OTV0P2BASE::MINS_PER_DAY) ? (mm0 - OTV0P2BASE::MINS_PER_DAY) : mm0;

  for(uint8_t which = 0; which < MAX_SIMPLE_SCHEDULES; ++which)
    {
    const uint_least16_t s = getSimpleScheduleOn(which);
    if(mm < s) { continue; } // Also deals with case where this schedule is not set at all (s == ~0);
    uint_least16_t e = getSimpleScheduleOff(which);
    if(e < s) { e += OTV0P2BASE::MINS_PER_DAY; } // Cope with schedule wrap around midnight.
    if(mm < e) { return(true); }
    }

  return(false);
  }




//#ifdef ENABLE_ANTICIPATION
//// Returns true iff room likely to be occupied and need warming at the specified hour's sample point based on collected stats.
//// Used for predictively warming a room in smart mode and for choosing setback depths.
//// Returns false if no good evidence to warm the room at the given time based on past history over about one week.
////   * hh hour to check for predictive warming [0,23]
//bool shouldBeWarmedAtHour(const uint_least8_t hh)
//  {
//#ifdef ENABLE_OCCUPANCY_DETECTION_FROM_AMBLIGHT
//  // Return false immediately if the sample hour's historic ambient light level falls in the bottom quartile (or is zero).
//  // Thus aim to shave off 'smart' warming for at least 25% of the daily cycle.
//  if(inOutlierQuartile(false, EE_STATS_SET_AMBLIGHT_BY_HOUR_SMOOTHED, hh)) { return(false); }
//#endif // ENABLE_OCCUPANCY_DETECTION_FROM_AMBLIGHT
//
//#ifdef
//  // Return false immediately if the sample hour's historic occupancy level falls in the bottom quartile (or is zero).
//  // Thus aim to shave off 'smart' warming for at least 25% of the daily cycle.
//  if(inOutlierQuartile(false, EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED, hh)) { return(false); }
//#endif // ENABLE_OCCUPANCY_SUPPORT
//
//  const uint8_t warmHistory = eeprom_read_byte((uint8_t *)(EE_STATS_START_ADDR(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK) + hh));
//  if(0 == (0x80 & warmHistory)) // This hour has a history.
//    {
////    // Return false immediately if no WARM mode this hour for the last week (ie the unit needs reminding at least once per week).
////    if(0 == warmHistory) // No explicit WARM for a week at this hour, prevents 'smart' warming.
////      { return(false); }
//    // Return true immediately if this hour was in WARM mode yesterday or a week ago, and at least one other day.
//    if((0 != (0x41 & warmHistory)) && (0 != (0x3e & warmHistory)))
//      { return(true); }
//    }
//
//  // Return true if immediately the sample hour is usually warm, ie at or above WARM target.
//  const int smoothedTempHHNext = expandTempC16(eeprom_read_byte((uint8_t *)(EE_STATS_START_ADDR(EE_STATS_SET_TEMP_BY_HOUR_SMOOTHED) + hh)));
//#if 0 && defined(DEBUG)
//  DEBUG_SERIAL_PRINT_FLASHSTRING("Smoothed C for ");
//  DEBUG_SERIAL_PRINT(hh);
//  DEBUG_SERIAL_PRINT_FLASHSTRING("h is ");
//  DEBUG_SERIAL_PRINT(smoothedTempHHNext >> 4);
//  DEBUG_SERIAL_PRINTLN();
//#endif
//  if((STATS_UNSET_INT != smoothedTempHHNext) && (((smoothedTempHHNext+8)>>4) >= getWARMTargetC()))
//    { return(true); }
//
//  // No good evidence for room to be warmed for specified hour.
//  return(false);
//  }
//#endif


}