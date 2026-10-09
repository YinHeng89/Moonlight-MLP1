/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "sdl.h"
#include "../sdl.h"

#include <Limelight.h>

#include <stdio.h>    /* fprintf for the quit-combination report */
#include <string.h>   /* strtok, memset */

#define ACTION_MODIFIERS (MODIFIER_SHIFT|MODIFIER_ALT|MODIFIER_CTRL)
#define QUIT_KEY SDLK_q
#define QUIT_BUTTONS (PLAY_FLAG|BACK_FLAG|LB_FLAG|RB_FLAG)
#define FULLSCREEN_KEY SDLK_f
#define UNGRAB_KEY SDLK_z

/* Neither way out of a stream is reachable on a handheld.

   The keyboard one wants Ctrl, Alt, Shift and Q together. A device with a
   d-pad, four face buttons and two shoulders has no modifiers to hold.

   The gamepad one wants Start, Select, LB and RB at once, which is a lot to
   hold one-handed, and if SDL does not recognise the device as a game
   controller at all there are no controller events in the first place.

   Left as it is, the only exit from a stream is the power button. So the
   combination is configurable: ML_QUIT_COMBO is up to four names separated by
   commas, and holding them together ends the stream.

   A name is looked up both as an SDL key and as a gamepad button, because the
   two overlap exactly where it matters -- "a", "b", "x" and "y" are each a
   letter key and a face button, and which one arrives depends on how the
   device was recognised. One name answers to either; they never arrive as the
   same event. On the MLP1's own mapping, "back,start" is the two centre
   buttons and "x,b" two of the face buttons. Which names exist is a question
   about the device, not about this code, which is what the key probe in the
   pak exists to answer.
*/
#define MAX_QUIT_COMBO 4

typedef struct {
  SDL_Keycode key;  /* SDLK_UNKNOWN when this name is not a key */
  int pad;          /* -1 when this name is not a gamepad button */
} QUIT_COMBO_PART;

static QUIT_COMBO_PART quit_combo[MAX_QUIT_COMBO];
static int quit_combo_count = 0;
static bool quit_combo_held[MAX_QUIT_COMBO];

static void quit_combo_init(void) {
  const char* spec = getenv("ML_QUIT_COMBO");
  if (spec == NULL || spec[0] == '\0')
    return;

  char buf[256];
  snprintf(buf, sizeof(buf), "%s", spec);
  for (char* tok = strtok(buf, ","); tok != NULL && quit_combo_count < MAX_QUIT_COMBO;
       tok = strtok(NULL, ",")) {
    QUIT_COMBO_PART part;
    part.key = SDL_GetKeyFromName(tok);
    part.pad = -1;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(tok);
    if (button != SDL_CONTROLLER_BUTTON_INVALID)
      part.pad = (int) button;
#endif

    if (part.key == SDLK_UNKNOWN && part.pad < 0) {
      fprintf(stderr, "ML_QUIT_COMBO: '%s' is neither an SDL key nor a gamepad button, ignoring\n", tok);
      continue;
    }
    quit_combo[quit_combo_count++] = part;
  }
  if (quit_combo_count > 0)
    fprintf(stderr, "Quit: hold %s together to end the stream\n", spec);
}

/* True when this event completes the hold. Releasing any part of the
   combination disarms all of it, so it can never be left half-pressed --
   a quit has to be one fresh hold of the whole combination. */
static bool quit_combo_check(SDL_Event* event) {
  int matched[MAX_QUIT_COMBO];
  int found = 0;

  if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
    for (int i = 0; i < quit_combo_count; ++i)
      if (quit_combo[i].key != SDLK_UNKNOWN && quit_combo[i].key == event->key.keysym.sym)
        matched[found++] = i;
  } else if (event->type == SDL_CONTROLLERBUTTONDOWN || event->type == SDL_CONTROLLERBUTTONUP) {
    for (int i = 0; i < quit_combo_count; ++i)
      if (quit_combo[i].pad >= 0 && quit_combo[i].pad == (int) event->cbutton.button)
        matched[found++] = i;
  }

  if (found == 0)
    return false;

  if (event->type == SDL_KEYUP || event->type == SDL_CONTROLLERBUTTONUP) {
    memset(quit_combo_held, 0, sizeof(quit_combo_held));
    return false;
  }

  for (int n = 0; n < found; ++n)
    quit_combo_held[matched[n]] = true;
  for (int i = 0; i < quit_combo_count; ++i)
    if (!quit_combo_held[i])
      return false;
  return true;
}

