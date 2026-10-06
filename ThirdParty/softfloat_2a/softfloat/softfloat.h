/* MX68K作成の転送shim(上流ファイルではない)。
 * 上流 softfloat.c は "softfloat/softfloat.h" をincludeする(Hatariは -I src/cpu で解決)。
 * MX68Kは ThirdParty/softfloat_2a を HEADER_SEARCH_PATHS に入れないため、softfloat.c と同じ
 * ディレクトリ基準の相対クォートincludeでこのファイルに解決させ、1階層上の本物へ転送する。 */
#include "../softfloat.h"
