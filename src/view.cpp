#include "debug.hpp"
#include "core.hpp"
#include "opengl.hpp"
#include "output.hpp"
#include "view.hpp"
#include "view-transform.hpp"
#include "decorator.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include "desktop-api.hpp"

#include <algorithm>
#include <glm/glm.hpp>
#include "signal-definitions.hpp"

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/region.h>
#undef static
}

/* TODO: clean up the code, currently it is a horrible mess */

/* misc definitions */

/* TODO: use surface->data instead of a global map */

bool operator == (const wf_geometry& a, const wf_geometry& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool operator != (const wf_geometry& a, const wf_geometry& b)
{
    return !(a == b);
}

wf_point operator + (const wf_point& a, const wf_point& b)
{
    return {a.x + b.x, a.y + b.y};
}

wf_point operator + (const wf_point& a, const wf_geometry& b)
{
    return {a.x + b.x, a.y + b.y};
}

wf_geometry operator + (const wf_geometry &a, const wf_point& b)
{
    return {
        a.x + b.x,
        a.y + b.y,
        a.width,
        a.height
    };
}

wf_point operator - (const wf_point& a)
{
    return {-a.x, -a.y};
}

bool point_inside(wf_point point, wf_geometry rect)
{
    if(point.x < rect.x || point.y < rect.y)
        return false;

    if(point.x > rect.x + rect.width)
        return false;

    if(point.y > rect.y + rect.height)
        return false;

    return true;
}

bool rect_intersect(wf_geometry screen, wf_geometry win)
{
    if (win.x + (int32_t)win.width <= screen.x ||
        win.y + (int32_t)win.height <= screen.y)
        return false;

    if (screen.x + (int32_t)screen.width <= win.x ||
        screen.y + (int32_t)screen.height <= win.y)
        return false;
    return true;
}

/* wayfire_surface_t implementation */
void surface_committed_cb(wl_listener*, void *data)
{
    auto surface = core->api->desktop_surfaces[(wlr_surface*) data];
    assert(surface);

    surface->commit();
}

void subsurface_created_cb(wl_listener*, void *data)
{
    auto sub = static_cast<wlr_subsurface*> (data);

    auto parent = core->api->desktop_surfaces[sub->parent];
    if (!parent)
    {
        log_error ("subsurface_created with invalid parent!");
        return;
    }

    if (core->api->desktop_surfaces.count(sub->surface))
    {
        log_error ("adding same subsurface twice!");
        return;
    }

    log_info("subsurface %p", sub->surface);
    new wayfire_surface_t(sub->surface, parent);
}

void surface_destroyed_cb(wl_listener*, void*);

wayfire_surface_t::wayfire_surface_t(wlr_surface *surface, wayfire_surface_t* parent)
{
    inc_keep_count();

    this->surface = surface;
    this->parent_surface = parent;

    /* map by default if this is a subsurface, only toplevels/popups have map/unmap events */
    if (surface->subsurface)
        is_mapped = true;

    if (parent)
    {
        set_output(parent->output);
        parent->surface_children.push_back(this);
    }

    new_sub.notify   = subsurface_created_cb;
    committed.notify = surface_committed_cb;
    destroy.notify   = surface_destroyed_cb;

    wl_signal_add(&surface->events.new_subsurface, &new_sub);
    wl_signal_add(&surface->events.commit,         &committed);
    wl_signal_add(&surface->events.destroy,        &destroy);

    log_info("map %p -> %p", surface, this);
    core->api->desktop_surfaces[surface] = this;
}

wayfire_surface_t::~wayfire_surface_t()
{
    log_info("unmap %p", this);
    core->api->desktop_surfaces.erase(surface);

    if (parent_surface)
    {
        auto it = parent_surface->surface_children.begin();
        while(it != parent_surface->surface_children.end())
        {
            if (*it == this)
                it = parent_surface->surface_children.erase(it);
            else
                ++it;
        }
    }

    for (auto c : surface_children)
        delete c;
}

wayfire_surface_t *wayfire_surface_t::get_main_surface()
{
    return this->parent_surface ? this->parent_surface->get_main_surface() : this;
}

void wayfire_surface_t::get_child_position(int &x, int &y)
{
    x = surface->current->subsurface_position.x;
    y = surface->current->subsurface_position.y;
}

wf_point wayfire_surface_t::get_output_position()
{
    auto pos = parent_surface->get_output_position();

    int dx, dy;
    get_child_position(dx, dy);
    pos.x += dx; pos.y += dy;

    return pos;
}

wf_geometry wayfire_surface_t::get_output_geometry()
{
    auto pos = get_output_position();

    return {
        pos.x, pos.y,
        surface->current ? surface->current->width : 0,
        surface->current ? surface->current->height : 0
    };
}

void wayfire_surface_t::map()
{
    is_mapped = true;
    damage();
}

void wayfire_surface_t::unmap()
{
    is_mapped = false;
    damage();
}

void wayfire_surface_t::damage(pixman_region32_t *region)
{
    int n_rect;
    auto rects = pixman_region32_rectangles(region, &n_rect);

    for (int i = 0; i < n_rect; i++)
        damage({rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1});
}

void wayfire_surface_t::damage(const wlr_box& box)
{
    assert(parent_surface);
    parent_surface->damage(box);
}

void wayfire_surface_t::damage()
{
    /* TODO: bounding box damage */
    damage(geometry);
}

void wayfire_surface_t::commit()
{

    wf_geometry rect = get_output_geometry();

    pixman_region32_t dmg;

    pixman_region32_init(&dmg);
    pixman_region32_copy(&dmg, &surface->current->surface_damage);

    pixman_region32_translate(&dmg, rect.x, rect.y);

    /* TODO: recursively damage children? */
    if (is_subsurface() && rect != geometry)
    {
        damage(geometry);
        damage(rect);

        geometry = rect;
    }

    /* TODO: transform damage */
    damage(&dmg);
}

void wayfire_surface_t::set_output(wayfire_output *out)
{
    output = out;
    for (auto c : surface_children)
        c->set_output(out);
}

void wayfire_surface_t::for_each_surface_recursive(wf_surface_iterator_callback call,
                                                   int x, int y, bool reverse)
{
    if (reverse)
    {
        call(this, x, y);

        int dx, dy;

        for (auto c : surface_children)
        {
            c->get_child_position(dx, dy);
            c->for_each_surface_recursive(call, x + dx, y + dy, reverse);
        }
    } else
    {
        auto it = surface_children.rbegin();
        int dx, dy;

        while(it != surface_children.rend())
        {
            auto& c = *it;
            c->get_child_position(dx, dy);
            c->for_each_surface_recursive(call, x + dx, y + dy, reverse);

            ++it;
        }

        call(this, x, y);
    }
}

void wayfire_surface_t::for_each_surface(wf_surface_iterator_callback call, bool reverse)
{
    auto pos = get_output_position();
    for_each_surface_recursive(call, pos.x, pos.y, reverse);
}

void wayfire_surface_t::render_fbo(int x, int y, int fb_w, int fb_h,
                                   pixman_region32_t *damage)
{
    if (!wlr_surface_has_buffer(surface))
        return;

    /* TODO: optimize, use offscreen_buffer.cached_damage */
    wlr_box fb_geometry;

    fb_geometry.x = x; fb_geometry.y = y;
    fb_geometry.width = surface->current->width;
    fb_geometry.height = surface->current->height;

    float id[9];
    wlr_matrix_projection(id, fb_w, fb_h, WL_OUTPUT_TRANSFORM_NORMAL);

    float matrix[9];
    wlr_matrix_project_box(matrix, &fb_geometry, WL_OUTPUT_TRANSFORM_NORMAL, 0, id);

    wlr_matrix_scale(matrix, 1.0 / fb_geometry.width, 1.0 / fb_geometry.height);

    wlr_renderer_scissor(core->renderer, NULL);
    wlr_render_texture(core->renderer, surface->texture, matrix, 0, 0, alpha);
}

static wlr_box get_scissor_box(wayfire_output *output, wlr_box *box)
{
    int ow, oh;
    wlr_output_transformed_resolution(output->handle, &ow, &oh);

    wlr_box result;
    memcpy(&result, box, sizeof(result));

    /* XXX: wlroots-git breaks this */
    // Scissor is in renderer coordinates, ie. upside down
    enum wl_output_transform transform = wlr_output_transform_compose(
        wlr_output_transform_invert(output->handle->transform),
        WL_OUTPUT_TRANSFORM_FLIPPED_180);

    wlr_box_transform(box, transform, ow, oh, &result);
    return result;
}

void wayfire_surface_t::render(int x, int y, wlr_box *damage)
{
    if (!wlr_surface_has_buffer(surface))
        return;

    wlr_box geometry;

    geometry.x = x * output->handle->scale;
    geometry.y = y * output->handle->scale;
    geometry.width = surface->current->width * output->handle->scale;
    geometry.height = surface->current->height * output->handle->scale;

    if (!damage) damage = &geometry;

    auto rr = core->renderer;
    float matrix[9];
    wlr_matrix_project_box(matrix, &geometry,
                           surface->current->transform,
                           0, output->handle->transform_matrix);

    auto box = get_scissor_box(output, damage);
    wlr_renderer_scissor(rr, &box);

    wlr_render_texture_with_matrix(rr, surface->texture, matrix, alpha);
}

void wayfire_surface_t::render_pixman(int x, int y, pixman_region32_t *damage)
{
    /* TODO: wlr_region_expand */
    pixman_region32_t scaled;
    pixman_region32_init(&scaled);
    pixman_region32_copy(&scaled, damage);

    wlr_region_scale(&scaled, &scaled, output->handle->scale);

    int n_rect;
    auto rects = pixman_region32_rectangles(&scaled, &n_rect);

    for (int i = 0; i < n_rect; i++)
    {
        wlr_box d;
        d.x = rects[i].x1;
        d.y = rects[i].y1;
        d.width = rects[i].x2 - rects[i].x1;
        d.height = rects[i].y2 - rects[i].y1;

        render(x, y, &d);
    }

    pixman_region32_fini(&scaled);
}

void wayfire_surface_t::render_fb(int x, int y, pixman_region32_t *damage, int fb)
{
    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb));
    render_pixman(x, y, damage);
}

