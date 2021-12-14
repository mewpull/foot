#if !defined(_DEBUG)
 #define _DEBUG
#endif
#undef NDEBUG

#include "../log.h"

#include "../config.c"

#define ALEN(v) (sizeof(v) / sizeof((v)[0]))

/*
 * Stubs
 */

void
user_notification_add_fmt(user_notifications_t *notifications,
                          enum user_notification_kind kind,
                          const char *fmt, ...)
{
}

static void
test_invalid_key(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                 const char *key)
{
    ctx->key = key;
    ctx->value = "value for invalid key";

    if (parse_fun(ctx)) {
        BUG("[%s].%s: did not fail to parse as expected"
            "(key should be invalid)", ctx->section, ctx->key);
    }
}

static void
test_string(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, char *const *conf_ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        const char *value;
        bool invalid;
    } input[] = {
        {"a string", "a string"},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (strcmp(*conf_ptr, input[i].value) != 0) {
                BUG("[%s].%s=%s: set value (%s) not the expected one (%s)",
                    ctx->section, ctx->key, ctx->value,
                    *conf_ptr, input[i].value);
            }
        }
    }
}

static void
test_wstring(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, wchar_t *const *conf_ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        const wchar_t *value;
        bool invalid;
    } input[] = {
        {"a string", L"a string"},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (wcscmp(*conf_ptr, input[i].value) != 0) {
                BUG("[%s].%s=%s: set value (%ls) not the expected one (%ls)",
                    ctx->section, ctx->key, ctx->value,
                    *conf_ptr, input[i].value);
            }
        }
    }
}

static void
test_boolean(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, const bool *conf_ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        bool value;
        bool invalid;
    } input[] = {
        {"1", true}, {"0", false},
        {"on", true}, {"off", false},
        {"true", true}, {"false", false},
        {"unittest-invalid-boolean-value", false, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*conf_ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%s) not the expected one (%s)",
                    ctx->section, ctx->key, ctx->value,
                    *conf_ptr ? "true" : "false",
                    input[i].value ? "true" : "false");
            }
        }
    }
}

static void
test_uint16(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const uint16_t *conf_ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        uint16_t value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"65535", 65535}, {"65536", 0, true},
        {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*conf_ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%hu) not the expected one (%hu)",
                    ctx->section, ctx->key, ctx->value,
                    *conf_ptr, input[i].value);
            }
        }
    }
}

static void
test_pt_or_px(struct context *ctx, bool (*parse_fun)(struct context *ctx),
              const char *key, const struct pt_or_px *conf_ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        struct pt_or_px value;
        bool invalid;
    } input[] = {
        {"12", {.pt = 12}}, {"12px", {.px = 12}},
        {"unittest-invalid-pt-or-px-value", {0}, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (memcmp(conf_ptr, &input[i].value, sizeof(*conf_ptr)) != 0) {
                BUG("[%s].%s=%s: "
                    "set value (pt=%f, px=%d) not the expected one (pt=%f, px=%d)",
                    ctx->section, ctx->key, ctx->value,
                    conf_ptr->pt, conf_ptr->px,
                    input[i].value.pt, input[i].value.px);
            }
        }
    }
}

static void
test_section_main(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "main", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_main, "invalid-key");

    test_string(&ctx, &parse_section_main, "shell", &conf.shell);
    test_string(&ctx, &parse_section_main, "term", &conf.term);
    test_string(&ctx, &parse_section_main, "app-id", &conf.app_id);

    test_wstring(&ctx, &parse_section_main, "word-delimiters", &conf.word_delimiters);

    test_boolean(&ctx, &parse_section_main, "login-shell", &conf.login_shell);
    test_boolean(&ctx, &parse_section_main, "box-drawings-uses-font-glyphs", &conf.box_drawings_uses_font_glyphs);
    test_boolean(&ctx, &parse_section_main, "locked-title", &conf.locked_title);
    test_boolean(&ctx, &parse_section_main, "notify-focus-inhibit", &conf.notify_focus_inhibit);

    test_pt_or_px(&ctx, &parse_section_main, "line-height", &conf.line_height);
    test_pt_or_px(&ctx, &parse_section_main, "letter-spacing", &conf.letter_spacing);
    test_pt_or_px(&ctx, &parse_section_main, "horizontal-letter-offset", &conf.horizontal_letter_offset);
    test_pt_or_px(&ctx, &parse_section_main, "vertical-letter-offset", &conf.vertical_letter_offset);

    test_uint16(&ctx, &parse_section_main, "resize-delay-ms", &conf.resize_delay_ms);
    test_uint16(&ctx, &parse_section_main, "workers", &conf.render_worker_count);

    /* TODO: font (custom) */
    /* TODO: include (custom) */
    /* TODO: dpi-aware (enum/boolean) */
    /* TODO: bold-text-in-bright (enum/boolean) */
    /* TODO: pad (geometry + optional string)*/
    /* TODO: initial-window-size-pixels (geometry) */
    /* TODO: initial-window-size-chars (geometry) */
    /* TODO: notify (spawn template)*/
    /* TODO: selection-target (enum) */
    /* TODO: initial-window-mode (enum) */

    config_free(conf);
}

