/*
 * The external OpenGL renderer (Harrison's library).  The library draws the
 * map and the cars its own way, with its own camera and textures, into the
 * host's OpenGL 3.3 core context; the game's own drawing (game_frame) is not
 * called, so what game_frame does besides drawing is done here: the engine
 * sound, the keys that change the game (Tab, Q, [, ], F9) and the text.
 *
 * The calls into the library are marked LIB: they are compiled only with
 * the library (make sdl GLR=1 ...).  What else it will need: TODO-lib.md.
 */
#include <string>
#include "imgui.h"
#include "game_renderer.hpp"
#include "gui.hpp"
#include "renderer_api.hpp"

#ifdef TEREP2_GLR
    #include <SDL_opengl.h>
    #include "renderizeitor/include/renderizeitor.h"
    static RzContext* ctx;

    struct carrodef_s{
        int32_t id;
        int16_t n_points;
        uint16_t camera_pt;
    };

    carrodef_s carros[5];
    int currTgt = 0;

    static void updateCars();
    static int isLine(uint16_t[], int);
    static void loadCar(int, std::string);
    static void updateSprites();
    static void updateCamDists();

    #define F_8_24(_val) ({ \
        const float scale = 1.0f / (float)(1 << 24); \
        float vv = _val;   \
        vv * scale;        \
    })

    #define F_UV(u, v, _raw) ({ \
        uint32_t uraw = (_raw); \
        float scale = 1.0f / 65535.0f;      \
        u = (float)(uraw & 0xFFFF) * scale; \
        v = (float)(uraw >> 16   ) * scale; \
    })

    extern int broke_some_ice;

// rzCreateWindow makes an OpenGL context of its own on the host's window and
// leaves it current, and rzDestroy deletes it.  The host (ImGui: the map
// chooser, the minimap) draws with its own context, so the library's is
// made current only around its calls, and the host's is always put back
// (otherwise, after Esc, nothing would reach the window: the chooser would
// not show and the library's last frame would stay on the screen).
static SDL_GLContext lib_gl;

static void use_lib() { SDL_GL_MakeCurrent(gui_window(), lib_gl); }

static void use_host()
{
    glFlush();                              // the library's drawing first, then the host's
    SDL_GL_MakeCurrent(gui_window(), gui_context());
}
#endif

static uint8_t banner_color;               // as game_frame's, which is not called here

bool opengl_start()
{
    banner_color = 0;
#ifdef TEREP2_GLR
    MapFiles files = map_files(".");        // real paths of the map's files
    MapLayers layers = map_layers();        // height and tile, 256 x 256, change while playing
    (void)files;
    (void)layers;

    auto w = gui_window();

    int ok = rzCreateWindow(w, &ctx);
    SDL_GL_SetSwapInterval(1);
    lib_gl = SDL_GL_GetCurrentContext();
    if(ok != RZ_OK){
        SDL_GL_MakeCurrent(w, gui_context());
        return false;
    }
    rzSetHeightmap(ctx, layers.height, 256,256);
    rzLoadTileAtlas(ctx, files.maptex.c_str());
    rzSetTileMap(ctx, layers.tile, 256,256);

    rzLoadFallbackTexture(ctx,files.textures.c_str());

    if (rzGetError(ctx, NULL) != RZ_OK){
        //TODO actually say the error
        rzDestroy(ctx);
        use_host();
        return false;
    }

    for(int i = 0; i < 5; i++){
        carros[i].id = -1;
        if(i >= world.num_cars){
            continue;
        }

        auto cpcx = files.cars[i].pcx;

        loadCar(i, cpcx);

    }

    currTgt = 0;
    rzSetCameraTarget(ctx, carros[0].id, carros[0].camera_pt);

    updateCars();

    rzSetBackgroundColor(ctx, 255);
    
    //TODO one more error checking here
    
    use_host();                             // only now: every rz* call above needs the library's context
    SDL_GL_SetSwapInterval(1);              // the library's context turns vsync off for the window
    return true;
#else
    return false;
#endif
}

// game_frame's text, in ImGui's font:
// at (x, y) of the original 320 x 200 screen, in palette colour colour.
static void draw_text(const PictureRect &at, const char *s, int x, int y, uint8_t colour)
{
    std::string text;
    for (; *s && *s != 13; s++)
        if (*s == 27 && s[1])
            s++;                            // a colour change: ignored
        else
            text += *s == 9 ? ' ' : *s;
    const float px = at.k * render_scale;   // window pixels per pixel of the original
    const uint8_t *c = host_palette(colour);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddText(nullptr, 7.5f * px, ImVec2(at.x0 + x * px, at.y0 + (y - 2) * px), IM_COL32(c[0], c[1], c[2], 255),
                text.c_str());
}

