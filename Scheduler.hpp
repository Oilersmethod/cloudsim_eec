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

class Scheduler {
public:
    Scheduler()                 {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
    void SchedulerCheck(Time_t now);
private:
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
    vector<VMId_t> win;
    vector<VMId_t> aix;
    vector<VMId_t> linux;
    vector<VMId_t> linux_rt;
    unordered_set<VMId_t> migrating_vms;
    unordered_map<MachineId_t, float> util_map;
};



#endif /* Scheduler_hpp */
