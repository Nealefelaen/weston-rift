/*
 * Copyright © 2014 Chameleon
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _POSTCOMPOSITOR_RIFT_H_
#define _POSTCOMPOSITOR_RIFT_H_

#include <EGL/egl.h>

#include "compositor.h"

void
show_error_(const char *file, int line);

int
config_rift(struct weston_compositor *compositor);
//config_rift(struct weston_compositor *compositor, EGLConfig egl_config, EGLDisplay egl_display, EGLSurface orig_surface, EGLContext egl_context);

int
setup_rift(struct weston_compositor *compositor);

int
render_rift(struct weston_compositor *compositor, GLuint original_program);

#endif