static void draw_texts(const PictureRect &at)
{
    draw_text(at, title_text, 10, 10, 15);
    draw_text(at, subtitle_text, 100, 190, 15);
    if (!extra_options.hide_banner)
        draw_text(at, "GAMBIARRA FOREVER!", 5, 190, (uint8_t)(banner_color >> 2));
    banner_color++;
}

// The keys game_frame would handle (keys[0] is the last key pressed); the
// cameras' keys are the library's business.
static void game_keys()
{
    switch (keys[0]) {
    case 0x0f:{                             // Tab: next car for view 1
        world.player_car[0]++;
        if (world.player_car[0] >= (uint16_t)world.num_cars){
            world.player_car[0] = 0;
        }
        world.player_car[1] = world.player_car[0];
        #ifdef TEREP2_GLR
        auto c = carros[world.player_car[0]];
        rzSetCameraTarget(ctx, c.id, c.camera_pt);
        currTgt = c.id;
        #endif
        break;
    }
    case 63:{ //F5: overview
        #ifdef TEREP2_GLR
        if(currTgt == -1){
            auto c = carros[world.player_car[0]];
            rzSetCameraTarget(ctx, c.id, c.camera_pt);
            currTgt = c.id;
        }else{
            rzSetCameraTarget(ctx, -1, -1);
            currTgt = -1;
        }
        #endif
        break;
    }
    case 0x1a:                              // [ and ]: gravity
        world.gravity += 50;
        break;
    case 0x1b:
        world.gravity -= 50;
        break;
    }
    keys[0] = 0;
}

bool opengl_frame(int win_w, int win_h, PictureRect *where)
{
    #ifdef TEREP2_GLR
        use_lib();                          // game_keys calls the library too
    #endif
    game_keys();
    engine_sound(world.player_car[0], 0);      // what draw_view does for the sound

    #ifdef TEREP2_GLR
        if(broke_some_ice){
            rzUpdateTerrain(ctx);
            broke_some_ice = 0;
        }
        
        updateCars();
        updateSprites();
        updateCamDists();
        rzRender(ctx);
        use_host();
    #endif
    
    // LIB: draw the frame (cars[i].points, map_layers(), ...) on the whole window,
    // into framebuffer 0, without swapping
    *where = { win_w - screen_w() * ((float)win_h / screen_h()), 0, (float)win_h / screen_h() };
    draw_texts({ 0, 0, where->k });        // from the left edge, as the lib fills the window
    return true;
}

void opengl_stop()
{
#ifdef TEREP2_GLR
    use_lib();
    rzDestroy(ctx);
    for(int i = 0; i < 5; i++){
        carros[i].id = -1;
    }
    use_host();
#endif
}

#ifdef TEREP2_GLR
static void updateCars(){
    for(int ci = 0; ci< 5 ; ci++){
        int id = carros[ci].id;
        if(id < 0){
            continue;
        }

        int n = carros[ci].n_points;
        auto car = car_get(ci);
        carros[ci].camera_pt = car.ref_index;

        RzVertex ptrs[n];

        for(int i =0; i< n; i++){
            auto p = car.points[i].pos;

            //prevents underflow related graphical glitches,
            //most notable when the car gets upside down in a low part of the map
            auto z = p.z.raw < 0xfe000000 ? p.z.raw : 0;

            ptrs[i].x = F_8_24(p.x.raw);
            ptrs[i].y = F_8_24(p.y.raw);
            ptrs[i].z = F_8_24(z);
        }

        //TODO review this constant, the limits are 45 deg
        float steering = (float)car.steer.raw * -0.0001f;
        rzUpdateObjectWheels(ctx, id, steering);

        float spd = F_8_24(car.wheel_speed[0]);
        rzSetObjectWheelSpin(ctx, id, spd);

        rzUpdateObjectVertices(ctx, id, ptrs);
    }
}

