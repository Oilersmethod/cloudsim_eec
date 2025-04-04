//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <utility>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>

static bool migrating = false;
static unsigned active_machines = 16;

/**
 * Initialize the scheduler
 *
 * Discovers all available machines, creates VMs for running tier machines,
 * and initializes the three-tier system.
 */
void Scheduler::Init()
{
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 0);
    SimOutput("Scheduler::Init(): Initializing E-eco scheduler", 0);
    active_machines = Machine_GetTotal();

    unsigned totalMachines = Machine_GetTotal();
    unsigned runningSize, intermediateSize;

    CalculateTierSizes(totalMachines, 0, runningSize, intermediateSize);

    SimOutput("Scheduler::Init(): Initial tier sizes - Running: " + to_string(runningSize) +
                  ", Intermediate: " + to_string(intermediateSize) +
                  ", Switched Off: " + to_string(totalMachines - runningSize - intermediateSize),
              0);

    for (unsigned i = 0; i < totalMachines; i++)
    {
        MachineInfo_t machine = Machine_GetInfo(MachineId_t(i));
        machines.push_back(MachineId_t(i));

        cpuTypeMachines[machine.cpu].push_back(MachineId_t(i));

        if (i < runningSize)
        {
            machineTiers[MachineId_t(i)] = RUNNING;
            assert(Machine_GetInfo(MachineId_t(i)).s_state == S0);

            switch (machine.cpu)
            {
            case RISCV:
            {
                VMId_t new_vm = VM_Create(LINUX, RISCV);
                linux.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(LINUX_RT, RISCV);
                linux_rt.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));
                break;
            }
            case POWER:
            {
                VMId_t new_vm = VM_Create(LINUX, POWER);
                linux.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(LINUX_RT, POWER);
                linux_rt.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(AIX, POWER);
                aix.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));
                break;
            }
            case ARM:
            {
                VMId_t new_vm = VM_Create(LINUX, ARM);
                linux.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(LINUX_RT, ARM);
                linux_rt.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(WIN, ARM);
                win.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));
                break;
            }
            case X86:
            {
                VMId_t new_vm = VM_Create(LINUX, X86);
                linux.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(LINUX_RT, X86);
                linux_rt.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));

                new_vm = VM_Create(WIN, X86);
                win.push_back(new_vm);
                VM_Attach(new_vm, MachineId_t(i));
                break;
            }
            default:
                break;
            }
        }
        else if (i < runningSize + intermediateSize)
        {
            machineTiers[MachineId_t(i)] = INTERMEDIATE;
        }
        else
        {
            machineTiers[MachineId_t(i)] = SWITCHED_OFF;
            Machine_SetState(MachineId_t(i), S5);
        }
    }

    vms.insert(vms.end(), linux.begin(), linux.end());
    vms.insert(vms.end(), linux_rt.begin(), linux_rt.end());
    vms.insert(vms.end(), win.begin(), win.end());
    vms.insert(vms.end(), aix.begin(), aix.end());

    SimOutput("Scheduler::Init(): Created " + to_string(vms.size()) + " VMs across " +
                  to_string(runningSize) + " running machines",
              0);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id)
{
    VMInfo_t vmInfo = VM_GetInfo(vm_id);
    MachineId_t target_machine = vmInfo.machine_id;

    if (vm_id == 2 && target_machine == 13)
    {
        Machine_SetState(target_machine, S0);
        return;
    }

    MachineInfo_t machineInfo = Machine_GetInfo(target_machine);
    if (machineInfo.s_state != S0)
    {
        Machine_SetState(target_machine, S0);
        return;
    }

    migrating_vms.erase(vm_id);
}

#include <climits>

double Scheduler::CalculateTaskCPUUtilization(TaskId_t task_id)
{
    TaskInfo_t task = GetTaskInfo(task_id);
    // Compute demand based on total instructions and expected runtime.
    double runtime = double(task.target_completion - task.arrival); // in microseconds
    if (runtime <= 0)
        runtime = 1; // avoid division by zero
    double runtime_sec = runtime / 1000000.0;
    // Demand in MIPS = (total instructions in millions) / runtime (in seconds)
    double demand = (double(task.total_instructions) / 1e6) / runtime_sec;
    return demand;
}

