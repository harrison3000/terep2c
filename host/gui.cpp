/*
 * The window and its OpenGL context: OpenGL 3.3 core profile (what the
 * external renderer uses; the ImGui OpenGL3 backend loads its own
 * functions).  The host itself only calls OpenGL 1.1 functions, which every
 * OpenGL library exports, so it needs no loader of its own.
 */
#include <cstdio>
#include <SDL_opengl.h>
#include "gui.hpp"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

static SDL_Window *window;
static SDL_GLContext context;

SDL_Window *gui_open(int w, int h)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);     // the external renderer needs it
    window = SDL_CreateWindow("Terep2 vibe code edition", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window || !(context = SDL_GL_CreateContext(window))) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return nullptr;
    }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;       // nothing written to the map directories
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    return window;
}

void gui_close()
{
    if (!context)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    context = nullptr;
    window = nullptr;
}

SDL_Window *gui_window() { return window; }
SDL_GLContext gui_context() { return context; }

void gui_event(const SDL_Event &e)
{
    ImGui_ImplSDL2_ProcessEvent(&e);
}

void gui_begin()
{
    int w, h;                                   // cleared here: a renderer draws before gui_end
    SDL_GL_GetDrawableSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void gui_end()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
}

GuiTexture::~GuiTexture()
{
    if (tex)
        glDeleteTextures(1, &tex);
}

void GuiTexture::upload(const uint32_t *pixels, int w, int h, bool smooth)
{
    if (!tex)
        glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != tw || h != th) {
        GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        tw = w;
        th = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
}