/* wayfire_view_t implementation */
uint32_t _last_view_id = 0;
wayfire_view_t::wayfire_view_t(wlr_surface *surf)
    : wayfire_surface_t (surf, NULL), id(_last_view_id++)
{
    set_output(core->get_active_output());
    output->render->schedule_redraw();

    GetTuple(sw, sh, output->get_screen_size());
    log_info("it was %dx%d", sw, sh);

    surface = surf;

    geometry.x = geometry.y = 0;
    geometry.width = surface->current->width;
    geometry.height = surface->current->height;

    pixman_region32_init(&offscreen_buffer.cached_damage);
}

wayfire_view wayfire_view_t::self()
{
    return core->find_view(surface);
}

// TODO: implement is_visible
bool wayfire_view_t::is_visible()
{
    return true;
}

bool wayfire_view_t::update_size()
{
    int old_w = geometry.width, old_h = geometry.height;

    geometry.width = surface->current ? surface->current->width  : 0;
    geometry.height = surface->current? surface->current->height : 0;

    return geometry.width != old_w || geometry.height != old_h;
}

void wayfire_view_t::set_moving(bool moving)
{
    in_continuous_move += moving ? 1 : -1;
    if (decoration)
        decoration->set_moving(moving);
}

void wayfire_view_t::set_resizing(bool resizing)
{
    in_continuous_resize += resizing ? 1 : -1;
    if (decoration)
        decoration->set_resizing(resizing);
}

