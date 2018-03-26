#ifndef FIRE_H
#define FIRE_H

#include <functional>
#include <memory>
#include <vector>
#include <map>

extern "C"
{
#include <wlr/backend.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_compositor.h>
#include <wayland-server.h>
}

struct desktop_apis_t;
class input_manager;
class wayfire_config;
class wayfire_output;
class wayfire_view_t;

using wayfire_view = std::shared_ptr<wayfire_view_t>;
using output_callback_proc = std::function<void(wayfire_output *)>;

class wayfire_core
{
        friend struct plugin_manager;
        friend class wayfire_output;

        wayfire_config *config;

        wayfire_output *active_output;

        std::vector<wlr_output*> pending_outputs;
        std::map<wlr_output*, wayfire_output*> outputs;
        std::map<wlr_surface*, wayfire_view> views;

        void configure(wayfire_config *config);

        int times_wake = 0;

    public:

        std::vector<wl_resource*> shell_clients;

        desktop_apis_t *api;

        wl_display *display;
        wl_event_loop *ev_loop;
        wlr_backend *backend;
        wlr_renderer *renderer;
        wlr_output_layout *output_layout;
        wlr_compositor *compositor;


        std::string wayland_display, xwayland_display;

        input_manager *input;

        void init(wayfire_config *config);
        void wake();
        void sleep();
        void refocus_active_output_active_view();

        wlr_seat *get_current_seat();

        void add_view(wayfire_view view);
        wayfire_view find_view(wlr_surface *);

        /* completely destroy a view */
        void erase_view(wayfire_view view);

        /* brings the view to the top
         * and also focuses its output */
        void focus_view(wayfire_view win, wlr_seat *seat);
        void move_view_to_output(wayfire_view v, wayfire_output *new_output);

        void add_output(wlr_output *output);
        wayfire_output *get_output(wlr_output *output);

        void focus_output(wayfire_output *o);
        void remove_output(wayfire_output *o);

        wayfire_output *get_active_output();
        wayfire_output *get_next_output(wayfire_output *output);
        wayfire_output *get_output_at(int x, int y);
        size_t          get_num_outputs();

        void for_each_output(output_callback_proc);

        void run(const char *command);

        int vwidth, vheight;

        std::string shadersrc, plugin_path, plugins;
        bool run_panel;
};

extern wayfire_core *core;
#endif
