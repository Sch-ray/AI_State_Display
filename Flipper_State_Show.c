#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>

#define STATE_SHOW_UART_BAUD 230400
#define STATE_SHOW_FRAME_WIDTH 128
#define STATE_SHOW_FRAME_HEIGHT 64
#define STATE_SHOW_FRAME_BUFFER_SIZE 1024
#define STATE_SHOW_FRAME_INTERVAL_MS 500

typedef enum {
    StateShowStateIdle,
    StateShowStateThinking,
    StateShowStateRunning,
    StateShowStateSuccess,
    StateShowStateError,
} StateShowState;

typedef enum {
    StateShowEventTypeRxChar,
    StateShowEventTypeInput,
} StateShowEventType;

typedef struct {
    StateShowEventType type;
    union {
        uint8_t ch;
        InputEvent input;
    };
} StateShowEvent;

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Storage* storage;
    File* frame_file;
    char current_state[32];
    StateShowState state;
    uint8_t frame_buffer[STATE_SHOW_FRAME_BUFFER_SIZE];
    size_t frame_size;
    uint32_t current_frame;
    uint32_t frame_count;
    uint32_t last_frame_tick;
    bool uart_busy;
    bool speaker_acquired;
    bool speaker_on;
    bool success_beep_active;
    uint32_t success_beep_tick;
} StateShowApp;

static void draw_callback(Canvas* canvas, void* context) {
    StateShowApp* app = context;

    canvas_clear(canvas);

    if(app->uart_busy) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 10, 25, "UART busy");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 10, 45, "Close logs first");
    } else if(app->frame_size > 0) {
        canvas_draw_bitmap(
            canvas,
            0,
            0,
            STATE_SHOW_FRAME_WIDTH,
            STATE_SHOW_FRAME_HEIGHT,
            app->frame_buffer);
    } else {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 10, 35, app->current_state);
    }
}

static void input_callback(InputEvent* input_event, void* context) {
    StateShowApp* app = context;
    StateShowEvent event = {
        .type = StateShowEventTypeInput,
        .input = *input_event,
    };

    furi_message_queue_put(app->event_queue, &event, 0);
}

static void serial_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent rx_event,
    void* context) {
    StateShowApp* app = context;

    if(rx_event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            StateShowEvent event = {
                .type = StateShowEventTypeRxChar,
                .ch = furi_hal_serial_async_rx(handle),
            };

            furi_message_queue_put(app->event_queue, &event, 0);
        }
    }
}

static bool state_from_string(const char* state_name, StateShowState* state) {
    if(strcmp(state_name, "idle") == 0) {
        *state = StateShowStateIdle;
    } else if(strcmp(state_name, "thinking") == 0) {
        *state = StateShowStateThinking;
    } else if(strcmp(state_name, "running") == 0) {
        *state = StateShowStateRunning;
    } else if(strcmp(state_name, "success") == 0) {
        *state = StateShowStateSuccess;
    } else if(strcmp(state_name, "error") == 0) {
        *state = StateShowStateError;
    } else {
        return false;
    }

    return true;
}

static const char* state_to_string(StateShowState state) {
    if(state == StateShowStateThinking) {
        return "thinking";
    } else if(state == StateShowStateRunning) {
        return "running";
    } else if(state == StateShowStateSuccess) {
        return "success";
    } else if(state == StateShowStateError) {
        return "error";
    }

    return "idle";
}

static uint32_t frame_count_for_state(StateShowState state) {
    if(state == StateShowStateThinking) {
        return 15;
    } else if(state == StateShowStateRunning) {
        return 65;
    } else if(state == StateShowStateSuccess) {
        return 38;
    } else if(state == StateShowStateError) {
        return 20;
    }

    return 70;
}

