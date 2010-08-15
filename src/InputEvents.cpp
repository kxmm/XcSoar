/*
Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009

	M Roberts (original release)
	Robin Birch <robinb@ruffnready.co.uk>
	Samuel Gisiger <samuel.gisiger@triadis.ch>
	Jeff Goodenough <jeff@enborne.f2s.com>
	Alastair Harrison <aharrison@magic.force9.co.uk>
	Scott Penrose <scottp@dd.com.au>
	John Wharington <jwharington@gmail.com>
	Lars H <lars_hn@hotmail.com>
	Rob Dunning <rob@raspberryridgesheepfarm.com>
	Russell King <rmk@arm.linux.org.uk>
	Paolo Ventafridda <coolwind@email.it>
	Tobias Lohner <tobias@lohner-net.de>
	Mirek Jezek <mjezek@ipplc.cz>
	Max Kellermann <max@duempel.org>
	Tobias Bieniek <tobias.bieniek@gmx.de>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
}
*/

/*

InputEvents

This class is used to control all user and extrnal InputEvents.
This includes some Nmea strings, virtual events (Glide Computer
Evnets) and Keyboard.

What it does not cover is Glide Computer normal processing - this
includes GPS and Vario processing.

What it does include is what to do when an automatic event (switch
to Climb mode) and user events are entered.

It also covers the configuration side of on screen labels.

For further information on config file formats see

source/Common/Data/Input/ALL
doc/html/advanced/input/ALL		http://xcsoar.sourceforge.net/advanced/input/

*/

#include "InputEvents.h"
#include "Protection.hpp"
#include "LogFile.hpp"
#include "Compatibility/vk.h"
#include "ButtonLabel.hpp"
#include "Profile.hpp"
#include "LocalPath.hpp"
#include "UtilsText.hpp"
#include "StringUtil.hpp"
#include "Asset.hpp"
#include "MenuData.hpp"
#include "IO/FileLineReader.hpp"
#include "SettingsUser.hpp"
#include "Screen/Blank.hpp"
#include "MapWindowProjection.hpp"
#include "InfoBoxes/InfoBoxManager.hpp"
#include "Compatibility/string.h" /* for _ttoi() */

#include <algorithm>
#include <assert.h>
#include <ctype.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>

// Sensible maximums
enum {
  MAX_MODE = 64,
  MAX_MODE_STRING = 24,
  MAX_KEY = 255,
  MAX_EVENTS = 2048,
};

/*
  TODO code - All of this input_Errors code needs to be removed and
  replaced with standard logger.  The logger can then display messages
  through Message if ncessary and log to files etc This code, and
  baddly written #ifdef should be moved to Macros in the Log class.
*/

#ifdef _INPUTDEBUG_
// Log first NN input event errors for display in simulator mode
#define MAX_INPUT_ERRORS 5
TCHAR input_errors[MAX_INPUT_ERRORS][3000];
unsigned input_errors_count = 0;
// JMW this is just far too annoying right now,
// since "title" "note" and commencts are not parsed, they
// come up as errors.
#endif

/**
 * For data generated by xci2cpp.pl.
 */
struct flat_event_map {
  unsigned char mode, key;
  unsigned short event;
};

/**
 * For data generated by xci2cpp.pl.
 */
struct flat_label {
  unsigned char mode, location;
  unsigned short event;
  const TCHAR *label;
};

// Current modes - map mode to integer (primitive hash)
InputEvents::mode InputEvents::current_mode = InputEvents::MODE_DEFAULT;

/** Map mode to location */
static TCHAR mode_map[MAX_MODE][MAX_MODE_STRING] = {
  _T("default"),
  _T("pan"),
  _T("infobox"),
  _T("Menu"),
};

static unsigned mode_map_count = 4;

// Key map to Event - Keys (per mode) mapped to events
static unsigned Key2Event[MAX_MODE][MAX_KEY];		// Points to Events location

// Glide Computer Events
static unsigned GC2Event[MAX_MODE][GCE_COUNT];

