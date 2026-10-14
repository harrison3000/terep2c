# TODO: renderizador OpenGL externo (a lib)

O que falta para desenhar o jogo com a lib OpenGL, em vez do renderizador por software. O que já
existe no host está em `host/renderer_api.hpp` (contexto, arquivos do mapa, camadas do mapa).

## Contexto e quadro

- [x] Contexto OpenGL 3.3 core único, do host (`host/gui.cpp`): janela `SDL_WINDOW_OPENGL`,
      vsync, ImGui pelo backend OpenGL3 (`#version 330 core`).
- [ ] A lib aceita a janela e o contexto do host (`gui_window()`, `gui_context()`) em vez de
      criar os seus, desenha no framebuffer 0 e **não** chama `SDL_GL_SwapWindow` (o host desenha
      o ImGui por cima e faz o swap em `gui_end()`).
- [x] Hoje o `rzCreateWindow` cria um contexto próprio na janela do host e o deixa corrente, e o
      `rzDestroy` o apaga: depois do Esc não sobrava contexto corrente, e o seletor não aparecia
      (ficava a última imagem da lib). O `host/renderer_gl.cpp` agora guarda o contexto da lib e
      só o deixa corrente em volta das chamadas dela (`use_lib()` / `use_host()`, com `glFlush`
      entre os dois), voltando sempre ao do host; e o host pede `SDL_GL_DEPTH_SIZE` 24. Toda
      chamada nova `rz*` precisa ficar entre `use_lib()` e `use_host()`. Alternativa melhor, na
      lib: usar o contexto que já estiver corrente, ou ao menos devolver o que estava corrente
      ao sair de `rzCreateWindow`/`rzDestroy`.
- [ ] No fim do quadro a lib deixa o framebuffer 0 ligado. O backend OpenGL3 do ImGui salva e
      restaura o resto do estado que usa.
- [ ] Core profile: nada de pipeline fixo (`glBegin`, matrizes do GL); funções além do GL 1.1
      precisam de loader. O ImGui tem o loader interno dele, só para ele; a lib traz o seu
      (glad, epoxy...) ou carrega pelo `SDL_GL_GetProcAddress`.
- [x] Encaixe: `host/renderer_gl.cpp` (`opengl_start`, `opengl_frame`, `opengl_stop`), chamado
      pelo `run()` entre `gui_begin()` e `gui_end()`; as chamadas da lib vão nas linhas `LIB:`.
      O `gui_begin()` limpa a tela, então o que a lib desenha não é apagado.
- [x] Escolher o renderizador: combo "Renderer" no seletor e `-r software|opengl`; o padrão é o
      por software. O `opengl` só existe com `make sdl GLR=1 GLR_DIR=... GLR_LIB=...`.
- [ ] Trocar as linhas `LIB:` pelas chamadas reais: `#include` do header, init com
      `gui_window()`/`gui_context()`/`map_files(".")`/`map_layers()`, o draw a cada quadro, o
      shutdown.
- [ ] Alternativas, se a lib não puder dividir o contexto: dois contextos na mesma janela com
      `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` e `SDL_GL_MakeCurrent` + `glFlush` a cada troca, ou a
      lib desenhando num FBO que o host mostra como textura.

## Estado do jogo que a lib lê

- [x] Arquivos do mapa com os nomes reais (`map_files()`): map, col, maptex, textures, fog,
      smoke e, por carro carregado, `carN.dat` e `carN.pcx` (vazio se o carro usa textures.pcx).
- [x] Camadas do mapa (`map_layers()`): altura e tile, 256 x 256, em `[x | y << 8]`.
- [ ] O mapa muda durante o jogo: o gelo lascado baixa a altura de um ponto e o nibble baixo do
      tile. Reler as camadas a cada quadro (64 KB cada) ou comparar e atualizar só o que mudou.
- [ ] Carros: `cars[i].points` (posição 8.24 em células / 16.16 em unidades do mapa, mudam a
      cada tick: corpo mole, VBO dinâmico); `cars[i].model_ops` (polígonos decodificados: pontos
      em `G->car_proj`, cor, uv, sprites, rodas). Com z-buffer, SORT2/FACING/JUMP_* (ordem do
      pintor) podem ser ignorados; manter o descarte de faces de costas. `car.steer_used` para
      virar as rodas da frente; `car.ref_vertex()` é o ponto da câmera F1.
- [ ] Partículas: `G->particles` (`G->particle_bytes / sizeof(Particle)`), fumaça, faíscas e
      lascas de gelo, desenhadas como sprites (`G->smoke_sprite`).
- [ ] Texto: título, subtítulo e a linha "GAMBIARRA FOREVER!" (`hide-banner`), se quiser; pode
      ser pelo ImGui.

## O que o `game_frame()` faz e deixa de ser feito

- [x] Som: `opengl_frame` chama `engine_sound(G->player_car[0], 0)` a cada quadro de 60 Hz (e
      o `[1], 1` com a tela dividida), como o original.
- [x] Teclas que mudam o jogo, tratadas pelo host no modo OpenGL: Tab / Q (`G->player_car`),
      `[` / `]` (gravidade), F9 (tela dividida); o `keys[0]` é zerado ali.
- [ ] F1–F6, F10 e o numpad (câmeras e neblina do original): a lib decide o que fazer.

## Build

- [x] `Makefile`: `make sdl GLR=1 GLR_DIR=<include> GLR_LIB="<.a e o que ele precisar>"` (mudar
      essas variáveis recompila o que elas afetam, pelo `build/sdl/flags`; funciona com `-j`). Sem `GLR`, o `terep2vibed`
      compila sem dependência nova (`GL_LIBS`: `-lGL` no Linux, `-lopengl32` no Windows).
