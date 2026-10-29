#ifndef CRTEMU_OSD_H
#define CRTEMU_OSD_H

/* crt-emu in-game panel (OpenGL renderer only).
 *
 * keyosd (default F12) opens a Dear ImGui window drawn on top of the shaded frame. It edits the
 * "#pragma parameter" values of the active shader live: the shader file is compiled with
 * PARAMETER_UNIFORM defined, so the parameters are uniforms that the renderer sets every frame.
 * "Save" writes the values back into the #define / #pragma lines of the same file, so the file
 * keeps working in stock cnc-ddraw, ShaderGlass and RetroArch.
 *
 * Optional "//@..." comment lines in the shader add Russian labels, groups, hints, option lists
 * for enum-like parameters and presets (see the crt-emu build.py).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* set while the panel is shown; read by the game thread (input) and the render thread */
extern volatile long g_crtemu_osd_visible;

/* render thread: parse parameters of <shader_path>.pass1 (or <shader_path> itself).
   Keeps the live values when the same file is loaded again after a renderer restart. */
void crtemu_osd_load(const char* shader_path);

/* opengl_utils: TRUE if this shader file holds the parameters (compile it with PARAMETER_UNIFORM) */
int crtemu_osd_is_param_file(const char* file_path);

/* parameter table for the renderer, which sets the uniforms itself */
int crtemu_osd_param_count(void);
const char* crtemu_osd_param_name(int i);
float crtemu_osd_param_value(int i);

/* render thread, GL context current, default framebuffer bound: draw the panel if visible */
void crtemu_osd_render(void* hwnd, int width, int height);

/* render thread: the GL context is about to be destroyed (renderer restart / exit) */
void crtemu_osd_gl_shutdown(void);

/* game thread (keyboard hook): show / hide the panel. No-op unless the OpenGL renderer loaded
   a shader with parameters. */
void crtemu_osd_toggle(void* hwnd);

#ifdef __cplusplus
}
#endif

#endif
