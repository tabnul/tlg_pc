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
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backports_table  # noqa: E402
import logtail  # noqa: E402
import preset_share  # noqa: E402
import tlg_run  # noqa: E402

TEXT = {
    'title': ('The Last Guardian — PC', 'The Last Guardian — PC'),
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
    'level_select': ('Level select and QA menu', 'Seleção de fases e menu de QA'),
    'debug_hint': ('Level select: hold L1 while the title menu closes (Continue/New Game). QA menu: in game, hold L1 and press L3. '
                   'GoldHEN patches by illusionyy and DanielSvoboda.',
                   'Seleção de fases: segure L1 enquanto o menu inicial fecha (Continuar/Novo jogo). Menu de QA: no jogo, segure L1 e aperte L3. '
                   'Patches GoldHEN de illusionyy e DanielSvoboda.'),
    'show_fps': ('Show FPS overlay at start (F10 toggles)', 'Mostrar overlay de FPS ao iniciar (F10 alterna)'),
    'vram': ('Texture cache budget (MB of VRAM)', 'Orçamento do cache de texturas (MB de VRAM)'),
    'vram_hint': ('5000 suits every card with 6 GB or more: keep it there, also on a big card. Do not set it higher: the cache is only cleaned when it gets full, '
                  'so a high budget lets old textures pile up and slows the game down over a long session. '
                  'Only lower it if your card needs it: less than 6 GB of VRAM (4 GB card: 3700). This is video card memory (VRAM), not RAM.',
                  '5000 serve para toda placa com 6 GB ou mais: deixe assim, também numa placa grande. Não aumente: o cache só é limpo quando enche, '
                  'então um orçamento alto deixa texturas antigas se acumularem e deixa o jogo mais lento numa sessão longa. '
                  'Só diminua se a sua placa precisar: menos de 6 GB de VRAM (placa de 4 GB: 3700). É a memória da placa de vídeo (VRAM), não a RAM.'),
    'texture_cleanup': ('Clean up textures after each load',
                        'Limpar texturas após cada carregamento'),
    'texture_cleanup_hint': ('Frees the textures of earlier stages all at once after each stage or checkpoint load. Not needed any more: since the texture cache '
                             'cleanup was fixed (issue #104), old textures no longer pile up with the budget at 5000. It can cause a short stutter after a load, '
                             'so it is off by default. Also in the in-game menu (Insert → Performance).',
                             'Libera de uma vez as texturas de fases anteriores após cada carregamento de fase ou de checkpoint. Não é mais necessário: desde que a limpeza '
                             'do cache de texturas foi corrigida (issue #104), texturas antigas não se acumulam mais com o orçamento em 5000. Pode causar um breve travamento '
                             'após um carregamento, então vem desligado. Também no menu do jogo (Insert → Desempenho).'),
    'filters': ('Image filters (live in the in-game menu too, Insert → Image)',
                'Filtros de imagem (também no menu do jogo, Insert → Imagem)'),
    'smaa': ('SMAA anti-aliasing (smooths edges; the HUD too)', 'Anti-aliasing SMAA (suaviza bordas; o HUD também)'),
    'aa_mode': ('Anti-aliasing (F8 in game)', 'Anti-aliasing (F8 no jogo)'),
    'aa_off': ('Off', 'Desligado'),
    'aa_dlaa': ('DLAA (NVIDIA RTX)', 'DLAA (NVIDIA RTX)'),
    'aa_fsr3': ('FSR 3.1 Native AA', 'FSR 3.1 AA nativo'),
    'aa_no_dlss': ('No DLSS on this GPU: DLAA runs as FSR 3.1 Native AA',
                   'Sem DLSS nesta GPU: o DLAA roda como FSR 3.1 AA nativo'),
    'smaa_hint': ('Not recommended: DLAA and FSR 3.1 already smooth the edges; SMAA on top blurs the image and the HUD.',
                  'Não recomendado: o DLAA e o FSR 3.1 já suavizam as bordas; o SMAA por cima desfoca a imagem e o HUD.'),
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
# NVIDIA RTX card; FSR 3.1 native AA runs on any card.
AA_MODES = [('aa_off', None), ('aa_dlaa', ('1', 'dlss')), ('aa_fsr3', ('1', 'fsr3'))]


def dlss_unsupported(ini):
    # The game writes its DLSS check (dlss_supported); without DLSS, DLAA runs as FSR 3.1.
    return ini.get('dlss_supported') == '0'


def aa_mode_of(ini):
    # The AA_MODES index tlg.ini picks (TlgSettings::AaModeOf: every upscaler but TAA and FSR 3.1
    # loads as DLAA).
    if ini.get('dlaa', '1') in ('0', ''):
        return 0
    if ini.get('aa_row', '0') in ('0', ''):
        return 1  # a file from before the row: DLAA
    if ini.get('upscaler') == 'taa':
        return 0
    return 2 if ini.get('upscaler') == 'fsr3' else 1


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


class Launcher:
    def __init__(self, root):
        self.root = root
        self.settings = tlg_run.load_settings()
        self.process = None
        self.lines = queue.Queue()
        self.build()
        self.root.after(100, self.pump)

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
        # DLAA stays the choice in tlg.ini; the game runs FSR 3.1 while DLSS is missing.
        if self.aa_box.current() == 1 and dlss_unsupported(ini):
            self.aa_note.grid()
        else:
            self.aa_note.grid_remove()

    # -- window -----------------------------------------------------------------------------
    def build(self):
        """Horizontal layout: banner on top; Game + Audio | Language + Video | game log; buttons below."""
        for child in self.root.winfo_children():
            child.destroy()
        root = self.root
        root.title(self.t('title'))
        root.minsize(1120, 680)
        pad = {'padx': 6, 'pady': 4}
        main = ttk.Frame(root, padding=12)
        main.pack(fill='both', expand=True)
        for column, weight in enumerate((3, 3, 3, 4)):
            main.columnconfigure(column, weight=weight, uniform='cols' if column < 3 else '')
        main.rowconfigure(2, weight=1)

        header = ttk.Frame(main)
        header.grid(row=0, column=0, columnspan=4, sticky='ew', pady=(0, 8))
        self.load_art()
        if self.banner:
            ttk.Label(header, image=self.banner).pack(side='left', padx=(0, 14))
        titles = ttk.Frame(header)
        titles.pack(side='left', fill='y')
        ttk.Label(titles, text='The Last Guardian', font=('TkDefaultFont', 22, 'bold')).pack(anchor='w')
        ttk.Label(titles, text='PC — CUSA03627 v1.03', foreground='#777').pack(anchor='w')
        self.status = ttk.Label(titles, text='', foreground='#555', wraplength=420, justify='left')
        self.status.pack(anchor='w', pady=(10, 0))

        self.vars = {}
        left = ttk.Frame(main)
        left.grid(row=2, column=0, sticky='nsew', **pad)
        left.columnconfigure(0, weight=1)
        game = ttk.LabelFrame(left, text=self.t('game'), padding=8)
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
        audio = ttk.LabelFrame(left, text=self.t('audio'), padding=8)
        audio.grid(sticky='ew', pady=(0, 8))
        self.check(audio, 0, 'native_audio')
        controls = ttk.LabelFrame(left, text=self.t('controls'), padding=8)
        controls.grid(sticky='ew')
        controls.columnconfigure(1, weight=1)
        self.check(controls, 0, 'mouse_enabled')
        self.check(controls, 1, 'mouse_invert_y')
        self.combo(controls, 4, 'menu_confirm', ['cross', 'circle'],
                   lambda k: 'Cross (X / A)' if k == 'cross' else 'Circle (O / B)')
        ttk.Label(controls, text=self.t('mouse_sensitivity')).grid(row=2, column=0, sticky='w', pady=(4, 0))
        sens = tk.DoubleVar(value=float(self.settings.get('mouse_sensitivity', 1.0)))
        self.vars['mouse_sensitivity'] = sens
        sens_spin = ttk.Spinbox(controls, from_=0.1, to=5.0, increment=0.1, format='%.2f', textvariable=sens,
                                width=6, command=self.changed)
        sens_spin.grid(row=2, column=1, sticky='w', padx=6, pady=(4, 0))
        sens_spin.bind('<FocusOut>', lambda _: self.changed())
        ttk.Button(controls, text=self.t('remap'), command=self.remap_dialog).grid(row=2, column=2, sticky='e', pady=(4, 0))
        ttk.Label(controls, text=self.t('mouse_hint'), foreground='#777', wraplength=420, justify='left').grid(
            row=3, column=0, columnspan=3, sticky='w', pady=(2, 0))

        middle = ttk.Frame(main)
        middle.grid(row=2, column=1, sticky='nsew', **pad)
        middle.columnconfigure(0, weight=1)
        lang = ttk.LabelFrame(middle, text=self.t('language'), padding=8)
        lang.grid(sticky='ew', pady=(0, 8))
        lang.columnconfigure(1, weight=1)
        self.game_lang_box = None
        self.build_game_language(lang)
        self.combo(lang, 1, 'ui_language', UI_LANGUAGES,
                   lambda k: self.t('auto') if k == 'auto' else UI_LANGUAGE_NAMES[k], on_change=self.build)
        self.check(lang, 2, 'skip_logos')
        video = ttk.LabelFrame(middle, text=self.t('video'), padding=8)
        video.grid(sticky='ew')
        video.columnconfigure(1, weight=1)
        self.combo(video, 0, 'display_mode', DISPLAY_MODES, lambda k: self.t(k))
        self.check(video, 1, 'vsync')
        self.check(video, 2, 'show_fps')
        self.combo(video, 5, 'frame_rate', tlg_run.FRAME_RATES, lambda k: self.t(f'fps_{k}'))
        ttk.Label(video, text=self.t('fps_hint'), foreground='#777', wraplength=320).grid(
            row=6, column=0, columnspan=2, sticky='w')
        ttk.Label(video, text=self.t('vram'), wraplength=220).grid(row=3, column=0, sticky='w', pady=(6, 0))
        vram = tk.IntVar(value=int(self.settings['vram_budget_mb']))
        self.vars['vram_budget_mb'] = vram
        spin = ttk.Spinbox(video, from_=2000, to=24000, increment=200, textvariable=vram, width=8,
                           command=self.changed)
        spin.grid(row=3, column=1, sticky='w', padx=6, pady=(6, 0))
        spin.bind('<FocusOut>', lambda _: self.changed())
        memory = ttk.Frame(video)
        memory.grid(row=4, column=0, columnspan=2, sticky='w')
        ttk.Label(memory, text=self.t('vram_hint'), foreground='#777', wraplength=360).grid(row=0, column=0, sticky='w')
        # The in-game menu's texture cleanup (tlg.ini, applies at once).
        cleanup = tk.BooleanVar(value=tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').get('texture_cleanup', '0') != '0')

        def cleanup_toggled():
            tlg_run.USER_DIR.mkdir(parents=True, exist_ok=True)
            tlg_run.update_ini(tlg_run.USER_DIR / 'tlg.ini', {'texture_cleanup': '1' if cleanup.get() else '0'})
            self.ini_stamp = self.ini_files_stamp()  # our own write: nothing to reload

        ttk.Checkbutton(memory, text=self.t('texture_cleanup'), variable=cleanup, command=cleanup_toggled).grid(
            row=1, column=0, sticky='w', pady=(4, 0))
        ttk.Label(memory, text=self.t('texture_cleanup_hint'), foreground='#777', wraplength=360).grid(
            row=2, column=0, sticky='w')
        self.combo(video, 7, 'resolution', tlg_run.RESOLUTIONS, tlg_run.resolution_label)
        ttk.Label(video, text=self.t('resolution_hint'), foreground='#777', wraplength=320).grid(
            row=8, column=0, columnspan=2, sticky='w')
        ttk.Label(video, text=self.t('aa_mode')).grid(row=9, column=0, sticky='w', pady=(6, 0))
        self.aa_box = ttk.Combobox(video, values=[self.t(label) for label, _ in AA_MODES],
                                   state='readonly', width=20)
        self.aa_box.current(aa_mode_of(tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini')))
        self.aa_box.grid(row=9, column=1, sticky='w', padx=6, pady=(6, 2))
        self.aa_box.bind('<<ComboboxSelected>>', lambda _: self.filters_changed())
        self.aa_note = ttk.Label(video, text=self.t('aa_no_dlss'), foreground='#777', wraplength=320)
        self.aa_note.grid(row=10, column=0, columnspan=2, sticky='w')
        self.update_aa_note(tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini'))
        image = ttk.LabelFrame(main, text=self.t('image'), padding=8)
        image.grid(row=2, column=2, sticky='new', **pad)
        image.columnconfigure(1, weight=1)
        self.build_filters(image, 0)
        self.build_preset(image, 20)
        debug = ttk.LabelFrame(middle, text=self.t('debug'), padding=8)
        debug.grid(sticky='ew', pady=(8, 0))
        self.check(debug, 0, 'level_select')
        ttk.Label(debug, text=self.t('debug_hint'), foreground='#777', wraplength=360, justify='left').grid(
            row=1, column=0, columnspan=3, sticky='w')

        log_frame = ttk.LabelFrame(main, text='Log', padding=4)
        log_frame.grid(row=2, column=3, sticky='nsew', **pad)
        self.log = tk.Text(log_frame, wrap='none', font=('TkFixedFont', 9), state='disabled', width=48)
        scroll = ttk.Scrollbar(log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')

        buttons = ttk.Frame(main)
        buttons.grid(row=3, column=0, columnspan=4, sticky='ew', pady=(10, 0))
        ttk.Button(buttons, text=self.t('quit'), command=self.quit).pack(side='left')
        ttk.Label(buttons, text=self.t('menu_note'), foreground='#555', wraplength=300).pack(side='left', padx=12)
        style = ttk.Style()
        style.configure('Play.TButton', font=('TkDefaultFont', 13, 'bold'))
        self.play_button = ttk.Button(buttons, text=self.t('play'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', ipadx=30, ipady=8)
        ttk.Button(buttons, text=self.t('logs'), command=lambda: self.open(tlg_run.LOG_DIR)).pack(side='right', padx=6)
        ttk.Button(buttons, text=self.t('mods'), command=self.mods_dialog).pack(side='right', padx=6)
        ttk.Button(buttons, text=self.t('graphics'), command=self.graphics_dialog).pack(side='right', padx=(6, 0))
        ttk.Button(buttons, text=self.t('advanced'), command=self.advanced_dialog).pack(side='right', padx=(6, 0))
        ttk.Button(buttons, text=self.t('saves'), command=lambda: self.open(tlg_run.USER_DIR / 'savedata')).pack(side='right')
        self.cache_button = ttk.Button(buttons, text=self.t('clear_cache'), command=self.clear_cache)
        self.cache_button.pack(side='right', padx=(0, 6))
        self.refresh_status()

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
        ttk.Label(parent, text=self.t('filters'), foreground='#555', wraplength=360).grid(
            row=row + 1, column=0, columnspan=2, sticky='w')
        ini = dict(INI_DEFAULTS)
        ini.update({k: v for k, v in tlg_run.read_ini(tlg_run.USER_DIR / 'tlg.ini').items() if k in ini})
        self.ini_vars = {}
        for offset, key in ((2, 'smaa'), (4, 'color_adjust')):
            var = tk.BooleanVar(value=ini[key] not in ('0', ''))
            self.ini_vars[key] = var
            cell = ttk.Frame(parent)
            cell.grid(row=row + offset, column=0, columnspan=2, sticky='w', pady=2)
            ttk.Checkbutton(cell, text=self.t(key), variable=var, command=self.filters_changed).pack(anchor='w')
            if key == 'smaa':
                ttk.Label(cell, text=self.t('smaa_hint'), foreground='#777', wraplength=360).pack(anchor='w', padx=(20, 0))
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
        ttk.Separator(parent).grid(row=row, column=0, columnspan=2, sticky='ew', pady=(8, 4))
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
        ttk.Label(parent, text=self.t('graphics_preset_hint'), foreground='#777', wraplength=360,
                  justify='left').grid(row=row + 3, column=0, columnspan=2, sticky='w')
        # Texture pack: tlg.ini texture_pack (a pack folder's name or off), the in-game Image row.
        ttk.Label(parent, text=self.t('texture_pack')).grid(row=row + 4, column=0, sticky='w', pady=(6, 0))
        self.pack_box = ttk.Combobox(parent, state='readonly', width=24)
        self.pack_box.grid(row=row + 4, column=1, sticky='w', padx=6, pady=(6, 2))
        self.pack_box.bind('<<ComboboxSelected>>', lambda _: self.texture_pack_picked())
        ttk.Label(parent, text=self.t('texture_pack_hint'), foreground='#777', wraplength=360,
                  justify='left').grid(row=row + 5, column=0, columnspan=2, sticky='w')
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
        for key, var in self.ini_vars.items():
            value = ini.get(key, INI_DEFAULTS[key])
            var.set(value not in ('0', '') if isinstance(var, tk.BooleanVar) else value)
        self.sharpener.current(2 if ini.get('cas', '0') not in ('0', '') else 1 if ini.get('rcas', '1') not in ('0', '') else 0)
        self.update_strength(ini)
        self.aa_box.current(aa_mode_of(ini))
        self.update_aa_note(ini)
        if getattr(self, 'graphics_window', None) and self.graphics_window.winfo_exists():
            self.graphics_window.destroy()  # its sliders show the old values
            self.graphics_dialog()
        self.refresh_preset()
        self.refresh_texture_pack()

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

    def remap_dialog(self):
        """PS4 button -> keys and mouse buttons (tlg.ini key_map through tlg_run.launch)."""
        if getattr(self, 'remap_window', None) and self.remap_window.winfo_exists():
            self.remap_window.lift()
            return
        win = self.remap_window = tk.Toplevel(self.root)
        win.title(self.t('remap_title'))
        win.transient(self.root)
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
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
        ttk.Label(frame, text=self.t('remap_help'), foreground='#555', wraplength=760, justify='left').grid(
            row=half + 1, column=0, columnspan=8, sticky='w', pady=(10, 6))
        bottom = ttk.Frame(frame)
        bottom.grid(row=half + 2, column=0, columnspan=8, sticky='ew')

        def reset():
            for entry in entries.values():
                entry.delete(0, 'end')
            save()

        def close():
            save()
            win.destroy()
        ttk.Button(bottom, text=self.t('reset'), command=reset).pack(side='left')
        ttk.Button(bottom, text=self.t('close'), command=close).pack(side='right')
        win.protocol('WM_DELETE_WINDOW', close)

    def mods_dialog(self):
        """Mods/cheats (shadPS4/GoldHEN XML in tlg_run.mods_dir()); toggles write mods.json
        through tlg_run.set_mod_enabled. Nothing here is on unless the player turns it on."""
        if getattr(self, 'mods_window', None) and self.mods_window.winfo_exists():
            self.mods_window.lift()
            return
        win = self.mods_window = tk.Toplevel(self.root)
        win.title(self.t('mods_title'))
        win.transient(self.root)
        win.minsize(560, 360)
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text=self.t('mods_hint'), wraplength=520, justify='left').pack(anchor='w')
        ttk.Label(frame, text=self.t('mods_warning'), foreground='#a33', wraplength=520, justify='left').pack(
            anchor='w', pady=(4, 8))

        list_frame = ttk.Frame(frame)
        list_frame.pack(fill='both', expand=True)
        canvas = tk.Canvas(list_frame, highlightthickness=0)
        scroll = ttk.Scrollbar(list_frame, orient='vertical', command=canvas.yview)
        rows = ttk.Frame(canvas)
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
                ttk.Label(rows, text=self.t('mods_empty'), foreground='#777').pack(anchor='w', pady=8)
            for key, name, note, enabled in mods:
                row = ttk.Frame(rows)
                row.pack(fill='x', anchor='w', pady=2)
                var = tk.BooleanVar(value=enabled)
                ttk.Checkbutton(row, text=name or key, variable=var,
                                command=lambda k=key, v=var: toggle(k, v)).pack(anchor='w')
                if note:
                    ttk.Label(row, text=note, foreground='#777', wraplength=480, justify='left').pack(
                        anchor='w', padx=(22, 0))
        populate()

        bottom = ttk.Frame(frame)
        bottom.pack(fill='x', pady=(10, 0))
        ttk.Button(bottom, text=self.t('mods_folder'), command=lambda: self.open(tlg_run.mods_dir())).pack(side='left')
        ttk.Button(bottom, text=self.t('mods_refresh'), command=populate).pack(side='left', padx=6)
        ttk.Button(bottom, text=self.t('close'), command=win.destroy).pack(side='right')

    def graphics_dialog(self):
        """HDR, shadow resolution and the game's effect strengths (the in-game menu's, Insert),
        written to tlg.ini right away."""
        if getattr(self, 'graphics_window', None) and self.graphics_window.winfo_exists():
            self.graphics_window.lift()
            return
        win = self.graphics_window = tk.Toplevel(self.root)
        win.title(self.t('graphics_title'))
        win.transient(self.root)
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
        frame.columnconfigure((0, 1, 2), weight=1)
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

        # Each hint starts with the row's default and, where known, its value on PS4 (same as the
        # in-game menu).
        def hint(parent, row, key, default, ps4=None):
            marks = self.t('mark_default', value=default) + ('' if ps4 is None else f' · PS4: {ps4}')
            ttk.Label(parent, text=marks + '\n' + self.t(key), foreground='#777', wraplength=340,
                      justify='left').grid(row=row, column=0, columnspan=4, sticky='w', pady=(0, 6))

        on_off = lambda on: self.t('mark_on' if on else 'mark_off')
        # Advice: tweak while playing (changes show at once), with the in-game shortcuts.
        tips = ttk.LabelFrame(frame, text=self.t('graphics_tips'), padding=6)
        tips.grid(row=0, column=0, columnspan=3, sticky='ew', pady=(0, 8))
        ttk.Label(tips, text=self.t('graphics_hint'), wraplength=440, justify='left').pack(side='left', anchor='n')
        ttk.Label(tips, text=self.t('graphics_keys'), justify='left',
                  font=('TkFixedFont', 9)).pack(side='left', anchor='n', padx=(16, 0))
        # Three columns: display, shadows, grass and textures on the left; the effects in the other two.
        left, middle, right = ttk.Frame(frame), ttk.Frame(frame), ttk.Frame(frame)
        for index, column in enumerate((left, middle, right)):
            column.grid(row=1, column=index, sticky='nsew', padx=(0 if index == 0 else 10, 0 if index == 2 else 10))
            column.columnconfigure(1, weight=1)
        hdr = tk.BooleanVar(value=number('hdr', 1) != 0)
        ttk.Checkbutton(left, text='HDR', variable=hdr, command=lambda: write({'hdr': int(hdr.get())})).grid(
            row=1, column=0, columnspan=4, sticky='w')
        hint(left, 2, 'hdr_hint', on_off(True))
        ttk.Label(left, text=self.t('shadow_scale')).grid(row=3, column=0, sticky='w')
        shadow = ttk.Combobox(left, values=[self.t('shadow_game'), '4096', '8192'], state='readonly', width=14)
        scale = number('shadow_scale', 1)
        shadow.current(2 if scale >= 4 else 1 if scale >= 2 else 0)
        shadow.grid(row=3, column=1, sticky='w', padx=6)
        shadow.bind('<<ComboboxSelected>>', lambda _: write({'shadow_scale': SHADOW_SCALES[shadow.current()]}))
        hint(left, 4, 'shadow_hint', self.t('shadow_game'), self.t('shadow_game'))
        grass = tk.BooleanVar(value=number('grass_smooth', 1) != 0)
        ttk.Checkbutton(left, text=self.t('grass_smooth'), variable=grass,
                        command=lambda: write({'grass_smooth': int(grass.get())})).grid(
            row=5, column=0, columnspan=4, sticky='w')
        hint(left, 6, 'grass_hint', on_off(True), on_off(False))
        ttk.Label(middle, text=self.t('effects'), foreground='#555').grid(row=0, column=0, columnspan=4, sticky='w')
        half = (len(EFFECTS) + 1) // 2

        sliders = {}
        for index, (key, (default, low, high, *unit)) in enumerate(EFFECTS.items()):
            unit = unit[0] if unit else '%'
            parent = middle if index < half else right
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

            slider = ttk.Scale(parent, from_=low, to=high, value=value, length=180, command=moved)
            slider.grid(row=row, column=1, sticky='ew', padx=6)
            sliders[key] = (slider, moved)
            # Default and PS4 buttons, as in the in-game menu (the effects' defaults are as shipped).
            buttons = ttk.Frame(parent)
            buttons.grid(row=row, column=3, sticky='e')
            for text in (self.t('mark_default_button'), 'PS4'):
                ttk.Button(buttons, text=text, width=len(text) + 2,
                           command=lambda slider=slider, moved=moved, default=default: (
                               slider.set(default), moved(default))).pack(side='left', padx=(2, 0))
            hint(parent, row + 1, 'effect_' + key + '_hint', f'{default}{unit}', f'{default}{unit}')

        # Textures (the in-game menu's Image tab): LOD bias, anisotropic filtering, texture pack.
        row = 7
        ttk.Separator(left).grid(row=row, column=0, columnspan=4, sticky='ew', pady=4)
        ttk.Label(left, text=self.t('textures'), foreground='#555').grid(row=row + 1, column=0, columnspan=4, sticky='w')
        bias_text = lambda quarters: f'{quarters * 0.25:+.2f}'
        ttk.Label(left, text=self.t('lod_bias')).grid(row=row + 2, column=0, sticky='w')
        bias = min(max(number('lod_bias', LOD_BIAS_DEFAULT), -12), 4)
        bias_shown = ttk.Label(left, text=bias_text(bias), width=5)
        bias_shown.grid(row=row + 2, column=2, sticky='e')

        def bias_moved(raw, current={'value': bias}):
            step = int(round(float(raw)))
            bias_shown.configure(text=bias_text(step))
            if step != current['value']:
                current['value'] = step
                write({'lod_bias': step})

        bias_slider = ttk.Scale(left, from_=-12, to=4, value=bias, length=260, command=bias_moved)
        bias_slider.grid(row=row + 2, column=1, sticky='ew', padx=6)
        buttons = ttk.Frame(left)
        buttons.grid(row=row + 2, column=3, sticky='e')
        for text, value in ((self.t('mark_default_button'), LOD_BIAS_DEFAULT), ('PS4', 0)):
            ttk.Button(buttons, text=text, width=len(text) + 2,
                       command=lambda value=value: (bias_slider.set(value), bias_moved(value))).pack(side='left', padx=(2, 0))
        hint(left, row + 3, 'lod_bias_hint', bias_text(LOD_BIAS_DEFAULT), bias_text(0))
        ttk.Label(left, text=self.t('aniso')).grid(row=row + 4, column=0, sticky='w')
        aniso_labels = [self.t('aniso_game'), '2x', '4x', '8x', '16x']
        aniso = ttk.Combobox(left, values=aniso_labels, state='readonly', width=14)
        level = number('aniso', 16)
        aniso.current(max(i for i, v in enumerate(ANISO_LEVELS) if v <= level) if level > 0 else 0)
        aniso.grid(row=row + 4, column=1, sticky='w', padx=6)
        aniso.bind('<<ComboboxSelected>>', lambda _: write({'aniso': ANISO_LEVELS[aniso.current()]}))
        hint(left, row + 5, 'aniso_hint', '16x', aniso_labels[0])
        ttk.Label(left, text=self.t('texture_pack')).grid(row=row + 6, column=0, sticky='w')
        packs = tlg_run.texture_packs(tlg_run.texpack_dir(self.settings))
        pack = ttk.Combobox(left, values=[self.t('texture_pack_off'), *packs], state='readonly', width=24)
        chosen = ini.get('texture_pack', 'off')
        pack.current(packs.index(chosen) + 1 if chosen in packs else 0)
        pack.grid(row=row + 6, column=1, sticky='w', padx=6)

        def pack_picked():
            write({'texture_pack': packs[pack.current() - 1] if pack.current() > 0 else 'off'})
            self.refresh_texture_pack()

        pack.bind('<<ComboboxSelected>>', lambda _: pack_picked())
        hint(left, row + 7, 'texture_pack_dialog_hint', self.t('mark_off'), self.t('mark_off'))

        def reset():
            hdr.set(True)
            shadow.current(0)
            grass.set(True)
            aniso.current(len(ANISO_LEVELS) - 1)
            pack.current(0)
            write({'hdr': 1, 'shadow_scale': 1, 'grass_smooth': 1, 'aniso': 16, 'texture_pack': 'off'})
            self.refresh_texture_pack()
            bias_slider.set(LOD_BIAS_DEFAULT)
            bias_moved(LOD_BIAS_DEFAULT)
            for key, (slider, moved) in sliders.items():
                slider.set(EFFECTS[key][0])
                moved(EFFECTS[key][0])

        bottom = ttk.Frame(frame)
        bottom.grid(row=2, column=0, columnspan=3, sticky='ew', pady=(10, 0))
        ttk.Button(bottom, text=self.t('reset'), command=reset).pack(side='left')
        ttk.Button(bottom, text=self.t('close'), command=win.destroy).pack(side='right')

    def advanced_dialog(self):
        """The GPU readback switches (backports_table.READBACK, the readback mode, the in-game
        readback batch). Each row shows its default and why; graphics presets never set them."""
        if getattr(self, 'advanced_window', None) and self.advanced_window.winfo_exists():
            self.advanced_window.lift()
            return
        win = self.advanced_window = tk.Toplevel(self.root)
        win.title(self.t('advanced_title'))
        win.transient(self.root)
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
        lang = int(self.pt())
        ttk.Label(frame, text=self.t('advanced_hint'), wraplength=740, justify='left').grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 8))
        columns = ttk.Frame(frame), ttk.Frame(frame)
        for index, column in enumerate(columns):
            column.grid(row=1, column=index, sticky='nw', padx=(0 if index == 0 else 10, 0 if index == 1 else 10))
        rows = [0, 0]

        def place(column):
            rows[column] += 2
            return columns[column], rows[column] - 2

        def note(parent, row, what, default, why):
            text = what + '\n' + self.t('mark_default', value=default) + ' — ' + why
            ttk.Label(parent, text=text, foreground='#777', wraplength=360, justify='left').grid(
                row=row + 1, column=0, columnspan=2, sticky='w', pady=(0, 6))

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

        bottom = ttk.Frame(frame)
        bottom.grid(row=2, column=0, columnspan=2, sticky='ew', pady=(10, 0))
        ttk.Button(bottom, text=self.t('reset'), command=lambda: [reset() for reset in resets]).pack(side='left')
        ttk.Button(bottom, text=self.t('close'), command=win.destroy).pack(side='right')

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

    def append(self, line):
        self.log.configure(state='normal')
        self.log.insert('end', line if line.endswith('\n') else line + '\n')
        if int(self.log.index('end-1c').split('.')[0]) > 2000:
            self.log.delete('1.0', '500.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def play(self):
        if self.process:
            messagebox.showinfo(self.t('title'), self.t('already'))
            return
        self.changed()
        self.append(self.t('preparing'))
        self.play_button.configure(state='disabled')
        self.cache_button.configure(state='disabled')
        threading.Thread(target=self.run_game, daemon=True).start()

    def run_game(self):
        try:
            tlg_run.prepare(self.settings, log=lambda line: self.lines.put(line))
            header = [f'Frame rate: requested {tlg_run.frame_rate(self.settings)}, '
                      f'applied {tlg_run.applied_frame_rate}']
            self.process, log = tlg_run.launch(self.settings, header=header)
        except Exception as error:  # shown to the user, the launcher keeps running
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

    def quit(self):
        if self.process:
            self.process.terminate()
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
    root = tk.Tk(className='tlg-pc')  # matches StartupWMClass in the desktop entry
    try:
        ttk.Style().theme_use('clam')
    except tk.TclError:
        pass
    Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    main()