// NMEA Triggered Events
static unsigned N2Event[MAX_MODE][NE_COUNT];

// Events - What do you want to DO
typedef struct {
  // Which function to call (can be any, but should be here)
  pt2Event event;
  // Parameters
  const TCHAR *misc;
  // Next in event list - eg: Macros
  unsigned next;
} EventSTRUCT;

static EventSTRUCT Events[MAX_EVENTS];
// How many have we defined
/**
 * How many have we defined.
 *
 * This is initialized with 1 because event 0 is reserved - it stands
 * for "no event".
 */
static unsigned Events_count = 1;

static Menu menus[MAX_MODE];

#define MAX_GCE_QUEUE 10
static int GCE_Queue[MAX_GCE_QUEUE];
#define MAX_NMEA_QUEUE 10
static int NMEA_Queue[MAX_NMEA_QUEUE];

// -----------------------------------------------------------------------
// Initialisation and Defaults
// -----------------------------------------------------------------------

bool InitONCE = false;

// Mapping text names of events to the real thing
typedef struct {
  const TCHAR *text;
  pt2Event event;
} Text2EventSTRUCT;

static const Text2EventSTRUCT Text2Event[] = {
#include "InputEvents_Text2Event.cpp"
  { NULL, NULL }
};

// Mapping text names of events to the real thing
static const TCHAR *const Text2GCE[] = {
#include "InputEvents_Text2GCE.cpp"
  NULL
};

// Mapping text names of events to the real thing
static const TCHAR *const Text2NE[] = {
#include "InputEvents_Text2NE.cpp"
  NULL
};

Mutex InputEvents::mutexEventQueue;

static void
apply_defaults(const TCHAR *const* default_modes,
               const EventSTRUCT *default_events, unsigned num_default_events,
               const flat_event_map *default_key2event,
               const flat_event_map *default_gc2event,
               const flat_event_map *default_n2event,
               const flat_label *default_labels)
{
  assert(num_default_events <= MAX_EVENTS);

  for (mode_map_count = 0; default_modes[mode_map_count] != NULL;
       ++mode_map_count)
    _tcscpy(mode_map[mode_map_count], default_modes[mode_map_count]);

  Events_count = num_default_events + 1;
  memcpy(Events + 1, default_events, num_default_events * sizeof(Events[0]));

  while (default_key2event->event > 0) {
    Key2Event[default_key2event->mode][default_key2event->key] =
      default_key2event->event;
    ++default_key2event;
  }

  while (default_gc2event->event > 0) {
    GC2Event[default_gc2event->mode][default_gc2event->key] =
      default_gc2event->event;
    ++default_gc2event;
  }

  while (default_n2event->event > 0) {
    N2Event[default_n2event->mode][default_n2event->key] =
      default_n2event->event;
    ++default_n2event;
  }

  while (default_labels->label != NULL) {
    InputEvents::makeLabel((InputEvents::mode)default_labels->mode,
                           default_labels->label,
                           default_labels->location, default_labels->event);
    ++default_labels;
  }
}

static bool
parse_assignment(TCHAR *buffer, const TCHAR *&key, const TCHAR *&value)
{
  TCHAR *separator = _tcschr(buffer, '=');
  if (separator == NULL || separator == buffer)
    return false;

  *separator = _T('\0');

  key = buffer;
  value = separator + 1;

  return true;
}