/* The MLP1's four face buttons sit the way a Nintendo pad's do -- A on the
   right, B at the bottom, X on top, Y on the left -- and its SDL mapping keeps
   the letter that is printed on the button: pressing the button marked A gives
   the host an A. Games draw their prompts for an Xbox pad, where confirm is
   the bottom button and cancel the right one. So the confirmation is on the
   right by the labels and at the bottom by the pictures, and the two disagree.

   Which of them should win turns out to be personal, and to differ from one
   game to the next -- some read the labels, some read the positions, some let
   you choose inside the game. So it is a setting, and not a coin flip:
   ML_PAD_LAYOUT names the two arrangements, and ML_PAD_MAP points each printed
   button at whichever button the host should hear, one at a time.

   It is applied after the quit combination has been checked, so the names in
   ML_QUIT_COMBO stay the ones the key probe reported, whichever is chosen. */
static const char* PAD_FACE_NAMES[4] = { "A", "B", "X", "Y" };

/* Indexed by the button as it is printed on the device -- which is what
   pad_face_wiring has just made of what SDL reported -- giving the button the
   host is told about. Identity until told. */
static SDL_GameControllerButton pad_face_target[4] = {
  SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
  SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y
};

static int pad_face_index(const char* name) {
  for (int i = 0; i < 4; ++i)
    if (SDL_strcasecmp(name, PAD_FACE_NAMES[i]) == 0)
      return i;
  return -1;
}

static void pad_layout_init(void) {
  const char* spec = getenv("ML_PAD_MAP");
  if (spec != NULL && spec[0] != '\0') {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char* tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
      char* eq = strchr(tok, '=');
      if (eq == NULL) {
        fprintf(stderr, "ML_PAD_MAP: '%s' is not name=name, ignoring\n", tok);
        continue;
      }
      *eq = '\0';
      int from = pad_face_index(tok);
      int to = pad_face_index(eq + 1);
      if (from < 0 || to < 0) {
        fprintf(stderr, "ML_PAD_MAP: '%s' names something other than a, b, x or y, ignoring\n", tok);
        continue;
      }
      pad_face_target[from] = (SDL_GameControllerButton) to;
    }
    fprintf(stderr, "Pad map: printed A->%s B->%s X->%s Y->%s\n",
            PAD_FACE_NAMES[pad_face_target[0]], PAD_FACE_NAMES[pad_face_target[1]],
            PAD_FACE_NAMES[pad_face_target[2]], PAD_FACE_NAMES[pad_face_target[3]]);
    return;
  }

  const char* layout = getenv("ML_PAD_LAYOUT");
  if (layout == NULL || layout[0] == '\0')
    return;

  if (SDL_strcasecmp(layout, "xbox") == 0 || SDL_strcasecmp(layout, "swap") == 0) {
    pad_face_target[0] = SDL_CONTROLLER_BUTTON_B;
    pad_face_target[1] = SDL_CONTROLLER_BUTTON_A;
    pad_face_target[2] = SDL_CONTROLLER_BUTTON_Y;
    pad_face_target[3] = SDL_CONTROLLER_BUTTON_X;
    fprintf(stderr, "Pad layout: xbox positions (A<->B, X<->Y)\n");
  } else if (SDL_strcasecmp(layout, "nintendo") == 0 || SDL_strcasecmp(layout, "labels") == 0) {
    fprintf(stderr, "Pad layout: as printed on the buttons\n");
  } else {
    fprintf(stderr, "ML_PAD_LAYOUT: '%s' is neither 'nintendo' nor 'xbox', using the printed layout\n", layout);
  }
}

/* Whether what SDL reports is the button printed on the device.

   Everything above is written for what is printed on the keys -- that is what
   ML_PAD_LAYOUT names and what the settings screen draws -- and the two are
   not always the same. The MLP1's pad is not in SDL's database, so SDL numbers
   its buttons the way Linux does, which is by position: 0 is the bottom button,
   1 the one on the right. The device is laid out the way a Nintendo pad is,
   with A printed on the right and B at the bottom, so what SDL calls A is the
   button printed B. Reconciled here, once, rather than in each of the places
   that would otherwise have to know about it.

   X and Y need no swap: Linux numbers north before west, and X is printed on
   top and Y on the left.

   ML_PAD_WIRING says which it is outright, for the day a device turns out to
   be wired the other way round. */
