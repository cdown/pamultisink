#include <errno.h>
#include <pulse/pulseaudio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODULE_ARGS_MAX 2048 // Large enough for all the combine sink args
#define SINK_INPUT_MAX 128   // Large enough for sink names and inputs
#define SINK_MAX 128         // Max number of sinks

#define MODULE_NAME "module-combine-sink"
#define APP_NAME "pamultisink"
#define COMBINED_SINK_NAME "__autogenerated_by_" APP_NAME

#define die(format, ...)                                                       \
    do {                                                                       \
        fprintf(stderr, "FATAL: " format, __VA_ARGS__);                        \
        cleanup();                                                             \
        abort();                                                               \
    } while (0)

#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            die("!(%s) at %s:%s:%d\n", #x, __FILE__, __func__, __LINE__);      \
        }                                                                      \
    } while (0)

static char output_buf[512];

struct ms_sink_info {
    char name[SINK_INPUT_MAX];
    char description[SINK_INPUT_MAX];
    bool selected;
};
static struct ms_sink_info sinks[SINK_MAX];
static int nr_sinks;

static pa_mainloop *pa_ml;
static pa_mainloop_api *pa_mlapi;
static pa_context *pa_ctx;

static void cleanup(void) {
    if (pa_ctx) {
        pa_context_disconnect(pa_ctx);
        pa_context_unref(pa_ctx);
    }
    if (pa_ml) {
        pa_mainloop_free(pa_ml);
    }
}

__attribute__((format(printf, 3, 4))) static size_t
snprintf_check(char *buf, const size_t len, const char *fmt, ...) {
    int needed;
    va_list args;

    expect(len > 0);

    va_start(args, fmt);
    needed = vsnprintf(buf, len, fmt, args);
    va_end(args);

    expect(needed >= 0 && (size_t)needed < len);

    return (size_t)needed;
}

#define strncpy_check(dst, src, len) snprintf_check(dst, len, "%s", src)

static bool sink_is_managed(const char *name) {
    // If we somehow ended up with multiple they will have ".[idx]" appended,
    // so only check the beginning
    return strncmp(name, COMBINED_SINK_NAME, strlen(COMBINED_SINK_NAME)) == 0;
}

static void module_unload_cb(pa_context *c, int success, void *userdata) {
    (void)c;
    (void)userdata;

    if (!success) {
        die("Failed to unload %s.\n", MODULE_NAME);
    }
}

static void module_load_cb(pa_context *c, const uint32_t idx, void *userdata) {
    (void)c;
    (void)userdata;

    if (idx == PA_INVALID_INDEX) {
        die("Failed to load %s.\n", MODULE_NAME);
    }

    printf("Sinks merged with index %u.\n", idx);
}

static void sink_populate_local_cb(pa_context *c, const pa_sink_info *i,
                                   const int eol, void *userdata) {
    static bool warned = false;
    struct ms_sink_info *sink_info = &sinks[nr_sinks];

    (void)c;
    (void)userdata;

    if (nr_sinks >= SINK_MAX) {
        if (!warned) {
            fprintf(stderr, "Too many sinks, truncating to %d\n", SINK_MAX);
            warned = true;
        }
        return;
    }

    if (eol || sink_is_managed(i->name)) {
        return;
    }

    // Dynamically allocated inside the structure, so copy one by one.
    strncpy_check(sink_info->name, i->name, SINK_INPUT_MAX);
    strncpy_check(sink_info->description, i->description, SINK_INPUT_MAX);
    nr_sinks++;
}

static void module_find_and_unload_combined_sink_cb(pa_context *c,
                                                    const pa_module_info *i,
                                                    const int eol,
                                                    void *userdata) {
    bool *unloaded = userdata;
    if (!eol && strcmp(i->name, MODULE_NAME) == 0) {
        *unloaded = true;
        pa_operation_unref(
            pa_context_unload_module(c, i->index, module_unload_cb, userdata));
    }
}

static void module_make_args(char *buf, const size_t buf_length,
                             char sel_sinks[][SINK_INPUT_MAX],
                             const size_t nr_sel_sinks) {
    size_t pos = 0, i;

    pos += snprintf_check(&buf[pos],
                          buf_length - pos,
                          "sink_name=%s slaves=",
                          COMBINED_SINK_NAME);
    for (i = 0; i < nr_sel_sinks; i++) {
        pos += snprintf_check(&buf[pos], buf_length - pos, "%s,", sel_sinks[i]);
    }
    buf[pos - 1] = '\0';
}

static int op_finish_and_unref(pa_operation *pa_op) {
    int ret = 0, pa_errno;
    enum pa_operation_state state = pa_operation_get_state(pa_op);

    while (state == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(pa_ml, 1, NULL);
        state = pa_operation_get_state(pa_op);
    }

    if (state != PA_OPERATION_DONE) {
        pa_errno = pa_context_errno(pa_ctx);
        fprintf(
            stderr, "PulseAudio operation failed: %s\n", pa_strerror(pa_errno));
        ret = -pa_errno;
    }

    pa_operation_unref(pa_op);
    return ret;
}