// Read the data files
void
InputEvents::readFile()
{
  LogStartUp(_T("Loading input events file"));

  // clear the GCE and NMEA queues
  mutexEventQueue.Lock();
  std::fill(GCE_Queue, GCE_Queue + MAX_GCE_QUEUE, -1);
  std::fill(NMEA_Queue, NMEA_Queue + MAX_NMEA_QUEUE, -1);
  mutexEventQueue.Unlock();

  // Get defaults
  if (!InitONCE) {
    if (!is_embedded()) {
      #include "InputEvents_pc.cpp"
      apply_defaults(default_modes,
                     default_events,
                     sizeof(default_events) / sizeof(default_events[0]),
                     default_key2event, default_gc2event, default_n2event,
                     default_labels);
    } else if (is_altair()) {
      #include "InputEvents_altair.cpp"
      apply_defaults(default_modes,
                     default_events,
                     sizeof(default_events) / sizeof(default_events[0]),
                     default_key2event, default_gc2event, default_n2event,
                     default_labels);
    } else {
      #include "InputEvents_default.cpp"
      apply_defaults(default_modes,
                     default_events,
                     sizeof(default_events) / sizeof(default_events[0]),
                     default_key2event, default_gc2event, default_n2event,
                     default_labels);
    }

    InitONCE = true;
  }

  // Read in user defined configuration file
  TCHAR szFile1[MAX_PATH];

  // Open file from registry
  Profile::Get(szProfileInputFile, szFile1, MAX_PATH);
  ExpandLocalPath(szFile1);

  if (string_is_empty(szFile1))
    return;

  FileLineReader reader(szFile1);
  if (reader.error())
    return;

  // TODO code - Safer sizes, strings etc - use C++ (can scanf restrict length?)

  TCHAR *new_label = NULL;

  // Init first entry

  // Did we find some in the last loop...
  bool some_data = false;
  // Multiple modes (so large string)
  TCHAR d_mode[1024] = _T("");
  TCHAR d_type[256] = _T("");
  TCHAR d_data[256] = _T("");
  unsigned event_id = 0;
  TCHAR d_label[256] = _T("");
  int d_location = 0;
  TCHAR d_event[256] = _T("");
  TCHAR d_misc[256] = _T("");

  int line = 0;

  // Read from the file
  // TODO code: What about \r - as in \r\n ?
  // TODO code: Note that ^# does not allow # in key - might be required (probably not)
  //   Better way is to separate the check for # and the scanf
  TCHAR *buffer;
  while ((buffer = reader.read()) != NULL) {
    TrimRight(buffer);
    line++;

    const TCHAR *key, *value;

    // experimental: if the first line is "#CLEAR" then the whole default config is cleared
    //               and can be overwritten by file
    if (line == 1 && _tcscmp(buffer, _T("#CLEAR")) == 0) {
      memset(&Key2Event, 0, sizeof(Key2Event));
      memset(&GC2Event, 0, sizeof(GC2Event));
      memset(&Events, 0, sizeof(Events));
      Events_count = 1;
    } else if (buffer[0] == _T('\0')) {
      // Check valid line? If not valid, assume next record (primative, but works ok!)
      // General checks before continue...
      if (some_data && (d_mode != NULL) && (_tcscmp(d_mode, _T("")) != 0)) {

        TCHAR *token;

        // For each mode
        token = _tcstok(d_mode, _T(" "));

        // General errors - these should be true
        assert(d_location >= 0);
        assert(d_location < 1024); // Scott arbitrary limit
        assert(d_mode != NULL);
        assert(d_type != NULL);
        assert(d_label != NULL);

        // These could indicate bad data - thus not an ASSERT (debug only)
        // assert(_tcslen(d_mode) < 1024);
        // assert(_tcslen(d_type) < 1024);
        // assert(_tcslen(d_label) < 1024);

        while (token != NULL) {

          // All modes are valid at this point
          mode mode_id = mode2int(token, true);
          assert(mode_id != MODE_INVALID);

          // Make label event
          // TODO code: Consider Reuse existing entries...
          if (d_location > 0) {
            // Only copy this once per object - save string space
            if (!new_label) {
              new_label = StringMallocParse(d_label);
            }
            InputEvents::makeLabel(mode_id, new_label, d_location, event_id);
          }

          // Make key (Keyboard input)
          // key - Hardware key or keyboard
          if (_tcscmp(d_type, _T("key")) == 0) {
            // Get the int key (eg: APP1 vs 'a')
            unsigned key = findKey(d_data);
            if (key > 0)
              Key2Event[mode_id][key] = event_id;

            #ifdef _INPUTDEBUG_
            else if (input_errors_count < MAX_INPUT_ERRORS)
            _stprintf(input_errors[input_errors_count++], _T("Invalid key data: %s at %i"), d_data, line);
            #endif

          // Make gce (Glide Computer Event)
          // GCE - Glide Computer Event
          } else if (_tcscmp(d_type, _T("gce")) == 0) {
            // Get the int key (eg: APP1 vs 'a')
            int key = findGCE(d_data);
            if (key >= 0)
              GC2Event[mode_id][key] = event_id;

            #ifdef _INPUTDEBUG_
            else if (input_errors_count < MAX_INPUT_ERRORS)
            _stprintf(input_errors[input_errors_count++], _T("Invalid GCE data: %s at %i"), d_data, line);
            #endif

          // Make ne (NMEA Event)
          // NE - NMEA Event
          } else if (_tcscmp(d_type, _T("ne")) == 0) {
            // Get the int key (eg: APP1 vs 'a')
            int key = findNE(d_data);
            if (key >= 0)
              N2Event[mode_id][key] = event_id;

            #ifdef _INPUTDEBUG_
            else if (input_errors_count < MAX_INPUT_ERRORS)
            _stprintf(input_errors[input_errors_count++], _T("Invalid GCE data: %s at %i"), d_data, line);
            #endif

          // label only - no key associated (label can still be touch screen)
          } else if (_tcscmp(d_type, _T("label")) == 0) {
            // Nothing to do here...

          #ifdef _INPUTDEBUG_
          } else if (input_errors_count < MAX_INPUT_ERRORS) {
            _stprintf(input_errors[input_errors_count++], _T("Invalid type: %s at %i"), d_type, line);
          #endif

          }

          token = _tcstok(NULL, _T(" "));
        }
      }

      // Clear all data.
      some_data = false;
      _tcscpy(d_mode, _T(""));
      _tcscpy(d_type, _T(""));
      _tcscpy(d_data, _T(""));
      event_id = 0;
      _tcscpy(d_label, _T(""));
      d_location = 0;
      new_label = NULL;

    } else if (string_is_empty(buffer) || buffer[0] == _T('#')) {
      // Do nothing - we probably just have a comment line
      // JG removed "void;" - causes warning (void is declaration and needs variable)
      // NOTE: Do NOT display buffer to user as it may contain an invalid stirng !

    } else if (parse_assignment(buffer, key, value)) {
      if (_tcscmp(key, _T("mode")) == 0) {
        if (_tcslen(value) < 1024) {
          some_data = true; // Success, we have a real entry
          _tcscpy(d_mode, value);
        }
      } else if (_tcscmp(key, _T("type")) == 0) {
        if (_tcslen(value) < 256)
          _tcscpy(d_type, value);
      } else if (_tcscmp(key, _T("data")) == 0) {
        if (_tcslen(value) < 256)
          _tcscpy(d_data, value);
      } else if (_tcscmp(key, _T("event")) == 0) {
        if (_tcslen(value) < 256) {
          _tcscpy(d_event, _T(""));
          _tcscpy(d_misc, _T(""));
          int ef;

          #if defined(__BORLANDC__)
          memset(d_event, 0, sizeof(d_event));
          memset(d_misc, 0, sizeof(d_event));
          if (_tcschr(value, ' ') == NULL) {
            _tcscpy(d_event, value);
          } else {
          #endif

          ef = _stscanf(value, _T("%[^ ] %[A-Za-z0-9 \\/().,]"), d_event,
              d_misc);

          #if defined(__BORLANDC__)
          }
          #endif

          // TODO code: Can't use token here - breaks
          // other token - damn C - how about
          // C++ String class ?

          // TCHAR *eventtoken;
          // eventtoken = _tcstok(value, _T(" "));
          // d_event = token;
          // eventtoken = _tcstok(value, _T(" "));

          if ((ef == 1) || (ef == 2)) {

            // TODO code: Consider reusing existing identical events

            pt2Event event = findEvent(d_event);
            if (event) {
              event_id = makeEvent(event, StringMallocParse(d_misc), event_id);

            #ifdef _INPUTDEBUG_
            } else if (input_errors_count < MAX_INPUT_ERRORS) {
              _stprintf(input_errors[input_errors_count++],
                  _T("Invalid event type: %s at %i"), d_event, line);
            #endif

            }

          #ifdef _INPUTDEBUG_
          } else if (input_errors_count < MAX_INPUT_ERRORS) {
            _stprintf(input_errors[input_errors_count++],
                _T("Invalid event type at %i"), line);
          #endif

          }
        }
      } else if (_tcscmp(key, _T("label")) == 0) {
        _tcscpy(d_label, value);
      } else if (_tcscmp(key, _T("location")) == 0) {
        d_location = _ttoi(value);

      #ifdef _INPUTDEBUG_
      } else if (input_errors_count < MAX_INPUT_ERRORS) {
        _stprintf(input_errors[input_errors_count++], _T("Invalid key/value pair %s=%s at %i"), key, value, line);
      #endif

      }
#ifdef _INPUTDEBUG_
    } else if (input_errors_count < MAX_INPUT_ERRORS) {
      _stprintf(input_errors[input_errors_count++],
                _T("Invalid line at %i"), line);
#endif
    }

  } // end while
}

