#ifndef INPUT_MANAGER_HPP
#define INPUT_MANAGER_HPP

#include <unordered_set>
#include <map>
#include <vector>

#include "plugin.hpp"

extern "C"
{
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
}


struct wf_gesture_recognizer;
struct key_callback_data;
struct button_callback_data;
struct wlr_seat;

class input_manager
{
    friend void handle_new_input_cb(wl_listener*, void *data);

    private:
        wayfire_grab_interface active_grab = nullptr;
        bool session_active = true;

        wl_listener input_device_created,
                    key, modifier,
                    button, motion, motion_absolute, axis,
                    touch_down, touch_up, touch_motion;

        wf_gesture_recognizer *gr;

        void handle_gesture(wayfire_touch_gesture g);

        int gesture_id;
        struct wf_gesture_listener
        {
            wayfire_touch_gesture gesture;
            touch_gesture_callback* call;
            wayfire_output *output;
        };

        struct touch_listener {
            uint32_t mod;
            touch_callback* call;
            wayfire_output *output;
        };

        std::map<int, wf_gesture_listener> gesture_listeners;
        std::map<int, touch_listener> touch_listeners;
        std::map<int, key_callback_data*> key_bindings;
        std::map<int, button_callback_data*> button_bindings;

        bool is_touch_enabled();

        void create_seat();
        void setup_keyboard(wlr_input_device *dev);
        void handle_new_input(wlr_input_device *dev);

        void update_cursor_position(uint32_t time_msec);

    public:
        input_manager();
        wlr_seat *seat = nullptr;
        wlr_cursor *cursor = NULL;
        wlr_xcursor_manager *xcursor;

 
        int pointer_count = 0, keyboard_count = 0, touch_count = 0;
        void update_capabilities();


        bool grab_input(wayfire_grab_interface);
        void ungrab_input();
        bool input_grabbed();

        void toggle_session();

        void free_output_bindings(wayfire_output *output);

        bool handle_pointer_axis  (wlr_pointer *ptr, wlr_event_pointer_axis *ev);
        void handle_pointer_motion(wlr_pointer *ptr, wlr_event_pointer_motion *ev);
        void handle_pointer_motion_absolute(wlr_pointer *ptr, wlr_event_pointer_motion_absolute *ev);
        void handle_pointer_button(wlr_pointer *ptr, uint32_t button, uint32_t state);

        bool handle_keyboard_key(uint32_t key, uint32_t state);
        bool handle_keyboard_mod(uint32_t depressed, uint32_t locked,
                                 uint32_t latched, uint32_t group);

        bool handle_touch_down  (wlr_touch*, int32_t, wl_fixed_t, wl_fixed_t);
        bool handle_touch_up    (wlr_touch*, int32_t);
        bool handle_touch_motion(wlr_touch*, int32_t, wl_fixed_t, wl_fixed_t);

        void check_touch_bindings(wlr_touch*, wl_fixed_t sx, wl_fixed_t sy);

        int  add_key(uint32_t mod, uint32_t key, key_callback *, wayfire_output *output);
        void rem_key(int);
        void rem_key(key_callback *callback);

        int  add_button(uint32_t mod, uint32_t button,
                        button_callback *, wayfire_output *output);
        void rem_button(int);
        void rem_button(button_callback *callback);

        int  add_touch(uint32_t mod, touch_callback*, wayfire_output *output);
        void rem_touch(int32_t id);
        void rem_touch(touch_callback*);

        int add_gesture(const wayfire_touch_gesture& gesture,
                        touch_gesture_callback* callback, wayfire_output *output);
        void rem_gesture(int id);
        void rem_gesture(touch_gesture_callback*);
};

#endif /* end of include guard: INPUT_MANAGER_HPP */