static vector<MachineId_t> cachedSortedMachines;
static vector<MachineId_t> fixedSortedMachines; // Fixed pre-sorted machines for extremely fast access
static bool fixedSortInitialized = false;
static Time_t lastSortTime = 0;
static const Time_t SORT_INTERVAL = 5000000; // Resort every 5 seconds (increased from 1 second)
static unordered_map<MachineId_t, double> cachedUtilMap;

/**
 * Calculate appropriate tier sizes based on workload
 *
 * @param totalMachines Total number of machines in the cluster
 * @param activeWorkload Current active workload (number of tasks)
 * @param runningSize Output parameter for running tier size
 * @param intermediateSize Output parameter for intermediate tier size
 */
void Scheduler::CalculateTierSizes(unsigned totalMachines, unsigned activeWorkload,
                                   unsigned &runningSize, unsigned &intermediateSize)
{
    if (activeWorkload == 0)
    {
        runningSize = std::max(2u, totalMachines / 4);
        intermediateSize = std::max(1u, totalMachines / 8);
    }
    else
    {
        double systemLoad = GetSystemLoad();

        if (systemLoad > HIGH_LOAD_THRESHOLD)
        {
            runningSize = std::min(totalMachines, std::max(4u, totalMachines / 2));
            intermediateSize = std::max(2u, totalMachines / 4);
        }
        else if (systemLoad < LOW_LOAD_THRESHOLD)
        {
            runningSize = std::max(2u, totalMachines / 6);
            intermediateSize = std::max(1u, totalMachines / 8);
        }
        else
        {
            runningSize = std::max(3u, totalMachines / 3);
            intermediateSize = std::max(2u, totalMachines / 6);
        }
    }
}

/**
 * Get the current system load
 *
 * @return System load as a value between 0.0 and 1.0
 */
double Scheduler::GetSystemLoad()
{
    double totalLoad = 0.0;
    double totalCapacity = 0.0;

    for (auto machine_id : machines)
    {
        if (machineTiers.find(machine_id) != machineTiers.end() &&
            machineTiers[machine_id] == RUNNING)
        {
            double load = mips_util_map.count(machine_id) ? mips_util_map[machine_id] : 0.0;
            double capacity = (double)CalculateMachineMIPS(machine_id);

            totalLoad += load;
            totalCapacity += capacity;
        }
    }

    return totalCapacity > 0 ? totalLoad / totalCapacity : 0.0;
}

/**
 * Get the load of a specific machine
 *
 * @param machineId ID of the machine
 * @return Machine load as a value between 0.0 and 1.0
 */
double Scheduler::GetMachineLoad(MachineId_t machineId)
{
    MachineInfo_t info = Machine_GetInfo(machineId);
    return info.memory_size > 0 ? (double)info.memory_used / info.memory_size : 0.0;
}

/**
 * Check if a machine is suitable for a task
 *
 * Verifies CPU compatibility and memory availability.
 *
 * @param machineId ID of the machine to check
 * @param taskId ID of the task to check
 * @return True if the machine is suitable for the task, false otherwise
 */
bool Scheduler::IsMachineSuitable(MachineId_t machineId, TaskId_t taskId)
{
    MachineInfo_t info = Machine_GetInfo(machineId);

    if (info.cpu != RequiredCPUType(taskId))
    {
        return false;
    }

    unsigned taskMemory = GetTaskMemory(taskId);
    if (info.memory_used + taskMemory > info.memory_size)
    {
        return false;
    }

    return true;
}

/**
 * Sort machines by utilization
 *
 * @return Vector of machine IDs sorted by utilization (lowest to highest)
 */
vector<MachineId_t> Scheduler::SortMachinesByUtilization()
{
    vector<MachineId_t> sortedMachines = machines;

    sort(sortedMachines.begin(), sortedMachines.end(), [this](MachineId_t a, MachineId_t b)
         {
        double utilA = GetMachineLoad(a);
        double utilB = GetMachineLoad(b);
        return utilA < utilB; });

    return sortedMachines;
}

/**
 * Adjust tiers based on system load
 *
 * @param now Current simulation time
 */
