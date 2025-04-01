//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#ifndef Scheduler_hpp
#define Scheduler_hpp

#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "Interfaces.h"

struct PendingAttachment
{
    VMId_t vm;
    MachineId_t machine_id;
    TaskId_t task_id;
    Priority_t priority;
    double demand;
};

class Scheduler
{
public:
    Scheduler() {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
    void SchedulerCheck(Time_t now);
    void SLAWarning(Time_t time, TaskId_t task_id);
    void MigrateVM(VMId_t, MachineId_t target_machine);

    bool IsVMReady(VMId_t vm);

    double CalculateCPUUtilization(MachineId_t machine_id);
    double CalculateMemoryUtilization(MachineId_t machine_id);

    double CalculateTaskCPUUtilization(TaskId_t task_id);
    double CalculateTaskMemoryUtilization(TaskId_t task_id);

    unsigned CalculateMachineMIPS(MachineId_t machine_id);

    void StateChangeComplete(Time_t time, MachineId_t machine_id);

    vector<MachineId_t> SortMachinesByUtilization();

    int CountVMsOnMachine(MachineId_t machine_id);

    struct PendingAttachment
    {
        VMId_t vm;
        MachineId_t machine_id;
        TaskId_t task_id;
        Priority_t priority;
        double demand;
    };

private:
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
    vector<VMId_t> win;
    vector<VMId_t> aix;
    vector<VMId_t> linux;
    vector<VMId_t> linux_rt;
    unordered_set<VMId_t> migrating_vms;
    unordered_map<MachineId_t, float> mips_util_map;
    unordered_map<TaskId_t, MachineId_t> machine_with_task;
    unordered_set<MachineId_t> transitioningMachines;

    vector<PendingAttachment> pendingAttachments;
};

#endif /* Scheduler_hpp */
