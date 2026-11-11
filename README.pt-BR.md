# LEGO Dimensions PC

[English](README.md) | **Português**

Port nativo **não oficial** do LEGO Dimensions do PS4 (CUSA01176 / CUSA00935, versão 01.24) para Windows x64.

Não é um emulador de PS4. O executável do jogo roda direto no processador do PC, e as bibliotecas do sistema do PS4 foram reescritas para o Windows. Os gráficos do PS4 (GCN) são traduzidos para Vulkan, e os shaders do jogo são traduzidos antes de jogar, a partir dos próprios pacotes do jogo.

> **Este repositório não contém nada do jogo.** Para jogar você precisa do **seu próprio dump** do LEGO Dimensions do PS4 (e, se quiser, das suas DLCs). Nada de jogo, DLC, eboot, módulos do sistema ou saves é distribuído aqui.

## Download

Baixe o instalador (`LEGO_Dimensions_PC_Setup.exe`) ou o zip portátil na página de **[Releases](../../releases)**.

## O que você precisa

- Windows 10 (1803 ou mais novo) ou Windows 11, 64 bits.
- Placa de vídeo com **Vulkan 1.3** e driver atualizado. Testado em AMD Radeon RX 6600.
- **16 GB de RAM**. O jogo usa uns 5 GB; com 8 GB pode funcionar, mas com travadas.
- O dump do jogo: a pasta `CUSA01176` ou `CUSA00935` (com `eboot.bin`, `sce_sys` e `data\chunk*`), **versão 01.24**. Duas estruturas funcionam:
  1. tudo junto: a atualização já dentro do dump (`data\chunk1` com `PATCH.DAT`, `PATCH0.DAT`...);
  2. jogo base + atualização numa pasta separada, `CUSA01176-patch` ou `CUSA00935-patch` (formato do shadPS4), dentro da pasta do jogo ou ao lado dela.
- Opcional: DLCs (uma subpasta por DLC, no formato da pasta `addcont` do shadPS4), controle DS4/Xbox e o app LegoToypad para o portal.
- Jogo, atualização ou DLCs em arquivos `.pkg`: o botão **Extrair PKG...** do launcher extrai no formato acima e preenche as pastas (extração de PKG do shadPS4 0.7.0).

## Como usar

1. Instale (ou extraia o zip) e abra o **LegoDimensionsLauncher.exe**.
2. Escolha a pasta do jogo. A pasta da atualização separada é achada sozinha. Escolha também a das DLCs, se tiver.
3. Clique em **Preparar** (só na primeira vez). Isso converte o `eboot.bin` do seu dump e traduz todos os shaders do jogo para Vulkan. Leva de 20 a 60 minutos.
4. Clique em **JOGAR**.
5. Depois, **Atualizar** no launcher baixa e instala as versões novas das releases do GitHub (a partir da 1.3).

Detalhes e dicas no [LEIA-ME](tools/dist/LEIA-ME.txt).

## Estado