#ifdef _INPUTDEBUG_
void
InputEvents::showErrors()
{
  TCHAR buffer[2048];
  int i;
  for (i = 0; i < input_errors_count; i++) {
    _stprintf(buffer, _T("%i of %i\r\n%s"), i + 1, input_errors_count, input_errors[i]);
    DoStatusMessage(_T("XCI Error"), buffer);
  }
  input_errors_count = 0;
}
#endif

unsigned
InputEvents::findKey(const TCHAR *data)
{
  if (_tcscmp(data, _T("APP1")) == 0)
    return VK_APP1;
  else if (_tcscmp(data, _T("APP2")) == 0)
    return VK_APP2;
  else if (_tcscmp(data, _T("APP3")) == 0)
    return VK_APP3;
  else if (_tcscmp(data, _T("APP4")) == 0)
    return VK_APP4;
  else if (_tcscmp(data, _T("APP5")) == 0)
    return VK_APP5;
  else if (_tcscmp(data, _T("APP6")) == 0)
    return VK_APP6;

  else if (_tcscmp(data, _T("F1")) == 0)
    return VK_F1;
  else if (_tcscmp(data, _T("F2")) == 0)
    return VK_F2;
  else if (_tcscmp(data, _T("F3")) == 0)
    return VK_F3;
  else if (_tcscmp(data, _T("F4")) == 0)
    return VK_F4;
  else if (_tcscmp(data, _T("F5")) == 0)
    return VK_F5;
  else if (_tcscmp(data, _T("F6")) == 0)
    return VK_F6;
  else if (_tcscmp(data, _T("F7")) == 0)
    return VK_F7;
  else if (_tcscmp(data, _T("F8")) == 0)
    return VK_F8;
  else if (_tcscmp(data, _T("F9")) == 0)
    return VK_F9;
  else if (_tcscmp(data, _T("F10")) == 0)
    return VK_F10;
// VENTA-TEST HANDLING EXTRA HW KEYS ON HX4700 and HP31X
// else if (_tcscmp(data, _T("F11")) == 0)
//  return VK_F11;
// else if (_tcscmp(data, _T("F12")) == 0)
//    return VK_F12;

  else if (_tcscmp(data, _T("LEFT")) == 0)
    return VK_LEFT;
  else if (_tcscmp(data, _T("RIGHT")) == 0)
    return VK_RIGHT;
  else if (_tcscmp(data, _T("UP")) == 0)
    return VK_UP;
  else if (_tcscmp(data, _T("DOWN")) == 0)
    return VK_DOWN;

  else if (_tcscmp(data, _T("RETURN")) == 0)
    return VK_RETURN;
  else if (_tcscmp(data, _T("ESCAPE")) == 0)
    return VK_ESCAPE;

  else if (_tcslen(data) == 1)
    return _totupper(data[0]);

  else
    return 0;

}

