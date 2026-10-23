#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void state_setslot(int);
int state_save(void);
int state_load(void);
#ifdef __cplusplus
}
#endif