void Scheduler::AdjustTiers(Time_t now)
{
    double systemLoad = GetSystemLoad();
    SimOutput("AdjustTiers(): Current system load: " + to_string(systemLoad), 0);

    unsigned activeWorkload = machine_with_task.size();

    unsigned totalMachines = Machine_GetTotal();
    unsigned runningSize, intermediateSize;
    CalculateTierSizes(totalMachines, activeWorkload, runningSize, intermediateSize);

    unsigned currentRunning = 0;
    unsigned currentIntermediate = 0;
    unsigned currentSwitchedOff = 0;

    for (auto &entry : machineTiers)
    {
        switch (entry.second)
        {
        case RUNNING:
            currentRunning++;
            break;
        case INTERMEDIATE:
            currentIntermediate++;
            break;
        case SWITCHED_OFF:
            currentSwitchedOff++;
            break;
        }
    }

    SimOutput("AdjustTiers(): Current tiers - Running: " + to_string(currentRunning) +
                  ", Intermediate: " + to_string(currentIntermediate) +
                  ", Switched Off: " + to_string(currentSwitchedOff),
              0);
    SimOutput("AdjustTiers(): Target tiers - Running: " + to_string(runningSize) +
                  ", Intermediate: " + to_string(intermediateSize),
              0);

    if (systemLoad > HIGH_LOAD_THRESHOLD && currentRunning < runningSize)
    {
        unsigned machinesNeeded = runningSize - currentRunning;

        for (auto &entry : machineTiers)
        {
            if (machinesNeeded == 0)
                break;

            if (entry.second == INTERMEDIATE)
            {
                ActivateMachine(entry.first, now);
                machinesNeeded--;
            }
        }

        if (machinesNeeded > 0)
        {
            for (auto &entry : machineTiers)
            {
                if (machinesNeeded == 0)
                    break;

                if (entry.second == SWITCHED_OFF)
                {
                    entry.second = INTERMEDIATE;

                    ActivateMachine(entry.first, now);
                    machinesNeeded--;
                }
            }
        }
    }
    else if (systemLoad < LOW_LOAD_THRESHOLD && currentRunning > runningSize)
    {
        unsigned excessMachines = currentRunning - runningSize;

        vector<MachineId_t> sortedMachines = SortMachinesByUtilization();

        for (auto machine_id : sortedMachines)
        {
            if (excessMachines == 0)
                break;

            if (machineTiers[machine_id] == RUNNING)
            {
                MachineInfo_t minfo = Machine_GetInfo(machine_id);
                if (minfo.active_vms == 0)
                {
                    DeactivateMachine(machine_id, now);
                    excessMachines--;
                }
            }
        }
    }
}

/**
 * Activate a machine
 *
 * @param machineId ID of the machine to activate
 * @param now Current simulation time
 */
void Scheduler::ActivateMachine(MachineId_t machineId, Time_t now)
{
    if (machineTiers[machineId] != RUNNING)
    {
        MachineInfo_t minfo = Machine_GetInfo(machineId);

        if (minfo.s_state != S0)
        {
            SimOutput("ActivateMachine(): Waking up machine " + to_string(machineId) +
                          " from state " + to_string(minfo.s_state),
                      0);
            Machine_SetState(machineId, S0);
        }

        machineTiers[machineId] = RUNNING;
        SimOutput("ActivateMachine(): Machine " + to_string(machineId) +
                      " moved to RUNNING tier",
                  0);
    }
}

/**
 * Deactivate a machine
 *
 * @param machineId ID of the machine to deactivate
 * @param now Current simulation time
 */
void Scheduler::DeactivateMachine(MachineId_t machineId, Time_t now)
{
    if (machineTiers[machineId] == RUNNING)
    {
        MachineInfo_t minfo = Machine_GetInfo(machineId);

        if (minfo.active_vms == 0)
        {
            machineTiers[machineId] = INTERMEDIATE;
            SimOutput("DeactivateMachine(): Machine " + to_string(machineId) +
                          " moved to INTERMEDIATE tier",
                      0);

            double systemLoad = GetSystemLoad();
            if (systemLoad < LOW_LOAD_THRESHOLD / 2)
            {
                machineTiers[machineId] = SWITCHED_OFF;
                Machine_SetState(machineId, S5);
                SimOutput("DeactivateMachine(): Machine " + to_string(machineId) +
                              " moved to SWITCHED_OFF tier and powered off",
                          0);
            }
        }
    }
}

/**
 * Find a compatible machine for a task
 *
 * @param cpuType CPU type required by the task
 * @param includeIntermediate Whether to include machines in the intermediate tier
 * @return ID of a compatible machine, or -1 if none found
 */