pt2Event
InputEvents::findEvent(const TCHAR *data)
{
  for (unsigned i = 0; Text2Event[i].text != NULL; ++i)
    if (_tcscmp(data, Text2Event[i].text) == 0)
      return Text2Event[i].event;

  return NULL;
}

int
InputEvents::findGCE(const TCHAR *data)
{
  int i;
  for (i = 0; i < GCE_COUNT; i++) {
    if (_tcscmp(data, Text2GCE[i]) == 0)
      return i;
  }

  return -1;
}

int
InputEvents::findNE(const TCHAR *data)
{
  int i;
  for (i = 0; i < NE_COUNT; i++) {
    if (_tcscmp(data, Text2NE[i]) == 0)
      return i;
  }

  return -1;
}

// Create EVENT Entry
// NOTE: String must already be copied (allows us to use literals
// without taking up more data - but when loading from file must copy string
unsigned
InputEvents::makeEvent(void (*event)(const TCHAR *), const TCHAR *misc,
                       unsigned next)
{
  if (Events_count >= MAX_EVENTS) {
    assert(0);
    return 0;
  }

  Events_count++;	// NOTE - Starts at 1 - 0 is a noop
  Events[Events_count].event = event;
  Events[Events_count].misc = misc;
  Events[Events_count].next = next;

  return Events_count;
}