void wayfire_view_t::move(int x, int y, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = core->find_view(surface);
    data.old_geometry = get_wm_geometry();

    damage();
    geometry.x = x;
    geometry.y = y;
    damage();

    if (send_signal)
        output->emit_signal("view-geometry-changed", &data);
}

void wayfire_view_t::resize(int w, int h, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = core->find_view(surface);
    data.old_geometry = get_wm_geometry();

    damage();
    geometry.width = w;
    geometry.height = h;
    damage();

    if (send_signal)
        output->emit_signal("view-geometry-changed", &data);
}

wayfire_surface_t *wayfire_view_t::map_input_coordinates(int cx, int cy, int& sx, int& sy)
{
    wayfire_surface_t *ret = NULL;

    auto wm = get_wm_geometry();
    int center_x = wm.x + wm.width / 2;
    int center_y = wm.y + wm.height / 2;

    for_each_surface(
        [&] (wayfire_surface_t *surface, int x, int y)
        {
            if (ret) return;

            int lx = cx - center_x,
                ly = center_y - cy;

            if (transform)
            {
                auto transformed = transform->transformed_to_local_point({lx, ly});
                lx = transformed.x;
                ly = transformed.y;
            }

            lx = lx + center_x;
            ly = center_y - ly;

            sx = lx - x;
            sy = ly - y;

            if (wlr_surface_point_accepts_input(surface->surface, sx, sy))
                ret = surface;
        });

    return ret;
}

void wayfire_view_t::set_geometry(wf_geometry g)
{
    move(g.x, g.y, false);
    resize(g.width, g.height);
}

wf_geometry wayfire_view_t::get_bounding_box()
{
    if (!transform)
        return get_output_geometry();

    wlr_box wm = get_wm_geometry();
    wlr_box box = get_output_geometry();

    box.x = (box.x - wm.x) - wm.width / 2;
    box.y = wm.height / 2 - (box.y - wm.y);

    box = transform->get_bounding_box(box);

    box.x = box.x + wm.x + wm.width / 2;
    box.y = (wm.height / 2 - box.y) + wm.y;

    return box;
}

void wayfire_view_t::set_maximized(bool maxim)
{
    maximized = maxim;
}

void wayfire_view_t::set_fullscreen(bool full)
{
    fullscreen = full;
}

void wayfire_view_t::activate(bool active)
{ }

void wayfire_view_t::set_parent(wayfire_view parent)
{
    if (this->parent)
    {
        auto it = std::remove(this->parent->children.begin(), this->parent->children.end(), self());
        this->parent->children.erase(it);
    }

    this->parent = parent;
    if (parent)
    {
        auto it = std::find(parent->children.begin(), parent->children.end(), self());
        if (it == parent->children.end())
            parent->children.push_back(self());
    }
}