static int pad_face_wiring[4] = { 0, 1, 2, 3 };   /* what SDL says -> printed */

static int pad_name_has(const char* hay, const char* needle) {
  if (hay == NULL || needle == NULL || needle[0] == '\0')
    return 0;
  size_t n = SDL_strlen(needle);
  for (const char* p = hay; *p != '\0'; ++p)
    if (SDL_strncasecmp(p, needle, n) == 0)
      return 1;
  return 0;
}

static void pad_wiring_init(void) {
  const char* wiring = getenv("ML_PAD_WIRING");
  if (wiring != NULL && wiring[0] != '\0') {
    int by_pos = (SDL_strcasecmp(wiring, "labels") != 0 &&
                  SDL_strcasecmp(wiring, "printed") != 0);
    pad_face_wiring[0] = by_pos ? 1 : 0;
    pad_face_wiring[1] = by_pos ? 0 : 1;
    fprintf(stderr, "Pad wiring: %s\n", by_pos ? "by position" : "as printed");
    return;
  }
  /* Not in the database means SDL fell back to numbering by position. */
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    const char* name = SDL_GameControllerNameForIndex(i);
    if (name == NULL)
      name = SDL_JoystickNameForIndex(i);
    if (name != NULL && pad_name_has(name, "Loong")) {
      pad_face_wiring[0] = 1;
      pad_face_wiring[1] = 0;
      fprintf(stderr, "Pad wiring: %s is not in SDL's database, so its buttons are numbered by position; A and B swapped to match the labels\n", name);
      return;
    }
  }
}

static SDL_GameControllerButton pad_face_button(SDL_GameControllerButton button) {
  if ((int) button < 0 || (int) button > 3)
    return button;
  return pad_face_target[pad_face_wiring[(int) button]];
}

static const int SDL_TO_LI_BUTTON_MAP[] = {
  A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
  BACK_FLAG, SPECIAL_FLAG, PLAY_FLAG,
  LS_CLK_FLAG, RS_CLK_FLAG,
  LB_FLAG, RB_FLAG,
  UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
  MISC_FLAG,
  PADDLE1_FLAG, PADDLE2_FLAG, PADDLE3_FLAG, PADDLE4_FLAG,
  TOUCHPAD_FLAG,
};

typedef struct _GAMEPAD_STATE {
  unsigned char leftTrigger, rightTrigger;
  short leftStickX, leftStickY;
  short rightStickX, rightStickY;
  int buttons;
  SDL_JoystickID sdl_id;
  SDL_GameController* controller;
#if !SDL_VERSION_ATLEAST(2, 0, 9)
  SDL_Haptic* haptic;
  int haptic_effect_id;
#endif
  short id;
  bool initialized;
} GAMEPAD_STATE, *PGAMEPAD_STATE;

// Limited by number of bits in activeGamepadMask
#define MAX_GAMEPADS 16

static GAMEPAD_STATE gamepads[MAX_GAMEPADS];

static int activeGamepadMask = 0;

int sdl_gamepads = 0;

#define VK_0 0x30
#define VK_A 0x41

// These are real Windows VK_* codes
#ifndef VK_F1
#define VK_F1 0x70
#define VK_F13 0x7C
#define VK_NUMPAD0 0x60
#endif

