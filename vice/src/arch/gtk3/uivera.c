/** \file   uivera.c
 * \brief   Window of the VERA video card (c64/cart/vera.c)
 *
 * The VERA has its own VGA output: its 640x480 picture is shown in a window
 * of its own, beside the one of the VIC-II. The emulation thread hands over
 * each complete VERA frame (vera_set_ui_callbacks); this file passes it
 * to the UI thread through three buffers and draws it scaled to the window
 * with the 4:3 aspect kept: through OpenGL (a texture in a GtkGLArea), or
 * through cairo on Windows and where OpenGL does not start. Drawing with
 * cairo every frame goes through the X server: on a slow machine that
 * costs more than the whole VERA emulation.
 *
 * \author  Gianluca Renzi <gianlucarenzi@eurek.it>
 */

/*
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#ifndef _WIN32
#define VERA_UI_OPENGL
#include <GL/glew.h>
#endif

#include <gtk/gtk.h>
#include <string.h>

#include "vera.h"
#include "uivera.h"

#define VERA_WIDTH  640
#define VERA_HEIGHT 480

static GtkWidget *vera_window = NULL;
static GtkWidget *vera_area = NULL;

/* the last frame, written by the emulation thread, read by the UI thread */
static GMutex frame_lock;
/* three frames: the emulation thread fills frames[frame_write], on_draw()
 * paints frames[frame_draw]; frame_ready is swapped with either under the
 * lock, so neither thread copies or waits for the other */
static uint32_t frames[3][VERA_WIDTH * VERA_HEIGHT];
static int frame_write = 0;
static int frame_ready = 1;
static int frame_draw = 2;
static int frame_new = 0;              /* frames[frame_ready] not painted yet */
static int frame_redraw_pending = 0;   /* an idle redraw is queued */
static int card_enabled = 0;           /* the card is in: the window is shown */

/* UI thread: the newest complete frame becomes frames[frame_draw] */
static void take_ready_frame(void)
{
    g_mutex_lock(&frame_lock);
    if (frame_new) {
        int t = frame_draw;
        frame_draw = frame_ready;
        frame_ready = t;
        frame_new = 0;
    }
    g_mutex_unlock(&frame_lock);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    cairo_surface_t *surface;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    double scale, ox, oy;

    scale = (double)w / VERA_WIDTH;
    if ((double)h / VERA_HEIGHT < scale) {
        scale = (double)h / VERA_HEIGHT;
    }
    ox = (w - VERA_WIDTH * scale) / 2.0;
    oy = (h - VERA_HEIGHT * scale) / 2.0;

    /* black bars around the 4:3 picture */
    if (ox > 0.0 || oy > 0.0) {
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
    }

    take_ready_frame();
    surface = cairo_image_surface_create_for_data((unsigned char *)frames[frame_draw], CAIRO_FORMAT_RGB24,
                                                  VERA_WIDTH, VERA_HEIGHT, VERA_WIDTH * 4);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    /* whole pixels when the window is a multiple of 640x480 */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(surface);
    return TRUE;
}

#ifdef VERA_UI_OPENGL
static GLuint gl_program = 0;
static GLuint gl_vao = 0;
static GLuint gl_texture = 0;

/* a quad over the viewport from 4 vertices, the frame as a texture */
static const char *gl_vertex_src =
    "#version 150\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *gl_fragment_src =
    "#version 150\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D frame;\n"
    "void main() {\n"
    "    color = vec4(texture(frame, uv).rgb, 1.0);\n"
    "}\n";

static GLuint gl_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void create_cairo_area(void);

/* OpenGL did not start: the cairo area takes the place of the GtkGLArea */
static gboolean fall_back_to_cairo(gpointer data)
{
    gtk_widget_destroy(vera_area);
    create_cairo_area();
    gtk_widget_show(vera_area);
    return G_SOURCE_REMOVE;
}

static void on_gl_realize(GtkGLArea *area, gpointer data)
{
    GLuint vs, fs;
    GLint ok = GL_FALSE;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) {
        g_idle_add(fall_back_to_cairo, NULL);
        return;
    }
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK && glCreateShader == NULL) {
        g_idle_add(fall_back_to_cairo, NULL);
        return;
    }

    vs = gl_shader(GL_VERTEX_SHADER, gl_vertex_src);
    fs = gl_shader(GL_FRAGMENT_SHADER, gl_fragment_src);
    if (vs != 0 && fs != 0) {
        gl_program = glCreateProgram();
        glAttachShader(gl_program, vs);
        glAttachShader(gl_program, fs);
        glLinkProgram(gl_program);
        glGetProgramiv(gl_program, GL_LINK_STATUS, &ok);
    }
    if (vs != 0) {
        glDeleteShader(vs);
    }
    if (fs != 0) {
        glDeleteShader(fs);
    }
    if (!ok) {
        if (gl_program != 0) {
            glDeleteProgram(gl_program);
            gl_program = 0;
        }
        g_idle_add(fall_back_to_cairo, NULL);
        return;
    }

    glGenVertexArrays(1, &gl_vao);
    glGenTextures(1, &gl_texture);
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    /* whole pixels when the window is a multiple of 640x480 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* 0x00RRGGBB words, as for CAIRO_FORMAT_RGB24 */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VERA_WIDTH, VERA_HEIGHT, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frames[frame_draw]);
}