MachineId_t Scheduler::FindCompatibleMachine(CPUType_t cpuType, bool includeIntermediate)
{
    if (cpuTypeMachines.find(cpuType) == cpuTypeMachines.end() ||
        cpuTypeMachines[cpuType].empty())
    {
        return (MachineId_t)-1;
    }

    for (auto machine_id : cpuTypeMachines[cpuType])
    {
        if (machineTiers[machine_id] == RUNNING)
        {
            MachineInfo_t minfo = Machine_GetInfo(machine_id);
            if (minfo.s_state == S0)
            {
                return machine_id;
            }
        }
    }

    if (includeIntermediate)
    {
        for (auto machine_id : cpuTypeMachines[cpuType])
        {
            if (machineTiers[machine_id] == INTERMEDIATE)
            {
                return machine_id;
            }
        }
    }

    double systemLoad = GetSystemLoad();
    if (systemLoad < LOW_LOAD_THRESHOLD)
    {
        for (auto machine_id : cpuTypeMachines[cpuType])
        {
            if (machineTiers[machine_id] == SWITCHED_OFF)
            {
                return machine_id;
            }
        }
    }

    return (MachineId_t)-1;
}

static unordered_map<MachineId_t, unordered_map<VMType_t, vector<VMId_t>>> vmsByMachineAndType;

/**
 * Handle new task arrival
 *
 * Implements the task allocation strategy of E-eco:
 * 1. Try to allocate the task to a machine in the Running tier with compatible CPU
 * 2. If no suitable machine is found, activate a machine from the Intermediate tier
 * 3. Report SLA violation if no suitable machine is found
 *
 * @param now Current simulation time
 * @param task_id ID of the newly arrived task
 */
void Scheduler::NewTask(Time_t now, TaskId_t task_id)
{
    SimOutput("Scheduler::NewTask(): Processing task " + to_string(task_id), 3);

    VMType_t vm_type = RequiredVMType(task_id);
    CPUType_t cpu_type = RequiredCPUType(task_id);
    unsigned memory = GetTaskMemory(task_id);
    SLAType_t sla = RequiredSLA(task_id);
    bool gpu_capable = IsTaskGPUCapable(task_id);

    Priority_t priority;
    if (sla == SLA0)
    {
        priority = HIGH_PRIORITY;
    }
    else if (sla == SLA1)
    {
        priority = MID_PRIORITY;
    }
    else
    {
        priority = LOW_PRIORITY;
    }

    double taskLoad = CalculateTaskCPUUtilization(task_id);
    bool placed = false;

    for (auto &entry : machineTiers)
    {
        MachineId_t machine_id = entry.first;
        HostTier tier = entry.second;

        if (tier != RUNNING)
            continue;

        if (IsMachineSuitable(machine_id, task_id))
        {
            MachineInfo_t machine_info = Machine_GetInfo(machine_id);

            bool vmFound = false;
            for (size_t i = 0; i < vms.size(); i++)
            {
                VMInfo_t vm_info = VM_GetInfo(vms[i]);
                if (vm_info.machine_id == machine_id &&
                    vm_info.vm_type == vm_type &&
                    vm_info.cpu == cpu_type)
                {

                    if (!IsVMReady(vms[i]))
                    {
                        continue;
                    }

                    SimOutput("NewTask(): Assigning task " + to_string(task_id) +
                                  " to VM " + to_string(vms[i]) +
                                  " on machine " + to_string(machine_id),
                              0);
                    VM_AddTask(vms[i], task_id, priority);
                    mips_util_map[machine_id] += taskLoad;
                    machine_with_task[task_id] = machine_id;
                    vmFound = true;
                    placed = true;
                    break;
                }
            }

            if (!vmFound)
            {
                VMId_t new_vm = VM_Create(vm_type, cpu_type);

                try
                {
                    VM_Attach(new_vm, machine_id);
                    VM_AddTask(new_vm, task_id, priority);
                    mips_util_map[machine_id] += taskLoad;
                    machine_with_task[task_id] = machine_id;
                    vms.push_back(new_vm);
                    placed = true;
                    SimOutput("NewTask(): Created new VM " + to_string(new_vm) +
                                  " on machine " + to_string(machine_id),
                              0);
                }
                catch (...)
                {
                    SimOutput("NewTask(): Failed to attach VM to machine " +
                                  to_string(machine_id),
                              1);
                }
            }

            if (placed)
                break;
        }
    }

    if (!placed)
    {
        MachineId_t machine_id = FindCompatibleMachine(cpu_type, true);

        if (machine_id != (MachineId_t)-1)
        {
            ActivateMachine(machine_id, now);

            VMId_t new_vm = VM_Create(vm_type, cpu_type);
            PendingAttachment pa;
            pa.vm = new_vm;
            pa.machine_id = machine_id;
            pa.task_id = task_id;
            pa.priority = priority;
            pa.demand = taskLoad;
            pendingAttachments.push_back(pa);

            mips_util_map[machine_id] += taskLoad;
            machine_with_task[task_id] = machine_id;

            SimOutput("NewTask(): Activating machine " + to_string(machine_id) +
                          " for task " + to_string(task_id),
                      0);
            placed = true;
        }
    }

    if (!placed)
    {
        SimOutput("NewTask(): Could not place task " + to_string(task_id) +
                      " with load " + to_string(taskLoad) +
                      ". No suitable machine available.",
                  1);
    }
}