// Make a new label (add to the end each time)
// NOTE: String must already be copied (allows us to use literals
// without taking up more data - but when loading from file must copy string
void
InputEvents::makeLabel(mode mode_id, const TCHAR* label,
                       unsigned location, unsigned event_id)
{
  assert((int)mode_id >= 0);
  assert((int)mode_id < MAX_MODE);

  menus[mode_id].Add(label, location, event_id);
}

// Return 0 for anything else - should probably return -1 !
InputEvents::mode
InputEvents::mode2int(const TCHAR *mode, bool create)
{
  // Better checks !
  if ((mode == NULL))
    return MODE_INVALID;

  for (unsigned i = 0; i < mode_map_count; i++) {
    if (_tcscmp(mode, mode_map[i]) == 0)
      return (InputEvents::mode)i;
  }

  if (create) {
    // Keep a copy
    _tcsncpy(mode_map[mode_map_count], mode, 25);
    return (InputEvents::mode)mode_map_count++;
  }

  // Should never reach this point
  assert(false);
  return MODE_INVALID;
}

void
InputEvents::setMode(mode mode)
{
  assert((unsigned)mode < mode_map_count);

  if (mode == current_mode)
    return;

  current_mode = mode;

  // TODO code: Enable this in debug modes
  // for debugging at least, set mode indicator on screen
  /*
  if (thismode == 0) {
    ButtonLabel::SetLabelText(0, NULL);
  } else {
    ButtonLabel::SetLabelText(0, mode);
  }
  */
  ButtonLabel::SetLabelText(0, NULL);

  drawButtons(current_mode);
}

void
InputEvents::setMode(const TCHAR *mode)
{
  InputEvents::mode thismode;

  assert(mode != NULL);

  // Mode must already exist to use it here...
  thismode = mode2int(mode, false);
  // Technically an error in config (eg event=Mode DoesNotExist)
  if (thismode == MODE_INVALID)
    // TODO enhancement: Add debugging here
    return;

  setMode(thismode);
}

void
InputEvents::drawButtons(mode Mode)
{
  if (!globalRunningEvent.test())
    return;

  const Menu &menu = menus[Mode];
  for (unsigned i = 0; i < menu.Count(); ++i) {
    const MenuItem &item = menu[i];

    if (item.location > 0)
      ButtonLabel::SetLabelText(item.location, item.label);
  }
}

InputEvents::mode
InputEvents::getModeID()
{
  return current_mode;
}

// -----------------------------------------------------------------------
// Processing functions - which one to do
// -----------------------------------------------------------------------