void wayfire_view_t::get_child_position(int &x, int &y)
{
    assert(decoration);

    x = decor_x;
    y = decor_y;
}

wf_point wayfire_view_t::get_output_position()
{
    return {geometry.x, geometry.y};
}

void wayfire_view_t::damage(const wlr_box& box)
{
    auto wm_geometry = get_wm_geometry();
    if (transform)
    {
        auto real_box = box;
        real_box.x -= wm_geometry.x;
        real_box.y -= wm_geometry.y;

        pixman_region32_union_rect(&offscreen_buffer.cached_damage,
                                   &offscreen_buffer.cached_damage,
                                   real_box.x, real_box.y,
                                   real_box.width, real_box.height);

        /* TODO: damage only the bounding box of region */
        output->render->damage(get_bounding_box());
    } else
    {
        output->render->damage(box);
    }
}

static wf_geometry get_output_centric_geometry(const wf_geometry& output, wf_geometry view)
{
    view.x = view.x - output.width / 2;
    view.y = output.height /2 - view.y;

    return view;
}

void wayfire_view_t::render_fb(int x, int y, pixman_region32_t* damage, int fb)
{
    if (!wlr_surface_has_buffer(surface))
        return;

    //log_info("render_pixman %p", surface);

    if (decoration && decoration->get_transformer())
        return;

    if (transform && !decoration)
    {
        auto output_geometry = get_output_geometry();

        if (output_geometry.width != offscreen_buffer.fb_width || output_geometry.height != offscreen_buffer.fb_height)
        {
            if (offscreen_buffer.fbo != (uint)-1)
            {
                glDeleteFramebuffers(1, &offscreen_buffer.fbo);
                glDeleteTextures(1, &offscreen_buffer.tex);

                offscreen_buffer.fbo = offscreen_buffer.tex = -1;
            }
        }

        if (offscreen_buffer.fbo == (uint32_t)-1)
        {
            pixman_region32_init(&offscreen_buffer.cached_damage);
            OpenGL::prepare_framebuffer_size(output_geometry.width, output_geometry.height,
                                             offscreen_buffer.fbo, offscreen_buffer.tex);

            offscreen_buffer.fb_width = output_geometry.width;
            offscreen_buffer.fb_height = output_geometry.height;

            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_buffer.fbo));
            GL_CALL(glViewport(0, 0, output_geometry.width, output_geometry.height));

            wlr_renderer_scissor(core->renderer, NULL);

            GL_CALL(glClearColor(1, 1, 1, 0));
            GL_CALL(glClear(GL_COLOR_BUFFER_BIT));
        }

        for_each_surface([=] (wayfire_surface_t *surface, int x, int y) {
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_buffer.fbo));
            GL_CALL(glViewport(0, 0, offscreen_buffer.fb_width, offscreen_buffer.fb_height));
            surface->render_fbo(x - output_geometry.x, y - output_geometry.y,
                                offscreen_buffer.fb_width, offscreen_buffer.fb_height,
                                NULL);
        }, true);

        auto og = output->get_full_geometry();

        auto obox = output_geometry;
        obox.x = x;
        obox.y = y;
        auto centric_geometry = get_output_centric_geometry(og, obox);

        int n_rect;
        auto rects = pixman_region32_rectangles(damage, &n_rect);

        for (int i = 0; i < n_rect; i++)
        {
            auto box = wlr_box{rects[i].x1, rects[i].y1,
                rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1};

            auto sbox = get_scissor_box(output, &box);
            transform->render_with_damage(offscreen_buffer.tex, fb, centric_geometry, sbox);
        }
    } else
    {
        wayfire_surface_t::render_fb(x, y, damage, fb);
    }
}

void wayfire_view_t::set_transformer(std::unique_ptr<wf_view_transformer_t> transformer)
{
    /* TODO: damage all */
    transform = std::move(transformer);
}

void wayfire_view_t::map()
{
    if (!is_special)
    {
        auto workarea = output->workspace->get_workarea();
        geometry.x += workarea.x;
        geometry.y += workarea.y;
    }

    if (update_size())
        damage();

    if (is_mapped)
    {
        log_error ("request to map %p twice!", surface);
        return;
    }

    is_mapped = true;

    /* TODO: consider not emitting a create-view for special surfaces */
    map_view_signal data;
    data.view = self();
    output->emit_signal("map-view", &data);


    if (!is_special)
    {
        output->attach_view(self());
        output->focus_view(self());
    }

    return;
}

void wayfire_view_t::unmap()
{
    wayfire_surface_t::unmap();

    auto old_output = output;
    output->detach_view(self());
    output = old_output;

    unmap_view_signal data;
    data.view = self();
    output->emit_signal("unmap-view", &data);
}


