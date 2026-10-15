# TODO

- [x] Parar de carregar o `data.bin`: colocar as constantes direto no código e inicializar o
      resto dinamicamente.
- [x] Parar de carregar os `carN.dat` direto na memória: interpretar o arquivo original e
      carregar de forma inteligente, para que as structs não precisem mais ser `packed`.
- [x] Tipos `Angle` (0x10000 = 360°) e `Q15` (seno/cosseno, matrizes), como `Pos`/`Vec3`, no
      lugar de `uint16_t`/`int16_t` soltos (`build_matrix`, câmeras, rodas).
- [x] Quebrar o `DataSeg` em estruturas por assunto: `world`, `camera`, `render`, `map_tables`,
      `contact` (terep.hpp, state.cpp); o que era só rascunho virou local ou parâmetro.
- [ ] `make test` mais completo: mais gravações do Win16 (tests/roteiro-gravacao.md), teste
      rápido em 4x com ASan/UBSan, build com g++ e clang; CI no GitHub Actions.
- [x] Corrigir o bug do pulo que corrompe o título (o carro procura o ponto mais baixo como
      num aperto novo). Os bugs do renderizador ficam como estão.

O renderizador OpenGL externo tem um TODO próprio: [TODO-lib.md](TODO-lib.md).

Fora do escopo daqui: widescreen (só no renderizador novo).
