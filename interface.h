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

int Zesto_Notify_Mmap(int coreID, unsigned int addr, unsigned int length, bool mod_brk);
int Zesto_Notify_Munmap(int coreID, unsigned int addr, unsigned int length, bool mod_brk);
void Zesto_SetBOS(int coreID, unsigned int stack_base);
void Zesto_UpdateBrk(int coreID, unsigned int brk_end, bool do_mmap);
 
void Zesto_Slice_Start(unsigned int slice_num);
void Zesto_Slice_End(int coreID, unsigned int slice_num, unsigned long long feeder_slice_length, unsigned long long slice_weight_times_1000);
void Zesto_Add_WriteByteCallback(ZESTO_WRITE_BYTE_CALLBACK callback);
void Zesto_WarmLLC(unsigned int addr, bool is_write);

void activate_core(int coreID);
void deactivate_core(int coreID);
bool is_core_active(int coreID);
void sim_drain_pipe(int coreID);
extern int num_cores;

void CheckIPCMessageQueue();

enum ipc_message_id_t { SLICE_START, SLICE_END, MMAP, MUNMAP, UPDATE_BRK, UPDATE_BOS, WARM_LLC, STOP_SIMULATION, INVALID_MSG };

struct ipc_message_t {
    ipc_message_id_t id;
    int coreID;
    int64_t arg1;
    int64_t arg2;
    int64_t arg3;
    XIOSIM_LOCK lock;

    ipc_message_t() : id(INVALID_MSG) {}

    void SliceStart(unsigned int slice_num)
    {
        this->id = SLICE_START;
        this->arg1 = slice_num;
    }

    void SliceEnd(int coreID, unsigned int slice_num, unsigned long long feeder_slice_length, unsigned long long slice_weight_times_1000)
    {
        this->id = SLICE_END;
        this->coreID = coreID;
        this->arg1 = slice_num;
        this->arg2 = feeder_slice_length;
        this->arg3 = slice_weight_times_1000;
    }

    void Mmap(int coreID, unsigned int addr, unsigned int length, bool mod_brk)
    {
        this->id = MMAP;
        this->coreID = coreID;
        this->arg1 = addr;
        this->arg2 = length;
        this->arg3 = mod_brk;
    }

    void Munmap(int coreID, unsigned int addr, unsigned int length, bool mod_brk)
    {
        this->id = MUNMAP;
        this->coreID = coreID;
        this->arg1 = addr;
        this->arg2 = length;
        this->arg3 = mod_brk;
    }

    void UpdateBrk(int coreID, unsigned int brk_end, bool do_mmap)
    {
        this->id = UPDATE_BRK;
        this->coreID = coreID;
        this->arg1 = brk_end;
        this->arg2 = do_mmap;
    }

    void UpdateBOS(int coreID, unsigned int stack_base)
    {
        this->id = UPDATE_BOS;
        this->coreID = coreID;
        this->arg1 = stack_base;
    }

    void StopSimulation(bool kill_sim_threads)
    {
        this->id = STOP_SIMULATION;
        this->arg1 = kill_sim_threads;
    }
};

#endif /*__PIN_ZESTO_INTERFACE__*/