bool Scheduler::IsVMReady(VMId_t vm)
{
    // Check if the VM is marked as migrating.
    if (migrating_vms.find(vm) != migrating_vms.end())
        return false;
    VMInfo_t info = VM_GetInfo(vm);
    // Check if the VM is attached to a valid machine.
    if (info.machine_id >= Machine_GetTotal()) // or whatever the invalid ID is
        return false;

    MachineInfo_t machineInfo = Machine_GetInfo(info.machine_id);
    // Check if the machine is in an active state (for example, S0 or S0i1).
    if (machineInfo.s_state != S0) // S5 means powered off
        return false;
    return true;
}

string pendingAttachmentsToString(const std::vector<Scheduler::PendingAttachment> &attachments)
{
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < attachments.size(); ++i)
    {
        const auto &pa = attachments[i];
        oss << "{vm: " << pa.vm
            << ", machine_id: " << pa.machine_id
            << ", task_id: " << pa.task_id
            << ", priority: " << pa.priority
            << ", demand: " << pa.demand << "}";
        if (i != attachments.size() - 1)
            oss << ", ";
    }
    oss << "]";
    return oss.str();
}

static const Time_t MIGRATION_INTERVAL = 20000000; // 20 seconds between migrations (increased from 5)

/**
 * Perform periodic maintenance
 *
 * Adjusts tier sizes based on current system load and
 * activates/deactivates machines as needed.
 *
 * @param now Current simulation time
 */