// Print as usual, and then move the cursor one line up to make redrawing work.
// Not to stderr, since then we don't know if we should move up or not since we
// don't know where it's going.
#define move_up_n_lines(n) printf("\033[%dA", n)
#define clear_line() printf("\033[K")
#define retryable_sink_select_error(...)                                       \
    do {                                                                       \
        printf(__VA_ARGS__);                                                   \
        move_up_n_lines(1);                                                    \
    } while (0)

static char *sink_select_from_user(char *args, const size_t len) {
    char selected_sinks[SINK_MAX][SINK_INPUT_MAX];
    size_t nr_selected_sinks = 0;

    printf("Available sinks:\n");
    for (int i = 0; i < nr_sinks; i++) {
        printf("%d: %s\n", i, sinks[i].description);
    }

    while (nr_selected_sinks < SINK_MAX) {
        int ch, idx, input_len;
        char input[SINK_INPUT_MAX];
        char sel_sink_nrs[SINK_INPUT_MAX];
        size_t pos = 0;

        for (int i = 0; i < nr_sinks; i++) {
            if (sinks[i].selected) {
                pos += snprintf_check(
                    &sel_sink_nrs[pos], SINK_INPUT_MAX - pos, "%d,", i);
            }
        }

        if (pos == 0) {
            snprintf_check(sel_sink_nrs, sizeof(sel_sink_nrs), "none");
        } else {
            sel_sink_nrs[pos - 1] = '\0';
        }

        // May be longer going from "none" to the first sink, so clear
        clear_line();
        printf("Sink number to add, enter to finish (current: %s): ",
               sel_sink_nrs);

        if (!fgets(input, sizeof(input), stdin)) {
            fprintf(stderr, "Error reading input.\n");
            return NULL;
        }
        if (input[0] == '\n') {
            break;
        }
        if (sscanf(input, "%d%n", &idx, &input_len) != 1 ||
            input[input_len] != '\n') {
            retryable_sink_select_error("Invalid input.\n");
        } else if (idx < 0 || idx >= nr_sinks) {
            retryable_sink_select_error("Invalid sink number.\n");
        } else if (sinks[idx].selected) {
            retryable_sink_select_error("Sink already selected.\n");
        } else {
            strncpy_check(selected_sinks[nr_selected_sinks],
                          sinks[idx].name,
                          SINK_INPUT_MAX - 1);
            sinks[idx].selected = true;
            nr_selected_sinks++;
            // Clear the upcoming line, it may have had an error before.
            clear_line();
        }

        if (input[strlen(input) - 1] != '\n') {
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
        }

        // To makes sure we're on the right line for the prompt redraw.
        move_up_n_lines(1);
    }

    printf("\n");

    if (nr_selected_sinks == 0) {
        fprintf(stderr, "No sinks selected.\n");
        return NULL;
    }

    module_make_args(args, len, selected_sinks, nr_selected_sinks);

    return args;
}

static int run_pa_mainloop(void) {
    int pa_errno;
    char args[MODULE_ARGS_MAX];
    pa_operation *unload_op;
    bool unloaded = false;

    for (;;) {
        pa_mainloop_iterate(pa_ml, 1, NULL);

        switch (pa_context_get_state(pa_ctx)) {
            case PA_CONTEXT_READY:
                unload_op = pa_context_get_module_info_list(
                    pa_ctx,
                    module_find_and_unload_combined_sink_cb,
                    (void *)&unloaded);

                expect(op_finish_and_unref(pa_context_get_sink_info_list(
                           pa_ctx, sink_populate_local_cb, NULL)) == 0);

                if (nr_sinks == 0) {
                    fprintf(stderr, "No sinks available\n");
                    return -ENODATA;
                }

                if (!sink_select_from_user(args, sizeof(args))) {
                    return -EINVAL;
                }

                // Must be done or we may unload the new one
                expect(op_finish_and_unref(unload_op) == 0);
                if (unloaded) {
                    printf("Found and unloaded existing combined sinks.\n");
                }

                expect(op_finish_and_unref(pa_context_load_module(
                           pa_ctx, MODULE_NAME, args, module_load_cb, NULL)) ==
                       0);

                return 0;
            case PA_CONTEXT_FAILED:
                pa_errno = pa_context_errno(pa_ctx);
                fprintf(stderr,
                        "PulseAudio context failed: %s\n",
                        pa_strerror(pa_errno));
                return -pa_errno;
            case PA_CONTEXT_TERMINATED:
                return 0;
            case PA_CONTEXT_AUTHORIZING:
            case PA_CONTEXT_CONNECTING:
            case PA_CONTEXT_SETTING_NAME:
            case PA_CONTEXT_UNCONNECTED:
                break;
        }
    }
}

int main(void) {
    int exit_code;

    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);

    expect(pa_ml = pa_mainloop_new());
    expect(pa_mlapi = pa_mainloop_get_api(pa_ml));
    expect(pa_ctx = pa_context_new(pa_mlapi, APP_NAME));
    expect(pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) == 0);

    exit_code = !!run_pa_mainloop();

    cleanup();

    return exit_code;
}
