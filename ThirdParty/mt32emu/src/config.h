/* MX68K独自作成(upstream munt由来ではない)。
 * munt mt32emu/src/config.h.in を、CMake静的ライブラリ構成
 * (libmt32emu_SHARED=OFF, C+C++ API両方, version tagging無し)相当の値で手書き展開したもの。
 * 元テンプレート: munt 6e7c01fba7e1d50c8fa705834889fd0eac136075 mt32emu/src/config.h.in
 * バージョン値: mt32emu/cmake/project_data.cmake (2.8.3)
 */

#ifndef MT32EMU_CONFIG_H
#define MT32EMU_CONFIG_H

#define MT32EMU_VERSION      "2.8.3"
#define MT32EMU_VERSION_MAJOR 2
#define MT32EMU_VERSION_MINOR 8
#define MT32EMU_VERSION_PATCH 3

/* 3: C++ API と C互換API の両方を提供(CMake静的ビルド時の既定値) */
#define MT32EMU_EXPORTS_TYPE 3

/* 静的ライブラリとして組み込むため MT32EMU_SHARED は定義しない */
#undef MT32EMU_SHARED

#define MT32EMU_WITH_VERSION_TAGGING 0

#if MT32EMU_WITH_VERSION_TAGGING
#  ifndef MT32EMU_RUNTIME_VERSION_CHECK
#    define MT32EMU_RUNTIME_VERSION_CHECK 0
#  endif
#else
#  undef MT32EMU_RUNTIME_VERSION_CHECK
#endif

#endif /* #ifndef MT32EMU_CONFIG_H */