static void
test_key_binding(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                 int action, int max_action, const char *const *map,
                 struct config_key_binding_list *bindings,
                 enum config_key_binding_type type)
{
    xassert(map[action] != NULL);
    xassert(bindings->count == 0);

    const char *key = map[action];

    /* “Randomize” which modifiers to enable */
    const bool ctrl = action % 2;
    const bool alt = action % 3;
    const bool shift = action % 4;
    const bool super = action % 5;

    /* Generate the modifier part of the ‘value’ */
    char modifier_string[32];
    sprintf(modifier_string, "%s%s%s%s",
            ctrl ? XKB_MOD_NAME_CTRL "+" : "",
            alt ? XKB_MOD_NAME_ALT "+" : "",
            shift ? XKB_MOD_NAME_SHIFT "+" : "",
            super ? XKB_MOD_NAME_LOGO "+" : "");

    /* Use a unique symbol for this action (key bindings) */
    const xkb_keysym_t sym = XKB_KEY_a + action;

    /* Mouse button (mouse bindings) */
    const int button_idx = action % ALEN(button_map);
    const int button = button_map[button_idx].code;
    const int click_count = action % 3 + 1;

    /* Finally, generate the ‘value’ (e.g. “Control+shift+x”) */
    char value[128];

    switch (type) {
    case KEY_BINDING: {
        char sym_name[16];
        xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));

        snprintf(value, sizeof(value), "%s%s", modifier_string, sym_name);
        break;
    }

    case MOUSE_BINDING: {
        const char *const button_name = button_map[button_idx].name;
        int chars = snprintf(
            value, sizeof(value), "%s%s", modifier_string, button_name);

        xassert(click_count > 0);
        if (click_count > 1)
            snprintf(&value[chars], sizeof(value) - chars, "-%d", click_count);
        break;
    }
    }

    ctx->key = key;
    ctx->value = value;

    if (!parse_fun(ctx)) {
        BUG("[%s].%s=%s failed to parse",
            ctx->section, ctx->key, ctx->value);
    }

    const struct config_key_binding *binding =
        &bindings->arr[bindings->count - 1];

    xassert(binding->pipe.argv.args == NULL);

    if (binding->action != action) {
        BUG("[%s].%s=%s: action mismatch: %d != %d",
            ctx->section, ctx->key, ctx->value, binding->action, action);
    }

    if (binding->modifiers.ctrl != ctrl ||
        binding->modifiers.alt != alt ||
        binding->modifiers.shift != shift ||
        binding->modifiers.super != super)
    {
        BUG("[%s].%s=%s: modifier mismatch:\n"
            "  have:     ctrl=%d, alt=%d, shift=%d, super=%d\n"
            "  expected: ctrl=%d, alt=%d, shift=%d, super=%d",
            ctx->section, ctx->key, ctx->value,
            binding->modifiers.ctrl, binding->modifiers.alt,
            binding->modifiers.shift, binding->modifiers.super,
            ctrl, alt, shift, super);
    }

    switch (type) {
    case KEY_BINDING:
        if (binding->k.sym != sym) {
            BUG("[%s].%s=%s: key symbol mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value, binding->k.sym, sym);
        }
        break;

    case MOUSE_BINDING:;
        if (binding->m.button != button) {
            BUG("[%s].%s=%s: mouse button mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value, binding->m.button, button);
        }

        if (binding->m.count != click_count) {
            BUG("[%s].%s=%s: mouse button click count mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value,
                binding->m.count, click_count);
        }
        break;
    }


    free_key_binding_list(bindings);
}

enum collision_test_mode {
    FAIL_DIFFERENT_ACTION,
    FAIL_DIFFERENT_ARGV,
    FAIL_MOUSE_OVERRIDE,
    SUCCED_SAME_ACTION_AND_ARGV,
};

static void
_test_binding_collisions(struct context *ctx,
                         int max_action, const char *const *map,
                         enum config_key_binding_type type,
                         enum collision_test_mode test_mode)
{
    struct config_key_binding *bindings_array =
        xcalloc(2, sizeof(bindings_array[0]));

    struct config_key_binding_list bindings = {
        .count = 2,
        .arr = bindings_array,
    };

    /* First, verify we get a collision when trying to assign the same
     * key combo to multiple actions */
    bindings.arr[0] = (struct config_key_binding){
        .action = (test_mode == FAIL_DIFFERENT_ACTION
                   ? max_action - 1 : max_action),
        .modifiers = {.ctrl = true},
        .path = "unittest",
    };
    bindings.arr[1] = (struct config_key_binding){
        .action = max_action,
        .modifiers = {.ctrl = true},
        .path = "unittest",
    };

    switch (type) {
    case KEY_BINDING:
        bindings.arr[0].k.sym = XKB_KEY_a;
        bindings.arr[1].k.sym = XKB_KEY_a;
        break;

    case MOUSE_BINDING:
        bindings.arr[0].m.button = BTN_LEFT;
        bindings.arr[0].m.count = 1;
        bindings.arr[1].m.button = BTN_LEFT;
        bindings.arr[1].m.count = 1;
        break;
    }

    switch (test_mode) {
    case FAIL_DIFFERENT_ACTION:
        break;

    case FAIL_MOUSE_OVERRIDE:
        ctx->conf->mouse.selection_override_modifiers.ctrl = true;
        break;

    case FAIL_DIFFERENT_ARGV:
    case SUCCED_SAME_ACTION_AND_ARGV:
        bindings.arr[0].pipe.master_copy = true;
        bindings.arr[0].pipe.argv.args = xcalloc(
            4, sizeof(bindings.arr[0].pipe.argv.args[0]));
        bindings.arr[0].pipe.argv.args[0] = xstrdup("/usr/bin/foobar");
        bindings.arr[0].pipe.argv.args[1] = xstrdup("hello");
        bindings.arr[0].pipe.argv.args[2] = xstrdup("world");

        bindings.arr[1].pipe.master_copy = true;
        bindings.arr[1].pipe.argv.args = xcalloc(
            4, sizeof(bindings.arr[1].pipe.argv.args[0]));
        bindings.arr[1].pipe.argv.args[0] = xstrdup("/usr/bin/foobar");
        bindings.arr[1].pipe.argv.args[1] = xstrdup("hello");

        if (test_mode == SUCCED_SAME_ACTION_AND_ARGV)
            bindings.arr[1].pipe.argv.args[2] = xstrdup("world");
        break;
    }

    bool expected_result =
        test_mode == SUCCED_SAME_ACTION_AND_ARGV ? true : false;

    if (resolve_key_binding_collisions(
            ctx->conf, ctx->section, map, &bindings, type) != expected_result)
    {
        BUG("[%s].%s vs. %s: %s",
            ctx->section, map[max_action - 1], map[max_action],
            (expected_result == true
             ? "invalid key combo collision detected"
             : "key combo collision not detected"));
    }

    if (expected_result == false) {
        if (bindings.count != 1)
            BUG("[%s]: colliding binding not removed", ctx->section);

        if (bindings.arr[0].action !=
            (test_mode == FAIL_DIFFERENT_ACTION ? max_action - 1 : max_action))
        {
            BUG("[%s]: wrong binding removed", ctx->section);
        }
    }

    free_key_binding_list(&bindings);
}

static void
test_binding_collisions(struct context *ctx,
                         int max_action, const char *const *map,
                        enum config_key_binding_type type)
{
    _test_binding_collisions(ctx, max_action, map, type, FAIL_DIFFERENT_ACTION);
    _test_binding_collisions(ctx, max_action, map, type, FAIL_DIFFERENT_ARGV);
    _test_binding_collisions(ctx, max_action, map, type, SUCCED_SAME_ACTION_AND_ARGV);

    if (type == MOUSE_BINDING) {
        _test_binding_collisions(
            ctx, max_action, map, type, FAIL_MOUSE_OVERRIDE);
    }
}

static void
test_section_key_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_key_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_KEY_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_key_bindings,
            action, BIND_ACTION_KEY_COUNT - 1,
            binding_action_map, &conf.bindings.key, KEY_BINDING);
    }

    config_free(conf);
}