int vk_for_sdl_scancode(SDL_Scancode scancode) {
  // Set keycode. We explicitly use scancode here because GFE will try to correct
  // for AZERTY layouts on the host but it depends on receiving VK_ values matching
  // a QWERTY layout to work.
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    // SDL defines SDL_SCANCODE_0 > SDL_SCANCODE_9, so we need to handle that manually
    return (scancode - SDL_SCANCODE_1) + VK_0 + 1;
  }
  else if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return (scancode - SDL_SCANCODE_A) + VK_A;
  }
  else if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
    return (scancode - SDL_SCANCODE_F1) + VK_F1;
  }
  else if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) {
    return (scancode - SDL_SCANCODE_F13) + VK_F13;
  }
  else if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
    // SDL defines SDL_SCANCODE_KP_0 > SDL_SCANCODE_KP_9, so we need to handle that manually
    return (scancode - SDL_SCANCODE_KP_1) + VK_NUMPAD0 + 1;
  }
  else {
    switch (scancode) {
    case SDL_SCANCODE_BACKSPACE:
        return 0x08;

    case SDL_SCANCODE_TAB:
        return 0x09;

    case SDL_SCANCODE_CLEAR:
        return 0x0C;

    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_RETURN:
        return 0x0D;

    case SDL_SCANCODE_PAUSE:
        return 0x13;

    case SDL_SCANCODE_CAPSLOCK:
        return 0x14;

    case SDL_SCANCODE_ESCAPE:
        return 0x1B;

    case SDL_SCANCODE_SPACE:
        return 0x20;

    case SDL_SCANCODE_PAGEUP:
        return 0x21;

    case SDL_SCANCODE_PAGEDOWN:
        return 0x22;

    case SDL_SCANCODE_END:
        return 0x23;

    case SDL_SCANCODE_HOME:
        return 0x24;

    case SDL_SCANCODE_LEFT:
        return 0x25;

    case SDL_SCANCODE_UP:
        return 0x26;

    case SDL_SCANCODE_RIGHT:
        return 0x27;

    case SDL_SCANCODE_DOWN:
        return 0x28;

    case SDL_SCANCODE_SELECT:
        return 0x29;

    case SDL_SCANCODE_EXECUTE:
        return 0x2B;

    case SDL_SCANCODE_PRINTSCREEN:
        return 0x2C;

    case SDL_SCANCODE_INSERT:
        return 0x2D;

    case SDL_SCANCODE_DELETE:
        return 0x2E;

    case SDL_SCANCODE_HELP:
        return 0x2F;

    case SDL_SCANCODE_KP_0:
        // See comment above about why we only handle SDL_SCANCODE_KP_0 here
        return VK_NUMPAD0;

    case SDL_SCANCODE_0:
        // See comment above about why we only handle SDL_SCANCODE_0 here
        return VK_0;

    case SDL_SCANCODE_KP_MULTIPLY:
        return 0x6A;

    case SDL_SCANCODE_KP_PLUS:
        return 0x6B;

    case SDL_SCANCODE_KP_COMMA:
        return 0x6C;

    case SDL_SCANCODE_KP_MINUS:
        return 0x6D;

    case SDL_SCANCODE_KP_PERIOD:
        return 0x6E;

    case SDL_SCANCODE_KP_DIVIDE:
        return 0x6F;

    case SDL_SCANCODE_NUMLOCKCLEAR:
        return 0x90;

    case SDL_SCANCODE_SCROLLLOCK:
        return 0x91;

    case SDL_SCANCODE_LSHIFT:
        return 0xA0;

    case SDL_SCANCODE_RSHIFT:
        return 0xA1;

    case SDL_SCANCODE_LCTRL:
        return 0xA2;

    case SDL_SCANCODE_RCTRL:
        return 0xA3;

    case SDL_SCANCODE_LALT:
        return 0xA4;

    case SDL_SCANCODE_RALT:
        return 0xA5;

    case SDL_SCANCODE_LGUI:
        return 0x5B;

    case SDL_SCANCODE_RGUI:
        return 0x5C;

    case SDL_SCANCODE_APPLICATION:
        return 0x5D;

    case SDL_SCANCODE_AC_BACK:
        return 0xA6;

    case SDL_SCANCODE_AC_FORWARD:
        return 0xA7;

    case SDL_SCANCODE_AC_REFRESH:
        return 0xA8;

    case SDL_SCANCODE_AC_STOP:
        return 0xA9;

    case SDL_SCANCODE_AC_SEARCH:
        return 0xAA;

    case SDL_SCANCODE_AC_BOOKMARKS:
        return 0xAB;

    case SDL_SCANCODE_AC_HOME:
        return 0xAC;

    case SDL_SCANCODE_SEMICOLON:
        return 0xBA;

    case SDL_SCANCODE_EQUALS:
        return 0xBB;

    case SDL_SCANCODE_COMMA:
        return 0xBC;

    case SDL_SCANCODE_MINUS:
        return 0xBD;

    case SDL_SCANCODE_PERIOD:
        return 0xBE;

    case SDL_SCANCODE_SLASH:
        return 0xBF;

    case SDL_SCANCODE_GRAVE:
        return 0xC0;

    case SDL_SCANCODE_LEFTBRACKET:
        return 0xDB;

    case SDL_SCANCODE_BACKSLASH:
        return 0xDC;

    case SDL_SCANCODE_RIGHTBRACKET:
        return 0xDD;

    case SDL_SCANCODE_APOSTROPHE:
        return 0xDE;

    case SDL_SCANCODE_NONUSBACKSLASH:
        return 0xE2;

    default:
        return 0;
    }
  }
}