// Input is a via the user touching the label on a touch screen / mouse
bool
InputEvents::processButton(unsigned bindex)
{
  if (!globalRunningEvent.test())
    return false;

  mode lastMode = getModeID();
  const Menu &menu = menus[lastMode];
  int i = menu.FindByLocation(bindex);
  if (i < 0)
    return false;

  const MenuItem &item = menu[i];

  /* JMW illegal, should be done by gui handler loop
  // JMW need a debounce method here..
  if (!Debounce()) return true;
  */

  processGo(item.event);

  // experimental: update button text, macro may change the label
  if (lastMode == getModeID() && item.label != NULL)
    drawButtons(lastMode);

  return true;
}

unsigned
InputEvents::key_to_event(mode mode, unsigned key_code)
{
  if (key_code >= MAX_KEY)
    return 0;

  unsigned event_id = Key2Event[mode][key_code];
  if (event_id == 0)
    /* not found in this mode - try the default binding */
    event_id = Key2Event[0][key_code];

  return event_id;
}

/*
  InputEvent::processKey(KeyID);
  Process keys normally brought in by hardware or keyboard presses
  Future will also allow for long and double click presses...
  Return = We had a valid key (even if nothing happens because of Bounce)
*/
bool
InputEvents::processKey(unsigned dWord)
{
  if (!globalRunningEvent.test())
    return false;

  /* JMW illegal, should be done by gui handler loop
  InterfaceTimeoutReset();
  */

  // get current mode
  InputEvents::mode mode = InputEvents::getModeID();

  // Which key - can be defined locally or at default (fall back to default)
  unsigned event_id = key_to_event(mode, dWord);
  if (event_id == 0)
    return false;

  int bindex = -1;
  InputEvents::mode lastMode = mode;
  const TCHAR *pLabelText = NULL;

  // JMW should be done by gui handler
  // if (!Debounce()) return true;

  const Menu &menu = menus[mode];
  int i = menu.FindByEvent(event_id);
  if (i >= 0 && menu[i].location > 0) {
    bindex = menu[i].location;
    pLabelText = menu[i].label;
  }

  if (bindex < 0 || ButtonLabel::IsEnabled(bindex))
    InputEvents::processGo(event_id);

  // experimental: update button text, macro may change the value
  if (lastMode == getModeID() && bindex > 0 && pLabelText != NULL)
    drawButtons(lastMode);

  return true;
}

bool
InputEvents::processNmea(unsigned ne_id)
{
  // add an event to the bottom of the queue
  mutexEventQueue.Lock();
  for (int i = 0; i < MAX_NMEA_QUEUE; i++) {
    if (NMEA_Queue[i] == -1) {
      NMEA_Queue[i] = ne_id;
      break;
    }
  }
  mutexEventQueue.Unlock();
  return true; // ok.
}

/*
  InputEvent::processNmea(TCHAR* data)
  Take hard coded inputs from NMEA processor.
  Return = TRUE if we have a valid key match
*/
bool
InputEvents::processNmea_real(unsigned ne_id)
{
  if (!globalRunningEvent.test())
    return false;

  int event_id = 0;

  // JMW not required
  //  InterfaceTimeoutReset();

  // Valid input ?
  if (ne_id >= NE_COUNT)
    return false;

  // get current mode
  InputEvents::mode mode = InputEvents::getModeID();

  // Which key - can be defined locally or at default (fall back to default)
  event_id = N2Event[mode][ne_id];
  if (event_id == 0) {
    // go with default key..
    event_id = N2Event[0][ne_id];
  }

  if (event_id > 0) {
    InputEvents::processGo(event_id);
    return true;
  }

  return false;
}