void wayfire_view_t::move_request()
{
    if (decoration)
        return decoration->move_request();

    move_request_signal data;
    data.view = self();
    output->emit_signal("move-request", &data);
}

void wayfire_view_t::resize_request()
{
    if (decoration)
        return decoration->resize_request();

    resize_request_signal data;
    data.view = self();
    output->emit_signal("resize-request", &data);
}

void wayfire_view_t::maximize_request(bool state)
{
    if (decoration)
        return decoration->maximize_request(state);

    if (maximized == state)
        return;

    view_maximized_signal data;
    data.view = self();
    data.state = state;

    if (is_mapped)
    {
        output->emit_signal("view-maximized-request", &data);
    } else if (state)
    {
        set_geometry(output->workspace->get_workarea());
        output->emit_signal("view-maximized", &data);
    }
}

void wayfire_view_t::fullscreen_request(wayfire_output *out, bool state)
{
    if (decoration)
        decoration->fullscreen_request(out, state);

    if (fullscreen == state)
        return;

    auto wo = (out ? out : (output ? output : core->get_active_output()));
    assert(wo);

    if (output != wo)
    {
        //auto pg = view->get_output()->get_full_geometry();
        //auto ng = wo->get_full_geometry();

        core->move_view_to_output(self(), wo);
        /* TODO: check if we really need global coordinates or just output-local */
       // view->move(view->geometry.x + ng.x - pg.x, view->geometry.y + ng.y - pg.y);
    }

    view_fullscreen_signal data;
    data.view = self();
    data.state = state;

    if (is_mapped) {
        wo->emit_signal("view-fullscreen-request", &data);
    } else if (state) {
        set_geometry(output->get_full_geometry());
        output->emit_signal("view-fullscreen", &data);
    }

    set_fullscreen(state);
}

/* xdg_shell_v6 implementation */
/* TODO: unmap */

static void handle_new_popup(wl_listener*, void*);
static void handle_v6_map(wl_listener*, void *data);
static void handle_v6_unmap(wl_listener*, void *data);

/* xdg_popup_v6 implementation
 * Currently we use a "hack": we treat the toplevel as a special popup,
 * so that we can use the same functions for adding a new popup, tracking them, etc. */

/* TODO: Figure out a way to animate this */
class wayfire_xdg6_popup : public wayfire_surface_t
{
    protected:
        wl_listener new_popup, destroy_popup,
                    m_popup_map, m_popup_unmap;

        wlr_xdg_popup_v6 *popup;
        wlr_xdg_surface_v6 *xdg_surface;

    public:
        wayfire_xdg6_popup(wlr_xdg_popup_v6 *popup)
            :wayfire_surface_t(popup->base->surface,
                               core->api->desktop_surfaces[popup->parent->surface])
        {
            assert(parent_surface);
            this->popup = popup;

            new_popup.notify     = handle_new_popup;
            m_popup_map.notify   = handle_v6_map;
            m_popup_unmap.notify = handle_v6_unmap;

            wl_signal_add(&popup->base->events.new_popup, &new_popup);
            wl_signal_add(&popup->base->events.map,       &m_popup_map);
            wl_signal_add(&popup->base->events.unmap,     &m_popup_unmap);
        }

        virtual void get_child_position(int &x, int &y)
        {
            double sx, sy;
            wlr_xdg_surface_v6_popup_get_position(popup->base, &sx, &sy);
            x = sx; y = sy;
        }

        virtual bool is_subsurface() { return true; }
};

void handle_new_popup(wl_listener*, void *data)
{
    auto popup = static_cast<wlr_xdg_popup_v6*> (data);
    auto it = core->api->desktop_surfaces.find(popup->parent->surface);
    if (it == core->api->desktop_surfaces.end())
    {
        log_error("attempting to create a popup with unknown parent");
        return;
    }

    new wayfire_xdg6_popup(popup);
}

static void handle_v6_map(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface_v6*> (data);
    auto wf_surface = core->api->desktop_surfaces[surface->surface];

    assert(wf_surface);
    wf_surface->map();
}

static void handle_v6_unmap(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface_v6*> (data);
    auto wf_surface = core->api->desktop_surfaces[surface->surface];

    assert(wf_surface);
    wf_surface->unmap();
}

static void handle_v6_request_move(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_v6_move_event*> (data);
    auto view = core->find_view(ev->surface->surface);

    view->move_request();
}

static void handle_v6_request_resize(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_v6_resize_event*> (data);
    auto view = core->find_view(ev->surface->surface);
    view->resize_request();
}

static void handle_v6_request_maximized(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface_v6*> (data);
    auto view = core->find_view(surf->surface);
    view->maximize_request(surf->toplevel->client_pending.maximized);
}