static void send_controller_arrival(PGAMEPAD_STATE state) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
  unsigned int supportedButtonFlags = 0;
  unsigned short capabilities = 0;
  unsigned char type = LI_CTYPE_UNKNOWN;

  for (int i = 0; i < SDL_arraysize(SDL_TO_LI_BUTTON_MAP); i++) {
    if (SDL_GameControllerHasButton(state->controller, (SDL_GameControllerButton)i)) {
        supportedButtonFlags |= SDL_TO_LI_BUTTON_MAP[i];
    }
  }

  if (SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
      SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS)
    capabilities |= LI_CCAP_ANALOG_TRIGGERS;
  if (SDL_GameControllerHasRumble(state->controller))
    capabilities |= LI_CCAP_RUMBLE;
  if (SDL_GameControllerHasRumbleTriggers(state->controller))
    capabilities |= LI_CCAP_TRIGGER_RUMBLE;
  if (SDL_GameControllerGetNumTouchpads(state->controller) > 0)
    capabilities |= LI_CCAP_TOUCHPAD;
  if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL))
    capabilities |= LI_CCAP_ACCEL;
  if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO))
    capabilities |= LI_CCAP_GYRO;
  if (SDL_GameControllerHasLED(state->controller))
    capabilities |= LI_CCAP_RGB_LED;

  switch (SDL_GameControllerGetType(state->controller)) {
  case SDL_CONTROLLER_TYPE_XBOX360:
  case SDL_CONTROLLER_TYPE_XBOXONE:
    type = LI_CTYPE_XBOX;
    break;
  case SDL_CONTROLLER_TYPE_PS3:
  case SDL_CONTROLLER_TYPE_PS4:
  case SDL_CONTROLLER_TYPE_PS5:
    type = LI_CTYPE_PS;
    break;
  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
  case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
    type = LI_CTYPE_NINTENDO;
    break;
  }

  LiSendControllerArrivalEvent(state->id, activeGamepadMask, type, supportedButtonFlags, capabilities);
#endif
}

static PGAMEPAD_STATE get_gamepad(SDL_JoystickID sdl_id, bool add) {
  // See if a gamepad already exists
  for (int i = 0;i<MAX_GAMEPADS;i++) {
    if (gamepads[i].initialized && gamepads[i].sdl_id == sdl_id)
      return &gamepads[i];
  }

  if (!add)
    return NULL;

  for (int i = 0;i<MAX_GAMEPADS;i++) {
    if (!gamepads[i].initialized) {
      gamepads[i].sdl_id = sdl_id;
      gamepads[i].id = i;
      gamepads[i].initialized = true;

      activeGamepadMask |= (1 << i);

      return &gamepads[i];
    }
  }

  return &gamepads[0];
}

static void add_gamepad(int joystick_index) {
  SDL_GameController* controller = SDL_GameControllerOpen(joystick_index);
  if (!controller) {
    fprintf(stderr, "Could not open gamecontroller %i: %s\n", joystick_index, SDL_GetError());
    return;
  }

  SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
  SDL_JoystickID joystick_id = SDL_JoystickInstanceID(joystick);

  // Check if we have already set up a state for this gamepad
  PGAMEPAD_STATE state = get_gamepad(joystick_id, false);
  if (state) {
    // This was probably a gamepad added during initialization, so we've already
    // got state set up. However, we still need to inform the host about it, since
    // we couldn't do that during initialization (since we weren't connected yet).
    send_controller_arrival(state);

    SDL_GameControllerClose(controller);
    return;
  }

  // Create a new gamepad state
  state = get_gamepad(joystick_id, true);
  state->controller = controller;

#if !SDL_VERSION_ATLEAST(2, 0, 9)
  state->haptic = SDL_HapticOpenFromJoystick(joystick);
  if (haptic && (SDL_HapticQuery(state->haptic) & SDL_HAPTIC_LEFTRIGHT) == 0) {
    SDL_HapticClose(state->haptic);
    state->haptic = NULL;
  }
  state->haptic_effect_id = -1;
#endif

  // Send the controller arrival event to the host
  send_controller_arrival(state);

  sdl_gamepads++;
}