static bool load_current_frame(StateShowApp* app) {
    char path[96];
    snprintf(
        path,
        sizeof(path),
        "/assets/%s/frame_%03lu.bm",
        state_to_string(app->state),
        app->current_frame);

    app->frame_size = 0;

    if(!storage_file_open(app->frame_file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(app->frame_file);
        return false;
    }

    size_t size = storage_file_read(
        app->frame_file, app->frame_buffer, sizeof(app->frame_buffer));
    storage_file_close(app->frame_file);

    if(size == 0 || size >= sizeof(app->frame_buffer)) {
        return false;
    }

    app->frame_size = size;
    return true;
}

static void update_animation_frame(StateShowApp* app) {
    uint32_t now = furi_get_tick();
    if(now - app->last_frame_tick < STATE_SHOW_FRAME_INTERVAL_MS) {
        return;
    }

    app->last_frame_tick = now;
    app->current_frame = (app->current_frame + 1) % app->frame_count;
    load_current_frame(app);
    view_port_update(app->view_port);
}

static void update_led(StateShowState state) {

    if(state == StateShowStateThinking) {

        furi_hal_light_set(LightBlue, 255);

        furi_hal_light_set(LightRed, 0);
        furi_hal_light_set(LightGreen, 0);
    }

    else if(state == StateShowStateRunning) {

        furi_hal_light_set(LightRed, 255);
        furi_hal_light_set(LightGreen, 180);
        furi_hal_light_set(LightBlue, 0);
    }

    else if(state == StateShowStateSuccess) {

        furi_hal_light_set(LightGreen, 255);

        furi_hal_light_set(LightRed, 0);
        furi_hal_light_set(LightBlue, 0);
    }

    else if(state == StateShowStateError) {

        furi_hal_light_set(LightRed, 255);

        furi_hal_light_set(LightGreen, 0);
        furi_hal_light_set(LightBlue, 0);
    }

    else {

        furi_hal_light_set(LightRed, 0);
        furi_hal_light_set(LightGreen, 0);
        furi_hal_light_set(LightBlue, 0);
    }
}

static void stop_error_beep(StateShowApp* app) {
    if(app->speaker_on) {
        furi_hal_speaker_stop();
        app->speaker_on = false;
    }

    if(app->speaker_acquired) {
        furi_hal_speaker_release();
        app->speaker_acquired = false;
    }
}

static void start_success_beep(StateShowApp* app) {
    if(app->speaker_acquired) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
        app->speaker_acquired = false;
        app->speaker_on = false;
    }

    app->success_beep_active = true;
    app->success_beep_tick = furi_get_tick();
}

static void stop_success_beep(StateShowApp* app) {
    if(!app->success_beep_active) {
        return;
    }

    if(app->speaker_on) {
        furi_hal_speaker_stop();
        app->speaker_on = false;
    }

    if(app->speaker_acquired) {
        furi_hal_speaker_release();
        app->speaker_acquired = false;
    }

    app->success_beep_active = false;
}

static void update_success_beep(StateShowApp* app) {
    if(!app->success_beep_active) {
        return;
    }

    static const float frequencies[] = {660.0f, 880.0f, 1175.0f};
    uint32_t elapsed = furi_get_tick() - app->success_beep_tick;
    uint8_t step = elapsed / 80;

    if(step >= 6) {
        stop_success_beep(app);
        return;
    }

    bool should_be_on = (step % 2) == 0;
    uint8_t note_index = step / 2;

    if(should_be_on && !app->speaker_on) {
        if(!app->speaker_acquired) {
            app->speaker_acquired = furi_hal_speaker_acquire(0);
        }

        if(app->speaker_acquired) {
            furi_hal_speaker_start(frequencies[note_index], 1.0f);
            app->speaker_on = true;
        }
    } else if(!should_be_on && app->speaker_on) {
        furi_hal_speaker_stop();
        app->speaker_on = false;
    }
}

static void update_error_beep(StateShowApp* app) {
    if(app->state != StateShowStateError) {
        return;
    }

    if(!app->speaker_acquired) {
        app->speaker_acquired = furi_hal_speaker_acquire(0);
    }

    if(!app->speaker_acquired) {
        return;
    }

    uint32_t phase = furi_get_tick() % 700;
    bool should_be_on = (phase < 80) || ((phase >= 160) && (phase < 240));

    if(should_be_on && !app->speaker_on) {
        furi_hal_speaker_start(880.0f, 0.6f);
        app->speaker_on = true;
    } else if(!should_be_on && app->speaker_on) {
        furi_hal_speaker_stop();
        app->speaker_on = false;
    }
}