static void handle_v6_request_fullscreen(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_v6_set_fullscreen_event*> (data);
    auto view = core->find_view(ev->surface->surface);
    auto wo = core->get_output(ev->output);
    view->fullscreen_request(wo, ev->fullscreen);
}

class wayfire_xdg6_view : public wayfire_view_t
{
    protected:
    wlr_xdg_surface_v6 *v6_surface;
    wl_listener map, new_popup,
                request_move, request_resize,
                request_maximize, request_fullscreen;


    public:
    wayfire_xdg6_view(wlr_xdg_surface_v6 *s)
        : wayfire_view_t (s->surface), v6_surface(s)
    {
        log_info ("new xdg_shell_v6 surface: %s app-id: %s",
                  nonull(v6_surface->toplevel->title),
                  nonull(v6_surface->toplevel->app_id));

        new_popup.notify          = handle_new_popup;
        map.notify                = handle_v6_map;
        request_move.notify       = handle_v6_request_move;
        request_resize.notify     = handle_v6_request_resize;
        request_maximize.notify   = handle_v6_request_maximized;
        request_fullscreen.notify = handle_v6_request_fullscreen;

        wlr_xdg_surface_v6_ping(s);

        wl_signal_add(&s->events.new_popup,    &new_popup);
        wl_signal_add(&v6_surface->events.map, &map);
        wl_signal_add(&v6_surface->toplevel->events.request_move,       &request_move);
        wl_signal_add(&v6_surface->toplevel->events.request_resize,     &request_resize);
        wl_signal_add(&v6_surface->toplevel->events.request_maximize,   &request_maximize);
        wl_signal_add(&v6_surface->toplevel->events.request_fullscreen, &request_fullscreen);

        set_maximized(true);
    }

    virtual wf_point get_output_position()
    {
        wf_point position {
            geometry.x - v6_surface->geometry.x,
            geometry.y - v6_surface->geometry.y,
        };

        return position;
    }

    virtual wf_geometry get_output_geometry()
    {
        auto pos = get_output_position();
        return {
            pos.x,
            pos.y,
            surface->current ? surface->current->width : 0,
            surface->current ? surface->current->height : 0
        };
    }

    virtual bool update_size()
    {
        auto old_w = geometry.width, old_h = geometry.height;
        if (v6_surface->geometry.width > 0 && v6_surface->geometry.height > 0)
        {
            geometry.width = v6_surface->geometry.width;
            geometry.height = v6_surface->geometry.height;
        } else
        {
            wayfire_view_t::update_size();
        }

        return old_w != geometry.width || old_h != geometry.height;
    }

    virtual void activate(bool act)
    {
        wayfire_view_t::activate(act);
        wlr_xdg_toplevel_v6_set_activated(v6_surface, act);
    }

    virtual void set_maximized(bool max)
    {
        wayfire_view_t::set_maximized(max);
        wlr_xdg_toplevel_v6_set_maximized(v6_surface, max);
    }

    virtual void set_fullscreen(bool full)
    {
        wayfire_view_t::set_fullscreen(full);
        wlr_xdg_toplevel_v6_set_fullscreen(v6_surface, full);
    }

    virtual void move(int w, int h, bool send)
    {
        wayfire_view_t::move(w, h, send);
    }

    virtual void resize(int w, int h, bool send)
    {
        wayfire_view_t::resize(w, h, send);
        wlr_xdg_toplevel_v6_set_size(v6_surface, w, h);
    }

    std::string get_app_id()
    {
        return nonull(v6_surface->toplevel->app_id);
    }

    std::string get_title()
    {
        return nonull(v6_surface->toplevel->title);
    }

    virtual void close()
    {
        wlr_xdg_surface_v6_send_close(v6_surface);
    }

    ~wayfire_xdg6_view()
    {
    }
};

/* end of xdg_shell_v6 implementation */

/* start xdg6_decoration implementation */

void handle_decoration_destroyed(wl_listener*, void*);

class wayfire_xdg6_decoration_view : public wayfire_xdg6_view
{
    wayfire_view contained = NULL;
    std::unique_ptr<wf_decorator_frame_t> frame;

    wf_point v6_surface_offset;
    wl_listener destroyed_listener;

    public:

    wayfire_xdg6_decoration_view(wlr_xdg_surface_v6 *decor) :
        wayfire_xdg6_view(decor)
    {
        destroyed_listener.notify = handle_decoration_destroyed;
        wl_signal_add(&decor->events.destroy, &destroyed_listener);
    }

    void init(wayfire_view view, std::unique_ptr<wf_decorator_frame_t>&& fr)
    {
        frame = std::move(fr);
        contained = view;
        geometry = view->get_wm_geometry();

        set_geometry(geometry);
        surface_children.push_back(view.get());

        v6_surface_offset = {v6_surface->geometry.x, v6_surface->geometry.y};
    }