static void remove_gamepad(SDL_JoystickID sdl_id) {
  for (int i = 0;i<MAX_GAMEPADS;i++) {
    if (gamepads[i].initialized && gamepads[i].sdl_id == sdl_id) {
#if !SDL_VERSION_ATLEAST(2, 0, 9)
      if (gamepads[i].haptic_effect_id >= 0) {
        SDL_HapticDestroyEffect(gamepads[i].haptic, gamepads[i].haptic_effect_id);
      }

      if (gamepads[i].haptic) {
        SDL_HapticClose(gamepads[i].haptic);
      }
#endif

      SDL_GameControllerClose(gamepads[i].controller);

      // This will cause disconnection of the virtual controller on the host PC
      activeGamepadMask &= ~(1 << i);
      LiSendMultiControllerEvent(i, activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);

      memset(&gamepads[i], 0, sizeof(*gamepads));
      sdl_gamepads--;
      break;
    }
  }
}

void sdlinput_init(char* mappings) {
  memset(gamepads, 0, sizeof(gamepads));

  quit_combo_init();
  pad_layout_init();

  SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
#if !SDL_VERSION_ATLEAST(2, 0, 9)
  SDL_InitSubSystem(SDL_INIT_HAPTIC);
#endif
  SDL_GameControllerAddMappingsFromFile(mappings);
  pad_wiring_init();

  // Add game controllers here to ensure an accurate count
  // goes to the host when starting a new session.
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i))
      add_gamepad(i);
  }
}

