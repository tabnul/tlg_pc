/* tlg-pad-tool: the launcher's Controller page reads connected controllers through this helper, so
 * it sees them exactly as the game does (same SDL3, same type detection and button layout).
 * Windows and Linux. Output, one line each, flushed:
 *   pads <n>                     then n lines: pad <id>\t<type>\t<guid>\t<gyro 0|1>\t<rumble 0|1>\t<name>
 *                                (sent at start and whenever a controller comes or goes)
 *   state <id> <buttons> <lx> <ly> <rx> <ry> <lt> <rt> <gx> <gy> <gz>
 *                                buttons: hex bitmask of SDL_GamepadButton; axes -32768..32767;
 *                                gyro in rad/s (0 without a sensor); about 60 per second per pad
 * Input (stdin): "rumble <id>" (half a second), "quit". End of stdin also quits. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define MAX_PADS 8
static SDL_Gamepad *pads[MAX_PADS];
static SDL_AtomicInt quit_flag, rumble_id;

static int stdin_thread(void *unused) {
    (void)unused;
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned id;
        if (!strncmp(line, "quit", 4)) break;
        if (sscanf(line, "rumble %u", &id) == 1) SDL_SetAtomicInt(&rumble_id, (int)id);
    }
    SDL_SetAtomicInt(&quit_flag, 1);
    return 0;
}

static const char *type_name(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_PS3: return "ps3";
    case SDL_GAMEPAD_TYPE_PS4: return "ps4";
    case SDL_GAMEPAD_TYPE_PS5: return "ps5";
    case SDL_GAMEPAD_TYPE_XBOX360: return "xbox360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "xboxone";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "switchpro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "joycon";
    default: return "generic";
    }
}

static void list_pads(void) {
    int n = 0;
    for (int i = 0; i < MAX_PADS; ++i) n += pads[i] != NULL;
    printf("pads %d\n", n);
    for (int i = 0; i < MAX_PADS; ++i) {
        SDL_Gamepad *g = pads[i];
        if (!g) continue;
        const SDL_JoystickID id = SDL_GetGamepadID(g);
        char guid[64];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
        const SDL_PropertiesID props = SDL_GetGamepadProperties(g);
        const int rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        const char *name = SDL_GetGamepadName(g);
        printf("pad %u\t%s\t%s\t%d\t%d\t%s\n", (unsigned)id, type_name(SDL_GetGamepadType(g)), guid,
               SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) ? 1 : 0, rumble, name ? name : "Controller");
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* No window of our own: read the pads while the launcher has the focus. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("error %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetAtomicInt(&rumble_id, -1);
    SDL_DetachThread(SDL_CreateThread(stdin_thread, "stdin", NULL));
    int changed = 1;
    while (!SDL_GetAtomicInt(&quit_flag)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                for (int i = 0; i < MAX_PADS; ++i)
                    if (!pads[i]) {
                        pads[i] = SDL_OpenGamepad(e.gdevice.which);
                        if (pads[i] && SDL_GamepadHasSensor(pads[i], SDL_SENSOR_GYRO))
                            SDL_SetGamepadSensorEnabled(pads[i], SDL_SENSOR_GYRO, true);
                        break;
                    }
                changed = 1;
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (int i = 0; i < MAX_PADS; ++i)
                    if (pads[i] && SDL_GetGamepadID(pads[i]) == e.gdevice.which) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
                changed = 1;
            }
        }
        if (changed) { list_pads(); changed = 0; }
        const int rumble = SDL_SetAtomicInt(&rumble_id, -1);
        for (int i = 0; i < MAX_PADS; ++i) {
            SDL_Gamepad *g = pads[i];
            if (!g) continue;
            const SDL_JoystickID id = SDL_GetGamepadID(g);
            if (rumble >= 0 && (SDL_JoystickID)rumble == id) SDL_RumbleGamepad(g, 0x8000, 0xC000, 500);
            unsigned buttons = 0;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
                if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) buttons |= 1u << b;
            float gyro[3] = {0, 0, 0};
            if (SDL_GamepadSensorEnabled(g, SDL_SENSOR_GYRO)) SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, gyro, 3);
            printf("state %u %x %d %d %d %d %d %d %.3f %.3f %.3f\n", (unsigned)id, buttons,
                   SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY),
                   SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY),
                   SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
                   gyro[0], gyro[1], gyro[2]);
        }
        if (fflush(stdout) != 0) break; /* the launcher went away */
        SDL_Delay(16);
    }
    for (int i = 0; i < MAX_PADS; ++i) if (pads[i]) SDL_CloseGamepad(pads[i]);
    SDL_Quit();
    return 0;
}