    void activate(bool state)
    {
        wayfire_xdg6_view::activate(state);
        contained->activate(state);
    }

    void commit()
    {
        wayfire_xdg6_view::commit();

        wf_point new_offset = {v6_surface->geometry.x, v6_surface->geometry.y};
        if (new_offset.x != v6_surface_offset.x || new_offset.y != v6_surface_offset.y)
        {
            move(geometry.x, geometry.y, false);
            v6_surface_offset = new_offset;
        }
    }

    void move(int x, int y, bool ss)
    {
        auto new_g = frame->get_child_geometry(geometry);
        new_g.x += v6_surface->geometry.x;
        new_g.y += v6_surface->geometry.y;

        log_info ("contained is moved to %d+%d, decor to %d+%d", new_g.x, new_g.y,
                  x, y);

        contained->decor_x = new_g.x - geometry.x;
        contained->decor_y = new_g.y - geometry.y;

        contained->move(new_g.x, new_g.y, false);
        wayfire_xdg6_view::move(x, y, ss);
    }

    void resize(int w, int h, bool ss)
    {
        auto new_geometry = geometry;
        new_geometry.width = w;
        new_geometry.height = h;

        auto new_g = frame->get_child_geometry(new_geometry);
        log_info ("contained is resized to %dx%d, decor to %dx%d", new_g.width, new_g.height,
                  w, h);

        contained->resize(new_g.width, new_g.height, false);
    }

    void child_configured(wf_geometry g)
    {
        auto new_g = frame->get_geometry_interior(g);
        log_info("contained configured %dx%d, we become: %dx%d",
                 g.width, g.height, new_g.width, new_g.height);
        if (new_g.width != geometry.width || new_g.height != geometry.height)
            wayfire_xdg6_view::resize(new_g.width, new_g.height, false);
    }

    void release_child()
    {
        if (!contained)
            return;

        log_info("release child");
        surface_children.clear();
        contained->set_decoration(nullptr, nullptr);

        if (!contained->destroyed)
            contained->close();

        contained = NULL;
    }

    wlr_surface *get_keyboard_focus_surface()
    { return contained->get_keyboard_focus_surface(); }

 //   void move_request() { contained->move_request(); }
  //  void resize_request() { contained->resize_request(); }
   // void maximize_request(bool state) { contained->maximize_request(state); }
   // void fullscreen_request(wayfire_output *wo, bool state)
    //{ contained->fullscreen_request(wo, state); }

    /* TODO: fullscreen ?
    void set_fullscreen(wayfire_output *wo, bool state)
    {
        _set_fullscreen(wo, state);
    }
    */

    ~wayfire_xdg6_decoration_view()
    { }
};

void handle_decoration_destroyed(wl_listener*, void* data)
{
    auto surf = static_cast<wlr_xdg_surface_v6*> (data);
    auto view = core->find_view(surf->surface);
    auto decor = std::dynamic_pointer_cast<wayfire_xdg6_decoration_view> (view);

    assert(decor);
    decor->release_child();
}

void wayfire_view_t::commit()
{
    wayfire_surface_t::commit();

    auto old = get_output_geometry();
    if (update_size())
    {
        if (output)
            output->render->damage(old);

        damage();
    }

    /* configure frame_interior */
    if (decoration)
    {
        auto decor = std::dynamic_pointer_cast<wayfire_xdg6_decoration_view> (decoration);
        assert(decor);
        decor->child_configured(geometry);
    }
}

void wayfire_view_t::damage()
{
    damage(get_bounding_box());
}

// TODO: do better
void surface_destroyed_cb(wl_listener*, void *data)
{
    auto surface = core->api->desktop_surfaces[(wlr_surface*) data];
    assert(surface);

    surface->destroyed = true;
    surface->dec_keep_count();
}

void wayfire_view_t::destruct()
{
    if (decoration)
    {
        auto decor = std::dynamic_pointer_cast<wayfire_xdg6_decoration_view> (decoration);
        assert(decor);

        decor->release_child();
        decor->close();
    }

    core->erase_view(self());
}

wayfire_view_t::~wayfire_view_t()
{
    pixman_region32_fini(&offscreen_buffer.cached_damage);
    for (auto& kv : custom_data)
        delete kv.second;
}

void wayfire_view_t::set_decoration(wayfire_view decor,
                                    std::unique_ptr<wf_decorator_frame_t> frame)
{
    if (decor)
    {
        auto raw_ptr = dynamic_cast<wayfire_xdg6_decoration_view*> (decor.get());
        assert(raw_ptr);

        if (output)
            output->detach_view(self());

        raw_ptr->init(self(), std::move(frame));
    }

    decoration = decor;
}

/* end xdg6_decoration_implementation */