int sdlinput_handle_event(SDL_Window* window, SDL_Event* event) {
  int button = 0;
  unsigned char touchEventType;
  PGAMEPAD_STATE gamepad;
  switch (event->type) {
  case SDL_MOUSEMOTION:
    if (SDL_GetRelativeMouseMode())
      LiSendMouseMoveEvent(event->motion.xrel, event->motion.yrel);
    else {
      int w, h;
      SDL_GetWindowSize(window, &w, &h);
      LiSendMousePositionEvent(event->motion.x, event->motion.y, w, h);
    }
    break;
  case SDL_MOUSEWHEEL:
#if SDL_VERSION_ATLEAST(2, 0, 18)
    LiSendHighResHScrollEvent((short)(event->wheel.preciseX * 120)); // WHEEL_DELTA
    LiSendHighResScrollEvent((short)(event->wheel.preciseY * 120)); // WHEEL_DELTA
#else
    LiSendHScrollEvent(event->wheel.x);
    LiSendScrollEvent(event->wheel.y);
#endif
    break;
  case SDL_MOUSEBUTTONUP:
  case SDL_MOUSEBUTTONDOWN:
    switch (event->button.button) {
    case SDL_BUTTON_LEFT:
      button = BUTTON_LEFT;
      break;
    case SDL_BUTTON_MIDDLE:
      button = BUTTON_MIDDLE;
      break;
    case SDL_BUTTON_RIGHT:
      button = BUTTON_RIGHT;
      break;
    case SDL_BUTTON_X1:
      button = BUTTON_X1;
      break;
    case SDL_BUTTON_X2:
      button = BUTTON_X2;
      break;
    }

    if (button != 0)
      LiSendMouseButtonEvent(event->type==SDL_MOUSEBUTTONDOWN?BUTTON_ACTION_PRESS:BUTTON_ACTION_RELEASE, button);

    return 0;
  case SDL_KEYDOWN:
  case SDL_KEYUP:
    button = vk_for_sdl_scancode(event->key.keysym.scancode);

    int modifiers = 0;
    if (event->key.keysym.mod & KMOD_CTRL) {
      modifiers |= MODIFIER_CTRL;
    }
    if (event->key.keysym.mod & KMOD_ALT) {
      modifiers |= MODIFIER_ALT;
    }
    if (event->key.keysym.mod & KMOD_SHIFT) {
      modifiers |= MODIFIER_SHIFT;
    }
    if (event->key.keysym.mod & KMOD_GUI) {
      modifiers |= MODIFIER_META;
    }

    LiSendKeyboardEvent(0x80 << 8 | button, event->type==SDL_KEYDOWN?KEY_ACTION_DOWN:KEY_ACTION_UP, modifiers);

    // Checked before the modifier combination below, which cannot fire on a
    // device with no modifier keys. Both stay available.
    if (quit_combo_count > 0 && quit_combo_check(event))
      return SDL_QUIT_APPLICATION;

    // Quit the stream if all the required quit keys are down
    if ((modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && event->key.keysym.sym == QUIT_KEY && event->type==SDL_KEYUP)
      return SDL_QUIT_APPLICATION;
    else if ((modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && event->key.keysym.sym == FULLSCREEN_KEY && event->type==SDL_KEYUP)
      return SDL_TOGGLE_FULLSCREEN;
    else if ((modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && event->key.keysym.sym == UNGRAB_KEY && event->type==SDL_KEYUP)
      return SDL_GetRelativeMouseMode() ? SDL_MOUSE_UNGRAB : SDL_MOUSE_GRAB;
    break;
  case SDL_FINGERDOWN:
  case SDL_FINGERMOTION:
  case SDL_FINGERUP:
    switch (event->type) {
    case SDL_FINGERDOWN:
        touchEventType = LI_TOUCH_EVENT_DOWN;
        break;
    case SDL_FINGERMOTION:
        touchEventType = LI_TOUCH_EVENT_MOVE;
        break;
    case SDL_FINGERUP:
        touchEventType = LI_TOUCH_EVENT_UP;
        break;
    default:
        return SDL_NOTHING;
    }

    // These are already window-relative normalized coordinates, so we just need to clamp them
    event->tfinger.x = SDL_max(SDL_min(1.0f, event->tfinger.x), 0.0f);
    event->tfinger.y = SDL_max(SDL_min(1.0f, event->tfinger.y), 0.0f);

    LiSendTouchEvent(touchEventType, event->tfinger.fingerId, event->tfinger.x, event->tfinger.y,
                     event->tfinger.pressure, 0.0f, 0.0f, LI_ROT_UNKNOWN);
    break;
  case SDL_CONTROLLERAXISMOTION:
    gamepad = get_gamepad(event->caxis.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    switch (event->caxis.axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
      gamepad->leftStickX = event->caxis.value;
      break;
    case SDL_CONTROLLER_AXIS_LEFTY:
      gamepad->leftStickY = -SDL_max(event->caxis.value, (short)-32767);
      break;
    case SDL_CONTROLLER_AXIS_RIGHTX:
      gamepad->rightStickX = event->caxis.value;
      break;
    case SDL_CONTROLLER_AXIS_RIGHTY:
      gamepad->rightStickY = -SDL_max(event->caxis.value, (short)-32767);
      break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
      gamepad->leftTrigger = (unsigned char)(event->caxis.value * 255UL / 32767);
      break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
      gamepad->rightTrigger = (unsigned char)(event->caxis.value * 255UL / 32767);
      break;
    default:
      return SDL_NOTHING;
    }
    LiSendMultiControllerEvent(gamepad->id, activeGamepadMask, gamepad->buttons, gamepad->leftTrigger, gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY);
    break;
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP:
    // Checked ahead of everything else in this branch: the d-pad sits past the
    // end of the button map below, and so do any other buttons moonlight has
    // no mapping for, and every one of them still has to be able to form the
    // quit combination.
    if (quit_combo_count > 0 && quit_combo_check(event))
      return SDL_QUIT_APPLICATION;

    SDL_GameControllerButton pad_button = pad_face_button((SDL_GameControllerButton) event->cbutton.button);

    gamepad = get_gamepad(event->cbutton.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    if ((int) pad_button >= SDL_arraysize(SDL_TO_LI_BUTTON_MAP))
      return SDL_NOTHING;

    if (event->type == SDL_CONTROLLERBUTTONDOWN)
      gamepad->buttons |= SDL_TO_LI_BUTTON_MAP[pad_button];
    else
      gamepad->buttons &= ~SDL_TO_LI_BUTTON_MAP[pad_button];

    if ((gamepad->buttons & QUIT_BUTTONS) == QUIT_BUTTONS)
      return SDL_QUIT_APPLICATION;

    LiSendMultiControllerEvent(gamepad->id, activeGamepadMask, gamepad->buttons, gamepad->leftTrigger, gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY);
    break;
  case SDL_CONTROLLERDEVICEADDED:
    add_gamepad(event->cdevice.which);
    break;
  case SDL_CONTROLLERDEVICEREMOVED:
    remove_gamepad(event->cdevice.which);
    break;
#if SDL_VERSION_ATLEAST(2, 0, 14)
  case SDL_CONTROLLERSENSORUPDATE:
    gamepad = get_gamepad(event->csensor.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    switch (event->csensor.sensor) {
    case SDL_SENSOR_ACCEL:
      LiSendControllerMotionEvent(gamepad->id, LI_MOTION_TYPE_ACCEL, event->csensor.data[0], event->csensor.data[1], event->csensor.data[2]);
      break;
    case SDL_SENSOR_GYRO:
      // Convert rad/s to deg/s
      LiSendControllerMotionEvent(gamepad->id, LI_MOTION_TYPE_GYRO,
                                  event->csensor.data[0] * 57.2957795f,
                                  event->csensor.data[1] * 57.2957795f,
                                  event->csensor.data[2] * 57.2957795f);
      break;
    }
    break;
  case SDL_CONTROLLERTOUCHPADDOWN:
  case SDL_CONTROLLERTOUCHPADUP:
  case SDL_CONTROLLERTOUCHPADMOTION:
    gamepad = get_gamepad(event->ctouchpad.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    switch (event->type) {
    case SDL_CONTROLLERTOUCHPADDOWN:
      touchEventType = LI_TOUCH_EVENT_DOWN;
      break;
    case SDL_CONTROLLERTOUCHPADUP:
      touchEventType = LI_TOUCH_EVENT_UP;
      break;
    case SDL_CONTROLLERTOUCHPADMOTION:
      touchEventType = LI_TOUCH_EVENT_MOVE;
      break;
    default:
      return SDL_NOTHING;
    }
    LiSendControllerTouchEvent(gamepad->id, touchEventType, event->ctouchpad.finger,
                               event->ctouchpad.x, event->ctouchpad.y, event->ctouchpad.pressure);
    break;
#endif
  }

  return SDL_NOTHING;
}

void sdlinput_rumble(unsigned short controller_id, unsigned short low_freq_motor, unsigned short high_freq_motor) {
  if (controller_id >= MAX_GAMEPADS)
    return;

  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

#if SDL_VERSION_ATLEAST(2, 0, 9)
  SDL_GameControllerRumble(state->controller, low_freq_motor, high_freq_motor, 30000);
#else
  SDL_Haptic* haptic = state->haptic;
  if (!haptic)
    return;

  if (state->haptic_effect_id >= 0)
    SDL_HapticDestroyEffect(haptic, state->haptic_effect_id);

  if (low_freq_motor == 0 && high_freq_motor == 0)
    return;

  SDL_HapticEffect effect;
  SDL_memset(&effect, 0, sizeof(effect));
  effect.type = SDL_HAPTIC_LEFTRIGHT;
  effect.leftright.length = SDL_HAPTIC_INFINITY;

  // SDL haptics range from 0-32767 but XInput uses 0-65535, so divide by 2 to correct for SDL's scaling
  effect.leftright.large_magnitude = low_freq_motor / 2;
  effect.leftright.small_magnitude = high_freq_motor / 2;

  state->haptic_effect_id = SDL_HapticNewEffect(haptic, &effect);
  if (state->haptic_effect_id >= 0)
    SDL_HapticRunEffect(haptic, state->haptic_effect_id, 1);
#endif
}

void sdlinput_rumble_triggers(unsigned short controller_id, unsigned short left_trigger, unsigned short right_trigger) {
  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

#if SDL_VERSION_ATLEAST(2, 0, 14)
  SDL_GameControllerRumbleTriggers(state->controller, left_trigger, right_trigger, 30000);
#endif
}

void sdlinput_set_motion_event_state(unsigned short controller_id, unsigned char motion_type, unsigned short report_rate_hz) {
  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

#if SDL_VERSION_ATLEAST(2, 0, 14)
  switch (motion_type) {
  case LI_MOTION_TYPE_ACCEL:
    SDL_GameControllerSetSensorEnabled(state->controller, SDL_SENSOR_ACCEL, report_rate_hz ? SDL_TRUE : SDL_FALSE);
    break;
  case LI_MOTION_TYPE_GYRO:
    SDL_GameControllerSetSensorEnabled(state->controller, SDL_SENSOR_GYRO, report_rate_hz ? SDL_TRUE : SDL_FALSE);
    break;
  }
#endif
}

void sdlinput_set_controller_led(unsigned short controller_id, unsigned char r, unsigned char g, unsigned char b) {
  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

#if SDL_VERSION_ATLEAST(2, 0, 14)
  SDL_GameControllerSetLED(state->controller, r, g, b);
#endif
}