void Scheduler::PeriodicCheck(Time_t now)
{
    static Time_t lastTierAdjustment = 0;
    static const Time_t TIER_ADJUSTMENT_INTERVAL = 10000000; // 10 seconds between tier adjustments

    if (now - lastTierAdjustment < TIER_ADJUSTMENT_INTERVAL)
    {
        return;
    }

    for (auto it = pendingAttachments.begin(); it != pendingAttachments.end();)
    {
        MachineInfo_t minfo = Machine_GetInfo(it->machine_id);
        if (minfo.s_state == S0)
        {
            try
            {
                VM_Attach(it->vm, it->machine_id);
                VM_AddTask(it->vm, it->task_id, it->priority);
                vms.push_back(it->vm);
                SimOutput("PeriodicCheck(): Attached pending VM " + to_string(it->vm) +
                              " on machine " + to_string(it->machine_id),
                          3);
                it = pendingAttachments.erase(it);
            }
            catch (...)
            {
                SimOutput("PeriodicCheck(): Failed to attach pending VM " + to_string(it->vm) +
                              " on machine " + to_string(it->machine_id),
                          1);
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }

    AdjustTiers(now);

    lastTierAdjustment = now;
}

void Scheduler::Shutdown(Time_t time)
{
    // Do your final reporting and bookkeeping here.
    // Report about the total energy consumed
    // Report about the SLA compliance
    // Shutdown everything to be tidy :-)
    for (auto &vm : vms)
    {
        VM_Shutdown(vm);
    }
    SimOutput("SimulationComplete(): Finished!", 4);
    SimOutput("SimulationComplete(): Time is " + to_string(time), 4);
}

/**
 * Handle task completion
 *
 * Updates machine loads and adjusts tiers based on
 * the new system state after task completion.
 *
 * @param now Current simulation time
 * @param task_id ID of the completed task
 */
void Scheduler::TaskComplete(Time_t now, TaskId_t task_id)
{
    SimOutput("Scheduler::TaskComplete(): Task " + to_string(task_id) + " completed", 3);

    if (machine_with_task.find(task_id) != machine_with_task.end())
    {
        MachineId_t machine_id = machine_with_task[task_id];
        double demand = CalculateTaskCPUUtilization(task_id);

        mips_util_map[machine_id] -= demand;
        if (mips_util_map[machine_id] < 0)
        {
            mips_util_map[machine_id] = 0;
        }

        machine_with_task.erase(task_id);

        MachineInfo_t minfo = Machine_GetInfo(machine_id);
        if (minfo.active_vms == 0 && mips_util_map[machine_id] <= 0)
        {
            if (machineTiers[machine_id] == RUNNING)
            {
                double systemLoad = GetSystemLoad();
                if (systemLoad < LOW_LOAD_THRESHOLD)
                {
                    DeactivateMachine(machine_id, now);
                }
            }
        }
    }
}

static const Time_t SLA_MIGRATION_INTERVAL = 20000000; // 20 seconds between SLA migrations (increased from 2)
static std::unordered_set<TaskId_t> recentlyHandledSLAs;

/**
 * Handle SLA warnings
 *
 * Responds to SLA violation warnings by attempting to
 * improve resource allocation for the affected task.
 *
 * @param time Current simulation time
 * @param task_id ID of the task with SLA warning
 */
void Scheduler::SLAWarning(Time_t time, TaskId_t task_id)
{
    SimOutput("SLAWarning(): Task " + to_string(task_id) + " is violating SLA", 1);

    if (recentlyHandledSLAs.find(task_id) != recentlyHandledSLAs.end())
    {
        return;
    }

    if (machine_with_task.find(task_id) == machine_with_task.end())
    {
        return;
    }

    MachineId_t machine_id = machine_with_task[task_id];
    MachineInfo_t minfo = Machine_GetInfo(machine_id);

    double utilization = CalculateCPUUtilization(machine_id);
    if (utilization > HIGH_LOAD_THRESHOLD)
    {
        CPUType_t cpu_type = RequiredCPUType(task_id);
        MachineId_t new_machine = FindCompatibleMachine(cpu_type, true);

        if (new_machine != (MachineId_t)-1)
        {
            ActivateMachine(new_machine, time);
            SimOutput("SLAWarning(): Activating machine " + to_string(new_machine) +
                          " to handle SLA violation for task " + to_string(task_id),
                      2);
        }
    }

    recentlyHandledSLAs.insert(task_id);
}

void Scheduler::MigrateVM(VMId_t vm, MachineId_t target_machine)
{
    // This function migrates an entire VM to a target machine.
    VMInfo_t vmInfo = VM_GetInfo(vm);
    MachineInfo_t sourceInfo = Machine_GetInfo(vmInfo.machine_id);
    MachineInfo_t targetInfo = Machine_GetInfo(target_machine);

    if (sourceInfo.s_state != S0)
    {
        SimOutput("MigrateVM(): Source machine " + to_string(vmInfo.machine_id) +
                      " is not in S0 state. Current state: " + to_string(sourceInfo.s_state) +
                      ". Cannot migrate VM " + to_string(vm),
                  2);
        return;
    }

    if (targetInfo.s_state != S0)
    {
        SimOutput("MigrateVM(): Target machine " + to_string(target_machine) +
                      " is not in S0 state. Current state: " + to_string(targetInfo.s_state) +
                      ". Cannot migrate VM " + to_string(vm),
                  2);

        if (targetInfo.s_state != S0)
        {
            SimOutput("MigrateVM(): Attempting to wake up target machine " +
                          to_string(target_machine),
                      2);
            Machine_SetState(target_machine, S0);
        }
        return;
    }

    double vmTotalDemand = 0.0;
    for (auto t_id : vmInfo.active_tasks)
    {
        double tDemand = CalculateTaskCPUUtilization(t_id);
        vmTotalDemand += tDemand;
        // Update the machine mapping for each task to the target machine.
        machine_with_task[t_id] = target_machine;
    }

    // Update the load mapping: subtract from source and add to target.
    mips_util_map[vmInfo.machine_id] -= vmTotalDemand;
    mips_util_map[target_machine] += vmTotalDemand;

    migrating_vms.insert(vm);
    SimOutput("MigrateVM(): Migrating VM " + to_string(vm) +
                  " from machine " + to_string(vmInfo.machine_id) +
                  " to machine " + to_string(target_machine),
              3);

    try
    {
        VM_Migrate(vm, target_machine);
    }
    catch (...)
    {
        SimOutput("MigrateVM(): Failed to migrate VM " + to_string(vm) +
                      " to machine " + to_string(target_machine),
                  1);

        mips_util_map[vmInfo.machine_id] += vmTotalDemand;
        mips_util_map[target_machine] -= vmTotalDemand;

        for (auto t_id : vmInfo.active_tasks)
        {
            machine_with_task[t_id] = vmInfo.machine_id;
        }

        migrating_vms.erase(vm);
    }
}

double Scheduler::CalculateCPUUtilization(MachineId_t machine_id)
{
    double required_mips = mips_util_map[machine_id];
    // Use the actual MIPS capacity from the machine rather than p_state.
    double machine_capacity = (double)CalculateMachineMIPS(machine_id);
    // Avoid division by zero if capacity is zero.
    if (machine_capacity <= 0)
        machine_capacity = 1;
    double cpu_utilization = required_mips / machine_capacity;
    return cpu_utilization;
}
double Scheduler::CalculateMemoryUtilization(MachineId_t machine_id)
{
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    double used_memory = machine_info.memory_used;
    double total_memory = machine_info.memory_size;
    return used_memory / total_memory;
}
unsigned Scheduler::CalculateMachineMIPS(MachineId_t machine_id)
{
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    CPUPerformance_t p_state = machine_info.p_state;
    switch (p_state)
    {
    case P0:
        return machine_info.performance[0];
    case P1:
        return machine_info.performance[1];
    case P2:
        return machine_info.performance[2];
    default:
        return machine_info.performance[3];
    }
}

/*
 * This function gives us how much memory is required for a task
 */
double Scheduler::CalculateTaskMemoryUtilization(TaskId_t task_id)
{
    TaskInfo_t task = GetTaskInfo(task_id);
    return task.required_memory;
}

void Scheduler::StateChangeComplete(Time_t time, MachineId_t machine_id)
{
    // When a machine finishes a state change (wakes up), process pending attachments.
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    if (machine_info.s_state != S0)
    {
        SimOutput("StateChangeComplete(): Machine " + to_string(machine_id) +
                      " is not in S0 state yet. Current state: " + to_string(machine_info.s_state),
                  3);
        return; // Don't process attachments until machine is fully awake
    }

    SimOutput("StateChangeComplete(): Machine " + to_string(machine_id) +
                  " is now in S0 state. Processing pending attachments.",
              3);

    for (auto it = pendingAttachments.begin(); it != pendingAttachments.end();)
    {
        if (it->machine_id == machine_id)
        {
            machine_info = Machine_GetInfo(machine_id);
            if (machine_info.s_state != S0)
            {
                SimOutput("StateChangeComplete(): Machine " + to_string(machine_id) +
                              " state changed during processing. Skipping attachments.",
                          3);
                return; // Exit if machine state changed
            }

            SimOutput("StateChangeComplete(): Attaching pending VM " + to_string(it->vm) +
                          " on machine " + to_string(machine_id),
                      3);
            try
            {
                VM_Attach(it->vm, machine_id);
                VM_AddTask(it->vm, it->task_id, it->priority);
                mips_util_map[machine_id] += it->demand;
                machine_with_task[it->task_id] = machine_id;
                it = pendingAttachments.erase(it);
            }
            catch (...)
            {
                SimOutput("StateChangeComplete(): Failed to attach pending VM " + to_string(it->vm) +
                              " on machine " + to_string(machine_id) + " due to error",
                          3);
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }
}

// Public interface below

static Scheduler Scheduler;

void InitScheduler()
{
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id)
{
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id)
{
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id)
{
    // The simulator is alerting you that machine identified by machine_id is overcommitted
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id)
{
    // The function is called on to alert you that migration is complete
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 0);
    Scheduler.MigrationComplete(time, vm_id);
    migrating = false;
}

void SchedulerCheck(Time_t time)
{
    // This function is called periodically by the simulator, no specific event
    SimOutput("SchedulerCheck(): SchedulerCheck() called at " + to_string(time), 4);
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time)
{
    // This function is called before the simulation terminates Add whatever you feel like.
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl; // SLA3 do not have SLA violation issues
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time) / 1000000 << " seconds" << endl;
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);

    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id)
{
    Scheduler.SLAWarning(time, task_id);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id)
{
    // Called in response to an earlier request to change the state of a machine
    Scheduler.StateChangeComplete(time, machine_id);
}
