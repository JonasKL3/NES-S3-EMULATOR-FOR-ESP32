#pragma once
#include <limits.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rominfo_t { char filename[PATH_MAX+1]; } rominfo_t;
typedef struct nes_t {rominfo_t* rominfo;} nes_t;
nes_t *nes_getcontextptr(void);
#ifdef __cplusplus
}
#endif