// This should be called ONLY by the GUI thread.
void
InputEvents::DoQueuedEvents(void)
{
  int GCE_Queue_copy[MAX_GCE_QUEUE];
  int NMEA_Queue_copy[MAX_NMEA_QUEUE];
  int i;

  // copy the queue first, blocking
  mutexEventQueue.Lock();
  std::copy(GCE_Queue, GCE_Queue + MAX_GCE_QUEUE, GCE_Queue_copy);
  std::fill(GCE_Queue, GCE_Queue + MAX_GCE_QUEUE, -1);
  std::copy(NMEA_Queue, NMEA_Queue + MAX_NMEA_QUEUE, NMEA_Queue_copy);
  std::fill(NMEA_Queue, NMEA_Queue + MAX_NMEA_QUEUE, -1);
  mutexEventQueue.Unlock();

  // process each item in the queue
  for (i = 0; i < MAX_GCE_QUEUE; i++) {
    if (GCE_Queue_copy[i] != -1) {
      processGlideComputer_real(GCE_Queue_copy[i]);
    }
  }
  for (i = 0; i < MAX_NMEA_QUEUE; i++) {
    if (NMEA_Queue_copy[i] != -1) {
      processNmea_real(NMEA_Queue_copy[i]);
    }
  }
}

bool
InputEvents::processGlideComputer(unsigned gce_id)
{
  // add an event to the bottom of the queue
  mutexEventQueue.Lock();
  for (int i = 0; i < MAX_GCE_QUEUE; i++) {
    if (GCE_Queue[i] == -1) {
      GCE_Queue[i] = gce_id;
      break;
    }
  }
  mutexEventQueue.Unlock();
  return true;
}

/*
  InputEvents::processGlideComputer
  Take virtual inputs from a Glide Computer to do special events
*/
bool
InputEvents::processGlideComputer_real(unsigned gce_id)
{
  if (!globalRunningEvent.test())
    return false;
  int event_id = 0;

  // TODO feature: Log glide computer events to IGC file

  // Valid input ?
  if (gce_id >= GCE_COUNT)
    return false;

  // get current mode
  InputEvents::mode mode = InputEvents::getModeID();

  // Which key - can be defined locally or at default (fall back to default)
  event_id = GC2Event[mode][gce_id];
  if (event_id == 0) {
    // go with default key..
    event_id = GC2Event[0][gce_id];
  }

  if (event_id > 0) {
    InputEvents::processGo(event_id);
    return true;
  }

  return false;
}

// EXECUTE an Event - lookup event handler and call back - no return
void
InputEvents::processGo(unsigned eventid)
{
  if (!globalRunningEvent.test())
    return;

  // TODO feature: event/macro recorder
  /*
  if (LoggerActive) {
    LoggerNoteEvent(Events[eventid].);
  }
  */

  // evnentid 0 is special for "noop" - otherwise check event
  // exists (pointer to function)
  if (eventid) {
    if (Events[eventid].event) {
      Events[eventid].event(Events[eventid].misc);
      MenuTimeOut = 0;
    }
    if (Events[eventid].next > 0)
      InputEvents::processGo(Events[eventid].next);
  }
}

unsigned InputEvents::MenuTimeOut = 0;

void
InputEvents::HideMenu()
{
  MenuTimeOut = MenuTimeoutMax;
  ProcessMenuTimer();
  ResetDisplayTimeOut();
}

void
InputEvents::ResetMenuTimeOut()
{
  ResetDisplayTimeOut();
  MenuTimeOut = 0;
}

void
InputEvents::ShowMenu()
{
  #if !defined(GNAV) && !defined(PCGNAV)
  // Popup exit button if in .xci
  // setMode(_T("Exit"));
  setMode(MODE_MENU); // VENTA3
  #endif

  ResetDisplayTimeOut();
  MenuTimeOut = 0;
  ProcessMenuTimer();
}

void
InputEvents::ProcessMenuTimer()
{
  if (!InfoBoxManager::HasFocus()) {
    if (MenuTimeOut == MenuTimeoutMax) {
      if (SettingsMap().EnablePan && !SettingsMap().TargetPan) {
        setMode(MODE_PAN);
      } else {
        setMode(MODE_DEFAULT);
      }
    }

    MenuTimeOut++;
  }
}

void
InputEvents::ProcessTimer()
{
  if (globalRunningEvent.test()) {
    DoQueuedEvents();
  }
  ProcessMenuTimer();
}
