/* ===========================================================================
 *  P534 (D-40 症状1): 診断プローブ [P534-GVWCOL] 用 force-include ヘッダ
 * ---------------------------------------------------------------------------
 *  Core/px68k/x68k/gvram.c の GVRAM_Write() を関数名ごとマクロで改名し、
 *  Bridge/EmulatorBridge.c に置いた同名(GVRAM_Write)の計数ラッパを
 *  リンク上の実体にする。ラッパは列別カウンタを進めたあと、改名後の
 *  本体(p534_gvram_write_core)を引数無変更でそのまま呼ぶ。
 *  Core は編集しない(CLAUDE.md「Core File Modification Policy」)。
 *
 *  機構は P502 の Bridge/sasi_io_cache.h と同一 —— project.pbxproj の
 *  "gvram.c in Sources" の COMPILER_FLAGS に
 *    -include $(SRCROOT)/../Bridge/p534_gvwcol_hook.h
 *  を 1 個追記するだけで、★このヘッダは gvram.c 以外の翻訳単位には
 *  force-include されない。他 TU(mem_wrap.c 等)の GVRAM_Write 呼出しは
 *  改名されないため、そのまま Bridge のラッパへ届く。
 *
 *  ★このヘッダは記憶域を一切定義しない(プリプロセッサマクロ 1 個のみ)。
 *    gvram.c の TU 内グローバル配置(GVRAM / Grp_LineBuf32 …)をバイト単位で
 *    現状のまま保つためで、Fix Plan「グローバル配置安全性」節(§Z 事故の
 *    再発防止)の要求そのものである。include する EmulatorBridge.h も
 *    #pragma once + <stdint.h>/<stdbool.h> + 宣言のみで記憶域定義を持たない。
 *
 *  休止手順: P214_ENABLE=0 → P534_ENABLE=0 → 下のマクロが未定義 →
 *  gvram.c は本来の GVRAM_Write を定義し、Bridge 側ラッパは #if で消える。
 *  リンクは通り、ホットパス上のオーバーヘッドは完全にゼロになる。
 * =========================================================================== */

#ifndef MX68K_P534_GVWCOL_HOOK_H
#define MX68K_P534_GVWCOL_HOOK_H

#include "EmulatorBridge.h"   /* P534_ENABLE カスケードの単一の出所 */

#if P534_ENABLE
#define GVRAM_Write p534_gvram_write_core
#endif

#endif /* MX68K_P534_GVWCOL_HOOK_H */
