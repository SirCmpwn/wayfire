#include "wm.hpp"
#include "output.hpp"
#include "view.hpp"
#include "core.hpp"
#include "workspace-manager.hpp"
#include "../shared/config.hpp"
#include <linux/input.h>
#include "signal-definitions.hpp"

void wayfire_exit::init(wayfire_config*)
{
    key = [](uint32_t key)
    {
        wl_display_terminate(core->display);
    };

    output->add_key(MODIFIER_SUPER, KEY_Z,       &key);
    output->add_key(MODIFIER_ALT   | MODIFIER_CTRL,  KEY_BACKSPACE, &key);
}

void wayfire_close::init(wayfire_config *config)
{
    auto key = config->get_section("core")->get_key("view_close", {MODIFIER_SUPER, KEY_Q});
    callback = [=] (uint32_t key)
    {
        auto view = output->get_top_view();
        if (view) view->close();
    };

    output->add_key(key.mod, key.keyval, &callback);
}

void wayfire_focus::init(wayfire_config *)
{
    grab_interface->name = "_wf_focus";
    grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

    /*
    callback = [=] (wlr_pointer *ptr, uint32_t button)
    {
        core->focus_output(core->get_output_at(
                    wl_fixed_to_int(ptr->x), wl_fixed_to_int(ptr->y)));

        wayfire_view view;
        if (!ptr->focus ||
            !(view = core->find_view(weston_surface_get_main_surface(ptr->focus->surface))))
            return;

        if (view->is_special || view->destroyed || !output->activate_plugin(grab_interface, false))
            return;
        output->deactivate_plugin(grab_interface);
        view->output->focus_view(view, ptr->seat);
    };

    output->add_button((weston_keyboard_modifier)0, BTN_LEFT, &callback);

    touch = [=] (weston_touch *touch, wl_fixed_t sx, wl_fixed_t sy)
    {
        core->focus_output(core->get_output_at(
                    wl_fixed_to_int(sx), wl_fixed_to_int(sy)));

        wayfire_view view;
        if (!touch->focus || !(view = core->find_view(weston_surface_get_main_surface(touch->focus->surface))))
            return;
        if (view->is_special || view->destroyed || !output->activate_plugin(grab_interface, false))
            return;

        output->deactivate_plugin(grab_interface);
        view->output->focus_view(view, touch->seat);
    };

    output->add_touch(0, &touch);
    */
}

/* TODO: remove, it is no longer necessary */
void wayfire_fullscreen::init(wayfire_config *conf)
{
    grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;
    grab_interface->name = "__fs_grab";
}

void wayfire_handle_focus_parent::focus_view(wayfire_view view)
{
    last_view = view;
    view->output->bring_to_front(view);
    for (auto child : view->children)
        focus_view(child);
}

void wayfire_handle_focus_parent::init(wayfire_config*)
{
    focus_event = [&] (signal_data *data)
    {
        auto conv = static_cast<focus_view_signal*> (data);
        assert(conv);
        if (!conv->focus || intercept_recursion)
            return;


        auto to_focus = conv->focus;
        while(to_focus->parent)
            to_focus = to_focus->parent;

        focus_view(to_focus);

        /* because output->focus_view() will fire focus-view signal again,
         * we use this flag to know that this is happening and don't fall
         * into the depths of the infinite recursion */
        intercept_recursion = true;
        output->focus_view(last_view);
        intercept_recursion = false;
    };
    output->connect_signal("focus-view", &focus_event);
}
