/* Host-test mock: rput32 feeds the fake command processor in sim.c. */
#ifndef MOCK_XE_INTERNAL_H
#define MOCK_XE_INTERNAL_H
void mock_rput32(struct XenosDevice *xe, u32 d);
#define rput32(d) mock_rput32(xe, (d))
void Xe_pSyncFromDevice(struct XenosDevice *xe, volatile void *data, int len);
#endif
