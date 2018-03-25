#include "core.hpp"
#include "opengl.hpp"
#include "output.hpp"
#include "view.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"

#include <glm/glm.hpp>
#include "signal-definitions.hpp"

#include <xwayland-api.h>
#include <libweston-desktop.h>
#include <gl-renderer-api.h>

/* misc definitions */

glm::mat4 wayfire_view_transform::global_rotation;
glm::mat4 wayfire_view_transform::global_scale;
glm::mat4 wayfire_view_transform::global_translate;
glm::mat4 wayfire_view_transform::global_view_projection;

glm::mat4 wayfire_view_transform::calculate_total_transform()
{
    return global_view_projection * (global_translate * translation) *
           (global_rotation * rotation) * (global_scale * scale);
}

bool operator == (const weston_geometry& a, const weston_geometry& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool operator != (const weston_geometry& a, const weston_geometry& b)
{
    return !(a == b);
}

bool point_inside(wayfire_point point, weston_geometry rect)
{
    if(point.x < rect.x || point.y < rect.y)
        return false;

    if(point.x > rect.x + rect.width)
        return false;

    if(point.y > rect.y + rect.height)
        return false;

    return true;
}

bool rect_intersect(weston_geometry screen, weston_geometry win)
{
    if (win.x + (int32_t)win.width <= screen.x ||
        win.y + (int32_t)win.height <= screen.y)
        return false;

    if (screen.x + (int32_t)screen.width <= win.x ||
        screen.y + (int32_t)screen.height <= win.y)
        return false;
    return true;
}


const weston_xwayland_surface_api *xwayland_surface_api = nullptr;

wayfire_view_t::wayfire_view_t(weston_desktop_surface *ds)
{
    output  = core->get_active_output();
    handle = weston_desktop_surface_create_view(ds);

    if (!handle)
    {
        errio << "Failed to allocate handle for desktop surface\n"<< std::endl;
    }

    weston_desktop_surface_set_user_data(ds, NULL);
    weston_desktop_surface_set_activated(ds, true);

    desktop_surface = ds;
    ds_geometry = {0, 0, 0, 0};
    surface = weston_desktop_surface_get_surface(ds);

    geometry.width = surface->width;
    geometry.height = surface->height;

    transform.color = glm::vec4(1, 1, 1, 1);

    if (!xwayland_surface_api)
        xwayland_surface_api = weston_xwayland_surface_get_api(core->ec);
}

wayfire_view_t::~wayfire_view_t()
{
    if (source_resize_plus)
        wl_event_source_remove(source_resize_plus);

    if (source_resize_minus)
        wl_event_source_remove(source_resize_minus);

    for (auto& kv : custom_data)
        delete kv.second;
}

#define Mod(x,m) (((x)%(m)+(m))%(m))

// TODO: implement is_visible
bool wayfire_view_t::is_visible()
{
    return true;
}

void idle_resize_minus(void *data)
{
    auto view = static_cast<wayfire_view_t*> (data);
    assert(view);

    if (view->desktop_surface && !view->destroyed)
    {
        weston_desktop_surface_set_size(view->desktop_surface,
                                        view->geometry.width, view->geometry.height);

    }

    view->source_resize_minus = NULL;
}

void idle_resize_plus(void *data)
{
    auto view = static_cast<wayfire_view_t*> (data);
    assert(view);

    if (view->desktop_surface && !view->destroyed && !view->source_resize_minus)
    {
        weston_desktop_surface_set_size(view->desktop_surface,
                                        view->geometry.width - 1, view->geometry.height);

        auto loop = wl_display_get_event_loop(core->ec->wl_display);
        view->source_resize_minus = wl_event_loop_add_idle(loop,
                                                           idle_resize_minus,
                                                           data);
    }

    view->source_resize_plus = NULL;
}

/* To properly position override-redirect windows (such as menus),
 * the xwayland apps need to know their position on screen. However, due
 * to the way weston's window-manager works, the app receives such events
 * only when it is resized. That's why we force a resize at the end
 * of each continuous move(to avoid unnecessary resizes at each coordinate change */
void wayfire_view_t::force_update_xwayland_position()
{
    if (!source_resize_plus && !source_resize_minus)
    {
        auto loop = wl_display_get_event_loop(core->ec->wl_display);
        source_resize_plus = wl_event_loop_add_idle(loop,
                                                    idle_resize_plus, this);
    }
}

void wayfire_view_t::set_moving(bool moving)
{
    in_continuous_move += moving ? 1 : -1;
    if (!moving && xwayland_surface_api &&
        xwayland_surface_api->is_xwayland_surface(surface))
        force_update_xwayland_position();
}

void wayfire_view_t::set_resizing(bool resizing)
{
    in_continuous_resize += resizing ? 1 : -1;
    weston_desktop_surface_set_resizing(desktop_surface, resizing);
}


void wayfire_view_t::move(int x, int y, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = core->find_view(handle);
    data.old_geometry = geometry;

    geometry.x = x;
    geometry.y = y;
    weston_view_set_position(handle, x - ds_geometry.x, y - ds_geometry.y);

    if (xwayland_surface_api && xwayland_surface_api->is_xwayland_surface(surface))
    {
        xwayland_surface_api->send_position(surface, x, y);
        if (!in_continuous_move)
            force_update_xwayland_position();
    }

    if (send_signal)
        output->emit_signal("view-geometry-changed", &data);
}

void wayfire_view_t::resize(int w, int h, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = core->find_view(handle);
    data.old_geometry = geometry;

    weston_desktop_surface_set_size(desktop_surface, w, h);
    geometry.width = w;
    geometry.height = h;

    if (send_signal)
        output->emit_signal("view-geometry-changed", &data);
}

void wayfire_view_t::set_geometry(weston_geometry g)
{
    move(g.x, g.y, false);
    resize(g.width, g.height);
}

void wayfire_view_t::set_geometry(int x, int y, int w, int h)
{
    move(x, y, false);
    resize(w, h);
}

void wayfire_view_t::set_maximized(bool maxim)
{
    maximized = maxim;
    weston_desktop_surface_set_maximized(desktop_surface, maximized);
}

void wayfire_view_t::set_fullscreen(bool full)
{
    fullscreen = full;
    weston_desktop_surface_set_fullscreen(desktop_surface, fullscreen);
}

void wayfire_view_t::map(int sx, int sy)
{
    if (!weston_surface_is_mapped(surface))
    {
        /* special views are panels/backgrounds, workspace_manager handles their position */

        if (!is_special)
        {
            if (xwayland.is_xorg)
            {
                sx = xwayland.x;
                sy = xwayland.y;
            } else
            {
                sx = sy = 0;
            }

            ds_geometry = weston_desktop_surface_get_geometry(desktop_surface);
            geometry.width = ds_geometry.width;
            geometry.height = ds_geometry.height;

            auto workarea = output->workspace->get_workarea();
            if (parent)
            {
                if (parent->is_mapped)
                {
                    sx += parent->geometry.x + (parent->geometry.width  - geometry.width) / 2;
                    sy += parent->geometry.y + (parent->geometry.height - geometry.height) / 2;
                } else
                {
                    /* if we have a parent which still isn't mapped, we cannot determine
                     * the view's position, so we center it on the screen */
                    sx += workarea.width / 2 - geometry.width / 2;
                    sy += workarea.height/ 2 - geometry.height/ 2;
                }
            } else
            {
                sx += workarea.x;
                sy += workarea.y;
            }

            move(sx, sy);

            geometry.x = sx;
            geometry.y = sy;
        }

        weston_view_update_transform(handle);
        handle->is_mapped  = true;
        surface->is_mapped = true;
        is_mapped = true;

        auto sig_data = create_view_signal{core->find_view(handle)};
        output->emit_signal("create-view", &sig_data);

        if (!is_special)
        {
            output->focus_view(core->find_view(handle));

            auto seat = core->get_current_seat();
            auto kbd = seat ? weston_seat_get_keyboard(seat) : NULL;

            if (kbd)
            {
                /* we send zero depressed modifiers, because some modifiers are
                 * stuck when opening a window(for example if the app was opened while some plugin
                 * was working or similar) */
                weston_keyboard_send_modifiers(kbd, wl_display_next_serial(core->ec->wl_display),
                                               0, kbd->modifiers.mods_latched,
                                               kbd->modifiers.mods_locked, kbd->modifiers.group);
            }
        }

        return;
    }

    auto new_ds_g = weston_desktop_surface_get_geometry(desktop_surface);
    if (new_ds_g.x != ds_geometry.x || new_ds_g.y != ds_geometry.y) {
        ds_geometry = new_ds_g;
        move(geometry.x, geometry.y);
    }

    geometry.width = new_ds_g.width;
    geometry.height = new_ds_g.height;

    auto full  = weston_desktop_surface_get_fullscreen(desktop_surface),
         maxim = weston_desktop_surface_get_maximized(desktop_surface);

    if (full != fullscreen)
    {
        view_fullscreen_signal data;
        data.view = core->find_view(handle);
        data.state = full;
        output->emit_signal("view-fullscreen-request", &data);

        set_fullscreen(full);
    } else if (maxim != maximized)
    {
        view_maximized_signal data;
        data.view = core->find_view(handle);
        data.state = maximized;

        output->emit_signal("view-maximized-request", &data);
        set_maximized(maximized);
    }
}

static void render_surface(weston_surface *surface, pixman_region32_t *damage,
        int x, int y, glm::mat4, glm::vec4, uint32_t bits);

/* TODO: use bits */
void wayfire_view_t::simple_render(uint32_t bits, pixman_region32_t *damage)
{
    pixman_region32_t our_damage;
    bool free_damage = false;
    auto og = output->get_full_geometry();

    if (damage == nullptr)
    {
        pixman_region32_init_rect(&our_damage, og.x, og.y, og.width, og.height);
        damage = &our_damage;
        free_damage = true;
    }

    pixman_region32_translate(damage, -og.x, -og.y);

    render_surface(surface, damage,
            geometry.x - ds_geometry.x - og.x, geometry.y - ds_geometry.y - og.y,
            transform.calculate_total_transform(), transform.color, bits);

    pixman_region32_translate(damage, og.x, og.y);

    if (free_damage)
        pixman_region32_fini(&our_damage);
}

void wayfire_view_t::render(uint32_t bits, pixman_region32_t *damage)
{
    simple_render(bits, damage);

    std::vector<effect_hook_t*> hooks_to_run;
    for (auto hook : effects)
        hooks_to_run.push_back(hook);

    for (auto hook : hooks_to_run)
        (*hook)();
}

static inline void render_surface_box(GLuint tex[], int n_tex, GLenum target,
                                      const pixman_box32_t& surface_box,
                                      const pixman_box32_t& subbox,
                                      glm::mat4 transform, glm::vec4 color, uint32_t bits)
{
    OpenGL::texture_geometry texg = {
        1.0f * (subbox.x1 - surface_box.x1) / (surface_box.x2 - surface_box.x1),
        1.0f * (subbox.y1 - surface_box.y1) / (surface_box.y2 - surface_box.y1),
        1.0f * (subbox.x2 - surface_box.x1) / (surface_box.x2 - surface_box.x1),
        1.0f * (subbox.y2 - surface_box.y1) / (surface_box.y2 - surface_box.y1),
    };

    weston_geometry geometry =
    {
        subbox.x1, subbox.y1,
        subbox.x2 - subbox.x1, subbox.y2 - subbox.y1
    };

    OpenGL::render_transformed_texture(tex, n_tex, target, geometry, texg, transform,
                                       color, bits);
}

static inline void render_surface_region(GLuint tex[], int n_tex, GLenum target,
                                         const pixman_box32_t& surface_box,
                                         pixman_region32_t *region,
                                         glm::mat4 transform, glm::vec4 color, uint32_t bits)
{
	int n = 0;
	pixman_box32_t *boxes = pixman_region32_rectangles(region, &n);
	for (int i = 0; i < n; i++) {
		render_surface_box(tex, n_tex, target, surface_box, boxes[i],
				transform, color, bits | TEXTURE_USE_TEX_GEOMETRY);
	}
}

uint32_t get_format_bit(gl_texture_format format)
{
    switch(format)
    {
        case GL_TEXTURE_FORMAT_RGBA:
            return TEXTURE_RGBA;
        case GL_TEXTURE_FORMAT_RGBX:
            return TEXTURE_RGBX;
        case GL_TEXTURE_FORMAT_EGL:
            return TEXTURE_EGL;
        case GL_TEXTURE_FORMAT_Y_UV:
            return TEXTURE_Y_UV;
        case GL_TEXTURE_FORMAT_Y_U_V:
            return TEXTURE_Y_U_V;
        case GL_TEXTURE_FORMAT_Y_XUXV:
            return TEXTURE_Y_XUXV;
        default:
            errio << "encountered wrong texture format" << std::endl;
            return TEXTURE_RGBA;
    };
}

static void render_surface(weston_surface *surface, pixman_region32_t *damage,
        int x, int y, glm::mat4 transform, glm::vec4 color, uint32_t bits)
{
    if (!surface->is_mapped || !surface->renderer_state ||
            surface->width * surface->height == 0)
        return;

    if (!render_manager::renderer_api)
	return;

    pixman_region32_t damaged_region;
    pixman_region32_init_rect(&damaged_region, x, y,
            surface->width, surface->height);
    pixman_region32_intersect(&damaged_region, &damaged_region, damage);

    pixman_box32_t surface_box;
    surface_box.x1 = x; surface_box.y1 = y;
    surface_box.x2 = x + surface->width; surface_box.y2 = y + surface->height;

    int n_tex;
    GLuint *tex = (GLuint*)render_manager::renderer_api->surface_get_textures(surface, &n_tex);

    uint32_t target;
    gl_texture_format format = render_manager::renderer_api->surface_get_texture_format(surface, &target);

    if (format == GL_TEXTURE_FORMAT_RGBA)
    {
        pixman_region32_t opaque;
        pixman_region32_init(&opaque);
        pixman_region32_copy(&opaque, &surface->opaque);
        pixman_region32_translate(&opaque, x, y);

        pixman_region32_intersect(&opaque, &damaged_region, &opaque);
        render_surface_region(tex, n_tex, target,
                              surface_box, &opaque,
                              transform, color, bits | TEXTURE_RGBX);

        pixman_region32_subtract(&damaged_region, &damaged_region, &opaque);
        render_surface_region(tex, n_tex, target,
                              surface_box, &damaged_region,
                              transform, color, bits);

        pixman_region32_fini(&damaged_region);
        pixman_region32_fini(&opaque);
    } else
    {
        uint32_t fmt_bits = get_format_bit(format);
        render_surface_region(tex, n_tex, target,
                              surface_box, &damaged_region,
                              transform, color, bits | fmt_bits);
    }

    weston_subsurface *sub;
    if (!wl_list_empty(&surface->subsurface_list)) {
        wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
            if (sub && sub->surface != surface) {
                render_surface(sub->surface, damage,
                        sub->position.x + x, sub->position.y + y,
                        transform, color, bits);
            }
        }
    }
}