static void loadCar(int i, std::string cpcx){
    auto car = car_get(i);
    carros[i].n_points = car.points.size();

    int idCar;
    rzCreateObject(ctx, carros[i].n_points, &idCar);
    carros[i].id = idCar;

    if(cpcx != ""){
        rzLoadObjectTexture(ctx, idCar, cpcx.c_str());
    }

    int wi = 0;

    for (const auto &o : car.model_ops){
        if(o.op == 4){ //flat polygon
            auto color = o.word & 0xff;
            auto pts = o.outline;

            int n = pts.size();
            uint16_t idxs[n];
            int flag = 0;

            for(int i = 0; i < n; i++){
                int v = pts[i].point;
                flag |= v & 1;
                idxs[i] = v >> 1;
            }

            if(flag){
                //shadow polygon, we dont need that
                continue;
            }

            if(isLine(idxs, n)){
                rzAddObjectLine(ctx, idCar, idxs[0], idxs[1], 0.006f, color);
                continue;
            }

            if(color == 240){
                //TODO handle all the different tones
                rzAddObjectTranslucentPolygon(ctx, idCar, idxs, n, 8);
            }else{
                rzAddObjectPolygon(ctx, idCar, idxs, n, color);
            }

            continue;
        }
        if(o.op == 8){ //OP_TEXTURED
            auto pts = o.outline;

            int n = pts.size();

            RzTexVertex vtx[n];

            for(int i =0; i < n;i++){
                auto v = pts[i];
                uint32_t attr = v.attr;
                
                vtx[i].index = v.point >> 1;
                F_UV(vtx[i].u, vtx[i].v, attr);                
            }
            
            rzAddObjectTexturedPolygon(ctx, idCar, vtx, n);
            continue;
        }
        if(o.op == 10){
            //will be actually handled below
            wi++;
            continue;
        }

        printf("unhandled: %d\n", o.op);
    }

    if(wi == 4){
        RzWheel wee[4] = {0};

        int widx = 0;
        RzWheelFace f, b;
        for (const auto &w : car.model_ops){
            if(w.op != 10){
                continue;
            }
            auto pi = w.p[0] >> 1;
            auto p = car_get(i).points[pi];

            wee[widx].hubVertex = pi;
            wee[widx].front = p.wheel != 1;
            wee[widx].diameter = (float)p.radius / 150.0f;

            if(widx == 0){
                F_UV(f.u0, f.v0, w.views[0].uv[0]);
                F_UV(f.u1, f.v1, w.views[0].uv[2]);
                
                F_UV(b.u0, b.v0, w.views[8].uv[0]);
                F_UV(b.u1, b.v1, w.views[8].uv[2]);
            }
            
            widx++;
        }

        rzSetObjectWheels(ctx, idCar, wee);
        rzSetObjectWheelFaces(ctx, idCar, &f, &b);
    }else{
        printf("WAT? %d wheels? HOW?\n", wi);
    }
}

static int isLine(uint16_t vertx[], int n){
    //checks if its a polygon with only 2 distinct vertices
    uint8_t z[256] = {0};
    for(int i = 0;i < n;i++){
        auto v = vertx[i];
        z[v] = 1;
    }
    int cnt = 0;
    for(size_t i = 0;i < sizeof(z);i++){
        cnt += z[i];
    }
    return cnt == 2;
}

static void updateSprites(){
    auto n_sprites = world.particle_count;

    RzSprite spr[256];

    for(int i = 0; i < n_sprites && i < 256; i++){
        auto s = world.particles[i];
        auto ds = &spr[i];
        
        ds[0] = {
            .x = F_8_24(s.pos.x.raw),
            .y = F_8_24(s.pos.y.raw),
            .z = F_8_24(s.pos.z.raw),
        };

        //the game always sets the lifetime to 0x64 (100 in decimal)
        //so we make the size inversely proportional to the lifetime, to make the particle expand
        //110 so they begin visible and not 0 size
        int sz = 110 - s.life;

        if(smoke_quad_of_kind[(uint8_t)s.kind >> 4] != 0){ // not the smoke puff
            ds->type = RZ_SPRITE_DEBRIS;
            ds->opacity = 1.0f;
            ds->color = s.kind;
            if(s.life < 32){
                float o = (float)s.life * 0.03125f;
                ds->opacity = o;
            }
        } else {
            ds->type = RZ_SPRITE_SMOKE;
            ds->opacity = 0.8f;
            if(s.life < 50){
                float o = (float)s.life * 0.016f;
                ds->opacity = o;
            }
        }

        ds->size = (float)sz * 0.005f;
        ds->z += ds->size * 0.3f; //add a small amount to z proportional to the size, so the dont spawn buried
    }

    rzSetSprites(ctx,spr,n_sprites);
}

static void updateCamDists(){
    static float camD = 3.0f;
    static float camH = 1.0f;

    if((keys[78] & 0x80) == 0){//plus
        camH += 0.04f;
        camH = fminf(camH, 12.0f);
    }else if((keys[74] & 0x80) == 0){ //minus
        camH -= 0.04f;
        camH = fmaxf(camH, 0.5f);
    }else if((keys[53] & 0x80) == 0){  // /
        camD += 0.04f;
        camD = fminf(camD, 10.0f);
    }else if((keys[55] & 0x80) == 0){ //asterisk
        camD -= 0.04f;
        camD = fmaxf(camD, 0.5f);
    } else{
        return;
    }

    rzSetCameraFollow(ctx, camD, camH, 0.08f);
}

#endif