void notify_v6_created(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface_v6*> (data);

    if (surf->role == WLR_XDG_SURFACE_V6_ROLE_TOPLEVEL)
    {
        if (surf->toplevel->title &&
            core->api->decorator &&
            core->api->decorator->is_decoration_window(surf->toplevel->title))
        {
            log_info("create wf decoration view");
            core->add_view(std::make_shared<wayfire_xdg6_decoration_view> (surf));
            auto view = core->find_view(surf->surface);

            core->api->decorator->decoration_ready(view);
        } else
        {
            core->add_view(std::make_shared<wayfire_xdg6_view> (surf));
        }
    }
}

/* xwayland implementation */
static void handle_xwayland_request_move(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_move_event*> (data);
    auto view = core->find_view(ev->surface->surface);
    view->move_request();
}

static void handle_xwayland_request_resize(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_resize_event*> (data);
    auto view = core->find_view(ev->surface->surface);
    view->resize_request();
}

static void handle_xwayland_request_configure(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_surface_configure_event*> (data);
    auto view = core->find_view(ev->surface->surface);
    view->set_geometry({ev->x, ev->y, ev->width, ev->height});
}

static void handle_xwayland_request_maximize(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xwayland_surface*> (data);
    auto view = core->find_view(surf->surface);
    view->maximize_request(surf->maximized_horz && surf->maximized_vert);
}

static void handle_xwayland_request_fullscreen(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xwayland_surface*> (data);
    auto view = core->find_view(surf->surface);
    view->fullscreen_request(view->get_output(), surf->fullscreen);
}

class wayfire_xwayland_view : public wayfire_view_t
{
    wlr_xwayland_surface *xw;
    wl_listener configure,
                request_move, request_resize,
                request_maximize, request_fullscreen;

    public:
    wayfire_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_view_t(xww->surface), xw(xww)
    {
        log_info("new xwayland surface %s class: %s instance: %s",
                 nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        configure.notify          = handle_xwayland_request_configure;
        request_move.notify       = handle_xwayland_request_move;
        request_resize.notify     = handle_xwayland_request_resize;
        request_maximize.notify   = handle_xwayland_request_maximize;
        request_fullscreen.notify = handle_xwayland_request_fullscreen;

        wl_signal_add(&xw->events.request_move,       &request_move);
        wl_signal_add(&xw->events.request_resize,     &request_resize);
        wl_signal_add(&xw->events.request_maximize,   &request_maximize);
        wl_signal_add(&xw->events.request_fullscreen, &request_fullscreen);
        wl_signal_add(&xw->events.request_configure,  &configure);
    }

    virtual void commit()
    {
        wayfire_view_t::commit();
        if (!is_mapped)
            map();
    }

    void activate(bool active)
    {
        wayfire_view_t::activate(active);
        wlr_xwayland_surface_activate(xw, active);
    }

    void send_configure()
    {
        wlr_xwayland_surface_configure(xw, geometry.x, geometry.y,
                                       geometry.width, geometry.height);
    }

    void move(int x, int y, bool s)
    {
        wayfire_view_t::move(x, y, s);
        send_configure();
    }

    void resize(int w, int h, bool s)
    {
        wayfire_view_t::resize(w, h, s);
        send_configure();
    }

    /* TODO: bad with decoration */
    void set_geometry(wf_geometry g)
    {
        damage();
        geometry = g;

        /* send the geometry-changed signal */
        resize(g.width, g.height, true);
        send_configure();
    }

    void close()
    {
        wlr_xwayland_surface_close(xw);
    }

    void set_maximized(bool maxim)
    {
        wayfire_view_t::set_maximized(maxim);
        wlr_xwayland_surface_set_maximized(xw, maxim);
    }

    std::string get_title() { return nonull(xw->title); }
    std::string get_app_id() {return nonull(xw->class_t); }

    void set_fullscreen(bool full)
    {
        wayfire_view_t::set_fullscreen(full);
        wlr_xwayland_surface_set_fullscreen(xw, full);
    }
};

void notify_xwayland_created(wl_listener *, void *data)
{
    core->add_view(std::make_shared<wayfire_xwayland_view> ((wlr_xwayland_surface*) data));
}
/* end of xwayland implementation */

void init_desktop_apis()
{
    core->api = new desktop_apis_t;

    core->api->v6_created.notify = notify_v6_created;
    core->api->v6 = wlr_xdg_shell_v6_create(core->display);
    wl_signal_add(&core->api->v6->events.new_surface, &core->api->v6_created);

    core->api->xwayland_created.notify = notify_xwayland_created;
    core->api->xwayland = wlr_xwayland_create(core->display, core->compositor);
    log_info("xwayland display started at%d", core->api->xwayland->display);
    wl_signal_add(&core->api->xwayland->events.new_surface, &core->api->xwayland_created);
}


