#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Last Guardian launcher (Tkinter, Linux and Windows), in pt_BR and en_US.

Game folder, game and interface language, display, FPS overlay, VRAM budget and native audio;
Play starts the game through tlg_run.py (the same code path as run_tlg.sh) and shows its log.
Settings live in ~/.config/tlg-launcher/settings.json (%APPDATA% on Windows).
"""
import locale
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backports_table  # noqa: E402
import logtail  # noqa: E402
import pad_page  # noqa: E402
import preset_share  # noqa: E402
import tlg_run  # noqa: E402
import savemanager  # noqa: E402,F401  (puts scripts/ on sys.path)
import soundtrack  # noqa: E402
import practice_save  # noqa: E402
import start_points  # noqa: E402

# Colors: warm dark with one gold accent (the home screen fades the art into BG).
BG, PANEL, FIELD, LINE = '#16140f', '#201d17', '#0f0e0b', '#3a352b'
TEXT_COLOR, MUTED, DIM, ACCENT = '#efe9dd', '#a9a394', '#8a8476', '#e2b878'
SERIF = 'Georgia' if sys.platform == 'win32' else 'serif'
HOME_SIZE = (1100, 620)
BACKGROUND_NAMES = ('launcher_background.png', 'launcher_background.jpg')

TEXT = {
    'title': ('The Last Guardian — PC', 'The Last Guardian — PC'),
    'home_settings': ('Settings', 'Configurações'),
    'home_graphics': ('Graphics', 'Gráficos'),
    'home_advanced': ('Advanced', 'Avançado'),
    'home_controller': ('Controller', 'Controle'),
    'home_keys': ('Keys & mouse', 'Teclas e mouse'),
    'home_mods': ('Mods & cheats', 'Mods e cheats'),
    'home_video': ('Video', 'Vídeo'),
    'home_controls': ('Controls', 'Controles'),
    'mouse_section': ('Mouse', 'Mouse'),
    'memory': ('Video memory', 'Memória de vídeo'),
    'display': ('Display', 'Tela'),
    'launcher_section': ('Launcher', 'Launcher'),
    'scene_section': ('Display, shadows and textures', 'Tela, sombras e texturas'),
    'home_logs': ('Debug', 'Depuração'),
    'tab_log': ('Log', 'Log'),
    'tab_bench': ('Benchmark', 'Benchmark'),
    'home_soundtrack': ('Soundtrack', 'Trilha sonora'),
    'soundtrack_hint': ("All the game's music, read from your own game folder. The game has no track titles: each piece is "
                        "named by the stage it plays in, with the game's own working name where it has one. The music is "
                        "mixed in surround: pick Stereo for two speakers or headphones, 5.1 surround for a surround set.",
                        "Toda a música do jogo, lida da sua própria pasta do jogo. O jogo não tem títulos de faixas: cada peça "
                        "leva o nome da fase em que toca, com o nome de trabalho do próprio jogo quando existe. A música é "
                        "mixada em surround: escolha Estéreo para duas caixas ou fones, Surround 5.1 para um sistema surround."),
    'soundtrack_piece': ('Piece {n}', 'Peça {n}'),
    'soundtrack_length': ('Length', 'Duração'),
    'soundtrack_play': ('Play', 'Tocar'),
    'soundtrack_pause': ('Pause', 'Pausar'),
    'soundtrack_next': ('Next', 'Próxima'),
    'soundtrack_stop': ('Stop', 'Parar'),
    'soundtrack_volume': ('Volume', 'Volume'),
    'soundtrack_speakers': ('Speakers', 'Caixas de som'),
    'soundtrack_stereo': ('Stereo', 'Estéreo'),
    'soundtrack_surround': ('5.1 surround', 'Surround 5.1'),
    'soundtrack_loading': ('Reading the music from the game folder…', 'Lendo a música da pasta do jogo…'),
    'soundtrack_count': ('{count} pieces, {minutes} minutes', '{count} peças, {minutes} minutos'),
    'soundtrack_none': ('No music found. Set the game folder in Settings first.',
                        'Nenhuma música encontrada. Defina primeiro a pasta do jogo nas Configurações.'),
    'soundtrack_failed': ('This piece could not be played.', 'Esta peça não pôde ser tocada.'),
    'home_start': ('Start at…', 'Começar em…'),
    'start_title': ('Start at a point', 'Começar num ponto'),
    'start_hint': ("Starts the game at the chosen start point: it continues your save, then jumps there.\n\nYour saves are copied before the game starts and put back when it closes, so nothing from this run is kept. If the launcher or the PC stops first, the next launcher start puts them back.",
                   "Inicia o jogo no ponto escolhido: continua o seu save e depois salta para lá.\n\nOs seus saves são copiados antes do jogo iniciar e devolvidos quando ele fecha; nada desta sessão fica. Se o launcher ou o PC parar antes, o próximo início do launcher devolve os saves."),
    'start_stage': ('Stage', 'Fase'),
    'start_point': ('Start point', 'Ponto de início'),
    'start_go': ('Start here', 'Começar aqui'),
    'start_note': ('Practice run: your saves are put back when the game closes. Use Play for normal play.',
                   'Sessão de treino: os seus saves voltam quando o jogo fecha. Use Jogar para jogar normalmente.'),
    'start_no_save': ('Needs a save to continue from. Play normally until the game has saved once, then come back.',
                      'Precisa de um save para continuar. Jogue normalmente até o jogo salvar uma vez e volte.'),
    'start_starting': ('Practice start at {point}: saves copied.', 'Início de treino em {point}: saves copiados.'),
    'start_restored': ('Your saves from before the practice start are back.',
                       'Os seus saves de antes do início de treino voltaram.'),
    'start_restore_failed': ('Could not put your saves back yet ({error}). They are safe in {path}; the '
                             'launcher tries again at its next start.',
                             'Ainda não foi possível devolver os saves ({error}). Eles estão seguros em {path}; '
                             'o launcher tenta de novo no próximo início.'),
    'home_quit': ('Quit', 'Sair'),
    'legacy_menu': ('Switch to legacy menu', 'Mudar para o menu antigo'),
    'home_back': ('‹  Back', '‹  Voltar'),
    'home_picture': ('Background picture…', 'Imagem de fundo…'),
    'home_picture_reset': ('Default picture', 'Imagem padrão'),
    'home_cache': ('Clear shader cache', 'Limpar cache de shaders'),
    'copy_command_tip': ('Copies the command that starts the game with the current settings, without this '
                         'launcher (for use in, for instance, Steam: Add a Non-Steam Game).',
                         'Copia o comando que inicia o jogo com as configurações atuais, sem este launcher '
                         '(para usar, por exemplo, no Steam: Adicionar jogo não-Steam).'),
    'copy_command': ('Start command copied: paste it in Steam (Add a Non-Steam Game > Properties > Target).',
                     'Comando de início copiado: cole no Steam (Adicionar jogo não-Steam > Propriedades > Destino).'),
    'home_music_on': ('♪ Music on', '♪ Música ligada'),
    'home_music_off': ('♪ Music off', '♪ Música desligada'),
    'home_running': ('Running', 'Rodando'),
    'game': ('Game', 'Jogo'),
    'game_dir': ('Game folder', 'Pasta do jogo'),
    'system_lib_dir': ('Console system modules', 'Módulos do sistema do console'),
    'browse': ('Browse…', 'Procurar…'),
    'texpack_dir': ('Texture packs folder (empty = default)', 'Pasta dos pacotes de texturas (vazio = padrão)'),
    'texture_pack': ('Texture pack', 'Pacote de texturas'),
    'texture_pack_off': ('Off (original textures)', 'Desligado (texturas originais)'),
    'texture_pack_hint': ('Default: off. Packs are folders with an index.txt in the texture packs folder; '
                          'make one with ps4-texturepack-generator (docs/texture-pack.md). Takes effect at the next start '
                          '(switch live in the in-game menu, Image tab).',
                          'Padrão: desligado. Pacotes são pastas com um index.txt na pasta dos pacotes; '
                          'crie um com ps4-texturepack-generator (docs/texture-pack.md). Vale no próximo início '
                          '(troque ao vivo no menu do jogo, aba Imagem).'),
    'language': ('Language', 'Idioma'),
    'game_language': ('Game language (voice and text)', 'Idioma do jogo (voz e texto)'),
    'ui_language': ('Interface language', 'Idioma da interface'),
    'auto': ('Automatic (system)', 'Automático (sistema)'),
    'video': ('Video', 'Vídeo'),
    'image': ('Image', 'Imagem'),
    'display_mode': ('Display mode', 'Modo de exibição'),
    'Windowed': ('Windowed', 'Janela'),
    'Borderless': ('Borderless window', 'Janela sem borda'),
    'Fullscreen': ('Fullscreen', 'Tela cheia'),
    'vsync': ('VSync', 'VSync'),
    'frame_rate': ('Frame rate (experimental)', 'Taxa de quadros (experimental)'),
    'resolution': ('Resolution', 'Resolução'),
    'resolution_hint': ('Rendered natively; wider than 16:9 shows more of the scene and moves the HUD out to the edges',
                        'Renderizado nativamente; mais largo que 16:9 mostra mais da cena e leva o HUD às bordas'),
    'fps_30': ('30 FPS (as on PS4)', '30 FPS (como no PS4)'),
    'fps_60': ('60 FPS', '60 FPS'),
    'fps_unlocked': ('Unlocked (up to the display)', 'Livre (até a taxa do monitor)'),
    'fps_hint': ('60/unlocked patch eboot 1.03 in memory. Unlocked lowers the base frame rate by about '
                 '10 FPS against 60 at the same spot; pick 60 unless your PC runs well above 60',
                 '60/livre alteram o eboot 1.03 na memória. Livre reduz a taxa base em cerca de 10 FPS '
                 'em relação a 60 no mesmo lugar; escolha 60, a não ser que seu PC rode bem acima de 60'),
    'skip_logos': ('Skip the boot logos', 'Pular os logotipos de abertura'),
    'debug': ('Debug (at start)', 'Depuração (ao iniciar)'),
    'bench_title': ('Benchmark', 'Benchmark'),
    'bench_text': ('Measures how fast the game runs on this PC, scene by scene, all by itself: each scene is '
                   'loaded from a clean save, the camera stands still, and the frame rate of every second is '
                   'recorded. Your own saves are set aside first and put back afterwards.\n\n'
                   'Hands off while it runs: the game opens full screen for every scene, about a minute each. '
                   'The result is a single .zip report file — attach it to a GitHub issue so the developers can '
                   'see exactly how each scene runs on your PC.',
                   'Mede a velocidade do jogo neste PC, cena por cena, sozinho: cada cena é carregada de um '
                   'save limpo, a câmera fica parada e a taxa de quadros de cada segundo é registrada. Seus '
                   'próprios saves são guardados antes e devolvidos depois.\n\n'
                   'Não mexa enquanto roda: o jogo abre em tela cheia para cada cena, cerca de um minuto cada. '
                   'O resultado é um único arquivo .zip — anexe-o a uma issue no GitHub para os desenvolvedores '
                   'verem exatamente como cada cena roda no seu PC.'),
    'bench_full': ('All scenes ({count})', 'Todas as cenas ({count})'),
    'bench_pick': ('Pick scenes from the list', 'Escolher cenas da lista'),
    'bench_custom': ('Custom list (paste)', 'Lista própria (colar)'),
    'bench_custom_hint': ('One scene per line, as stage:start point. The list to run usually comes from a '
                          'developer on GitHub; the lines below are examples.',
                          'Uma cena por linha, como fase:ponto de início. A lista geralmente vem de um '
                          'desenvolvedor no GitHub; as linhas abaixo são exemplos.'),
    'bench_seconds': ('Seconds measured per scene', 'Segundos medidos por cena'),
    'bench_estimate': ('Estimated time: about {minutes} min for {count} scenes',
                       'Tempo estimado: cerca de {minutes} min para {count} cenas'),
    'bench_pilot': ('Let Trico drive (Tricopilot)', 'Deixar o Trico dirigir (Tricopiloto)'),
    'bench_pilot_help': (
        "Every scene is measured with Trico walking around instead of standing still, so the "
        "numbers include his animation, his physics and the camera following him. Tricopilot is the "
        "developers' own debug tool, still in the game: they left the game running by itself for "
        "hours with it. Keep it off to compare with other people's runs, and say it was on when you "
        "share a run with it (the report notes it too).",
        'Cada cena é medida com o Trico andando em vez de parado, então os números incluem a '
        'animação dele, a física e a câmera que o segue. O Tricopiloto é a ferramenta de depuração '
        'dos próprios desenvolvedores, ainda presente no jogo. Deixe desligado para comparar com as '
        'medições de outras pessoas (o relatório também anota isso).'),
    'bench_pilot_cam': ('Camera', 'Câmera'),
    'bench_cam_near': ('Near', 'Perto'),
    'bench_cam_middle': ('Middle', 'Médio'),
    'bench_cam_far': ('Far', 'Longe'),
    'bench_cam_game': ('Game camera', 'Câmera do jogo'),
    'bench_pilot_cam_help': (
        "Which camera watches Trico while he drives. Near keeps him big in the picture and is the "
        "best of the four in open scenes. Middle is the one the developers' tool picks itself: a "
        "classic top-down view, but in a narrow or roofed place the camera can end up inside the "
        "rock. Far looks down on the whole area, lovely in a valley and useless indoors, because "
        "Trico becomes a dot. Game camera leaves the normal camera alone, so it keeps following the "
        "boy while Trico wanders off. It only changes what you see, not what is measured.",
        'Qual câmera acompanha o Trico enquanto ele dirige. Perto o mantém grande na tela e é a '
        'melhor das quatro em cenas abertas. Médio é a que a ferramenta dos desenvolvedores escolhe '
        'sozinha: uma vista de cima clássica, mas num lugar estreito ou coberto a câmera pode '
        'acabar dentro da rocha. Longe mostra a área inteira, bonito num vale e inútil em '
        'interiores. Câmera do jogo não mexe na câmera normal, que continua seguindo o menino. '
        'Só muda o que você vê, não o que é medido.'),
    'bench_start': ('Start', 'Iniciar'),
    'bench_stop': ('Stop', 'Parar'),
    'bench_open_folder': ('Open the report folder', 'Abrir a pasta do relatório'),
    'bench_running': ('Scene {index} of {total}: {point}', 'Cena {index} de {total}: {point}'),
    'bench_done': ('Done. Report: {name}', 'Concluído. Relatório: {name}'),
    'bench_stopped': ('Stopped. A partial report was still written.',
                      'Parado. Um relatório parcial foi gravado mesmo assim.'),
    'bench_bad_points': ('Not in the scene list: {points}', 'Fora da lista de cenas: {points}'),
    'bench_no_points': ('The custom list is empty.', 'A lista própria está vazia.'),
    'bench_game_running': ('Close the game first.', 'Feche o jogo primeiro.'),
    'bench_busy': ('A benchmark is running.', 'Um benchmark está em andamento.'),
    'level_select': ('Level select and QA menu', 'Seleção de fases e menu de QA'),
    'debug_logging': ('Advanced logging (for bug reports)', 'Registro avançado (para relatar erros)'),
    'debug_logging_hint': ('Can cost some FPS and makes the log bigger. Turn it on, make the problem happen, close the game, '
                           'send the newest file from the Logs folder, then turn it off again.',
                           'Pode reduzir um pouco o FPS e deixa o log maior. Ligue, faça o problema acontecer, feche o jogo, '
                           'envie o arquivo mais recente da Pasta de logs e depois desligue.'),
    'debug_hint': ('Level select: hold L1 while the title menu closes (Continue/New Game). QA menu: in game, hold L1 and press L3. '
                   'GoldHEN patches by illusionyy and DanielSvoboda.',
                   'Seleção de fases: segure L1 enquanto o menu inicial fecha (Continuar/Novo jogo). Menu de QA: no jogo, segure L1 e aperte L3. '
                   'Patches GoldHEN de illusionyy e DanielSvoboda.'),
    'show_fps': ('Show FPS overlay at start (F10 toggles)', 'Mostrar overlay de FPS ao iniciar (F10 alterna)'),
    'vram': ('Texture cache budget (MB of VRAM)', 'Orçamento do cache de texturas (MB de VRAM)'),
    'vram_hint': ('7000 suits every card with 8 GB or more: keep it there, also on a big card. A low budget throws textures away all the time; higher is not faster: the cache is only cleaned when it gets full, '
                  'so a high budget lets old textures pile up and slows the game down over a long session. '
                  'Only lower it if your card has less than 8 GB of VRAM (6 GB: 5600, 4 GB: 3700). This is video card memory (VRAM), not RAM.',
                  '7000 serve para toda placa com 8 GB ou mais: deixe assim, também numa placa grande. Um orçamento baixo descarta texturas o tempo todo; mais alto não é mais rápido: o cache só é limpo quando enche, '
                  'então um orçamento alto deixa texturas antigas se acumularem e deixa o jogo mais lento numa sessão longa. '
                  'Só diminua se a sua placa tiver menos de 8 GB de VRAM (6 GB: 5600, 4 GB: 3700). É a memória da placa de vídeo (VRAM), não a RAM.'),
    'texture_cleanup': ('Clean up textures after each load',
                        'Limpar texturas após cada carregamento'),
    'arena_release': ('Debug: give back freed buffer memory', 'Debug: devolver memória de buffer liberada'),
    'arena_release_hint': ('Since v0.5.2: when the game frees memory, its graphics card copy goes too. Test switch (issue #38): turn it off to check whether a problem started with v0.5.2. Off is the behaviour before v0.5.2. From the Shadow of the Colossus PC port (@AjncSystem, @Oussama268). Also in the in-game menu (Insert → Performance).', 'Desde a v0.5.2: quando o jogo libera memória, a cópia na placa de vídeo vai junto. Chave de teste (issue #38): desligue para ver se um problema começou na v0.5.2. Desligado é o comportamento antes da v0.5.2. Do port de Shadow of the Colossus para PC (@AjncSystem, @Oussama268). Também no menu do jogo (Insert → Desempenho).'),
    'gc_live_budget': ('Debug: texture cleanup follows the driver', 'Debug: limpeza de texturas segue o driver'),
    'gc_live_budget_hint': ('Since v0.5.2: texture cleanup starts at the video memory the driver offers now, never below the start-up limits. Test switch (issue #38): off uses only the start-up limits (the behaviour before v0.5.2), so textures are cleaned up sooner. Also in the in-game menu (Insert → Performance).', 'Desde a v0.5.2: a limpeza de texturas começa pela memória de vídeo que o driver oferece agora, nunca abaixo dos limites do início. Chave de teste (issue #38): desligado usa só os limites do início (o comportamento antes da v0.5.2), então as texturas são limpas mais cedo. Também no menu do jogo (Insert → Desempenho).'),
    'keep_gpu_textures': ('Keep GPU-only textures', 'Manter texturas só na GPU'),
    'keep_gpu_textures_hint': ('Keeps a texture the GPU drew into while its only correct copy is on the GPU (a game write right next to it blocks the copy back), until video memory is almost full. Without it, cleanup can throw that picture away and the game gets old data back. From SpartanPort (@maze1377). On by default; turn it off if video memory runs full more often. Also in the in-game menu (Insert → Performance).',
                               'Mantém uma textura desenhada pela GPU enquanto a única cópia correta está na GPU (uma escrita do jogo logo ao lado impede a cópia de volta), até a memória de vídeo quase encher. Sem isso, a limpeza pode descartar essa imagem e o jogo recebe dados antigos. Do SpartanPort (@maze1377). Ligado por padrão; desligue se a memória de vídeo encher com mais frequência. Também no menu do jogo (Insert → Desempenho).'),
    'texture_cleanup_hint': ('Frees the textures of earlier stages all at once after each stage or checkpoint load. Not needed any more: since the texture cache '
                             'cleanup was fixed (issue #104), old textures no longer pile up. It can cause a short stutter after a load, '
                             'so it is off by default. Also in the in-game menu (Insert → Performance).',
                             'Libera de uma vez as texturas de fases anteriores após cada carregamento de fase ou de checkpoint. Não é mais necessário: desde que a limpeza '
                             'do cache de texturas foi corrigida (issue #104), texturas antigas não se acumulam mais. Pode causar um breve travamento '
                             'após um carregamento, então vem desligado. Também no menu do jogo (Insert → Desempenho).'),
    'filters': ('Image filters (live in the in-game menu too, Insert → Image)',
                'Filtros de imagem (também no menu do jogo, Insert → Imagem)'),
    'smaa': ('SMAA anti-aliasing (smooths edges; the HUD too)', 'Anti-aliasing SMAA (suaviza bordas; o HUD também)'),
    'aa_mode': ('Anti-aliasing (F8 in game)', 'Anti-aliasing (F8 no jogo)'),
    'aa_off': ('Off', 'Desligado'),
    'aa_dlaa': ('DLAA (NVIDIA RTX)', 'DLAA (NVIDIA RTX)'),
    'aa_fsr3': ('FSR 3.1 Native AA', 'FSR 3.1 AA nativo'),
    'aa_fsr4': ('FSR 4 Native AA', 'FSR 4 AA nativo'),
    'fg_mode': ('Frame generation', 'Geração de quadros'),
    'fg_hint': ('One made-up image between two game frames: smoother, a little more input delay. DLSS needs NVIDIA RTX 40 or newer; FSR is not tied to one brand.',
                'Uma imagem gerada entre dois quadros do jogo: mais suave, um pouco mais de atraso. DLSS precisa de NVIDIA RTX 40 ou mais nova; FSR não depende de marca.'),
    'aa_no_dlss': ('No DLSS on this GPU: DLAA runs as FSR 3.1 Native AA',
                   'Sem DLSS nesta GPU: o DLAA roda como FSR 3.1 AA nativo'),
    'aa_no_fsr4': ('This GPU cannot run FSR 4: it runs as FSR 3.1 Native AA',
                   'Esta GPU não roda o FSR 4: ele roda como FSR 3.1 AA nativo'),
    'smaa_hint': ('Not recommended: DLAA and FSR already smooth the edges; SMAA on top blurs the image and the HUD.',
                  'Não recomendado: o DLAA e o FSR já suavizam as bordas; o SMAA por cima desfoca a imagem e o HUD.'),
    'sharpening': ('Sharpening', 'Nitidez'),
    'sharpen_off': ('Off', 'Desligada'),
    'sharpen_rcas': ('RCAS (FSR 1)', 'RCAS (FSR 1)'),
    'sharpen_cas': ('CAS (AMD)', 'CAS (AMD)'),
    'color_adjust': ('Color adjustments', 'Ajustes de cor'),
    'color_brightness': ('Brightness (0 = unchanged)', 'Brilho (0 = sem alteração)'),
    'color_contrast': ('Contrast (1 = unchanged)', 'Contraste (1 = sem alteração)'),
    'color_saturation': ('Saturation (1 = unchanged)', 'Saturação (1 = sem alteração)'),
    'color_gamma': ('Gamma (1 = unchanged)', 'Gama (1 = sem alteração)'),
    'audio': ('Audio', 'Áudio'),
    'native_audio': ('Native audio (libSceNgs2.sprx from your console)', 'Áudio nativo (libSceNgs2.sprx do seu console)'),
    'controls': ('Controls', 'Controles'),
    'menu_confirm': ('Menu confirm button (next launch)', 'Botão de confirmação (próxima inicialização)'),
    'mouse_enabled': ('Mouse turns the camera', 'Mouse gira a câmera'),
    'mouse_invert_y': ('Invert mouse Y axis', 'Inverter eixo Y do mouse'),
    'mouse_sensitivity': ('Mouse sensitivity', 'Sensibilidade do mouse'),
    'mouse_hint': ('Esc releases the mouse; a click in the game window takes it back.',
                   'Esc solta o mouse; um clique na janela do jogo prende de novo.'),
    'remap': ('Keyboard and mouse buttons…', 'Teclas e botões do mouse…'),
    'remap_title': ('Keyboard and mouse buttons', 'Teclas e botões do mouse'),
    'remap_help': ('Click Set and press a key or mouse button, or type SDL names (Space, Left Shift, '
                   'Mouse Left, Mouse X1, Wheel Up…). "|" gives several: Q|Mouse Left. "none": no key. '
                   'Empty: the default (grey). Applies at the next start.',
                   'Clique em Definir e aperte uma tecla ou botão do mouse, ou digite nomes do SDL (Space, Left Shift, '
                   'Mouse Left, Mouse X1, Wheel Up…). "|" junta vários: Q|Mouse Left. "none": nenhuma tecla. '
                   'Vazio: o padrão (cinza). Vale na próxima vez que o jogo abrir.'),
    'button': ('PS4 button', 'Botão do PS4'),
    'keys': ('Keys / mouse', 'Teclas / mouse'),
    'default': ('Default', 'Padrão'),
    'set': ('Set', 'Definir'),
    'press': ('Press a key…', 'Aperte uma tecla…'),
    'reset': ('Reset all', 'Restaurar tudo'),
    'textures': ('Textures', 'Texturas'),
    'lod_bias': ('Texture LOD bias (DLAA)', 'LOD bias das texturas (DLAA)'),
    'lod_bias_hint': ("Sharper (below 0) or softer (above 0) scene textures under DLAA; no effect without DLAA. "
                      "-1.00 is NVIDIA's advice for native resolution. Lower brings out fine detail, mostly on "
                      'grass (-3.00 has 31% more fine detail than +1.00), but can shimmer in motion.',
                      'Texturas da cena mais nítidas (abaixo de 0) ou mais suaves (acima de 0) com DLAA; sem efeito '
                      'sem DLAA. -1,00 é o conselho da NVIDIA para resolução nativa. Mais baixo realça detalhes '
                      'finos, sobretudo na grama (-3,00 tem 31% mais detalhe fino que +1,00), mas pode cintilar em '
                      'movimento.'),
    'aniso': ('Anisotropic filtering', 'Filtragem anisotrópica'),
    'aniso_game': ('Game', 'Jogo'),
    'aniso_hint': ('Keeps ground and wall textures sharp at shallow angles. The game uses trilinear and 4x. In our '
                   'tests (a stone path, a pillar hall) 16x looked the same as Game; it stays on because the cost '
                   'is very small.',
                   'Mantém nítidas as texturas do chão e das paredes em ângulos rasos. O jogo usa trilinear e 4x. '
                   'Nos nossos testes (um caminho de pedra, um salão de pilares) 16x ficou igual ao Jogo; fica '
                   'ligado porque o custo é muito pequeno.'),
    'texture_pack_dialog_hint': ("AI-upscaled versions of the game's textures, made from your own copy of the game "
                                 '(ps4-texturepack-generator, docs/texture-pack.md); the game files are not changed. In game it '
                                 'switches live; textures appear as they load (a second or two).',
                                 'Versões ampliadas por IA das texturas do jogo, feitas da sua própria cópia do jogo '
                                 '(ps4-texturepack-generator, docs/texture-pack.md); os arquivos do jogo não são alterados. No jogo '
                                 'troca ao vivo; as texturas aparecem conforme carregam (um ou dois segundos).'),
    'close': ('Close', 'Fechar'),
    'mark_default': ('Default: {value}', 'Padrão: {value}'),
    'mark_default_button': ('Default', 'Padrão'),
    'mark_on': ('on', 'ligado'),
    'mark_off': ('off', 'desligado'),
    'play': ('Play', 'Jogar'),
    'saves': ('Saves folder', 'Pasta de saves'),
    'logs': ('Logs folder', 'Pasta de logs'),
    'report_issue': ('Report a problem on GitHub', 'Relatar um problema no GitHub'),
    'rep_unknown': ('unknown', 'desconhecido'),
    'rep_yes': ('yes', 'sim'),
    'rep_no': ('no', 'não'),
    'rep_row_version': ('Port version', 'Versão do port'),
    'rep_row_game': ('Game', 'Jogo'),
    'rep_row_os': ('System', 'Sistema'),
    'rep_row_cpu': ('CPU', 'CPU'),
    'rep_row_ram': ('RAM', 'RAM'),
    'rep_row_gpu': ('GPU', 'GPU'),
    'rep_row_driver': ('Driver', 'Driver'),
    'rep_gpu_none': ('not detected (vulkaninfo missing or no Vulkan driver)', 'não detectada (sem vulkaninfo ou sem driver Vulkan)'),
    'rep_h_hw': ('Hardware and version', 'Hardware e versão'),
    'rep_h_log': ('Log summary', 'Resumo do log'),
    'rep_h_known': ('Known problems for this GPU', 'Problemas conhecidos para esta GPU'),
    'rep_no_log': ('No log attached.', 'Nenhum log anexado.'),
    'rep_footer': ('Report generated locally by the launcher; user names in paths were replaced by <user>.', 'Relatório gerado localmente pelo launcher; nomes de usuário em caminhos foram trocados por <user>.'),
    'rep_attach_zip': ('The full report is long: please attach the .zip saved by the launcher to this issue.', 'O relatório completo é longo: anexe a esta issue o .zip salvo pelo launcher.'),
    'rep_fps': ('FPS: average {avg} (lowest {lo}, highest {hi}; {n} samples)', 'FPS: média {avg} (mínimo {lo}, máximo {hi}; {n} amostras)'),
    'rep_worst': ('Worst frame: {ms} ms', 'Pior quadro: {ms} ms'),
    'rep_no_fps': ('FPS: no "Frame stats" lines in this log', 'FPS: este log não tem linhas "Frame stats"'),
    'rep_no_events': ('No Fault:, STOP: or FREEZE: lines in this log.', 'Nenhuma linha Fault:, STOP: ou FREEZE: neste log.'),
    'rep_issue_title': ('Problem report: {gpu}', 'Relato de problema: {gpu}'),
    'report_busy': ('Collecting the report…', 'Coletando o relatório…'),
    'report_done': ('Issue form opened in the browser. Attach {zip} from the logs folder (log with user names removed).',
                    'Formulário aberto no navegador. Anexe {zip} da pasta de logs (log sem nomes de usuário).'),
    'clear_cache': ('Clear shader cache', 'Limpar cache de shaders'),
    'clear_cache_ask': ('This deletes the compiled shaders ({size}); the next start takes longer to load and '
                        'may stutter at first. Continue?',
                        'Isso apaga os shaders compilados ({size}); a próxima partida demora mais para carregar e '
                        'pode engasgar no começo. Continuar?'),
    'clear_cache_empty': ('The shader cache is already empty.', 'O cache de shaders já está vazio.'),
    'clear_cache_done': ('Shader cache cleared: {size} freed.', 'Cache de shaders limpo: {size} liberados.'),
    'clear_cache_failed': ('{size} freed, but {count} file(s) could not be deleted (in use? Close the game '
                           'and try again).',
                           '{size} liberados, mas {count} arquivo(s) não puderam ser apagados (em uso? Feche o '
                           'jogo e tente de novo).'),
    'mods': ('Mods / Cheats…', 'Mods / Cheats…'),
    'menu_note': ("Two menus in game. Insert: the port's graphics menu (every setting applies live). "
                  "Development menu (the game's own QA menu): turn on 'Level select and QA menu' under Debug, "
                  'then in game hold L1 and press L3 (keyboard: hold 1, press Z).',
                  'Dois menus no jogo. Insert: o menu de gráficos do port (tudo vale na hora). '
                  'Menu de desenvolvimento (o menu de QA do próprio jogo): ligue "Seleção de fases e menu de QA" '
                  'em Depuração, depois no jogo segure L1 e aperte L3 (teclado: segure 1, aperte Z).'),
    'graphics': ('Graphics…', 'Gráficos…'),
    'graphics_title': ('Graphics', 'Gráficos'),
    'advanced': ('Advanced…', 'Avançado…'),
    'advanced_title': ('Advanced settings', 'Configurações avançadas'),
    'advanced_hint': ('GPU readbacks: how the game gets back what the GPU wrote. Keep the defaults unless a scene '
                      'breaks or you are testing. Changes apply at the next start (the readback batch at once). '
                      'Graphics presets never change these.',
                      'Leituras da GPU: como o jogo recebe de volta o que a GPU escreveu. Mantenha os padrões, a '
                      'não ser que uma cena quebre ou você esteja testando. As mudanças valem no próximo início (o '
                      'lote de leituras na hora). Os presets de gráficos nunca mudam estas opções.'),
    'gpu_readbacks': ('GPU readback mode', 'Modo de leitura da GPU'),
    'gpu_readbacks_hint': ('How strictly CPU reads of GPU-written memory wait for the GPU.',
                           'Quão rigorosamente leituras da CPU de memória escrita pela GPU esperam a GPU.'),
    'gpu_readbacks_why': ('The game reads GPU compute results on the CPU: Relaxed can give it stale data, Off none '
                          'at all.',
                          'O jogo lê resultados de compute da GPU na CPU: Relaxado pode dar dados velhos, '
                          'Desligado nenhum.'),
    'readbacks_2': ('Precise', 'Preciso'),
    'readbacks_1': ('Relaxed', 'Relaxado'),
    'readbacks_0': ('Off', 'Desligado'),
    'readback_batch_hint': ('When the game reads GPU-written memory, also copy back the GPU-written data around it.',
                            'Quando o jogo lê memória escrita pela GPU, copia também os dados escritos pela GPU ao redor.'),
    'graphics_tips': ('Tip: tweak while playing', 'Dica: ajuste jogando'),
    'graphics_hint': ('Best set in game: the in-game graphics menu has these settings and more (filters, colour, '
                      'Lab), and every change shows right away on the scene you are looking at. Go somewhere '
                      'outdoors with Trico in view, open the menu and compare. Changes are saved for the next start. '
                      '100% is the game as shipped.',
                      'Melhor ajustar no jogo: o menu de gráficos do jogo tem estas opções e mais (filtros, cor, '
                      'Lab), e cada mudança aparece na hora na cena que você está vendo. Vá para um lugar aberto com '
                      'o Trico à vista, abra o menu e compare. As mudanças ficam salvas para o próximo início. '
                      '100% é o jogo original.'),
    'graphics_keys': ('Insert          open / close the graphics menu (Esc closes)\n'
                      'L2 + R3         controller: hold L2, press R3\n'
                      'Q / E, L1 / R1  previous / next tab\n'
                      'F8              switch anti-aliasing (DLAA, FSR) / SMAA\n'
                      'F10             show / hide the FPS counter',
                      'Insert          abre / fecha o menu de gráficos (Esc fecha)\n'
                      'L2 + R3         controle: segure L2, aperte R3\n'
                      'Q / E, L1 / R1  aba anterior / próxima\n'
                      'F8              alterna anti-aliasing (DLAA, FSR) / SMAA\n'
                      'F10             mostra / esconde o contador de FPS'),
    'hdr_hint': ("The game's own HDR10 output, used when HDR is on in Windows display settings. Recommended: on.",
                 'A saída HDR10 do próprio jogo, usada quando o HDR está ligado nas configurações de tela do Windows. '
                 'Recomendado: ligado.'),
    'shadow_scale': ('Shadow resolution', 'Resolução das sombras'),
    'shadow_game': ('Game (2048)', 'Jogo (2048)'),
    'graphics_preset': ('Graphics preset', 'Predefinição gráfica'),
    'graphics_preset_hint': ('All look settings and the texture pack at once. Stock is the game as shipped; '
                             'save your own here or in the in-game menu (Insert, Image tab).',
                             'Todas as opções de imagem e o pacote de texturas de uma vez. Original é o jogo '
                             'como lançado; salve as suas aqui ou no menu do jogo (Insert, aba Imagem).'),
    'preset_save': ('Save as preset', 'Salvar como predefinição'),
    'preset_save_button': ('Save', 'Salvar'),
    'preset_save_bad': ('Type a name (letters, digits, space, - or _; not Stock or Recommended).',
                        'Digite um nome (letras, números, espaço, - ou _; não Original nem Recomendado).'),
    'preset_stock': ('Stock (as shipped)', 'Original (como lançado)'),
    'preset_recommended': ('Recommended (default)', 'Recomendado (padrão)'),
    'preset_custom': ('Custom', 'Personalizado'),
    'shadow_hint': ("Draws the shadow maps at 2x or 4x the game's 2048x2048 (default: Game). The main gain is much "
                    'less shimmer of tree and leaf shadows, with any anti-aliasing. 8192 costs about 8 FPS and about '
                    '1 GB of video memory; recommended when you have the headroom.',
                    'Desenha os mapas de sombra em 2x ou 4x os 2048x2048 do jogo (padrão: Jogo). O ganho principal é '
                    'muito menos tremulação das sombras de árvores e folhas, com qualquer anti-aliasing. 8192 custa '
                    'cerca de 8 FPS e cerca de 1 GB de memória de vídeo; recomendado quando houver folga.'),
    'grass_smooth': ('Grass animated every frame (default)', 'Grama animada todo quadro (padrão)'),
    'grass_hint': ('The game moves most grass only every 4th frame, which looks choppy at high frame rates. '
                   'Off is as shipped.',
                   'O jogo move a maior parte da grama só a cada 4 quadros, o que fica truncado em taxas altas. '
                   'Desligado é o original.'),
    'effect_fog_distance': ('Fog distance', 'Distância da neblina'),
    'effect_fog_distance_hint': ('Where the fog starts and reaches full strength. Default 100% is as shipped; lower '
                                 'brings the fog closer.',
                                 'Onde a neblina começa e chega à força total. Padrão 100% é o original; mais baixo '
                                 'traz a neblina mais perto.'),
    'effect_haze_fringe': ('Haze color fringe', 'Franja colorida na névoa'),
    'effect_haze_fringe_hint': ("The game's chromatic aberration on the light haze: yellow and blue edges around "
                                'objects against fog, strongest toward the screen edges. Sharp anti-aliasing (DLAA, '
                                'DLSS) makes it stand out more. Default 0% gives clean edges; 100% is as shipped.',
                                'A aberração cromática do jogo na névoa de luz: bordas amarelas e azuis em volta de '
                                'objetos contra a neblina, mais fortes perto das bordas da tela. O anti-aliasing '
                                'nítido (DLAA, DLSS) a deixa mais visível. Padrão 0% deixa as bordas limpas; 100% é o '
                                'original.'),
    'effect_boy_wind': ('Boy wind (hair + clothing)', 'Vento no menino (cabelo + roupa)'),
    'effect_boy_wind_hint': ("How strongly the wind moves the boy's hair and clothing. Default 100% is as shipped and "
                             'can look like a storm; 50% is recommended, 30% is calm.',
                             'Quanto o vento mexe o cabelo e a roupa do menino. Padrão 100% é o original e pode '
                             'parecer uma tempestade; 50% é o recomendado, 30% é calmo.'),
    'effect_tree_wind': ('Tree wind', 'Vento nas árvores'),
    'effect_tree_wind_hint': ('How strongly the wind moves trees, leaves and ivy. Default 100% is as shipped; 50% '
                              'looks more natural (recommended), 0% keeps them still.',
                              'Quanto o vento mexe árvores, folhas e hera. Padrão 100% é o original; 50% parece mais '
                              'natural (recomendado), 0% as deixa paradas.'),
    'effects': ('Game effects (100% = as shipped)', 'Efeitos do jogo (100% = original)'),
    'effect_ssao': ('Ambient occlusion', 'Oclusão de ambiente'),
    'effect_ssao_hint': ('Strength of the contact shadows in corners and under grass. 0% turns the pass off (saves a '
                         'little GPU time). Above 150% the half-resolution AO shows blocky halos.',
                         'Força das sombras de contato em cantos e sob a grama. 0% desliga o passe (economiza um pouco '
                         'de GPU). Acima de 150% a AO em meia resolução mostra halos em blocos.'),
    'effect_bloom': ('Bloom', 'Bloom'),
    'effect_bloom_hint': ('Glow around bright light. 0% turns the pass off.', 'Brilho em volta da luz forte. 0% desliga o passe.'),
    'effect_ssr': ('Screen-space reflections', 'Reflexos em espaço de tela'),
    'effect_ssr_hint': ('Reflections and light glare on water and wet stone. 0% turns the pass off.',
                        'Reflexos e brilho de luz na água e na pedra molhada. 0% desliga o passe.'),
    'effect_motion_blur': ('Motion blur', 'Desfoque de movimento'),
    'effect_motion_blur_hint': ('Camera motion blur. 0% turns the pass off.', 'Desfoque ao mover a câmera. 0% desliga o passe.'),
    'effect_vignette_add': ('Vignette', 'Vinheta'),
    'effect_vignette_add_hint': ('Darkened screen corners. The game ships with this off: 0% is as shipped and recommended.',
                                 'Cantos da tela escurecidos. O jogo vem com isto desligado: 0% é o original e o recomendado.'),
    'effect_game_sharpen': ('Game sharpening', 'Nitidez do jogo'),
    'effect_game_sharpen_hint': ("The game's own sharpening filter. On by default: 100% is the original and the maximum (recommended).",
                                 'O filtro de nitidez do próprio jogo. Ligado por padrão: 100% é o original e o máximo (recomendado).'),
    'effect_fov': ('Field of view', 'Campo de visão'),
    'effect_fov_hint': ('Camera field of view. Default 70° is as shipped; higher shows more around the boy.',
                        'Campo de visão da câmera. Padrão 70° é o original; mais alto mostra mais em volta do menino.'),
    'effect_trico_feathers': ('Trico feather wind', 'Vento nas penas do Trico'),
    'effect_trico_feathers_hint': ("How strongly the wind moves Trico's feathers. Default 100% is as shipped; 0% keeps them still.",
                                   'Quanto o vento mexe as penas do Trico. Padrão 100% é o original; 0% as deixa paradas.'),
    'mods_title': ('Mods / Cheats', 'Mods / Cheats'),
    'mods_hint': ('Drop shadPS4/GoldHEN cheat XML files in the mods folder, then turn them on here. '
                  'Nothing here changes the game until you enable it.',
                  'Solte arquivos de cheat XML no formato shadPS4/GoldHEN na pasta de mods e ligue aqui. '
                  'Nada muda no jogo até você ativar.'),
    'mods_warning': ('A cheat can crash the game or behave oddly; if that happens, turn it off here.',
                     'Um cheat pode travar o jogo ou se comportar de forma estranha; se isso acontecer, desligue aqui.'),
    'mods_folder': ('Open mods folder', 'Abrir pasta de mods'),
    'mods_refresh': ('Refresh list', 'Atualizar lista'),
    'mods_empty': ('No cheat XML files found in the mods folder yet.',
                   'Nenhum arquivo de cheat XML encontrado na pasta de mods ainda.'),
    'quit': ('Quit', 'Sair'),
    'ready': ('Ready to play.', 'Pronto para jogar.'),
    'audio_native': ('Audio: native (console module found).', 'Áudio: nativo (módulo do console encontrado).'),
    'audio_builtin': ('Audio: built-in mixer, experimental (libSceNgs2.sprx not found or not selected).',
                      'Áudio: mixer próprio, experimental (libSceNgs2.sprx não encontrado ou desmarcado).'),
    'missing': ('Missing', 'Faltando'),
    'not_built': ('Port not built (run build.sh)', 'Port não compilado (rode o build.sh)'),
    'preparing': ('Preparing the game…', 'Preparando o jogo…'),
    'running': ('Playing — log:', 'Jogando — log:'),
    'ended': ('Game closed (exit code {code}).', 'Jogo fechado (código {code}).'),
    'crashed': ('The game closed with an error (code {code}). The log has the details.',
                'O jogo fechou com erro (código {code}). Os detalhes estão no log.'),
    'already': ('The game is already running.', 'O jogo já está rodando.'),
    'error': ('Error', 'Erro'),
}
GAME_LANGUAGE_NAMES = {'ja': tlg_run.JAPANESE_FALLBACK_LABEL,
                       'en': 'English', 'fr': 'Français', 'es_ES': 'Español (España)',
                       'de': 'Deutsch', 'it': 'Italiano', 'nl': 'Nederlands',
                       'pt_PT': 'Português (Portugal)', 'ru': 'Русский', 'fi': 'Suomi',
                       'sv': 'Svenska', 'da': 'Dansk', 'no': 'Norsk', 'pl': 'Polski',
                       'tr': 'Türkçe', 'ar': 'العربية',
                       'pt_BR': 'Português (Brasil)', 'es_LA': 'Español (Latinoamérica)',
                       'fr_CA': 'Français (Canada)', 'ko': '한국어', 'zh_TW': '繁體中文',
                       'zh_CN': '简体中文'}
UI_LANGUAGES = ['auto', 'pt_BR', 'en_US']
UI_LANGUAGE_NAMES = {'pt_BR': 'Português (Brasil)', 'en_US': 'English (US)'}
DISPLAY_MODES = ['Windowed', 'Borderless', 'Fullscreen']


# The PS4 buttons the remap dialog lists, with what they do in The Last Guardian when known.
PS4_BUTTONS = [
    ('cross', 'Cross (X)', ('jump', 'pular')), ('circle', 'Circle', None), ('square', 'Square', None),
    ('triangle', 'Triangle', ('grab, hold on', 'agarrar, segurar')), ('l1', 'L1', None),
    ('r1', 'R1', ('call Trico', 'chamar o Trico')), ('l2', 'L2', None), ('r2', 'R2', None), ('l3', 'L3', None),
    ('r3', 'R3', None), ('options', 'Options', ('menu', 'menu')),
    ('touchpad_left', 'Touchpad (left)', ('photo mode', 'modo foto')),
    ('touchpad_right', 'Touchpad (right)', ('pause', 'pausa')),
    ('up', 'D-pad up', None), ('down', 'D-pad down', None), ('left', 'D-pad left', None), ('right', 'D-pad right', None),
]
# Tk key names -> SDL key names (src/runtime_pad.c reads SDL names); letters and digits are the same.
TK_TO_SDL = {
    'space': 'Space', 'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl',
    'Control_R': 'Right Ctrl', 'Alt_L': 'Left Alt', 'Alt_R': 'Right Alt', 'Return': 'Return', 'Tab': 'Tab',
    'BackSpace': 'Backspace', 'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Delete': 'Delete',
    'Insert': 'Insert', 'Home': 'Home', 'End': 'End', 'Prior': 'PageUp', 'Next': 'PageDown',
    'Caps_Lock': 'CapsLock', 'comma': ',', 'period': '.', 'minus': '-', 'equal': '=', 'slash': '/',
    'semicolon': ';', 'apostrophe': "'", 'bracketleft': '[', 'bracketright': ']', 'backslash': '\\',
    'grave': '`', 'KP_Enter': 'Keypad Enter',
}
TK_MOUSE = {1: 'Mouse Left', 2: 'Mouse Middle', 3: 'Mouse Right', 8: 'Mouse X1', 9: 'Mouse X2'}


def sdl_key_name(keysym):
    """The SDL name of a Tk key, or None (Esc releases the mouse in game; F-keys are the port's)."""
    if keysym in TK_TO_SDL:
        return TK_TO_SDL[keysym]
    if len(keysym) == 1 and keysym.isalnum():
        return keysym.upper()
    if keysym.startswith('KP_') and keysym[3:].isdigit():
        return f'Keypad {keysym[3:]}'
    return None

SHARPENERS = ['off', 'rcas', 'cas']
# Image filters: kept in tlg.ini only (the in-game menu writes the same keys), with their
# defaults and ranges (tlg_graphics.h).
INI_DEFAULTS = {'smaa': '0', 'rcas': '1', 'cas': '1', 'rcas_strength': '75', 'cas_strength': '100', 'color_adjust': '1', 'color_brightness': '0.00',
                'color_contrast': '1.00', 'color_saturation': '1.00', 'color_gamma': '0.75'}
# The anti-aliasing row (TlgSettings::AaMode): (label, tlg.ini dlaa / upscaler). DLAA needs an
# NVIDIA RTX card, FSR 4 native AA a recent card (the game checks it once per install and then
# moves FSR 3.1 to FSR 4); FSR 3.1 native AA runs on any card.
AA_MODES = [('aa_off', None), ('aa_dlaa', ('1', 'dlss')), ('aa_fsr3', ('1', 'fsr3')),
            ('aa_fsr4', ('1', 'fsr4'))]


def fg_mode_of(ini):
    value = ini.get('frame_generation', '0')
    return int(value) if value in ('1', '2') else 0


def dlss_unsupported(ini):
    # The game writes its DLSS check (dlss_supported); without DLSS, DLAA runs as FSR 3.1.
    return ini.get('dlss_supported') == '0'


def fsr4_unsupported(ini):
    # The game writes its FSR 4 check (fsr4_supported); without it, FSR 4 runs as FSR 3.1.
    return ini.get('fsr4_supported') == '0'


def aa_mode_of(ini):
    # The AA_MODES index tlg.ini picks (TlgSettings::AaModeOf: every upscaler but TAA, FSR 3.1 and
    # FSR 4 loads as DLAA).
    if ini.get('dlaa', '1') in ('0', ''):
        return 0
    if ini.get('aa_row', '0') in ('0', ''):
        return 1  # a file from before the row: DLAA
    if ini.get('upscaler') == 'taa':
        return 0
    return {'fsr3': 2, 'fsr4': 3}.get(ini.get('upscaler'), 1)


COLOR_RANGES = {'color_brightness': (-0.5, 0.5), 'color_contrast': (0.5, 1.5),
                'color_saturation': (0.0, 2.0), 'color_gamma': (0.5, 2.0)}
# Graphics dialog: the in-game menu's HDR, Enhancements and Game effects, same tlg.ini keys
# (tlg_settings.cpp). Effects: key -> (default, low, high[, unit]) in percent unless a unit is
# given, the default as shipped (so also the PS4 value).
SHADOW_SCALES = [1, 2, 4]
# tlg.ini lod_bias is in quarter steps (-12..4 = -3.00..+1.00); default -4 (-1.00), PS4 0.
LOD_BIAS_DEFAULT = -4
ANISO_LEVELS = [0, 2, 4, 8, 16]  # 0 = the game's own (PS4); default 16
EFFECTS = {'ssao': (100, 0, 400), 'bloom': (100, 0, 400), 'ssr': (100, 0, 400), 'motion_blur': (100, 0, 400),
           'vignette_add': (0, 0, 100), 'game_sharpen': (100, 0, 100),
           'fog_distance': (100, 25, 400), 'haze_fringe': (0, 0, 100), 'boy_wind': (100, 0, 200),
           'tree_wind': (100, 0, 200), 'trico_feathers': (100, 0, 400),
           'fov': (70, 50, 110, '°')}


def add_tooltip(widget, text):
    """Shows text in a small window next to the pointer while it is over the widget."""
    tip = []

    def hide(_=None):
        while tip:
            tip.pop().destroy()

    def show(event):
        hide()
        win = tk.Toplevel(widget)
        win.overrideredirect(True)
        tk.Label(win, text=text, background=PANEL, foreground=TEXT_COLOR, relief='solid', borderwidth=1,
                 wraplength=360, justify='left', padx=6, pady=4).pack()
        win.update_idletasks()
        x = min(event.x_root + 14, widget.winfo_screenwidth() - win.winfo_reqwidth() - 4)
        y = min(event.y_root + 14, widget.winfo_screenheight() - win.winfo_reqheight() - 4)
        win.geometry(f'+{x}+{y}')
        tip.append(win)

    widget.bind('<Enter>', show, add='+')
    widget.bind('<Leave>', hide, add='+')
    widget.bind('<Destroy>', hide, add='+')


def help_mark(parent, text, *also):
    """A small "?" that shows the help text while the pointer is over it (or over also)."""
    mark = tk.Label(parent, text='?', foreground=ACCENT, background=PANEL, cursor='question_arrow',
                    font=('TkDefaultFont', 8, 'bold'), padx=5, pady=0, relief='flat',
                    highlightthickness=1, highlightbackground=LINE)
    for widget in (mark, *also):
        add_tooltip(widget, text)
    return mark


class BenchmarkWindow:
    """The Benchmark window (Debug section): picks the scenes, runs benchmark.Sweep on a worker
    thread and shows the progress; the result is one .zip under <user data>/benchmark/."""

    def __init__(self, app, parent):
        import benchmark
        self.bm = benchmark
        self.app = app
        self.running = False
        self.sweep = None
        self.stop_event = threading.Event()
        self.events = queue.Queue()
        self.zip_path = None

        win = self.win = ttk.Frame(parent, padding=14)
        win.pack(fill='both', expand=True)
        win.columnconfigure(0, weight=1)
        full_count = len(benchmark.default_points())

        ttk.Label(win, text=app.t('bench_text'), wraplength=520, justify='left').grid(
            row=0, column=0, columnspan=3, sticky='w')
        self.mode = tk.StringVar(value='full')
        ttk.Radiobutton(win, text=app.t('bench_full', count=full_count), variable=self.mode, value='full',
                        command=self.refresh).grid(row=1, column=0, sticky='w', pady=(10, 0))
        ttk.Radiobutton(win, text=app.t('bench_pick'), variable=self.mode, value='pick',
                        command=self.refresh).grid(row=2, column=0, sticky='w')
        picker = ttk.Frame(win)
        picker.grid(row=3, column=0, columnspan=3, sticky='ew')
        self.pick = tk.Listbox(picker, selectmode='multiple', height=8, width=54, background=FIELD,
                               foreground=TEXT_COLOR, relief='flat', exportselection=False,
                               selectbackground=ACCENT, selectforeground=BG, font=('TkDefaultFont', 9))
        for stage, area in benchmark.default_points():
            self.pick.insert('end', f'{stage}:{area}')
        bar = ttk.Scrollbar(picker, orient='vertical', command=self.pick.yview)
        self.pick.configure(yscrollcommand=bar.set)
        self.pick.pack(side='left', fill='x', expand=True)
        bar.pack(side='left', fill='y')
        self.pick.bind('<<ListboxSelect>>', lambda _e: self.refresh())
        ttk.Radiobutton(win, text=app.t('bench_custom'), variable=self.mode, value='custom',
                        command=self.refresh).grid(row=4, column=0, sticky='w', pady=(6, 0))
        ttk.Label(win, text=app.t('bench_custom_hint'), wraplength=520, justify='left',
                  foreground=MUTED).grid(row=5, column=0, columnspan=3, sticky='w', pady=(2, 2))
        custom_box = ttk.Frame(win)
        custom_box.grid(row=6, column=0, columnspan=3, sticky='ew')
        custom_box.columnconfigure(0, weight=1)
        self.custom = tk.Text(custom_box, width=56, height=10, background=FIELD,
                              foreground=TEXT_COLOR, insertbackground=TEXT_COLOR, relief='flat',
                              font=('TkDefaultFont', 9))
        self.custom.insert('1.0', '\n'.join(benchmark.EXAMPLE_POINTS))
        self.custom.grid(row=0, column=0, sticky='ew')
        custom_bar = ttk.Scrollbar(custom_box, orient='vertical', command=self.custom.yview)
        custom_bar.grid(row=0, column=1, sticky='ns')
        self.custom.configure(yscrollcommand=custom_bar.set)
        self.custom.bind('<KeyRelease>', lambda _e: self.refresh())

        line = ttk.Frame(win)
        line.grid(row=7, column=0, columnspan=3, sticky='w', pady=(8, 0))
        ttk.Label(line, text=app.t('bench_seconds')).pack(side='left')
        self.seconds = tk.Spinbox(line, from_=10, to=120, increment=5, width=5,
                                  command=self.refresh, background=FIELD, foreground=TEXT_COLOR,
                                  insertbackground=TEXT_COLOR, relief='flat')
        self.seconds.delete(0, 'end')
        self.seconds.insert(0, '40')
        self.seconds.pack(side='left', padx=(6, 0))
        self.seconds.bind('<KeyRelease>', lambda _e: self.refresh())
        # Tricopilot: the game drives Trico himself all through the run (shim: TLG_TRICOPILOT=1).
        self.pilot = tk.BooleanVar(value=False)
        self.pilot_check = ttk.Checkbutton(line, text=app.t('bench_pilot'), variable=self.pilot)
        self.pilot_check.pack(side='left', padx=(20, 0))
        help_mark(line, app.t('bench_pilot_help'), self.pilot_check).pack(side='left', padx=(6, 0))
        # Which camera watches him (shim: TLG_TRICOPILOT_CAM). Picture only, not the numbers.
        self.pilot_cams = [('near', app.t('bench_cam_near')), ('middle', app.t('bench_cam_middle')),
                           ('far', app.t('bench_cam_far')), ('game', app.t('bench_cam_game'))]
        ttk.Label(line, text=app.t('bench_pilot_cam')).pack(side='left', padx=(14, 0))
        self.pilot_cam = tk.StringVar(value=self.pilot_cams[1][1])
        self.pilot_cam_box = ttk.Combobox(line, textvariable=self.pilot_cam, state='readonly',
                                          width=13, values=[name for _k, name in self.pilot_cams])
        self.pilot_cam_box.pack(side='left', padx=(6, 0))
        help_mark(line, app.t('bench_pilot_cam_help'), self.pilot_cam_box).pack(side='left', padx=(6, 0))
        self.estimate = ttk.Label(win, text='', foreground=MUTED)
        self.estimate.grid(row=8, column=0, columnspan=3, sticky='w', pady=(4, 0))

        self.progress = ttk.Progressbar(win, mode='determinate', length=520)
        self.progress.grid(row=9, column=0, columnspan=3, sticky='ew', pady=(10, 0))
        self.status = ttk.Label(win, text='', wraplength=520, justify='left')
        self.status.grid(row=10, column=0, columnspan=3, sticky='w', pady=(4, 0))

        buttons = ttk.Frame(win)
        buttons.grid(row=11, column=0, columnspan=3, sticky='w', pady=(10, 0))
        self.start_button = ttk.Button(buttons, text=app.t('bench_start'), command=self.start)
        self.start_button.pack(side='left')
        self.stop_button = ttk.Button(buttons, text=app.t('bench_stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='left', padx=(6, 0))
        self.open_button = ttk.Button(buttons, text=app.t('bench_open_folder'), command=self.open_folder,
                                      state='disabled')
        self.open_button.pack(side='left', padx=(6, 0))
        self.refresh()

    def alive(self):
        try:
            return bool(self.win.winfo_exists())
        except tk.TclError:
            return False

    # -- the choices -------------------------------------------------------------------------
    def chosen(self):
        if self.mode.get() == 'full':
            return self.bm.default_points(), []
        if self.mode.get() == 'pick':
            points = self.bm.default_points()
            return [points[i] for i in self.pick.curselection()], []
        return self.bm.parse_points(self.custom.get('1.0', 'end'))

    def chosen_seconds(self):
        try:
            return max(5, min(600, int(self.seconds.get())))
        except (tk.TclError, ValueError):
            return 40

    def refresh(self):
        points, _bad = self.chosen()
        mode = self.mode.get()
        self.custom.configure(state='normal' if mode == 'custom' and not self.running else 'disabled')
        self.pick.configure(state='normal' if mode == 'pick' and not self.running else 'disabled')
        self.pilot_check.configure(state='disabled' if self.running else 'normal')
        self.pilot_cam_box.configure(state='disabled' if self.running else 'readonly')
        self.estimate.configure(text=self.app.t(
            'bench_estimate', minutes=self.bm.estimate_minutes(len(points), self.chosen_seconds()),
            count=len(points)))

    def _cam_key(self):
        """The camera the shim wants (near/middle/far/game) for the name shown in the box."""
        name = self.pilot_cam.get()
        for key, shown in self.pilot_cams:
            if shown == name:
                return key
        return 'middle'

    # -- run ---------------------------------------------------------------------------------
    def start(self):
        app = self.app
        if app.process or tlg_run.game_running():
            messagebox.showinfo(app.t('bench_title'), app.t('bench_game_running'), parent=self.win)
            return
        points, bad = self.chosen()
        if bad:
            messagebox.showerror(app.t('bench_title'),
                                 app.t('bench_bad_points', points=', '.join(bad[:5])), parent=self.win)
            return
        if not points:
            messagebox.showerror(app.t('bench_title'), app.t('bench_no_points'), parent=self.win)
            return
        self.running = True
        self.zip_path = None
        self.stop_event.clear()
        app.stop_music()   # quiet while the game runs; back once the run is done (sync_music)
        app.stop_track()
        self.start_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.open_button.configure(state='disabled')
        self.progress.configure(maximum=len(points), value=0)
        self.refresh()
        notify = lambda kind, **data: self.events.put((kind, data))  # noqa: E731  (worker thread -> queue)
        self.sweep = self.bm.Sweep(app.settings, points, self.chosen_seconds(), notify, self.stop_event,
                                   tricopilot=bool(self.pilot.get()),
                                   tricopilot_cam=self._cam_key())
        threading.Thread(target=self.sweep.run, daemon=True).start()
        self.win.after(200, self.pump)

    def stop(self):
        self.stop_event.set()
        self.stop_button.configure(state='disabled')

    def pump(self):
        app = self.app
        done = False
        while True:
            try:
                kind, data = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == 'point':
                self.progress.configure(value=data['index'])
                self.status.configure(text=app.t('bench_running', index=data['index'] + 1,
                                                 total=data['total'],
                                                 point=f"{data['stage']}:{data['area']}"))
            elif kind == 'row':
                self.progress.step(1)
            elif kind == 'error':
                self.status.configure(text=data['text'])
            elif kind == 'done':
                done = True
                self.zip_path = data.get('zip')
        if done:
            self.running = False
            self.sweep = None
            self.start_button.configure(state='normal')
            self.stop_button.configure(state='disabled')
            if self.zip_path:
                text = app.t('bench_stopped') + ' ' if self.stop_event.is_set() else ''
                self.status.configure(text=text + app.t('bench_done', name=Path(self.zip_path).name))
                self.open_button.configure(state='normal')
            self.refresh()
            return
        if self.alive():
            self.win.after(200, self.pump)

    def open_folder(self):
        if self.zip_path:
            self.app.open(str(Path(self.zip_path).parent))

    def shutdown(self):
        """The launcher closes: end the run and the game with it."""
        if self.running:
            self.stop_event.set()
            if self.sweep:
                self.sweep.kill_game()


def section(parent, text, help_text=None, padding=8):
    """A titled box; with help_text a "?" sits next to the title."""
    box = ttk.LabelFrame(parent, padding=padding)
    title = ttk.Frame(box)
    ttk.Label(title, text=text, style='TLabelframe.Label').pack(side='left')
    if help_text:
        help_mark(title, help_text).pack(side='left', padx=(6, 0))
    box.configure(labelwidget=title)
    return box


def scroll_area(parent, padding=12):
    """A frame inside a canvas: scroll bars show only when the content does not fit. The mouse
    wheel reaches it through the window-wide handler (Launcher.wheel)."""
    holder = ttk.Frame(parent)
    holder.pack(fill='both', expand=True)
    holder.rowconfigure(0, weight=1)
    holder.columnconfigure(0, weight=1)
    canvas = tk.Canvas(holder, background=BG, highlightthickness=0, bd=0, yscrollincrement=24,
                       xscrollincrement=24)
    ybar = ttk.Scrollbar(holder, orient='vertical', command=canvas.yview)
    xbar = ttk.Scrollbar(holder, orient='horizontal', command=canvas.xview)
    canvas.configure(yscrollcommand=ybar.set, xscrollcommand=xbar.set)
    canvas.grid(row=0, column=0, sticky='nsew')
    inner = ttk.Frame(canvas, padding=padding)
    item = canvas.create_window(0, 0, window=inner, anchor='nw')

    def fit(_=None):
        cw, ch = canvas.winfo_width(), canvas.winfo_height()
        rw, rh = inner.winfo_reqwidth(), inner.winfo_reqheight()
        canvas.itemconfigure(item, width=max(cw, rw), height=max(ch, rh))
        canvas.configure(scrollregion=(0, 0, max(cw, rw), max(ch, rh)))
        for bar, need, cell in ((ybar, rh > ch, {'row': 0, 'column': 1, 'sticky': 'ns'}),
                                (xbar, rw > cw, {'row': 1, 'column': 0, 'sticky': 'ew'})):
            if need:
                bar.grid(**cell)
            else:
                bar.grid_remove()

    inner.bind('<Configure>', fit, add='+')
    canvas.bind('<Configure>', fit, add='+')
    canvas.scroll_inner = inner   # marks it for the wheel handler
    return inner


def art_path(settings, name):
    """Official art from the user's own dump (sce_sys); never shipped with the port."""
    for folder in (Path(settings['game_dir']) / 'sce_sys', tlg_run.REPO_DIR / 'dump' / 'sce_sys'):
        if (folder / name).is_file():
            return folder / name
    return None


def system_is_portuguese():
    for name in ('LC_ALL', 'LC_MESSAGES', 'LANG'):
        value = os.environ.get(name)
        if value:
            return value.lower().startswith('pt')
    try:
        return (locale.getlocale()[0] or '').lower().startswith('pt')
    except ValueError:
        return False


def apply_theme(root):
    """Dark warm ttk look on top of clam, for the settings page and every dialog."""
    style = ttk.Style(root)
    try:
        style.theme_use('clam')
    except tk.TclError:
        return
    style.configure('.', background=BG, foreground=TEXT_COLOR, fieldbackground=FIELD, bordercolor=LINE,
                    lightcolor=PANEL, darkcolor=BG, troughcolor=FIELD, insertcolor=TEXT_COLOR,
                    selectbackground=ACCENT, selectforeground=BG, arrowcolor=ACCENT)
    style.map('.', foreground=[('disabled', DIM)], background=[('active', PANEL)])
    style.configure('TLabelframe', background=BG, bordercolor=LINE)
    style.configure('TLabelframe.Label', background=BG, foreground=ACCENT, font=(SERIF, 11))
    style.configure('TButton', background=PANEL, foreground=TEXT_COLOR, bordercolor=LINE, padding=(10, 4))
    style.map('TButton', background=[('pressed', FIELD), ('active', LINE)], bordercolor=[('active', ACCENT)])
    style.configure('TCheckbutton', background=BG, indicatorbackground=FIELD, indicatorforeground=BG)
    style.map('TCheckbutton', indicatorbackground=[('selected', ACCENT)], background=[('active', BG)])
    style.configure('TRadiobutton', background=BG, indicatorbackground=FIELD)
    style.map('TRadiobutton', indicatorbackground=[('selected', ACCENT)], background=[('active', BG)])
    for name in ('TCombobox', 'TSpinbox', 'TEntry'):
        style.configure(name, fieldbackground=FIELD, foreground=TEXT_COLOR, background=PANEL)
        style.map(name, fieldbackground=[('readonly', FIELD)], foreground=[('readonly', TEXT_COLOR)],
                  selectbackground=[('readonly', FIELD)], selectforeground=[('readonly', TEXT_COLOR)])
    style.configure('TNotebook', background=BG, bordercolor=LINE)
    style.configure('TNotebook.Tab', background=PANEL, foreground=MUTED, padding=(12, 4))
    style.map('TNotebook.Tab', background=[('selected', BG)], foreground=[('selected', ACCENT)])
    style.configure('Treeview', background=FIELD, fieldbackground=FIELD, foreground=TEXT_COLOR)
    style.configure('Treeview.Heading', background=PANEL, foreground=MUTED)
    style.configure('Back.TLabel', foreground=MUTED, font=(SERIF, 12))
    style.configure('Play.TButton', font=(SERIF, 13), foreground=ACCENT, bordercolor=ACCENT)
    root.configure(background=BG)
    root.option_add('*Toplevel.background', BG)
    root.option_add('*TCombobox*Listbox.background', FIELD)
    root.option_add('*TCombobox*Listbox.foreground', TEXT_COLOR)
    root.option_add('*TCombobox*Listbox.selectBackground', ACCENT)
    root.option_add('*TCombobox*Listbox.selectForeground', BG)
    root.option_add('*Text.background', FIELD)
    root.option_add('*Text.foreground', MUTED)
    root.option_add('*Listbox.background', FIELD)
    root.option_add('*Listbox.foreground', TEXT_COLOR)


def background_source(settings):
    """The home screen picture: the player's own pick in the user folder, else pic1/pic0 from their dump.
    Nothing is shipped with the port."""
    for name in BACKGROUND_NAMES:
        if (tlg_run.USER_DIR / name).is_file():
            return tlg_run.USER_DIR / name
    return art_path(settings, 'pic1.png') or art_path(settings, 'pic0.png')


def home_background(source, size, faded=False):
    """The picture cover-fitted to size, softened and faded into BG on the left and bottom (Pillow);
    faded: blurred and darker, behind an open page. Without Pillow a plain Tk copy; None when
    there is no picture."""
    width, height = size
    try:
        from PIL import Image, ImageEnhance, ImageFilter, ImageTk
    except ImportError:
        if source is None:
            return None
        try:
            return tk.PhotoImage(file=str(source))
        except tk.TclError:
            return None
    dark = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
    if source is None:
        img = Image.new('RGB', size, dark)
    else:
        try:
            img = Image.open(source).convert('RGB')
        except OSError:
            return None
        if img.width < img.height * 1.4:
            # Square key art (icon0.png): a blurred, stretched copy fills the window and the sharp
            # picture sits on the right at full height, its left edge melting into the blur.
            sharp = img.resize((round(img.width * height / img.height), height), Image.LANCZOS)
            back = img.resize((width, round(img.height * width / img.width)), Image.BILINEAR)
            back = back.crop((0, (back.height - height) // 2, width, (back.height - height) // 2 + height))
            back = ImageEnhance.Brightness(back.filter(ImageFilter.GaussianBlur(28))).enhance(0.85)
            edge = Image.new('L', (sharp.width, 1))
            feather = sharp.width * 0.35
            edge.putdata([int(255 * min(1.0, x / feather)) for x in range(sharp.width)])
            back.paste(sharp, (width - sharp.width, 0), edge.resize(sharp.size))
            img = back
        else:
            scale = max(width / img.width, height / img.height)
            img = img.resize((max(width, round(img.width * scale)), max(height, round(img.height * scale))),
                             Image.LANCZOS)
            img = img.crop((0, (img.height - height) // 2, width, (img.height - height) // 2 + height))
        img = ImageEnhance.Color(img).enhance(0.9)
        img = ImageEnhance.Brightness(img).enhance(0.92)
        img = Image.blend(img, Image.new('RGB', size, (226, 220, 204)), 0.10)
    if faded:
        img = img.filter(ImageFilter.GaussianBlur(6))
        img = ImageEnhance.Color(img).enhance(0.55)
        img = ImageEnhance.Brightness(img).enhance(0.6)
        left = Image.new('L', (width, 1))
        left.putdata([int(255 * 0.95 * max(0.0, 1 - x / (width * 0.3)) ** 1.2) for x in range(width)])
        return ImageTk.PhotoImage(Image.composite(Image.new('RGB', size, dark), img, left.resize(size)))
    # Fade: strong at the left (menu side) and the bottom (status line), built from two 1-D ramps.
    left = Image.new('L', (width, 1))
    left.putdata([int(255 * 0.95 * max(0.0, 1 - x / (width * 0.5)) ** 1.3) for x in range(width)])
    bottom = Image.new('L', (1, height))
    bottom.putdata([int(255 * 0.85 * max(0.0, (y - height * 0.7) / (height * 0.3)) ** 1.6) for y in range(height)])
    from PIL import ImageChops
    mask = ImageChops.add(left.resize(size), bottom.resize(size))
    img = Image.composite(Image.new('RGB', size, dark), img, mask)
    return ImageTk.PhotoImage(img)


class Page(ttk.Frame):
    """A launcher page in the main window, standing in for what used to be a pop-up window:
    the window calls the pages make (title, transient, protocol, ...) are accepted and ignored."""

    def __init__(self, app, name):
        super().__init__(app.panel, padding=12)
        self.app, self.name = app, name

    def title(self, text, help_text=None):
        header = ttk.Frame(self)
        header.pack(fill='x', padx=12, pady=(4, 0))
        ttk.Label(header, text=text, font=(SERIF, 18)).pack(side='left')
        if help_text:
            help_mark(header, help_text).pack(side='left', padx=(8, 0), pady=(6, 0))
        return header

    def ignored(self, *_):
        pass

    transient = protocol = minsize = ignored

    def lift(self, *_):
        self.app.show_page(self.name)

    def destroy(self):
        current = self.app.page == self.name and self.winfo_ismapped()
        super().destroy()
        if current and not self.app.building:
            self.app.show_page('home')


class Launcher:
    def __init__(self, root):
        self.root = root
        self.settings = tlg_run.load_settings()
        self.process = None
        self.lines = queue.Queue()
        for sequence in ('<MouseWheel>', '<Button-4>', '<Button-5>', '<Shift-MouseWheel>'):
            root.bind_all(sequence, self.wheel, add='+')
        self.build()
        self.recover_practice()
        self.root.after(100, self.pump)
        self.root.after(5000, self.music_tick)

    def wheel(self, event):
        """The mouse wheel scrolls the scroll area under the pointer (lists and text scroll themselves)."""
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if widget is None or widget.winfo_class() in ('Text', 'Listbox', 'TCombobox', 'Treeview'):
            return
        while widget is not None and not hasattr(widget, 'scroll_inner'):
            widget = widget.master
        if widget is None:
            return
        steps = -1 if event.num == 4 else 1 if event.num == 5 else (-1 if event.delta > 0 else 1)
        if event.state & 1:   # Shift: sideways
            widget.xview_scroll(steps, 'units')
        else:
            widget.yview_scroll(steps, 'units')

    # -- language ---------------------------------------------------------------------------
    def pt(self):
        choice = self.settings.get('ui_language', 'en_US')
        return system_is_portuguese() if choice == 'auto' else choice == 'pt_BR'

    def t(self, key, **fmt):
        en, pt = TEXT[key]
        return (pt if self.pt() else en).format(**fmt)

    def strength_key(self):
        return {1: 'rcas_strength', 2: 'cas_strength'}.get(self.sharpener.current())

    def update_strength(self, ini):
        key = self.strength_key()
        if key is None:
            self.sharpen_strength.state(['disabled'])
            self.sharpen_shown.configure(text='')
            return
        try:
            value = min(max(int(float(ini.get(key, INI_DEFAULTS[key]))), 0), 100)
        except ValueError:
            value = int(INI_DEFAULTS[key])
        self.sharpen_strength.state(['!disabled'])
        self.sharpen_value = None  # set() calls strength_moved: no write while it shows the file
        self.sharpen_strength.set(value)
        self.sharpen_value = value
        self.sharpen_shown.configure(text=f'{value}%')

    def strength_moved(self, raw):
        key = self.strength_key()
        if key is None or self.sharpen_value is None:
            return
        step = int(round(float(raw) / 5) * 5)
        self.sharpen_shown.configure(text=f'{step}%')
        if step != getattr(self, 'sharpen_value', None):
            self.sharpen_value = step
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {key: step})
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload
            self.refresh_preset()

    def update_aa_note(self, ini):
        # DLAA or FSR 4 stays the choice in tlg.ini; the game runs FSR 3.1 while the card lacks it.
        mode = self.aa_box.current()
        if (mode == 1 and dlss_unsupported(ini)) or (mode == 3 and fsr4_unsupported(ini)):
            self.aa_note.configure(text=self.t('aa_no_dlss' if mode == 1 else 'aa_no_fsr4'))
            self.aa_note.grid()
        else:
            self.aa_note.grid_remove()

    # -- window -----------------------------------------------------------------------------
    def build(self):
        """The home screen (art, menu, Play) and the pages that open over it: Settings, Video,
        Graphics, Controls and Logs are made here; Advanced and Mods when first opened. Rebuilt on a
        language change."""
        if getattr(self, 'home_job', None):
            self.root.after_cancel(self.home_job)
            self.home_job = None
        if getattr(self, 'controller', None):   # its widgets go with the rebuild
            self.controller.stop()
            self.controller = None
        self.building = True
        for child in self.root.winfo_children():
            child.destroy()
        self.building = False
        root = self.root
        root.title(self.t('title'))
        self.load_art()
        self.home = tk.Canvas(root, highlightthickness=0, bd=0, background=BG,
                              width=HOME_SIZE[0], height=HOME_SIZE[1])
        self.home.pack(fill='both', expand=True)
        self.panel = tk.Frame(self.home, background=BG, highlightthickness=1, highlightbackground=LINE)
        self.vars = {}
        # Play and Clear-cache state for the home screen's canvas buttons (never shown themselves).
        self.play_button = ttk.Button(self.panel, command=self.play)
        self.cache_button = ttk.Button(self.panel, command=self.clear_cache)
        self.status = ttk.Label(self.panel, text='')
        self.build_settings()
        self.build_video()
        self.build_logs()
        self.build_soundtrack()
        self.graphics_window = Page(self, 'graphics')
        self.graphics_window.title(self.t('graphics_title'),
                                   self.t('graphics_hint') + '\n\n' + self.t('graphics_keys') + '\n\n' + self.t('menu_note'))
        self.graphics_body = scroll_area(self.graphics_window)
        self.build_graphics()
        self.build_controls()
        self.home_items = {}
        self.home.bind('<Configure>', lambda _: self.schedule_home())
        page = getattr(self, 'page', 'home')
        self.show_page(page if page in ('home', 'settings', 'video', 'graphics', 'controls', 'logs', 'soundtrack') else 'home')
        self.refresh_status()

    def build_settings(self):
        """Settings: Game + Audio | Language + Launcher + Debug."""
        win = self.settings_window = Page(self, 'settings')
        win.title(self.t('home_settings'))
        ttk.Label(win, text='PC — CUSA03627 v1.03', foreground=DIM).pack(anchor='w', padx=12)
        main = scroll_area(win)
        for column in (0, 1):
            main.columnconfigure(column, weight=1, uniform='cols')
        pad = {'padx': 6, 'pady': 4}
        left, right = ttk.Frame(main), ttk.Frame(main)
        left.grid(row=0, column=0, sticky='new', **pad)
        right.grid(row=0, column=1, sticky='new', **pad)
        left.columnconfigure(0, weight=1)
        right.columnconfigure(0, weight=1)
        game = section(left, self.t('game'))
        game.grid(sticky='ew', pady=(0, 8))
        game.columnconfigure(0, weight=1)
        for row, key in enumerate(('game_dir', 'system_lib_dir', 'texpack_dir')):
            ttk.Label(game, text=self.t(key)).grid(row=row * 2, column=0, columnspan=2, sticky='w')
            var = tk.StringVar(value=self.settings[key])
            var.trace_add('write', lambda *_: self.changed())
            self.vars[key] = var
            ttk.Entry(game, textvariable=var).grid(row=row * 2 + 1, column=0, sticky='ew', pady=(0, 6))
            ttk.Button(game, text=self.t('browse'), command=lambda k=key: self.browse(k)).grid(
                row=row * 2 + 1, column=1, padx=(6, 0), pady=(0, 6))
        self.vars['texpack_dir'].trace_add('write', lambda *_: self.refresh_texture_pack())
        audio = section(left, self.t('audio'))
        audio.grid(sticky='ew')
        self.check(audio, 0, 'native_audio')
        self.build_drool(left)

        lang = section(right, self.t('language'))
        lang.grid(sticky='ew', pady=(0, 8))
        lang.columnconfigure(1, weight=1)
        self.game_lang_box = None
        self.build_game_language(lang)
        self.combo(lang, 1, 'ui_language', UI_LANGUAGES,
                   lambda k: self.t('auto') if k == 'auto' else UI_LANGUAGE_NAMES[k], on_change=self.build)
        self.check(lang, 2, 'skip_logos')
        launcher = section(right, self.t('launcher_section'))
        launcher.grid(sticky='ew', pady=(0, 8))
        ttk.Button(launcher, text=self.t('home_picture'), command=self.pick_background).pack(side='left')
        ttk.Button(launcher, text=self.t('home_picture_reset'), command=self.reset_background).pack(
            side='left', padx=(6, 0))
        ttk.Button(launcher, text=self.t('legacy_menu'), command=self.switch_menu).pack(side='left', padx=(6, 0))
        debug = section(right, self.t('debug'))
        debug.grid(sticky='ew')
        self.check(debug, 0, 'level_select')
        self.hint(debug, 0, 3, 'debug_hint')
        self.check(debug, 2, 'debug_logging')
        self.hint(debug, 2, 3, 'debug_logging_hint')

    def build_drool(self, parent):
        """Trico's drool on the Settings page, with its story in plain sight: the developers built it
        and switched it off just before release (tlg.ini trico_drool; the game reads it live, also from
        the in-game Fun tab)."""
        drool = next(item for item in backports_table.MENU if item.key == 'trico_drool')
        box = section(parent, drool.name[0])
        box.grid(sticky='ew', pady=(8, 0))
        values = [choice[0] for choice in drool.choices]
        lang = 1 if self.pt() else 0
        combo = ttk.Combobox(box, values=[choice[1 + lang] for choice in drool.choices], state='readonly', width=28)
        chosen = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').get('trico_drool', drool.default)
        combo.current(values.index(chosen) if chosen in values else 0)
        combo.grid(row=0, column=0, sticky='w')

        def picked():
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {'trico_drool': values[combo.current()]})
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload

        combo.bind('<<ComboboxSelected>>', lambda _: picked())
        ttk.Label(box, text=drool.what[lang], foreground=DIM, wraplength=420, justify='left').grid(
            row=1, column=0, sticky='w', pady=(6, 0))

    def build_video(self):
        """Video: display, frame rate, resolution, anti-aliasing, frame generation | video memory."""
        win = self.video_window = Page(self, 'video')
        win.title(self.t('home_video'))
        main = scroll_area(win)
        for column in (0, 1):
            main.columnconfigure(column, weight=1, uniform='cols')
        video = section(main, self.t('display'))
        video.grid(row=0, column=0, sticky='new', padx=6, pady=4)
        video.columnconfigure(1, weight=1)
        self.combo(video, 0, 'display_mode', DISPLAY_MODES, lambda k: self.t(k))
        self.check(video, 1, 'vsync')
        self.check(video, 2, 'show_fps')
        self.combo(video, 5, 'frame_rate', tlg_run.FRAME_RATES, lambda k: self.t(f'fps_{k}'))
        self.hint(video, 5, 2, 'fps_hint')
        self.combo(video, 7, 'resolution', tlg_run.RESOLUTIONS, tlg_run.resolution_label)
        self.hint(video, 7, 2, 'resolution_hint')
        ini = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')
        ttk.Label(video, text=self.t('aa_mode')).grid(row=9, column=0, sticky='w', pady=(6, 0))
        self.aa_box = ttk.Combobox(video, values=[self.t(label) for label, _ in AA_MODES],
                                   state='readonly', width=20)
        self.aa_box.current(aa_mode_of(ini))
        self.aa_box.grid(row=9, column=1, sticky='w', padx=6, pady=(6, 2))
        self.aa_box.bind('<<ComboboxSelected>>', lambda _: self.filters_changed())
        self.aa_note = ttk.Label(video, text=self.t('aa_no_dlss'), foreground=DIM, wraplength=320)
        self.aa_note.grid(row=10, column=0, columnspan=2, sticky='w')
        self.update_aa_note(ini)
        ttk.Label(video, text=self.t('fg_mode')).grid(row=11, column=0, sticky='w', pady=(6, 0))
        self.fg_box = ttk.Combobox(video, values=[self.t('aa_off'), 'DLSS', 'FSR 3.1'], state='readonly', width=20)
        self.fg_box.current(fg_mode_of(ini))
        self.fg_box.grid(row=11, column=1, sticky='w', padx=6, pady=(6, 2))
        self.fg_box.bind('<<ComboboxSelected>>', lambda _: self.filters_changed())
        self.hint(video, 11, 2, 'fg_hint')
        self.hdr_var = tk.BooleanVar(value=ini.get('hdr', '1') != '0')

        def hdr_toggled():
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {'hdr': int(self.hdr_var.get())})
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload
            self.refresh_preset()
        ttk.Checkbutton(video, text='HDR', variable=self.hdr_var, command=hdr_toggled).grid(
            row=13, column=0, columnspan=2, sticky='w', pady=(6, 0))
        self.hint(video, 13, 2, 'hdr_hint')

        memory = section(main, self.t('memory'))
        memory.grid(row=0, column=1, sticky='new', padx=6, pady=4)
        ttk.Label(memory, text=self.t('vram')).grid(row=0, column=0, sticky='w')
        vram = tk.IntVar(value=int(self.settings['vram_budget_mb']))
        self.vars['vram_budget_mb'] = vram
        spin = ttk.Spinbox(memory, from_=2000, to=24000, increment=200, textvariable=vram, width=8,
                           command=self.changed)
        spin.grid(row=0, column=1, sticky='w', padx=6)
        spin.bind('<FocusOut>', lambda _: self.changed())
        self.hint(memory, 0, 2, 'vram_hint')

        def ini_check(row, key, default, hint_key):
            # The in-game menu's switches (tlg.ini, apply at once).
            var = tk.BooleanVar(value=ini.get(key, default) != '0')

            def toggled():
                tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
                tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {key: '1' if var.get() else '0'})
                self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload

            ttk.Checkbutton(memory, text=self.t(key), variable=var, command=toggled).grid(
                row=row, column=0, columnspan=2, sticky='w', pady=(4, 0))
            self.hint(memory, row, 2, hint_key)

        ini_check(1, 'texture_cleanup', '0', 'texture_cleanup_hint')
        ini_check(2, 'keep_gpu_textures', '1', 'keep_gpu_textures_hint')
        ini_check(3, 'arena_release', '1', 'arena_release_hint')
        ini_check(4, 'gc_live_budget', '1', 'gc_live_budget_hint')

    def build_logs(self):
        """Logs: the game log as it runs, and the logs folder."""
        win = self.logs_window = Page(self, 'logs')
        header = win.title(self.t('home_logs'))
        ttk.Button(header, text=self.t('logs'), command=lambda: self.open(tlg_run.LOG_DIR)).pack(
            side='right', pady=(6, 0))
        ttk.Button(header, text=self.t('report_issue'), command=self.report_issue).pack(
            side='right', padx=(0, 6), pady=(6, 0))
        tabs = ttk.Notebook(win)
        tabs.pack(fill='both', expand=True, padx=12, pady=(8, 12))
        frame = ttk.Frame(tabs, padding=12)
        tabs.add(frame, text=self.t('tab_log'))
        bench_tab = ttk.Frame(tabs)
        tabs.add(bench_tab, text=self.t('tab_bench'))
        self.bench_window = BenchmarkWindow(self, bench_tab)
        self.log = tk.Text(frame, wrap='none', font=('TkFixedFont', 9), state='disabled',
                           background=FIELD, foreground=MUTED, insertbackground=TEXT_COLOR,
                           highlightthickness=0, relief='flat')
        yscroll = ttk.Scrollbar(frame, command=self.log.yview)
        xscroll = ttk.Scrollbar(frame, orient='horizontal', command=self.log.xview)
        self.log.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        frame.rowconfigure(0, weight=1)
        frame.columnconfigure(0, weight=1)
        self.log.grid(row=0, column=0, sticky='nsew')
        yscroll.grid(row=0, column=1, sticky='ns')
        xscroll.grid(row=1, column=0, sticky='ew')
        for line in getattr(self, 'log_lines', []):   # kept over a language change
            self.append(line, keep=False)

    # -- soundtrack -------------------------------------------------------------------------
    def build_soundtrack(self):
        """Soundtrack: every piece of the game's music (soundtrack.py reads it from the game folder,
        the music tool plays it). The list is read when the page first opens."""
        win = self.soundtrack_window = Page(self, 'soundtrack')
        win.title(self.t('home_soundtrack'), self.t('soundtrack_hint'))
        bar = ttk.Frame(win, padding=(12, 8, 12, 0))
        bar.pack(fill='x')
        self.track_button = ttk.Button(bar, text=self.t('soundtrack_play'), command=self.track_play_pause)
        self.track_button.pack(side='left')
        ttk.Button(bar, text=self.t('soundtrack_next'), command=self.track_next).pack(side='left', padx=(6, 0))
        ttk.Button(bar, text=self.t('soundtrack_stop'), command=self.stop_track).pack(side='left', padx=(6, 0))
        self.track_volume = tk.IntVar(value=int(self.settings.get('soundtrack_volume', 80)))
        ttk.Scale(bar, from_=0, to=100, length=140, variable=self.track_volume,
                  command=lambda _: self.track_send(f'volume {self.track_volume.get()}')).pack(side='right')
        ttk.Label(bar, text=self.t('soundtrack_volume')).pack(side='right', padx=(0, 6))
        # Stereo mixes the game's surround music down; 5.1 sends each channel to its own speaker.
        modes = ('stereo', 'surround')
        speakers = ttk.Combobox(bar, values=[self.t('soundtrack_' + m) for m in modes], state='readonly', width=13)
        mode = self.settings.get('soundtrack_speakers', 'stereo')
        speakers.current(modes.index(mode) if mode in modes else 0)
        speakers.bind('<<ComboboxSelected>>', lambda _: self.track_speakers(modes[speakers.current()]))
        speakers.pack(side='right', padx=(0, 18))
        ttk.Label(bar, text=self.t('soundtrack_speakers')).pack(side='right', padx=(0, 6))
        # Second line: what plays, a progress slider to skip through the piece, and the time.
        now = ttk.Frame(win, padding=(12, 8, 12, 0))
        now.pack(fill='x')
        self.track_label = ttk.Label(now, text='', foreground=MUTED, width=34, anchor='w')
        self.track_label.pack(side='left')
        self.track_time = ttk.Label(now, text='', foreground=MUTED, width=11, anchor='e')
        self.track_time.pack(side='right')
        self.track_pos = tk.DoubleVar(value=0.0)
        self.track_dragging = False
        self.track_bar = ttk.Scale(now, from_=0, to=1, variable=self.track_pos, command=self.track_moved)
        self.track_bar.pack(side='left', fill='x', expand=True, padx=(8, 8))
        self.track_bar.bind('<ButtonPress-1>', lambda _: setattr(self, 'track_dragging', True))
        self.track_bar.bind('<ButtonRelease-1>', lambda _: self.track_seek())
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
        tree = self.track_tree = ttk.Treeview(frame, columns=('length',), selectmode='browse')
        tree.heading('#0', text=self.t('home_soundtrack'), anchor='w')
        tree.heading('length', text=self.t('soundtrack_length'), anchor='e')
        # Fixed columns, the list only as wide as they are: the length stays next to the name.
        tree.column('#0', width=440, stretch=False, anchor='w')
        tree.column('length', width=70, stretch=False, anchor='e')
        scroll = ttk.Scrollbar(frame, command=tree.yview)
        tree.configure(yscrollcommand=scroll.set)
        tree.pack(side='left', fill='y')
        scroll.pack(side='left', fill='y')
        tree.bind('<Double-1>', lambda _: self.track_start(self.track_selected()))
        tree.bind('<Return>', lambda _: self.track_start(self.track_selected()))
        self.track_tracks = None   # read when the page opens (again after a rebuild)
        self.stop_track()          # a rebuild (language change) starts the page fresh

    def load_soundtrack(self):
        game = self.settings.get('game_dir') or ''
        if self.track_tracks is not None and getattr(self, 'track_game', None) == game:
            return
        self.track_game, self.track_tracks = game, []
        self.track_tree.delete(*self.track_tree.get_children())
        self.track_label.configure(text=self.t('soundtrack_loading'))

        def work():
            try:
                tracks = soundtrack.scan(game, tlg_run.user_dir() / 'cache') if game else []
            except Exception:   # never take the launcher down over the music list
                tracks = []
            self.root.after(0, lambda: self.fill_soundtrack(game, tracks))
        threading.Thread(target=work, daemon=True).start()

    def fill_soundtrack(self, game, tracks):
        if game != self.track_game or not self.track_tree.winfo_exists():
            return
        self.track_tracks = tracks
        tree, parent, stage = self.track_tree, '', None
        for index, track in enumerate(tracks):
            if track['stage'] != stage:
                stage = track['stage']
                parent = tree.insert('', 'end', text=stage, open=True)
            text = self.t('soundtrack_piece', n=track['number'])
            if track.get('name'):
                text += f"   ·  {track['name']}"
            tree.insert(parent, 'end', iid=f't{index}', text=text,
                        values=(f"{int(track['seconds']) // 60}:{int(track['seconds']) % 60:02d}",))
        minutes = round(sum(t['seconds'] for t in tracks) / 60)
        if not getattr(self, 'track', None):
            self.track_label.configure(text=self.t('soundtrack_count', count=len(tracks), minutes=minutes)
                                       if tracks else self.t('soundtrack_none'))

    def track_selected(self):
        item = self.track_tree.focus()
        if item.startswith('t'):
            return int(item[1:])
        children = self.track_tree.get_children(item) if item else ()
        return int(children[0][1:]) if children else (0 if self.track_tracks else None)

    def track_speakers(self, mode):
        self.settings['soundtrack_speakers'] = mode
        if getattr(self, 'track', None):   # replay the piece from where it was, in the new mix
            at, paused = self.track_pos.get(), self.track_paused
            self.track_start(self.track_index)
            if getattr(self, 'track', None):
                self.track_send(f'seek {at:.2f}')
                if paused:
                    self.track_play_pause()

    def track_start(self, index):
        tracks = self.track_tracks or []
        if index is None or not 0 <= index < len(tracks) or self.process:
            return
        self.stop_track()
        self.stop_music()
        track, tool = tracks[index], tlg_run.music_tool()
        out = Path(tempfile.gettempdir()) / 'tlg-soundtrack.at9'
        try:
            soundtrack.extract(self.track_game, track, out)
            proc = subprocess.Popen([str(tool), str(out), str(self.track_volume.get()), '--track',
                                     self.settings.get('soundtrack_speakers', 'stereo')],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    env=tlg_run.pad_tool_env(), text=True, bufsize=1,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)) if tool else None
        except Exception:
            proc = None
        if not proc:
            self.track_label.configure(text=self.t('soundtrack_failed'))
            return
        self.track, self.track_index, self.track_paused = proc, index, False
        self.track_text = f"{track['stage']} · {self.t('soundtrack_piece', n=track['number'])}"
        self.track_button.configure(text=self.t('soundtrack_pause'))
        self.track_tree.selection_set(f't{index}')
        self.track_tree.focus(f't{index}')
        self.track_tree.see(f't{index}')

        def read():
            for line in proc.stdout:
                self.root.after(0, lambda line=line: self.track_output(proc, line.split()))
            self.root.after(0, lambda: self.track_output(proc, ['exit']))
        threading.Thread(target=read, daemon=True).start()

    def track_output(self, proc, words):
        if proc is not getattr(self, 'track', None) or not words:
            return
        if words[0] == 'pos' and len(words) == 3:
            done, total = float(words[1]), float(words[2])
            self.track_total = total
            self.track_label.configure(text=self.track_text)
            if not self.track_dragging:   # the slider follows the music unless it is being dragged
                self.track_bar.configure(to=max(total, 0.1))
                self.track_pos.set(done)
                self.track_clock(done)
        elif words[0] in ('end', 'exit'):
            ended = words[0] == 'end'
            self.stop_track()
            if ended:   # on to the next piece, like an album
                self.track_start(self.track_index + 1)
            else:
                self.track_label.configure(text=self.t('soundtrack_failed'))

    def track_clock(self, done):
        total = getattr(self, 'track_total', 0.0)
        self.track_time.configure(text=f"{int(done) // 60}:{int(done) % 60:02d} / {int(total) // 60}:{int(total) % 60:02d}")

    def track_moved(self, _value):
        if self.track_dragging and getattr(self, 'track', None):
            self.track_clock(self.track_pos.get())   # the time shows where a release would jump to

    def track_seek(self):
        self.track_dragging = False
        if getattr(self, 'track', None):
            self.track_send(f'seek {self.track_pos.get():.2f}')
        else:
            self.track_pos.set(0.0)

    def track_send(self, command):
        proc = getattr(self, 'track', None)
        try:
            if proc:
                proc.stdin.write(command + '\n')
                proc.stdin.flush()
        except (OSError, ValueError):
            pass
        if command.startswith('volume'):
            self.settings['soundtrack_volume'] = self.track_volume.get()

    def track_play_pause(self):
        if not getattr(self, 'track', None):
            self.track_start(self.track_selected())
            return
        self.track_paused = not self.track_paused
        self.track_send('pause' if self.track_paused else 'resume')
        self.track_button.configure(text=self.t('soundtrack_play' if self.track_paused else 'soundtrack_pause'))

    def track_next(self):
        if getattr(self, 'track', None):
            self.track_start(self.track_index + 1)
        else:
            self.track_start(self.track_selected())

    def stop_track(self):
        proc, self.track = getattr(self, 'track', None), None
        try:
            self.track_button.configure(text=self.t('soundtrack_play'))
            self.track_label.configure(text='')
            self.track_time.configure(text='')
            self.track_pos.set(0.0)
            self.track_dragging = False
        except (AttributeError, tk.TclError):
            pass
        if not proc:
            return
        try:
            proc.stdin.write('quit\n')
            proc.stdin.flush()
            proc.wait(1)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            proc.kill()

    def build_controls(self):
        """Controls: a Controller tab (pad_page.py) and a Keyboard & mouse tab. The game reads both at once."""
        win = self.controls_window = Page(self, 'controls')
        win.title(self.t('home_controls'))
        tabs = self.controls_tabs = ttk.Notebook(win)
        tabs.pack(fill='both', expand=True, padx=12, pady=(6, 12))
        pad_tab, keys_tab = ttk.Frame(tabs), ttk.Frame(tabs)
        tabs.add(pad_tab, text=self.t('home_controller'))
        tabs.add(keys_tab, text=self.t('home_keys'))
        self.controller = pad_page.ControllerPage(self, scroll_area(pad_tab, padding=0),
                                                  (BG, LINE, TEXT_COLOR, MUTED, ACCENT, FIELD))
        tabs.bind('<<NotebookTabChanged>>', lambda _: self.update_controller())
        frame = scroll_area(keys_tab)
        mouse = section(frame, self.t('mouse_section'), self.t('mouse_hint'))
        mouse.grid(row=0, column=0, sticky='nw', pady=(0, 8))
        mouse.columnconfigure(1, weight=1)
        self.check(mouse, 0, 'mouse_enabled')
        self.check(mouse, 1, 'mouse_invert_y')
        ttk.Label(mouse, text=self.t('mouse_sensitivity')).grid(row=2, column=0, sticky='w', pady=(4, 0))
        sens = tk.DoubleVar(value=float(self.settings.get('mouse_sensitivity', 1.0)))
        self.vars['mouse_sensitivity'] = sens
        sens_spin = ttk.Spinbox(mouse, from_=0.1, to=5.0, increment=0.1, format='%.2f', textvariable=sens,
                                width=6, command=self.changed)
        sens_spin.grid(row=2, column=1, sticky='w', padx=6, pady=(4, 0))
        sens_spin.bind('<FocusOut>', lambda _: self.changed())
        self.combo(mouse, 4, 'menu_confirm', ['cross', 'circle'],
                   lambda k: 'Cross (X / A)' if k == 'cross' else 'Circle (O / B)')
        keys = section(frame, self.t('remap_title'), self.t('remap_help'))
        keys.grid(row=1, column=0, sticky='nw')
        self.build_remap(keys, win)

    def update_controller(self):
        # The helper reads the pads only while the Controller tab shows.
        controller = getattr(self, 'controller', None)
        if not controller:
            return
        if self.page == 'controls' and self.controls_tabs.index('current') == 0:
            controller.start()
        else:
            controller.stop()

    # -- home screen ------------------------------------------------------------------------
    def show_page(self, page):
        """'home' is the full picture; any other page opens in the panel over the faded picture."""
        self.page = page
        if not getattr(self, 'sized', False):
            # Always 16:9: 70% of the screen height (at least 1280x720), the width from that.
            self.sized = True
            sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
            h = max(720, int(sh * 0.7))
            w = h * 16 // 9
            if w > sw:
                w, h = sw, sw * 9 // 16
            self.root.geometry(f'{w}x{h}+{max(0, (sw - w) // 2)}+{max(0, (sh - h) // 3)}')
            self.root.minsize(min(1280, sw), min(720, sh))
            self.root.aspect(16, 9, 16, 9)
        for child in self.panel.pack_slaves():
            child.pack_forget()
        if page != 'home':
            getattr(self, page + '_window').pack(fill='both', expand=True)
        if page == 'soundtrack':
            self.load_soundtrack()
        self.update_controller()
        self.draw_home()

    def schedule_home(self):
        # Redraw once the window stops changing size (the fade is rendered per size).
        if getattr(self, 'home_job', None):
            self.root.after_cancel(self.home_job)
        self.home_job = self.root.after(120, self.draw_home)

    def draw_home(self):
        self.home_job = None
        c = self.home
        width, height = c.winfo_width(), c.winfo_height()
        if width <= 1:
            width, height = 1280, 760
        home = self.page == 'home'
        c.delete('all')
        self.home_items = {}
        self.home_art = home_background(background_source(self.settings), (width, height), faded=not home)
        if self.home_art is not None:
            c.create_image(0, 0, image=self.home_art, anchor='nw')
        left = 64 if home else 48
        top = 92 if home else 56
        c.create_text(left, top, text='T H E   L A S T', anchor='w', fill=MUTED,
                      font=(SERIF, 14 if home else 11), tags='title')
        c.create_text(left, top + (44 if home else 30), text='G U A R D I A N', anchor='w', fill=TEXT_COLOR,
                      font=(SERIF, 34 if home else 22), tags='title')
        line_y = top + (84 if home else 56)
        c.create_line(left, line_y, left + 46, line_y, fill=ACCENT)
        if home:
            c.create_text(left, 198, text='\u4eba\u55b0\u3044\u306e\u5927\u9df2\u30c8\u30ea\u30b3', anchor='w',
                          fill=MUTED, font=('Yu Mincho' if sys.platform == 'win32' else 'serif', 11))
        else:
            c.tag_bind('title', '<Button-1>', lambda _: self.show_page('home'))
            c.tag_bind('title', '<Enter>', lambda _: c.configure(cursor='hand2'))
            c.tag_bind('title', '<Leave>', lambda _: c.configure(cursor=''))
        items = [('home_settings', 'settings', lambda: self.show_page('settings')),
                 ('home_video', 'video', lambda: self.show_page('video')),
                 ('home_graphics', 'graphics', self.graphics_dialog),
                 ('home_controls', 'controls', self.controller_dialog),
                 ('home_advanced', 'advanced', self.advanced_dialog),
                 ('home_start', 'start', self.start_dialog),
                 ('home_soundtrack', 'soundtrack', lambda: self.show_page('soundtrack')),
                 ('home_logs', 'logs', lambda: self.show_page('logs')),
                 ('home_quit', None, self.quit)]
        first = 250 if home else 170
        step = min(38 if home else 36, max(28, (height - first - 120) // len(items)))
        for index, (key, page, action) in enumerate(items):
            y = first + index * step
            on = page is not None and page == self.page
            item = c.create_text(left + (16 if on else 0), y, text=self.t(key), anchor='w',
                                 fill=TEXT_COLOR if on else MUTED,
                                 font=(SERIF, 14 if on else 13 if home else 12))
            if on:
                c.create_oval(left, y - 3, left + 6, y + 3, fill=ACCENT, outline='')
                continue
            dot = c.create_oval(left - 16, y - 3, left - 10, y + 3, fill=ACCENT, outline='', state='hidden')
            c.tag_bind(item, '<Enter>', lambda _, i=item, d=dot: (c.itemconfig(i, fill=TEXT_COLOR),
                                                                  c.itemconfig(d, state='normal'),
                                                                  c.configure(cursor='hand2')))
            c.tag_bind(item, '<Leave>', lambda _, i=item, d=dot: (c.itemconfig(i, fill=MUTED),
                                                                  c.itemconfig(d, state='hidden'),
                                                                  c.configure(cursor='')))
            c.tag_bind(item, '<Button-1>', lambda _, a=action: a())
        play_text = ' '.join(self.t('play').upper())
        if home:
            status_y, cache_y, status_width = height - 60, height - 34, max(200, width - 360)
            right, mid = width - 64, height - 60
            box = c.create_rectangle(right - 190, mid - 24, right, mid + 24, outline=ACCENT, width=1, fill='')
            label = c.create_text(right - 95, mid, text=play_text, fill=ACCENT, font=(SERIF, 15))
        else:
            status_y, cache_y, status_width = height - 100, height - 76, 230
            box = c.create_rectangle(left, height - 56, left + 190, height - 16, outline=ACCENT, width=1,
                                     fill=ACCENT)
            label = c.create_text(left + 95, height - 36, text=play_text, fill=BG, font=(SERIF, 13))
            c.create_window(300, 40, window=self.panel, anchor='nw', width=width - 340, height=height - 80)
        self.home_items['status'] = c.create_text(left, status_y, text='', anchor='w', fill=MUTED,
                                                  font=(SERIF, 11 if home else 10, 'italic'), width=status_width)
        c.create_text(left, cache_y, text=self.t('home_cache'), anchor='w', fill=DIM, font=(SERIF, 9),
                      tags='cache')
        c.tag_bind('cache', '<Button-1>', lambda _: self.clear_cache())
        # Menu music on/off, next to the cache link (saved at once).
        music = c.create_text(left + 150, cache_y, anchor='w', fill=DIM, font=(SERIF, 9), tags='music',
                              text=self.t('home_music_on' if self.settings.get('menu_music', True) else 'home_music_off'))
        c.tag_bind('music', '<Enter>', lambda _: (c.itemconfig(music, fill=ACCENT), c.configure(cursor='hand2')))
        c.tag_bind('music', '<Leave>', lambda _: (c.itemconfig(music, fill=DIM), c.configure(cursor='')))
        c.tag_bind('music', '<Button-1>', lambda _: self.toggle_music(music))
        self.home_items.update(play_box=box, play_label=label)
        # A small square next to PLAY copies the start command (Steam, SteamOS).
        if home:
            x0, y0 = right - 190 - 54, mid - 24
        else:
            x0, y0 = left + 198, height - 56
        size = 48 if home else 40
        c.create_rectangle(x0, y0, x0 + size, y0 + size, outline=DIM, width=1, fill='', tags='copycmd')
        c.create_text(x0 + size / 2, y0 + size / 2, text='⧉', fill=MUTED, font=(SERIF, 14 if home else 12),
                      tags=('copycmd', 'copycmd_glyph'))
        tip = []

        def enter(event):
            c.itemconfig('copycmd_glyph', fill=ACCENT)
            c.configure(cursor='hand2')
            if not tip:
                win = tk.Toplevel(c)
                win.overrideredirect(True)
                tk.Label(win, text=self.t('copy_command_tip'), background=PANEL, foreground=TEXT_COLOR,
                         relief='solid', borderwidth=1, wraplength=320, justify='left', padx=6, pady=4).pack()
                win.update_idletasks()
                x = min(event.x_root + 14, c.winfo_screenwidth() - win.winfo_reqwidth() - 4)
                win.geometry(f'+{x}+{event.y_root - win.winfo_reqheight() - 10}')
                tip.append(win)

        def leave(_):
            c.itemconfig('copycmd_glyph', fill=MUTED)
            c.configure(cursor='')
            while tip:
                tip.pop().destroy()
        c.tag_bind('copycmd', '<Enter>', enter)
        c.tag_bind('copycmd', '<Leave>', leave)
        c.tag_bind('copycmd', '<Button-1>', lambda _: self.copy_command())
        for tag in (box, label):
            c.tag_bind(tag, '<Enter>', lambda _: self.hover_play(True))
            c.tag_bind(tag, '<Leave>', lambda _: self.hover_play(False))
            c.tag_bind(tag, '<Button-1>', lambda _: self.play() if self.play_ready() else None)
        self.refresh_home()

    def play_ready(self):
        return str(self.play_button.cget('state')) == 'normal'

    def hover_play(self, on):
        ready = self.play_ready()
        on = on and ready
        if self.page != 'home':
            self.home.itemconfig(self.home_items['play_box'], fill='#f0cc93' if on else ACCENT if ready else LINE)
        else:
            self.home.itemconfig(self.home_items['play_box'], fill=ACCENT if on else '')
            self.home.itemconfig(self.home_items['play_label'], fill=BG if on else ACCENT if ready else DIM)
        self.home.configure(cursor='hand2' if on else '')

    def refresh_home(self):
        if (not getattr(self, 'home_items', None) or 'status' not in self.home_items
                or not self.status.winfo_exists() or not self.home.winfo_exists()):
            return
        text = self.status.cget('text')
        if self.process:
            text = f"{self.t('home_running')}  ·  {text}"
        self.home.itemconfig(self.home_items['status'], text=text)
        ready = self.play_ready()
        if self.page != 'home':
            self.home.itemconfig(self.home_items['play_box'], outline=ACCENT if ready else LINE,
                                 fill=ACCENT if ready else LINE)
            self.home.itemconfig(self.home_items['play_label'], fill=BG if ready else DIM)
        else:
            self.home.itemconfig(self.home_items['play_box'], outline=ACCENT if ready else DIM, fill='')
            self.home.itemconfig(self.home_items['play_label'], fill=ACCENT if ready else DIM)

    def pick_background(self):
        path = filedialog.askopenfilename(title=self.t('home_picture'),
                                          filetypes=[('Images', '*.png *.jpg *.jpeg')])
        if not path:
            return
        tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
        self.reset_background(redraw=False)
        suffix = '.jpg' if path.lower().endswith(('.jpg', '.jpeg')) else '.png'
        target = tlg_run.USER_DIR / ('launcher_background' + suffix)
        target.write_bytes(Path(path).read_bytes())  # a copy: the picture stays the player's own
        self.draw_home()

    def reset_background(self, redraw=True):
        for name in BACKGROUND_NAMES:
            (tlg_run.USER_DIR / name).unlink(missing_ok=True)
        if redraw:
            self.draw_home()

    def load_art(self):
        """Window icon (the port's tlg.png) and a 480x135 banner (a band of pic1.png from the dump, when present)."""
        self.banner = getattr(self, 'banner', None)
        if getattr(self, 'art_loaded', False):
            return
        self.art_loaded = True
        try:
            icon = tlg_run.PORT_DIR / 'launcher' / 'tlg.png'  # the port's own icon
            if icon.is_file():
                self.icon = tk.PhotoImage(file=str(icon))
                self.root.iconphoto(True, self.icon)
            picture = art_path(self.settings, 'pic1.png') or art_path(self.settings, 'pic0.png')
            if picture:
                full = tk.PhotoImage(file=str(picture))
                band = full.width() * 9 // 32           # a 32:9 band from the middle of the image
                top = (full.height() - band) // 2
                self.banner = tk.PhotoImage()
                self.banner.tk.call(self.banner, 'copy', full, '-from', 0, top, full.width(), top + band,
                                    '-subsample', 4, 4)  # Tk command: same on every Python version
        except tk.TclError:
            self.banner = None

    def build_filters(self, parent, row):
        """SMAA, sharpening and color adjustments, read from and written to tlg.ini right away."""
        ttk.Separator(parent).grid(row=row + 1, column=0, columnspan=3, sticky='ew', pady=(8, 4))
        ini = dict(INI_DEFAULTS)
        ini.update({k: v for k, v in tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').items() if k in ini})
        self.ini_vars = {}
        for offset, key in ((2, 'smaa'), (4, 'color_adjust')):
            var = tk.BooleanVar(value=ini[key] not in ('0', ''))
            self.ini_vars[key] = var
            cell = ttk.Frame(parent)
            cell.grid(row=row + offset, column=0, columnspan=2, sticky='w', pady=2)
            box = ttk.Checkbutton(cell, text=self.t(key), variable=var, command=self.filters_changed)
            box.pack(side='left')
            if key == 'smaa':
                self.hint_mark(cell, 'smaa_hint', box).pack(side='left', padx=(4, 0))
        ttk.Label(parent, text=self.t('sharpening')).grid(row=row + 3, column=0, sticky='w')
        cell = ttk.Frame(parent)
        cell.grid(row=row + 3, column=1, sticky='w', padx=6, pady=2)
        self.sharpener = ttk.Combobox(cell, values=[self.t('sharpen_' + k) for k in SHARPENERS],
                                      state='readonly', width=16)
        self.sharpener.current(2 if ini['cas'] not in ('0', '') else 1 if ini['rcas'] not in ('0', '') else 0)
        self.sharpener.pack(side='left')
        self.sharpener.bind('<<ComboboxSelected>>', lambda _: self.filters_changed())
        # Strength of the picked sharpener (rcas_strength or cas_strength), steps of 5.
        self.sharpen_shown = ttk.Label(cell, width=5)
        self.sharpen_strength = ttk.Scale(cell, from_=0, to=100, length=110, command=self.strength_moved)
        self.sharpen_strength.pack(side='left', padx=(8, 4))
        self.sharpen_shown.pack(side='left')
        self.update_strength(ini)
        for index, key in enumerate(COLOR_RANGES):
            low, high = COLOR_RANGES[key]
            ttk.Label(parent, text=self.t(key)).grid(row=row + 5 + index, column=0, sticky='w', padx=(16, 0))
            var = tk.StringVar(value=ini[key])
            self.ini_vars[key] = var
            spin = ttk.Spinbox(parent, from_=low, to=high, increment=0.05, format='%.2f', textvariable=var,
                               width=6, command=self.filters_changed)
            spin.grid(row=row + 5 + index, column=1, sticky='w', padx=6, pady=1)
            spin.bind('<FocusOut>', lambda _: self.filters_changed())

    def build_preset(self, parent, row):
        """The in-game menu's graphics presets (preset_share.game_presets): every look setting and the
        texture pack at once, written to tlg.ini."""
        ttk.Label(parent, text=self.t('graphics_preset')).grid(row=row + 1, column=0, sticky='w')
        self.preset_box = ttk.Combobox(parent, state='readonly', width=28)
        self.preset_box.grid(row=row + 1, column=1, sticky='w', padx=6, pady=2)
        self.preset_box.bind('<<ComboboxSelected>>', lambda _: self.preset_picked())
        # Save as preset: the current tlg.ini under a name, the same file the in-game menu writes
        # (an existing preset with the name is replaced).
        ttk.Label(parent, text=self.t('preset_save')).grid(row=row + 2, column=0, sticky='w')
        save = ttk.Frame(parent)
        save.grid(row=row + 2, column=1, sticky='w', padx=6, pady=2)
        name = tk.StringVar()
        ttk.Entry(save, textvariable=name, width=20).pack(side='left')

        def save_preset():
            ini = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')
            saved = preset_share.save_game_preset(tlg_run.USER_DIR / 'presets', name.get(), ini)
            if saved is None:
                messagebox.showwarning(self.t('preset_save'), self.t('preset_save_bad'), parent=self.root)
                return
            name.set('')
            self.ini_stamp = self.ini_files_stamp()
            self.refresh_preset()

        ttk.Button(save, text=self.t('preset_save_button'), command=save_preset).pack(side='left', padx=(6, 0))
        self.hint(parent, row + 1, 2, 'graphics_preset_hint')
        # Texture pack: tlg.ini texture_pack (a pack folder's name or off), the in-game Image row.
        ttk.Label(parent, text=self.t('texture_pack')).grid(row=row + 4, column=0, sticky='w', pady=(6, 0))
        self.pack_box = ttk.Combobox(parent, state='readonly', width=24)
        self.pack_box.grid(row=row + 4, column=1, sticky='w', padx=6, pady=(6, 2))
        self.pack_box.bind('<<ComboboxSelected>>', lambda _: self.texture_pack_picked())
        self.hint(parent, row + 4, 2, 'texture_pack_hint')
        self.refresh_preset()
        self.refresh_texture_pack()
        self.ini_stamp = self.ini_files_stamp()
        if not getattr(self, 'watching_ini', False):  # build() runs again on a language change
            self.watching_ini = True
            self.root.after(1000, self.watch_ini)

    def ini_files_stamp(self):
        """Changes when tlg.ini or the presets folder changes (the in-game menu saves them)."""
        paths = [tlg_run.USER_DIR / 'tlg.ini', *sorted((tlg_run.USER_DIR / 'presets').glob('*.ini'))]
        return tuple((str(path), path.stat().st_mtime_ns) for path in paths if path.is_file())

    def watch_ini(self):
        """Shows what the game saved in tlg.ini or the presets folder while the launcher is open."""
        stamp = self.ini_files_stamp()
        if stamp != self.ini_stamp:
            self.ini_stamp = stamp
            self.reload_ini()
        self.root.after(1000, self.watch_ini)

    def reload_ini(self):
        """The rows that show tlg.ini values read it again."""
        ini = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')
        self.aa_box.current(aa_mode_of(ini))
        self.fg_box.current(fg_mode_of(ini))
        self.update_aa_note(ini)
        self.hdr_var.set(ini.get('hdr', '1') != '0')
        # Its sliders show the old values; rebuilt after, not from inside one of its own events.
        self.root.after_idle(self.rebuild_graphics)

    def rebuild_graphics(self):
        if not self.graphics_body.winfo_exists():
            return
        for child in self.graphics_body.winfo_children():
            child.destroy()
        self.build_graphics()

    def refresh_texture_pack(self):
        """Lists the packs in the texture packs folder and shows the one tlg.ini picks."""
        if not getattr(self, 'pack_box', None):
            return  # build() is still making the rows
        # The folder row's text, not self.settings: this runs from that row's trace too.
        folder = {**self.settings, 'texpack_dir': self.vars['texpack_dir'].get()}
        self.texture_packs = tlg_run.texture_packs(tlg_run.texpack_dir(folder))
        self.pack_box.configure(values=[self.t('texture_pack_off'), *self.texture_packs],
                                postcommand=self.refresh_texture_pack)
        current = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').get('texture_pack', 'off')
        if current.isdigit():  # older tlg.ini: a 1-based index
            current = self.texture_packs[int(current) - 1] if 0 < int(current) <= len(self.texture_packs) else 'off'
        self.pack_box.set(current if current in self.texture_packs else self.t('texture_pack_off'))

    def texture_pack_picked(self):
        index = self.pack_box.current()
        if index < 0:
            return
        tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
        tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini',
                           {'texture_pack': self.texture_packs[index - 1] if index else 'off'})
        self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload
        self.refresh_preset()

    def preset_label(self, index, name):
        return self.t('preset_stock') if index == 0 else self.t('preset_recommended') if index == 1 else name

    def refresh_preset(self):
        """Lists the presets (the folder may have new ones) and shows the one tlg.ini equals."""
        self.presets = preset_share.game_presets(tlg_run.USER_DIR / 'presets')
        ini = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')
        names = [self.preset_label(i, name) for i, (name, _) in enumerate(self.presets)]
        self.preset_box.configure(values=names, postcommand=self.refresh_preset)
        current = next((i for i, (_, values) in enumerate(self.presets)
                        if preset_share.game_preset_matches(ini, values)), -1)
        self.preset_box.set(names[current] if current >= 0 else self.t('preset_custom'))

    def preset_picked(self):
        index = self.preset_box.current()
        if index < 0:
            return
        tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
        tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', self.presets[index][1])
        self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload
        self.reload_ini()

    def filters_changed(self):
        values = {'smaa': int(self.ini_vars['smaa'].get()), 'color_adjust': int(self.ini_vars['color_adjust'].get()),
                  'rcas': int(self.sharpener.current() == 1), 'cas': int(self.sharpener.current() == 2)}
        values['frame_generation'] = max(self.fg_box.current(), 0)
        aa = self.aa_box.current()
        current_ini = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')
        self.update_aa_note(current_ini)
        self.update_strength(current_ini)
        if aa >= 0 and aa != aa_mode_of(current_ini):
            picked = AA_MODES[aa][1]
            if picked is None:
                values['dlaa'] = 0  # F8 or the row brings the upscaler choice back
            else:
                values.update(dlaa=picked[0], upscaler=picked[1], preset=0, aa_row=1)
        for key, (low, high) in COLOR_RANGES.items():
            try:
                value = min(max(float(self.ini_vars[key].get().replace(',', '.')), low), high)
            except ValueError:
                continue  # while typing
            values[key] = f'{value:.2f}'
        tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
        tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', values)
        self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload
        self.refresh_preset()

    def combo(self, parent, row, key, values, label, on_change=None):
        ttk.Label(parent, text=self.t(key)).grid(row=row, column=0, sticky='w')
        names = [label(v) for v in values]
        box = ttk.Combobox(parent, values=names, state='readonly', width=28)
        current = self.settings.get(key, values[0])
        box.current(values.index(current) if current in values else 0)
        box.grid(row=row, column=1, sticky='w', padx=6, pady=2)

        def picked(_):
            self.settings[key] = values[box.current()]
            tlg_run.save_settings(self.settings)
            if on_change:
                on_change()
            else:
                self.refresh_status()
        box.bind('<<ComboboxSelected>>', picked)
        return box

    # -- game language (depends on the loaded CUSA) ---------------------------------------------
    def build_game_language(self, parent):
        """Game language: the ones the loaded CUSA ships (by its title id), US by default."""
        ttk.Label(parent, text=self.t('game_language')).grid(row=0, column=0, sticky='w')
        self.game_lang_box = ttk.Combobox(parent, state='readonly', width=28)
        self.game_lang_box.grid(row=0, column=1, sticky='w', padx=6, pady=2)
        self.game_lang_box.bind('<<ComboboxSelected>>', self.picked_language)
        self.refresh_language_choices()

    def picked_language(self, _=None):
        values = self.game_lang_values
        self.settings['game_language'] = values[self.game_lang_box.current()]
        tlg_run.save_settings(self.settings)
        self.refresh_status()

    def refresh_language_choices(self):
        """Reload the combo for the game folder: another CUSA ships another set of languages."""
        if getattr(self, 'game_lang_box', None) is None:
            return
        values = [key for key in tlg_run.game_language_choices(self.settings) if key != 'auto']
        self.game_lang_values = values
        self.game_lang_box.configure(values=[GAME_LANGUAGE_NAMES.get(v, v) for v in values])
        current = self.settings.get('game_language')
        if current not in values:
            current = tlg_run.default_language(self.settings.get('game_dir'))
        if current not in values:
            current = values[0]
        self.game_lang_box.current(values.index(current))
        if current != self.settings.get('game_language'):
            self.settings['game_language'] = current
            tlg_run.save_settings(self.settings)

    def controller_dialog(self):
        self.show_page('controls')

    def remap_dialog(self):
        self.controls_tabs.select(1)
        self.show_page('controls')

    def build_remap(self, frame, win):
        """PS4 button -> keys and mouse buttons (tlg.ini key_map through tlg_run.launch)."""
        mapping = dict(self.settings.get('key_map') or {})
        entries = {}
        half = (len(PS4_BUTTONS) + 1) // 2
        for column in (0, 4):
            ttk.Label(frame, text=self.t('button'), font=('TkDefaultFont', 9, 'bold')).grid(row=0, column=column, sticky='w')
            ttk.Label(frame, text=self.t('keys'), font=('TkDefaultFont', 9, 'bold')).grid(row=0, column=column + 1, sticky='w')
            ttk.Label(frame, text=self.t('default'), font=('TkDefaultFont', 9, 'bold')).grid(row=0, column=column + 3, sticky='w')

        def save(*_):
            self.settings['key_map'] = {b: e.get().strip() for b, e in entries.items() if e.get().strip()}
            tlg_run.save_settings(self.settings)

        def capture(button, entry, set_button):
            set_button.configure(text=self.t('press'))
            win.focus_set()

            def done(name):
                for sequence in ('<KeyPress>', '<ButtonPress>'):
                    win.unbind(sequence)
                set_button.configure(text=self.t('set'))
                if name:
                    entry.delete(0, 'end')
                    entry.insert(0, name)
                    save()

            def key(event):
                if event.keysym == 'Escape':
                    done(None)
                else:
                    done(sdl_key_name(event.keysym))
                return 'break'

            def mouse(event):
                done(TK_MOUSE.get(event.num))
                return 'break'
            win.after(150, lambda: (win.bind('<KeyPress>', key), win.bind('<ButtonPress>', mouse)))

        for index, (button, name, action) in enumerate(PS4_BUTTONS):
            row, column = index % half + 1, 0 if index < half else 4
            label = name if not action else f'{name} — {action[1] if self.pt() else action[0]}'
            ttk.Label(frame, text=label).grid(row=row, column=column, sticky='w', padx=(0, 6), pady=1)
            entry = ttk.Entry(frame, width=20)
            entry.insert(0, mapping.get(button, ''))
            entry.grid(row=row, column=column + 1, sticky='ew', pady=1)
            entry.bind('<FocusOut>', save)
            entry.bind('<Return>', save)
            entries[button] = entry
            set_button = ttk.Button(frame, text=self.t('set'), width=10)
            set_button.configure(command=lambda b=button, e=entry, s=set_button: capture(b, e, s))
            set_button.grid(row=row, column=column + 2, padx=4, pady=1)
            ttk.Label(frame, text=tlg_run.DEFAULT_KEYS[button], foreground='#888').grid(
                row=row, column=column + 3, sticky='w', padx=(0, 18))
        bottom = ttk.Frame(frame)
        bottom.grid(row=half + 2, column=0, columnspan=8, sticky='ew', pady=(10, 0))

        def reset():
            for entry in entries.values():
                entry.delete(0, 'end')
            save()
        ttk.Button(bottom, text=self.t('reset'), command=reset).pack(side='left')

    def mods_dialog(self):
        """Mods/cheats (shadPS4/GoldHEN XML in tlg_run.mods_dir()); toggles write mods.json
        through tlg_run.set_mod_enabled. Nothing here is on unless the player turns it on."""
        if getattr(self, 'mods_window', None) and self.mods_window.winfo_exists():
            self.mods_window.lift()
            return
        win = self.mods_window = Page(self, 'mods')
        win.title(self.t('mods_title'), self.t('mods_hint'))
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
        self.show_page('mods')
        ttk.Label(frame, text=self.t('mods_warning'), foreground='#d07060', wraplength=900, justify='left').pack(
            anchor='w', pady=(4, 8))

        list_frame = ttk.Frame(frame)
        list_frame.pack(fill='both', expand=True)
        canvas = tk.Canvas(list_frame, highlightthickness=0, background=BG, yscrollincrement=24)
        scroll = ttk.Scrollbar(list_frame, orient='vertical', command=canvas.yview)
        rows = ttk.Frame(canvas)
        canvas.scroll_inner = rows
        rows.bind('<Configure>', lambda _: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=rows, anchor='nw')
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')

        def toggle(key, var):
            tlg_run.set_mod_enabled(key, var.get())

        def populate():
            for child in rows.winfo_children():
                child.destroy()
            try:
                mods = tlg_run.list_mods(self.settings)
            except Exception as error:  # a malformed XML file is reported, never crashes the launcher
                mods = []
                ttk.Label(rows, text=str(error), foreground='#a33').pack(anchor='w')
            if not mods:
                ttk.Label(rows, text=self.t('mods_empty'), foreground=DIM).pack(anchor='w', pady=8)
            for key, name, note, enabled in mods:
                row = ttk.Frame(rows)
                row.pack(fill='x', anchor='w', pady=2)
                var = tk.BooleanVar(value=enabled)
                ttk.Checkbutton(row, text=name or key, variable=var,
                                command=lambda k=key, v=var: toggle(k, v)).pack(anchor='w')
                if note:
                    ttk.Label(row, text=note, foreground=DIM, wraplength=480, justify='left').pack(
                        anchor='w', padx=(22, 0))
        populate()

        bottom = ttk.Frame(frame)
        bottom.pack(fill='x', pady=(10, 0))
        ttk.Button(bottom, text=self.t('mods_folder'), command=lambda: self.open(tlg_run.mods_dir())).pack(side='left')
        ttk.Button(bottom, text=self.t('mods_refresh'), command=populate).pack(side='left', padx=6)

    def graphics_dialog(self):
        self.show_page('graphics')

    def build_graphics(self):
        """Graphics: preset, texture pack and image filters | HDR, shadows, grass and textures on
        top; the game's effect strengths below. All tlg.ini, applied at once (the in-game menu's, Insert)."""
        frame = self.graphics_body
        frame.columnconfigure((0, 1), weight=1, uniform='cols')
        ini = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')

        def number(key, default):
            try:
                return int(ini.get(key, default))
            except ValueError:
                return default

        def write(values):
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', values)
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload
            self.refresh_preset()

        # Each "?" starts with the row's default and, where known, its value on PS4 (same as the
        # in-game menu). It sits at the end of the row above (the setting's own row).
        def hint(parent, row, key, default, ps4=None):
            marks = self.t('mark_default', value=default) + ('' if ps4 is None else f' · PS4: {ps4}')
            help_mark(parent, marks + '\n\n' + self.t(key)).grid(row=row - 1, column=4, sticky='w', padx=(4, 0), pady=2)

        on_off = lambda on: self.t('mark_on' if on else 'mark_off')
        image = section(frame, self.t('image'), self.t('filters'))
        image.grid(row=0, column=0, sticky='nsew', padx=(0, 8), pady=(0, 8))
        image.columnconfigure(1, weight=1)
        self.build_preset(image, 0)
        self.build_filters(image, 10)

        scene = section(frame, self.t('scene_section'))
        scene.grid(row=0, column=1, sticky='nsew', padx=(8, 0), pady=(0, 8))
        scene.columnconfigure(1, weight=1)
        ttk.Label(scene, text=self.t('shadow_scale')).grid(row=3, column=0, sticky='w')
        shadow = ttk.Combobox(scene, values=[self.t('shadow_game'), '4096', '8192'], state='readonly', width=14)
        scale = number('shadow_scale', 1)
        shadow.current(2 if scale >= 4 else 1 if scale >= 2 else 0)
        shadow.grid(row=3, column=1, sticky='w', padx=6, pady=2)
        shadow.bind('<<ComboboxSelected>>', lambda _: write({'shadow_scale': SHADOW_SCALES[shadow.current()]}))
        hint(scene, 4, 'shadow_hint', self.t('shadow_game'), self.t('shadow_game'))
        grass = tk.BooleanVar(value=number('grass_smooth', 1) != 0)
        ttk.Checkbutton(scene, text=self.t('grass_smooth'), variable=grass,
                        command=lambda: write({'grass_smooth': int(grass.get())})).grid(
            row=5, column=0, columnspan=4, sticky='w')
        hint(scene, 6, 'grass_hint', on_off(True), on_off(False))
        # Textures (the in-game menu's Image tab): LOD bias, anisotropic filtering.
        row = 7
        ttk.Separator(scene).grid(row=row, column=0, columnspan=5, sticky='ew', pady=(8, 4))
        ttk.Label(scene, text=self.t('textures'), foreground=MUTED).grid(row=row + 1, column=0, columnspan=4, sticky='w')
        bias_text = lambda quarters: f'{quarters * 0.25:+.2f}'
        ttk.Label(scene, text=self.t('lod_bias')).grid(row=row + 2, column=0, sticky='w')
        bias = min(max(number('lod_bias', LOD_BIAS_DEFAULT), -12), 4)
        bias_shown = ttk.Label(scene, text=bias_text(bias), width=5)
        bias_shown.grid(row=row + 2, column=2, sticky='e')

        def bias_moved(raw, current={'value': bias}):
            step = int(round(float(raw)))
            bias_shown.configure(text=bias_text(step))
            if step != current['value']:
                current['value'] = step
                write({'lod_bias': step})

        bias_slider = ttk.Scale(scene, from_=-12, to=4, value=bias, length=160, command=bias_moved)
        bias_slider.grid(row=row + 2, column=1, sticky='ew', padx=6)
        buttons = ttk.Frame(scene)
        buttons.grid(row=row + 2, column=3, sticky='e')
        for text, value in ((self.t('mark_default_button'), LOD_BIAS_DEFAULT), ('PS4', 0)):
            ttk.Button(buttons, text=text, width=len(text) + 2,
                       command=lambda value=value: (bias_slider.set(value), bias_moved(value))).pack(side='left', padx=(2, 0))
        hint(scene, row + 3, 'lod_bias_hint', bias_text(LOD_BIAS_DEFAULT), bias_text(0))
        ttk.Label(scene, text=self.t('aniso')).grid(row=row + 4, column=0, sticky='w')
        aniso_labels = [self.t('aniso_game'), '2x', '4x', '8x', '16x']
        aniso = ttk.Combobox(scene, values=aniso_labels, state='readonly', width=14)
        level = number('aniso', 16)
        aniso.current(max(i for i, v in enumerate(ANISO_LEVELS) if v <= level) if level > 0 else 0)
        aniso.grid(row=row + 4, column=1, sticky='w', padx=6, pady=2)
        aniso.bind('<<ComboboxSelected>>', lambda _: write({'aniso': ANISO_LEVELS[aniso.current()]}))
        hint(scene, row + 5, 'aniso_hint', '16x', aniso_labels[0])

        # The game's effects, in two columns below.
        effects = section(frame, self.t('effects'))
        effects.grid(row=1, column=0, columnspan=2, sticky='ew')
        effects.columnconfigure((0, 1), weight=1, uniform='fx')
        columns = ttk.Frame(effects), ttk.Frame(effects)
        for index, column in enumerate(columns):
            column.grid(row=0, column=index, sticky='nsew', padx=(0, 16) if index == 0 else (16, 0))
            column.columnconfigure(1, weight=1)
        half = (len(EFFECTS) + 1) // 2
        sliders = {}
        for index, (key, (default, low, high, *unit)) in enumerate(EFFECTS.items()):
            unit = unit[0] if unit else '%'
            parent = columns[0 if index < half else 1]
            row = 1 + index % half * 2
            value = min(max(number(key, default), low), high)
            ttk.Label(parent, text=self.t('effect_' + key)).grid(row=row, column=0, sticky='w')
            shown = ttk.Label(parent, text=f'{value}{unit}', width=5)
            shown.grid(row=row, column=2, sticky='e')

            def moved(raw, key=key, shown=shown, unit=unit, current={'value': value}):
                step = int(round(float(raw) / 5) * 5)  # steps of 5
                shown.configure(text=f'{step}{unit}')
                if step != current['value']:
                    current['value'] = step
                    write({key: step})

            slider = ttk.Scale(parent, from_=low, to=high, value=value, length=160, command=moved)
            slider.grid(row=row, column=1, sticky='ew', padx=6, pady=3)
            sliders[key] = (slider, moved)
            # Default and PS4 buttons, as in the in-game menu (the effects' defaults are as shipped).
            buttons = ttk.Frame(parent)
            buttons.grid(row=row, column=3, sticky='e')
            for text in (self.t('mark_default_button'), 'PS4'):
                ttk.Button(buttons, text=text, width=len(text) + 2,
                           command=lambda slider=slider, moved=moved, default=default: (
                               slider.set(default), moved(default))).pack(side='left', padx=(2, 0))
            hint(parent, row + 1, 'effect_' + key + '_hint', f'{default}{unit}', f'{default}{unit}')

        def reset():
            shadow.current(0)
            grass.set(True)
            aniso.current(len(ANISO_LEVELS) - 1)
            write({'shadow_scale': 1, 'grass_smooth': 1, 'aniso': 16, 'texture_pack': 'off'})
            self.refresh_texture_pack()
            bias_slider.set(LOD_BIAS_DEFAULT)
            bias_moved(LOD_BIAS_DEFAULT)
            for key, (slider, moved) in sliders.items():
                slider.set(EFFECTS[key][0])
                moved(EFFECTS[key][0])

        bottom = ttk.Frame(frame)
        bottom.grid(row=2, column=0, columnspan=2, sticky='ew', pady=(10, 0))
        ttk.Button(bottom, text=self.t('reset'), command=reset).pack(side='left')

    def advanced_dialog(self):
        """The GPU readback switches (backports_table.READBACK, the readback mode, the in-game
        readback batch). Each row shows its default and why; graphics presets never set them."""
        if getattr(self, 'advanced_window', None) and self.advanced_window.winfo_exists():
            self.advanced_window.lift()
            return
        win = self.advanced_window = Page(self, 'advanced')
        win.title(self.t('advanced_title'), self.t('advanced_hint'))
        frame = scroll_area(win)
        self.show_page('advanced')
        lang = int(self.pt())
        columns = ttk.Frame(frame), ttk.Frame(frame)
        for index, column in enumerate(columns):
            column.grid(row=1, column=index, sticky='nw', padx=(0 if index == 0 else 10, 0 if index == 1 else 10))
        rows = [0, 0]

        def place(column):
            rows[column] += 2
            return columns[column], rows[column] - 2

        def note(parent, row, what, default, why):
            text = what + '\n\n' + self.t('mark_default', value=default) + ' — ' + why
            help_mark(parent, text).grid(row=row, column=2, sticky='w', padx=(4, 0), pady=3)

        def save(key, value, default):
            # A value equal to the default is not stored, so a later change of the default reaches it.
            if value == default:
                self.settings.pop(key, None)
            else:
                self.settings[key] = value
            tlg_run.save_settings(self.settings)

        resets = []
        # The readback mode (settings.json gpu_readbacks -> TLG_READBACKS).
        parent, row = place(0)
        ttk.Label(parent, text=self.t('gpu_readbacks')).grid(row=row, column=0, sticky='w')
        modes = ('2', '1', '0')
        mode = ttk.Combobox(parent, values=[self.t('readbacks_' + m) for m in modes], state='readonly', width=14)
        current = str(self.settings.get('gpu_readbacks', '2'))
        mode.current(modes.index(current) if current in modes else 0)
        mode.grid(row=row, column=1, sticky='w', padx=6)

        def mode_picked():
            self.settings['gpu_readbacks'] = modes[mode.current()]
            tlg_run.save_settings(self.settings)

        mode.bind('<<ComboboxSelected>>', lambda _: mode_picked())
        note(parent, row, self.t('gpu_readbacks_hint'), self.t('readbacks_2'), self.t('gpu_readbacks_why'))
        resets.append(lambda: (mode.current(0), mode_picked()))
        # Start-up switches: on by default in the left column, off in the right one.
        for item in backports_table.READBACK:
            default = item.default == '1'
            parent, row = place(0 if default else 1)
            var = tk.BooleanVar(value=bool(self.settings.get(item.key, default)))

            def toggled(item=item, var=var, default=default):
                save(item.key, var.get(), default)

            ttk.Checkbutton(parent, text=item.name[lang], variable=var, command=toggled).grid(
                row=row, column=0, columnspan=2, sticky='w')
            note(parent, row, item.what[lang], self.t('mark_on' if default else 'mark_off'),
                 backports_table.READBACK_WHY[item.id][lang])
            resets.append(lambda var=var, default=default, toggled=toggled: (var.set(default), toggled()))
        # The in-game menu's readback batch (tlg.ini, applies at once).
        batch = next(item for item in backports_table.MENU if item.key == 'readback_batch')
        parent, row = place(1)
        ttk.Label(parent, text=batch.name[lang]).grid(row=row, column=0, sticky='w')
        sizes = [choice[0] for choice in batch.choices]
        size = ttk.Combobox(parent, values=[choice[1 + lang] for choice in batch.choices], state='readonly', width=24)
        chosen = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').get('readback_batch', batch.default)
        size.current(sizes.index(chosen) if chosen in sizes else sizes.index(batch.default))
        size.grid(row=row, column=1, sticky='w', padx=6)

        def size_picked():
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {'readback_batch': sizes[size.current()]})
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload

        size.bind('<<ComboboxSelected>>', lambda _: size_picked())
        note(parent, row, self.t('readback_batch_hint'), batch.choices[sizes.index(batch.default)][1 + lang],
             backports_table.READBACK_WHY[batch.id][lang])
        resets.append(lambda: (size.current(sizes.index(batch.default)), size_picked()))
        # Slow-motion freeze fix (tlg.ini slow_motion, read when a scene loads).
        slow = next(item for item in backports_table.MENU if item.key == 'slow_motion')
        parent, row = place(1)
        ttk.Label(parent, text=slow.name[lang]).grid(row=row, column=0, sticky='w')
        slow_values = [choice[0] for choice in slow.choices]
        slow_box = ttk.Combobox(parent, values=[choice[1 + lang] for choice in slow.choices], state='readonly',
                                width=28)
        chosen = tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').get('slow_motion', slow.default)
        slow_box.current(slow_values.index(chosen) if chosen in slow_values else slow_values.index(slow.default))
        slow_box.grid(row=row, column=1, sticky='w', padx=6)

        def slow_picked():
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {'slow_motion': slow_values[slow_box.current()]})
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload

        slow_box.bind('<<ComboboxSelected>>', lambda _: slow_picked())
        help_mark(parent, slow.what[lang]).grid(row=row, column=2, sticky='w', padx=(4, 0), pady=3)
        resets.append(lambda: (slow_box.current(slow_values.index(slow.default)), slow_picked()))

        bottom = ttk.Frame(frame)
        bottom.grid(row=2, column=0, columnspan=2, sticky='ew', pady=(10, 0))
        ttk.Button(bottom, text=self.t('reset'), command=lambda: [reset() for reset in resets]).pack(side='left')

    def hint(self, parent, row, column, key):
        """A small question mark in that cell; the text shows on the mark and on the row's name."""
        self.hint_mark(parent, key, *parent.grid_slaves(row=row, column=0)).grid(row=row, column=column, sticky='w', padx=(4, 0))

    def hint_mark(self, parent, key, *also):
        return help_mark(parent, self.t(key), *also)

    def check(self, parent, row, key):
        var = tk.BooleanVar(value=bool(self.settings[key]))
        self.vars[key] = var
        ttk.Checkbutton(parent, text=self.t(key), variable=var, command=self.changed).grid(
            row=row, column=0, columnspan=3, sticky='w', pady=2)

    # -- actions ----------------------------------------------------------------------------
    def changed(self):
        for key, var in self.vars.items():
            try:
                self.settings[key] = var.get()
            except tk.TclError:
                pass  # spinbox while typing
        tlg_run.save_settings(self.settings)
        self.refresh_language_choices()
        self.refresh_status()

    def refresh_status(self):
        problems = tlg_run.check_game(self.settings['game_dir'])
        if problems:
            text = '; '.join(self.t('not_built') if k == 'not_built' else f"{self.t('missing')}: {d}"
                             for k, d in problems)
        else:
            text = self.t('ready')
        audio = self.t('audio_native') if tlg_run.ngs2_module(self.settings) else self.t('audio_builtin')
        self.status.configure(text=f'{text}  {audio}')
        self.play_button.configure(state='disabled' if problems or self.process else 'normal')
        self.cache_button.configure(state='disabled' if self.process else 'normal')
        self.refresh_home()

    def browse(self, key):
        path = filedialog.askdirectory(initialdir=self.vars[key].get() or str(Path.home()))
        if path:
            self.vars[key].set(path)

    def open(self, path):
        Path(path).mkdir(parents=True, exist_ok=True)
        tlg_run.open_external(path)

    def clear_cache(self):
        """Clear shader cache: asks first, never while the game runs; shows how much was freed."""
        if self.process:
            messagebox.showinfo(self.t('title'), self.t('already'))
            return
        size = tlg_run.shader_cache_size(self.settings)
        if not size:
            messagebox.showinfo(self.t('clear_cache'), self.t('clear_cache_empty'))
            return
        if not messagebox.askyesno(self.t('clear_cache'),
                                   self.t('clear_cache_ask', size=tlg_run.shader_cache.human_size(size))):
            return
        try:
            freed, failures = tlg_run.clear_shader_cache(self.settings)
        except Exception as error:  # shown to the user, the launcher keeps running
            messagebox.showerror(self.t('error'), str(error))
            return
        freed = tlg_run.shader_cache.human_size(freed)
        if failures:
            message = self.t('clear_cache_failed', size=freed, count=len(failures))
            messagebox.showwarning(self.t('clear_cache'), message)
        else:
            message = self.t('clear_cache_done', size=freed)
            messagebox.showinfo(self.t('clear_cache'), message)
        self.append(message)

    def append(self, line, keep=True):
        if keep:   # kept for the rebuild on a language change
            self.log_lines = (getattr(self, 'log_lines', []) + [line])[-2000:]
        self.log.configure(state='normal')
        self.log.insert('end', line if line.endswith('\n') else line + '\n')
        if int(self.log.index('end-1c').split('.')[0]) > 2000:
            self.log.delete('1.0', '500.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def recover_practice(self):
        """A practice start (Start at...) whose saves were never put back: the launcher or the PC
        stopped while the game ran. Puts them back now, unless the game is still running."""
        data = tlg_run.user_dir()
        if not practice_save.active(data) or tlg_run.game_running():
            return
        try:
            practice_save.end(data)
            self.append(self.t('start_restored'))
        except OSError as error:
            self.append(self.t('start_restore_failed', error=error, path=data / practice_save.COPY))

    def has_save(self):
        folder = tlg_run.user_dir() / 'savedata'
        return folder.is_dir() and any(p.is_file() for p in folder.rglob('*'))

    def start_dialog(self):
        """Start at a point (practice): the stage and start point lists come from start_points.py
        (tools/scene-check/start_points_gen.py); the game jumps there (TLG_START_POINT, tlg_lua.cpp)."""
        if getattr(self, 'start_window', None) and self.start_window.winfo_exists():
            self.start_window.lift()
            return
        win = self.start_window = Page(self, 'start')
        win.title(self.t('start_title'), self.t('start_hint'))
        frame = ttk.Frame(win, padding=(12, 12))
        frame.pack(fill='x')
        stages = list(dict.fromkeys(stage for stage, _ in start_points.POINTS))
        ttk.Label(frame, text=self.t('start_stage')).grid(row=0, column=0, sticky='w', pady=4)
        stage_box = ttk.Combobox(frame, values=stages, state='readonly', width=32)
        stage_box.grid(row=0, column=1, sticky='w', padx=6, pady=4)
        ttk.Label(frame, text=self.t('start_point')).grid(row=1, column=0, sticky='w', pady=4)
        point_box = ttk.Combobox(frame, state='readonly', width=32)
        point_box.grid(row=1, column=1, sticky='w', padx=6, pady=4)
        last = str(self.settings.get('start_point', '')).split(':')

        def stage_picked(area=None):
            areas = [a for s, a in start_points.POINTS if s == stage_box.get()]
            point_box.configure(values=areas)
            point_box.current(areas.index(area) if area in areas else 0)

        stage_box.current(stages.index(last[0]) if last[0] in stages else 0)
        stage_picked(last[1] if len(last) > 1 else None)
        stage_box.bind('<<ComboboxSelected>>', lambda _: stage_picked())

        def go():
            if not self.play_ready():
                return
            point = f'{stage_box.get()}:{point_box.get()}'
            self.settings['start_point'] = point
            tlg_run.save_settings(self.settings)
            self.play(point)

        button = ttk.Button(frame, text=self.t('start_go'), command=go)
        button.grid(row=2, column=1, sticky='w', padx=6, pady=(10, 0))
        note = ttk.Label(frame, foreground=MUTED, wraplength=520, justify='left')
        note.grid(row=3, column=0, columnspan=2, sticky='w', pady=(10, 0))
        if self.has_save():
            note.configure(text=self.t('start_note'))
        else:
            note.configure(text=self.t('start_no_save'))
            button.state(['disabled'])
        self.show_page('start')

    def play(self, point=None):
        """point 'stage:area' = a practice start (Start at...): saves copied before, put back after."""
        window = getattr(self, 'bench_window', None)
        if window and window.running:
            messagebox.showinfo(self.t('title'), self.t('bench_busy'))
            return
        if self.process:
            messagebox.showinfo(self.t('title'), self.t('already'))
            return
        self.changed()
        self.append(self.t('preparing'))
        self.play_button.configure(state='disabled')
        self.cache_button.configure(state='disabled')
        self.stop_music()
        self.stop_track()
        threading.Thread(target=self.run_game, args=(point,), daemon=True).start()

    def end_practice(self):
        try:
            if practice_save.end(tlg_run.user_dir()):
                self.lines.put(self.t('start_restored'))
        except OSError as error:
            self.lines.put(('error', self.t('start_restore_failed', error=error,
                                            path=tlg_run.user_dir() / practice_save.COPY)))

    def run_game(self, point=None):
        self.practice = bool(point)
        try:
            tlg_run.prepare(self.settings, log=lambda line: self.lines.put(line))
            header = [f'Frame rate: requested {tlg_run.frame_rate(self.settings)}, '
                      f'applied {tlg_run.applied_frame_rate}']
            if point:
                practice_save.begin(tlg_run.user_dir(), point)   # raises: the game does not start
                self.lines.put(self.t('start_starting', point=point))
            self.process, log = tlg_run.launch(self.settings, header=header,
                                               extra_env={'TLG_START_POINT': point} if point else None)
        except Exception as error:  # shown to the user, the launcher keeps running
            if point:
                self.end_practice()
            self.practice = False
            self.lines.put(('error', str(error)))
            return
        self.lines.put(f"{self.t('running')} {log}")
        tail = logtail.LogTail(log)
        while True:   # the game writes its own log file; this only follows it
            done = self.process.poll() is not None
            for line in tail.poll(final=done):
                self.lines.put(line)
            if done:
                break
            time.sleep(0.2)
        code = self.process.wait()
        if self.practice:
            self.practice = False
            self.end_practice()
        self.process = None
        self.lines.put(('ended', code))

    def pump(self):
        try:
            while True:
                item = self.lines.get_nowait()
                if isinstance(item, tuple):
                    kind, value = item
                    if kind == 'error':
                        messagebox.showerror(self.t('error'), value)
                    else:
                        message = self.t('ended' if value in (0, -15) else 'crashed', code=value)
                        self.append(message)
                        if value not in (0, -15):
                            messagebox.showwarning(self.t('title'), message)
                    self.refresh_status()
                else:
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def music_tick(self):
        """Every five seconds: the menu tune starts again once the game or a benchmark run has closed."""
        self.sync_music()
        self.root.after(5000, self.music_tick)

    def switch_menu(self):
        # main() starts the other menu once this window is closed.
        self.settings['legacy_menu'] = not self.settings.get('legacy_menu')
        tlg_run.save_settings(self.settings)
        self.menu_switch = True
        self.quit()

    def quit(self):
        # Clicked from the sidebar: destroying the window inside its own click handler leaves Tk
        # finishing that click on freed memory ("alloc: invalid block"), so close just after.
        self.root.after(50, self.close)

    def report_issue(self):
        # Upstream's report (report.py, @HayatoG): system + GPU, a summary of the newest log and the
        # known problems for this GPU, user names removed. The issue form opens filled in; the full
        # report + redacted log go in a .zip in the logs folder (opened) to attach.
        import threading
        import report
        import savemanager
        self.status.configure(text=self.t('report_busy'))

        def work():
            names = report.own_names()
            logs = report.list_logs(tlg_run.LOG_DIR)
            log_text, log_name = (report.read_log(logs[0])[0], logs[0].name) if logs else (None, '')
            try:
                info = report.gather((tlg_run.PORT_DIR, tlg_run.REPO_DIR), tlg_run.LOG_DIR,
                                     self.settings.get('game_dir', ''), savemanager.read_sfo,
                                     {'cpu': lambda: report.cpu_info(lambda: tlg_run.cpu_missing_sets(lambda line: None))})
            except Exception:   # the report still goes out, with fewer rows
                info = None
            rows = report.hardware_rows(info, self.t) if info else []
            summary = report.summary_lines(report.summarize_log(log_text), self.t) if log_text else []
            gpu = info.get('gpu') if info else None
            known = report.known_for(report.load_known_issues(), gpu)
            for k in known:
                k['title'] = k['title']['pt' if self.pt() else 'en']
            full = report.redact(report.markdown_report(rows, summary, log_name, known, '', self.t), names)[0]
            short = report.redact(report.short_report(rows, summary, self.t), names)[0]
            title = self.t('rep_issue_title', gpu=gpu['name'] if gpu else self.t('rep_unknown'))
            url = report.issue_url(title, full, short)[0]
            zip_name = 'report-' + time.strftime('%Y%m%d-%H%M%S') + '.zip'
            clean_log = report.redact(log_text, names)[0] if log_text else None
            if not report.leftover_home_paths((clean_log or '') + full, names):   # never save a leak
                try:
                    report.write_zip(tlg_run.LOG_DIR / zip_name, full, clean_log, log_name)
                except OSError:
                    pass
            self.root.after(0, lambda: (self.open(tlg_run.LOG_DIR), tlg_run.open_external(url),
                                        self.status.configure(text=self.t('report_done', zip=zip_name))))
        threading.Thread(target=work, daemon=True).start()

    def copy_command(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(tlg_run.play_command())
        self.home.itemconfig(self.home_items['status'], text=self.t('copy_command'))

    def toggle_music(self, item):
        on = not self.settings.get('menu_music', True)
        self.settings['menu_music'] = on
        tlg_run.save_settings(self.settings)
        self.home.itemconfig(item, text=self.t('home_music_on' if on else 'home_music_off'))
        self.sync_music()

    def sync_music(self):
        """The game's menu tune plays while the launcher is open and the game is not running, as
        in shadPS4's game list (which plays the same file, sce_sys/snd0.at9)."""
        path = tlg_run.menu_music_file(self.settings)
        bench = getattr(self, 'bench_window', None)
        wanted = (bool(self.settings.get('menu_music', True)) and not self.process and path is not None
                  and not (bench and bench.running)   # nor during a benchmark run
                  and not getattr(self, 'track', None))   # quiet while a soundtrack piece plays
        music = getattr(self, 'music', None)
        if music and (not wanted or music.poll() is not None or getattr(self, 'music_path', None) != path):
            self.stop_music()
            music = None
        if wanted and not music and getattr(self, 'music_failed', None) != path:
            tool = tlg_run.music_tool()
            try:
                self.music = subprocess.Popen([str(tool), str(path), '60'], stdin=subprocess.PIPE,
                                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                              env=tlg_run.pad_tool_env(), text=True,
                                              creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)) if tool else None
                self.music_path = path
            except OSError:
                self.music = None
            if not self.music:
                self.music_failed = path   # silent; tried again when the game folder changes

    def stop_music(self):
        music, self.music = getattr(self, 'music', None), None
        if not music:
            return
        try:
            music.stdin.write('quit\n')
            music.stdin.flush()
            music.wait(1)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            music.kill()

    def close(self):
        if getattr(self, 'bench_window', None):
            self.bench_window.shutdown()
        self.stop_music()
        self.stop_track()
        if getattr(self, 'controller', None):
            self.controller.stop()
        if self.process:
            self.process.terminate()
            if getattr(self, 'practice', False):   # the game thread stops with the window
                try:
                    self.process.wait(timeout=10)
                    self.practice = False
                    practice_save.end(tlg_run.user_dir())
                except Exception:   # the next launcher start puts the saves back
                    pass
        self.root.destroy()


def main():
    # Packaged executable roles: --script <file> runs a preparation script, --play starts the
    # game with the saved settings and no launcher window (shortcuts, Steam).
    args = sys.argv[1:]
    if args[:1] == ['--script'] and len(args) > 1:
        sys.exit(tlg_run.run_script(args[1:]))
    if args[:1] == ['--clear-shader-cache']:
        sys.exit(tlg_run.main(args))
    if args[:1] == ['--play']:
        sys.exit(tlg_run.main(args[1:]))
    # The legacy menu (the launcher before the 2026-11 redesign) as a fallback: --legacy, or the
    # switch in its Settings (saved as legacy_menu). The switch closes one menu and opens the other.
    import tlg_launcher_legacy
    legacy = '--legacy' in args or bool(tlg_run.load_settings().get('legacy_menu'))
    while True:
        module = tlg_launcher_legacy if legacy else sys.modules[__name__]
        root = tk.Tk(className='tlg-pc')  # matches StartupWMClass in the desktop entry
        module.apply_theme(root)
        app = module.Launcher(root)
        root.mainloop()
        if not getattr(app, 'menu_switch', False):
            break
        legacy = not legacy


if __name__ == '__main__':
    main()