static void on_gl_unrealize(GtkGLArea *area, gpointer data)
{
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) {
        return;
    }
    if (gl_texture != 0) {
        glDeleteTextures(1, &gl_texture);
        gl_texture = 0;
    }
    if (gl_vao != 0) {
        glDeleteVertexArrays(1, &gl_vao);
        gl_vao = 0;
    }
    if (gl_program != 0) {
        glDeleteProgram(gl_program);
        gl_program = 0;
    }
}

static gboolean on_gl_render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    GtkWidget *widget = GTK_WIDGET(area);
    int sf = gtk_widget_get_scale_factor(widget);
    int w = gtk_widget_get_allocated_width(widget) * sf;
    int h = gtk_widget_get_allocated_height(widget) * sf;
    int vw, vh;

    if (gl_program == 0) {
        return TRUE;
    }

    /* black bars around the 4:3 picture */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (w * VERA_HEIGHT > h * VERA_WIDTH) {
        vh = h;
        vw = h * VERA_WIDTH / VERA_HEIGHT;
    } else {
        vw = w;
        vh = w * VERA_HEIGHT / VERA_WIDTH;
    }
    glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);

    take_ready_frame();
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VERA_WIDTH, VERA_HEIGHT,
                    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frames[frame_draw]);
    glUseProgram(gl_program);
    glBindVertexArray(gl_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    return TRUE;
}
#endif

static void create_cairo_area(void)
{
    vera_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(vera_area, VERA_WIDTH / 2, VERA_HEIGHT / 2);
    gtk_container_add(GTK_CONTAINER(vera_window), vera_area);
    g_signal_connect(vera_area, "draw", G_CALLBACK(on_draw), NULL);
}

/* closing the window only hides it: the card is still in */
static gboolean on_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    gtk_widget_hide(widget);
    return TRUE;
}

static void create_window(void)
{
    vera_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(vera_window), "VERA [C64]");
    gtk_window_set_default_size(GTK_WINDOW(vera_window), VERA_WIDTH, VERA_HEIGHT);
#ifdef VERA_UI_OPENGL
    vera_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(vera_area), 3, 2);
    gtk_gl_area_set_has_alpha(GTK_GL_AREA(vera_area), FALSE);
    gtk_widget_set_size_request(vera_area, VERA_WIDTH / 2, VERA_HEIGHT / 2);
    gtk_container_add(GTK_CONTAINER(vera_window), vera_area);
    g_signal_connect(vera_area, "realize", G_CALLBACK(on_gl_realize), NULL);
    g_signal_connect(vera_area, "unrealize", G_CALLBACK(on_gl_unrealize), NULL);
    g_signal_connect(vera_area, "render", G_CALLBACK(on_gl_render), NULL);
#else
    create_cairo_area();
#endif
    g_signal_connect(vera_window, "delete-event", G_CALLBACK(on_delete), NULL);
}

/* UI thread: the window follows the card */
static gboolean update_visibility(gpointer data)
{
    if (card_enabled) {
        if (vera_window == NULL) {
            create_window();
        }
        gtk_widget_show_all(vera_window);
    } else if (vera_window != NULL) {
        gtk_widget_hide(vera_window);
    }
    return G_SOURCE_REMOVE;
}

/* UI thread: draw the frame that came in */
static gboolean redraw(gpointer data)
{
    g_mutex_lock(&frame_lock);
    frame_redraw_pending = 0;
    g_mutex_unlock(&frame_lock);
    if (vera_area != NULL && gtk_widget_get_visible(vera_window)) {
        gtk_widget_queue_draw(vera_area);
    }
    return G_SOURCE_REMOVE;
}

/* emulation thread: a complete VERA frame */
static void vera_frame(const uint32_t *fb, int width, int height)
{
    int queue;

    if (width != VERA_WIDTH || height != VERA_HEIGHT) {
        return;
    }
    memcpy(frames[frame_write], fb, sizeof(frames[0]));
    g_mutex_lock(&frame_lock);
    {
        int t = frame_write;
        frame_write = frame_ready;
        frame_ready = t;
    }
    frame_new = 1;
    queue = !frame_redraw_pending;
    frame_redraw_pending = 1;
    g_mutex_unlock(&frame_lock);
    if (queue) {
        g_idle_add(redraw, NULL);
    }
}

/* any thread: the card was inserted or removed */
static void vera_enabled(int enabled)
{
    card_enabled = enabled;
    g_idle_add(update_visibility, NULL);
}

void uivera_init(void)
{
    g_mutex_init(&frame_lock);
    vera_set_ui_callbacks(vera_frame, vera_enabled);
}