static void apply_state(StateShowApp* app, StateShowState state) {
    if(app->state == state) {
        return;
    }

    if(app->state == StateShowStateError) {
        stop_error_beep(app);
    }
    if(app->success_beep_active) {
        stop_success_beep(app);
    }

    app->state = state;
    strncpy(app->current_state, state_to_string(state), sizeof(app->current_state) - 1);
    app->current_state[sizeof(app->current_state) - 1] = '\0';

    update_led(app->state);
    if(app->state == StateShowStateSuccess) {
        start_success_beep(app);
    }

    app->frame_count = frame_count_for_state(app->state);
    app->current_frame = 0;
    app->last_frame_tick = furi_get_tick();
    load_current_frame(app);

    view_port_update(app->view_port);
}

static char* trim_command(char* text) {
    while((*text == ' ') || (*text == '\t')) {
        text++;
    }

    char* end = text + strlen(text);
    while((end > text) && ((*(end - 1) == ' ') || (*(end - 1) == '\t'))) {
        end--;
    }
    *end = '\0';

    return text;
}

static const char* parse_state_command(char* command) {
    command = trim_command(command);

    if(strncmp(command, "state", 5) == 0) {
        char next = command[5];
        if((next == ' ') || (next == '\t')) {
            return trim_command(command + 6);
        }
    }

    return command;
}

int32_t flipper_state_show_app(void* p) {

    UNUSED(p);

    StateShowApp* app = malloc(sizeof(StateShowApp));
    memset(app, 0, sizeof(StateShowApp));
    app->event_queue = furi_message_queue_alloc(64, sizeof(StateShowEvent));
    strlcpy(app->current_state, "idle", sizeof(app->current_state));
    app->state = StateShowStateIdle;
    app->frame_count = frame_count_for_state(app->state);
    update_led(app->state);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->frame_file = storage_file_alloc(app->storage);

    Gui* gui = furi_record_open(RECORD_GUI);

    ViewPort* view_port = view_port_alloc();
    app->view_port = view_port;

    view_port_draw_callback_set(view_port, draw_callback, app);
    view_port_input_callback_set(view_port, input_callback, app);

    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    load_current_frame(app);
    view_port_update(view_port);

    FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);

    if(serial) {
        furi_hal_serial_init(serial, STATE_SHOW_UART_BAUD);
        furi_hal_serial_async_rx_start(serial, serial_rx_callback, app, false);
    } else {
        strlcpy(app->current_state, "UART busy", sizeof(app->current_state));
        app->uart_busy = true;
        view_port_update(view_port);
    }

    char rx_buffer[64];
    size_t rx_index = 0;

    bool running = true;

    while(running) {

        StateShowEvent event;

        if(furi_message_queue_get(app->event_queue, &event, 25) == FuriStatusOk) {
            if(event.type == StateShowEventTypeInput) {
                if((event.input.key == InputKeyBack) && (event.input.type == InputTypeShort)) {
                    running = false;
                } else if(
                    (event.input.key == InputKeyOk) && (event.input.type == InputTypeShort) &&
                    (app->state == StateShowStateError)) {
                    apply_state(app, StateShowStateIdle);
                }
            } else if(event.type == StateShowEventTypeRxChar) {
                if((event.ch == '\n') || (event.ch == '\r')) {
                    if(rx_index > 0) {
                        StateShowState parsed_state;
                        rx_buffer[rx_index] = '\0';
                        if(state_from_string(parse_state_command(rx_buffer), &parsed_state)) {
                            apply_state(app, parsed_state);
                        }
                        rx_index = 0;
                    }
                } else if(rx_index < sizeof(rx_buffer) - 1) {
                    rx_buffer[rx_index++] = event.ch;
                } else {
                    rx_index = 0;
                }
            }
        }

        update_error_beep(app);
        update_success_beep(app);
        update_animation_frame(app);
    }

    if(serial) {
        furi_hal_serial_async_rx_stop(serial);
        furi_hal_serial_deinit(serial);
        furi_hal_serial_control_release(serial);
    }

    stop_error_beep(app);
    stop_success_beep(app);
    update_led(StateShowStateIdle);

    gui_remove_view_port(gui, view_port);

    view_port_free(view_port);

    furi_record_close(RECORD_GUI);

    storage_file_free(app->frame_file);
    furi_record_close(RECORD_STORAGE);

    furi_message_queue_free(app->event_queue);
    free(app);

    return 0;
}