static void
test_section_key_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx, BIND_ACTION_KEY_COUNT - 1, binding_action_map, KEY_BINDING);

    config_free(conf);
}

static void
test_section_search_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "search-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_search_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_SEARCH_COUNT; action++) {
        if (search_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_search_bindings,
            action, BIND_ACTION_SEARCH_COUNT - 1,
            search_binding_action_map, &conf.bindings.search, KEY_BINDING);
    }

    config_free(conf);
}

static void
test_section_search_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "search-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_SEARCH_COUNT - 1, search_binding_action_map, KEY_BINDING);

    config_free(conf);
}

static void
test_section_url_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "rul-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_url_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_URL_COUNT; action++) {
        if (url_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_url_bindings,
            action, BIND_ACTION_URL_COUNT - 1,
            url_binding_action_map, &conf.bindings.url, KEY_BINDING);
    }

    config_free(conf);
}

static void
test_section_url_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "url-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_URL_COUNT - 1, url_binding_action_map, KEY_BINDING);

    config_free(conf);
}

static void
test_section_mouse_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_mouse_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_mouse_bindings,
            action, BIND_ACTION_COUNT - 1,
            binding_action_map, &conf.bindings.mouse, MOUSE_BINDING);
    }

    config_free(conf);
}

static void
test_section_mouse_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_COUNT - 1, binding_action_map, MOUSE_BINDING);

    config_free(conf);
}

int
main(int argc, const char *const *argv)
{
    log_init(LOG_COLORIZE_AUTO, false, 0, LOG_CLASS_ERROR);
    test_section_main();
    test_section_key_bindings();
    test_section_key_bindings_collisions();
    test_section_search_bindings();
    test_section_search_bindings_collisions();
    test_section_url_bindings();
    test_section_url_bindings_collisions();
    test_section_mouse_bindings();
    test_section_mouse_bindings_collisions();
    log_deinit();
    return 0;
}