- Dá para jogar a história, Vorton e as fases com DLCs, a 60 fps (opção "60 FPS" no launcher).
- Áudio, saves, controle e portal (o Toy Pad de verdade no USB, o app LegoToypad ou um Toy Pad virtual) funcionam.
- Co-op local: até 4 controles; o jogador 2 entra em tela dividida apertando Options.
- Teclado e botões do mouse: cada tecla pode ser trocada em **Configurações > Controles** (os botões do mouse também valem), e dá para escolher se o teclado controla o jogador 1 ou o jogador 2. O teclado é um jeito a mais de controlar esse jogador; a ordem dos controles não muda. Com o teclado no jogador 2, o jogador 2 existe mesmo sem segundo controle.
- Ícones dos botões conforme o controle (Xbox 360, Xbox One/Series, PS3, PS4/PS5, Wii U para controles da Nintendo), montados a partir dos arquivos do próprio jogo; o launcher também deixa fixar um conjunto.
- **LD PC Transfer** (botão no launcher): converte o progresso entre o port, PS4 / shadPS4, Xbox 360 (Xenia), Wii U (Cemu) e PS3 (RPCS3, experimental). O save original não é alterado e o que o destino já tinha vai para a pasta `saves_backup`. Xbox One não dá: os saves dele ficam na nuvem, criptografados.
- **Contador de FPS e info** (opção do launcher, F10 no jogo): FPS, gráfico do tempo de cada quadro, temperatura / uso / clock / consumo / memória da placa de vídeo (AMD pelo ADL, NVIDIA pelo NVML), uso da CPU e RAM no canto de cima, à esquerda.
- **Apelidos dos jogadores** (launcher): os nomes que o jogo mostra no lugar de Player1, Player2...
- **Mods** (botão no launcher): funcionam os mods de PS4 / shadPS4 do LEGO Dimensions - pacotes `.HDR` + `.DAT` (ex. feitos com o BrickVault), pacotes do jogo editados como os `PATCH.HDR` + `PATCH.DAT` dos mods de shadPS4 (entram só os arquivos que mudaram) e arquivos soltos no formato da pasta do jogo. Instale de um `.zip`, `.rar` ou `.7z` e ligue / desligue cada um; os que já estiverem na pasta `CUSA01176-mods` do shadPS4 ou em `CUSA01176\Mods` também entram. Os arquivos do jogo nunca são alterados. Os pacotes de mod com nome próprio entram pelo [Anomaly](https://github.com/connorh315/anomaly), o carregador de mods do LEGO Dimensions de PS4 de connorh315, embutido no port (menu Mods, ligado por padrão). Falas de mod com hashes de evento de som em branco ou sem o sufixo do idioma (ex. o mod de voz do Bart, de connorh315) são consertadas no cache, então tocam com legenda, e o texto do jogo continua valendo com mods grandes.
- **Figuras desligadas**: figuras que o jogo tem mas deixa desligadas (ex. Lord Vortech, figura 73) são liberadas quando uma pasta de mod tem a gravação `.bin` da figura; o personagem é montado no cache com os arquivos do próprio jogo do jogador.
- O app LegoToypad (versão 1.9 Web App Extended inclusa): o launcher tem a opção "Abrir com LegoToypad" para abrir o emulador do portal junto com o jogo, e botão no topo para abrir a qualquer momento.
- **Criar Atalho**: botão no launcher para criar atalhos na Área de Trabalho e no Menu Iniciar.
- Resolução padrão 1080p (1920x1080) e aplicação imediata de configurações (resolução, ícones, idioma) sem precisar clicar em Preparar novamente.
- Tela cheia (opção "Tela cheia" no launcher ou F11 no jogo) e seletor do tamanho da janela ("Resolução"; o jogo continua desenhando em 1920x1080 e é escalado para a janela).
- Opção "Pular todas as cutscenes": qualquer cutscene pode ser pulada, mesmo as que o jogo só deixa pular depois de vistas ou com 100%.
- O controle só mexe no jogo com a janela do jogo na frente: ao clicar no app do Toy Pad, o personagem para.
- Launcher, instalador e telas do port em português ou inglês, conforme o idioma do Windows. O seletor **Idioma** do launcher escolhe na mão (português, inglês, espanhol, francês, alemão, italiano, holandês, dinamarquês); os idiomas fora português e inglês precisam dos textos originais do jogo (um dump com tradução de fã no `text.csv` fecha o jogo nesses idiomas).
- Na primeira visita a uma área nova o driver de vídeo ainda compila pipelines. Isso pode dar um engasgo curto, mas os objetos não somem (sem pop-in).
- Cenas muito pesadas (~6000 draws por quadro) ficam abaixo de 60 fps em processadores de 6 núcleos.
- Algumas cenas abertas (por exemplo a tomada aérea de Metropolis) ficam com muito mais névoa que no PS4. Em investigação.
- Só foi testado em placa AMD. Relatos com NVIDIA e Intel são bem-vindos (abra uma *issue* com o log de `build\logs\`).

## Compilar a partir do código

Requer:
- Visual Studio 2022 ou mais novo, com as ferramentas C++ e o **clang-cl**;
- CMake;
- Os cabeçalhos Vulkan estão incluídos. Um [Vulkan SDK](https://vulkan.lunarg.com/) instalado é usado quando disponível; é opcional para compilar o port e continua necessário para o tradutor de shaders abaixo;
- Python 3.

```bat
build.bat                    :: o port -> build\lego_dimensions.exe
tools\dist\build_dist.bat    :: launcher e instalador
```

**Tradutor de shaders** (`tools/shader_compiler_tool`): usa o recompilador de shaders do [shadPS4](https://github.com/shadps4-emu/shadPS4), sem modificações, no commit [`eeecd10`](https://github.com/shadps4-emu/shadPS4/commit/eeecd103f648572b249b422e759e76799274b34c). A única troca é `override/resource_patching_pass.cpp`, que fica nesta pasta.
Instale o Vulkan SDK 1.4 e defina `VULKAN_SDK` para compilar esse tradutor separado.
1. Compile esse commit do shadPS4 (Release, clang-cl).
2. Aponte `SHADPS4_DIR` para ele.
3. Rode `tools\shader_compiler_tool\build.bat`.

**Pacote de distribuição:** `python tools/dist/montar.py` gera o instalador e o zip em `dist\`.

## Licença

GPL-2.0. Veja [LICENSE](LICENSE). As bibliotecas de terceiros e suas licenças estão em [TERCEIROS.md](TERCEIROS.md).

## Aviso

Projeto de fã, sem fins lucrativos. Não tem ligação com o LEGO Group, a TT Games, a Warner Bros. Games ou a Sony Interactive Entertainment. LEGO e LEGO Dimensions são marcas do LEGO Group. PlayStation é marca da Sony. Use só com uma cópia do jogo que você possui.
