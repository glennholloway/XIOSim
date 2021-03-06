/* 
 * Interface to instruction feeder.
 * Copyright, Svilen Kanev, 2011
*/

#ifndef __PIN_ZESTO_INTERFACE__
#define __PIN_ZESTO_INTERFACE__

#include "synchronization.h"
#include "machine.h"
#include "pin.h"
#include "helix.h"

/* Calls from feeder to Zesto */
int Zesto_SlaveInit(int argc, char **argv);
void Zesto_Resume(int coreID, handshake_container_t * handshake);
void Zesto_Destroy();

void Zesto_Notify_Mmap(int asid, unsigned int addr, unsigned int length, bool mod_brk);
void Zesto_Notify_Munmap(int asid, unsigned int addr, unsigned int length, bool mod_brk);
void Zesto_UpdateBrk(int asid, unsigned int brk_end, bool do_mmap);
void Zesto_Map_Stack(int asid, unsigned int sp, unsigned int bos);
 
void Zesto_Slice_Start(unsigned int slice_num);
void Zesto_Slice_End(unsigned int slice_num, unsigned long long feeder_slice_length, unsigned long long slice_weight_times_1000);
void Zesto_Add_WriteByteCallback(ZESTO_WRITE_BYTE_CALLBACK callback);
void Zesto_WarmLLC(int asid, unsigned int addr, bool is_write);

void activate_core(int coreID);
void deactivate_core(int coreID);
bool is_core_active(int coreID);
void sim_drain_pipe(int coreID);

#endif /*__PIN_ZESTO_INTERFACE__*/
