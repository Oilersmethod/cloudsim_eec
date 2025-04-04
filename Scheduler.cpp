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

void Scheduler::Init()
{
    // Find the parameters of the clusters
    // Get the total number of machines
    // For each machine:
    //      Get the type of the machine
    //      Get the memory of the machine
    //      Get the number of CPUs
    //      Get if there is a GPU or not
    //
    SimOutput("Scheduler::Init(): Total number of machines is " + to_string(Machine_GetTotal()), 0);
    SimOutput("Scheduler::Init(): Initializing scheduler", 0);
    active_machines = Machine_GetTotal();

    for (unsigned i = 0; i < active_machines; i++)
    {
        MachineInfo_t machine = Machine_GetInfo(MachineId_t(i));
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

        machines.push_back(MachineId_t(i));
    }

    vms.insert(vms.end(), linux.begin(), linux.end());
    vms.insert(vms.end(), linux_rt.begin(), linux_rt.end());
    vms.insert(vms.end(), win.begin(), win.end());
    vms.insert(vms.end(), aix.begin(), aix.end());

    SimOutput("Scheduler::Init(): Created " + to_string(vms.size()) + " VMs across " + to_string(active_machines) + " machines", 3);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id)
{
    VMInfo_t vmInfo = VM_GetInfo(vm_id);
    MachineId_t target_machine = vmInfo.machine_id;

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
static Time_t lastSortTime = 0;
static const Time_t SORT_INTERVAL = 1000000; // Resort every 1 second of simulation time

vector<MachineId_t> Scheduler::SortMachinesByUtilization()
{
    Time_t currentTime = Now();
    if (!cachedSortedMachines.empty() && (currentTime - lastSortTime < SORT_INTERVAL))
    {
        return cachedSortedMachines;
    }

    vector<MachineId_t> sortedMachines = machines; // copy all machine IDs

    unordered_map<MachineId_t, double> utilMap;
    for (auto machine_id : machines)
    {
        double cpuUtil = CalculateCPUUtilization(machine_id);
        double memUtil = CalculateMemoryUtilization(machine_id);
        utilMap[machine_id] = std::max(cpuUtil, memUtil);
    }

    sort(sortedMachines.begin(), sortedMachines.end(), [&](MachineId_t a, MachineId_t b)
         { return utilMap[a] < utilMap[b]; });

    cachedSortedMachines = sortedMachines;
    lastSortTime = currentTime;

    return sortedMachines;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id)
{
    // Get the task parameters
    VMType_t vm_type = RequiredVMType(task_id);
    CPUType_t cpu_type = RequiredCPUType(task_id);
    unsigned memory = GetTaskMemory(task_id);
    bool gpu_capable = IsTaskGPUCapable(task_id);
    SLAType_t sla = RequiredSLA(task_id);

    Priority_t priority = (sla == SLA0)   ? HIGH_PRIORITY
                          : (sla == SLA1) ? MID_PRIORITY
                                          : LOW_PRIORITY;

    double taskLoad = CalculateTaskCPUUtilization(task_id);
    bool placed = false;

    // Get all machines sorted by utilization (in increasing order)
    vector<MachineId_t> sortedMachines = SortMachinesByUtilization();

    // Scan all powered-on (S0) machines that meet basic requirements and
    // choose the machine that minimizes the combined utilization after adding the task.
    MachineId_t bestMachine = UINT_MAX;
    double bestCombinedUtil = std::numeric_limits<double>::max();
    for (auto machine_id : sortedMachines)
    {
        MachineInfo_t machine_info = Machine_GetInfo(machine_id);
        // Only consider machines that are already powered on.
        if (machine_info.s_state != S0)
            continue;
        if (machine_info.cpu != cpu_type)
            continue;
        if (gpu_capable && !machine_info.gpus)
            continue;
        if (machine_info.memory_size - machine_info.memory_used < memory)
            continue;

        double machineCapacity = (double)CalculateMachineMIPS(machine_id);
        double currentLoad = mips_util_map.count(machine_id) ? mips_util_map[machine_id] : 0.0;
        double combinedUtil = (currentLoad + taskLoad) / machineCapacity;
        // Note: We allow combinedUtil to be >= 1.0 (overcommitment) if necessary.
        if (combinedUtil < bestCombinedUtil)
        {
            bestCombinedUtil = combinedUtil;
            bestMachine = machine_id;
        }
    }

    if (bestMachine != UINT_MAX)
    {
        // Try to assign the task to an existing VM on bestMachine.
        for (auto vm_id : vms)
        {
            VMInfo_t vm_info = VM_GetInfo(vm_id);
            if (vm_info.machine_id != bestMachine)
                continue;
            if (vm_info.vm_type != vm_type || vm_info.cpu != cpu_type)
                continue;
            if (!IsVMReady(vm_id))
                continue;

            VM_AddTask(vm_id, task_id, priority);
            mips_util_map[bestMachine] += taskLoad;
            machine_with_task[task_id] = bestMachine;
            SimOutput("NewTask(): Assigned task " + to_string(task_id) +
                          " to existing VM " + to_string(vm_id) +
                          " on machine " + to_string(bestMachine),
                      0);
            placed = true;
            break;
        }

        // If no suitable VM exists, create a new one on bestMachine.
        if (!placed)
        {
            VMId_t new_vm = VM_Create(vm_type, cpu_type);
            try
            {
                VM_Attach(new_vm, bestMachine);
                VM_AddTask(new_vm, task_id, priority);
                mips_util_map[bestMachine] += taskLoad;
                machine_with_task[task_id] = bestMachine;
                SimOutput("NewTask(): Created new VM " + to_string(new_vm) +
                              " for task " + to_string(task_id) +
                              " on machine " + to_string(bestMachine),
                          0);
                placed = true;
            }
            catch (...)
            {
                // If attaching the VM fails, add to pending attachments.
                PendingAttachment pa;
                pa.vm = new_vm;
                pa.machine_id = bestMachine;
                pa.task_id = task_id;
                pa.priority = priority;
                pa.demand = taskLoad;
                pendingAttachments.push_back(pa);
                mips_util_map[bestMachine] += taskLoad;
                machine_with_task[task_id] = bestMachine;
                SimOutput("NewTask(): Created VM " + to_string(new_vm) +
                              " (pending attachment) for task " + to_string(task_id),
                          0);
                placed = true;
            }
        }
    }
    else
    {
        // No powered-on machine could host the task. Try to wake up a machine that meets basic requirements.
        for (auto machine_id : machines)
        {
            MachineInfo_t machine_info = Machine_GetInfo(machine_id);
            // Only consider machines that are currently not in S0.
            if (machine_info.s_state == S0 || machine_info.cpu != cpu_type)
                continue;
            if (gpu_capable && !machine_info.gpus)
                continue;
            if (machine_info.memory_size < memory)
                continue;

            // Wake up the machine.
            Machine_SetState(machine_id, S0);
            SimOutput("NewTask(): Waking up machine " + to_string(machine_id) +
                          " for future placement of task " + to_string(task_id),
                      2);

            // Create a new VM for this task and add a pending attachment.
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
            placed = true;
            break;
        }

        if (!placed)
        {
            SimOutput("NewTask(): Could not place task " + to_string(task_id) +
                          " with load " + to_string(taskLoad) +
                          " due to insufficient capacity. SLA violation.",
                      0);
        }
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

static Time_t lastMigrationTime = 0;
static const Time_t MIGRATION_INTERVAL = 5000000; // 5 seconds between migrations

void Scheduler::PeriodicCheck(Time_t now)
{
    const double overloadThreshold = 0.95; // Increased threshold to reduce migrations

    for (auto it = pendingAttachments.begin(); it != pendingAttachments.end();)
    {
        MachineInfo_t minfo = Machine_GetInfo(it->machine_id);
        if (minfo.s_state == S0)
        {
            try
            {
                VM_Attach(it->vm, it->machine_id);
                VM_AddTask(it->vm, it->task_id, it->priority);
                SimOutput("PeriodicCheck(): Attached pending VM " + to_string(it->vm) +
                              " on machine " + to_string(it->machine_id),
                          3);
                it = pendingAttachments.erase(it);
            }
            catch (...)
            {
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }

    bool shouldCheckMigrations = (now - lastMigrationTime >= MIGRATION_INTERVAL);

    for (auto machine_id : machines)
    {
        double currentLoad = mips_util_map.count(machine_id) ? mips_util_map[machine_id] : 0.0;

        if (currentLoad <= 0)
        {
            bool hasPending = false;
            for (const auto &pa : pendingAttachments)
            {
                if (pa.machine_id == machine_id)
                {
                    hasPending = true;
                    break;
                }
            }

            if (!hasPending)
            {
                MachineInfo_t minfo = Machine_GetInfo(machine_id);
                if (minfo.s_state != S5 && minfo.active_vms == 0)
                {
                    Machine_SetState(machine_id, S5);
                }
            }
            continue; // Skip further processing for this machine
        }

        if (!shouldCheckMigrations)
            continue;

        unsigned machineMIPS = CalculateMachineMIPS(machine_id);
        double utilization = currentLoad / double(machineMIPS);

        if (utilization > overloadThreshold)
        {
            VMId_t bestVmToMigrate = UINT_MAX;
            MachineId_t bestTargetMachine = UINT_MAX;
            double bestVmLoad = 0.0;

            for (auto vm : vms)
            {
                VMInfo_t vmInfo = VM_GetInfo(vm);
                if (vmInfo.machine_id != machine_id || migrating_vms.find(vm) != migrating_vms.end())
                    continue;

                double vmLoad = 0.0;
                for (auto t_id : vmInfo.active_tasks)
                {
                    vmLoad += CalculateTaskCPUUtilization(t_id);
                }

                if (vmLoad <= 0)
                    continue;

                vector<MachineId_t> sortedMachines = SortMachinesByUtilization();
                for (auto target : sortedMachines)
                {
                    if (target == machine_id)
                        continue;

                    MachineInfo_t targetInfo = Machine_GetInfo(target);
                    if (targetInfo.cpu != vmInfo.cpu || targetInfo.s_state != S0)
                        continue;

                    double targetUtil = CalculateCPUUtilization(target);
                    if (targetUtil + vmLoad < overloadThreshold)
                    {
                        if (bestVmToMigrate == UINT_MAX || vmLoad < bestVmLoad)
                        {
                            bestVmToMigrate = vm;
                            bestTargetMachine = target;
                            bestVmLoad = vmLoad;
                        }
                        break; // Found a target for this VM
                    }
                }
            }

            if (bestVmToMigrate != UINT_MAX && bestTargetMachine != UINT_MAX)
            {
                SimOutput("PeriodicCheck(): Migrating VM " + to_string(bestVmToMigrate) +
                              " from overloaded machine " + to_string(machine_id) +
                              " to machine " + to_string(bestTargetMachine),
                          3);
                MigrateVM(bestVmToMigrate, bestTargetMachine);
                lastMigrationTime = now; // Update last migration time
                break;                   // Only do one migration per periodic check
            }
        }
    }

    if (shouldCheckMigrations)
    {
        lastMigrationTime = now;
    }
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

static Time_t lastTaskCompleteMigrationTime = 0;
static const Time_t TASK_COMPLETE_MIGRATION_INTERVAL = 10000000; // 10 seconds between migrations

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id)
{
    if (machine_with_task.find(task_id) == machine_with_task.end())
    {
        return; // Task not found, nothing to do
    }

    MachineId_t machine_id = machine_with_task[task_id];
    double demand = CalculateTaskCPUUtilization(task_id);
    mips_util_map[machine_id] -= demand;
    machine_with_task.erase(task_id);
    if (mips_util_map[machine_id] < 0)
        mips_util_map[machine_id] = 0;

    bool shouldCheckMigrations = (now - lastTaskCompleteMigrationTime >= TASK_COMPLETE_MIGRATION_INTERVAL);
    if (shouldCheckMigrations)
    {
        vector<MachineId_t> sortedMachines = SortMachinesByUtilization();
        const size_t MAX_MACHINES_TO_CHECK = 5; // Only check a few machines

        size_t machinesChecked = 0;
        for (auto machine_id : sortedMachines)
        {
            if (machinesChecked >= MAX_MACHINES_TO_CHECK)
                break;

            double u = CalculateCPUUtilization(machine_id);
            if (u <= 0.2)
            { // Only consider machines with low utilization
                MachineInfo_t minfo = Machine_GetInfo(machine_id);
                if (minfo.active_vms == 0)
                    continue;

                for (auto vm : vms)
                {
                    VMInfo_t vmInfo = VM_GetInfo(vm);
                    if (vmInfo.machine_id != machine_id || vmInfo.active_tasks.empty() ||
                        migrating_vms.find(vm) != migrating_vms.end())
                        continue;

                    for (auto target_machine : sortedMachines)
                    {
                        if (target_machine <= machine_id)
                            continue;

                        double vm_load = 0.0;
                        for (auto t_id : vmInfo.active_tasks)
                        {
                            vm_load += CalculateTaskCPUUtilization(t_id);
                        }

                        if (vm_load < 0.1)
                            continue;

                        double target_u = CalculateCPUUtilization(target_machine);
                        if (target_u + vm_load < 0.9)
                        { // Use a threshold to avoid overloading
                            MachineInfo_t targetInfo = Machine_GetInfo(target_machine);
                            if (targetInfo.cpu != vmInfo.cpu)
                                continue;
                            if (targetInfo.s_state != S0)
                                continue;

                            MigrateVM(vm, target_machine);
                            lastTaskCompleteMigrationTime = now;
                            return; // Only do one migration per task completion
                        }
                    }
                }

                machinesChecked++;
            }
        }

        lastTaskCompleteMigrationTime = now;
    }

    if (mips_util_map[machine_id] <= 0)
    {
        MachineInfo_t minfo = Machine_GetInfo(machine_id);
        if (minfo.s_state != S5 && minfo.active_vms == 0)
        {
            bool hasPending = false;
            for (const auto &pa : pendingAttachments)
            {
                if (pa.machine_id == machine_id)
                {
                    hasPending = true;
                    break;
                }
            }

            if (!hasPending)
            {
                Machine_SetState(machine_id, S5);
            }
        }
    }
}

static Time_t lastSLAMigrationTime = 0;
static const Time_t SLA_MIGRATION_INTERVAL = 2000000; // 2 seconds between SLA migrations
static std::unordered_set<TaskId_t> recentlyHandledSLAs;

void Scheduler::SLAWarning(Time_t time, TaskId_t task_id)
{
    if (recentlyHandledSLAs.find(task_id) != recentlyHandledSLAs.end())
    {
        return; // Skip if we've recently handled this task
    }

    bool shouldHandleSLA = (time - lastSLAMigrationTime >= SLA_MIGRATION_INTERVAL);
    if (!shouldHandleSLA)
    {
        return; // Skip if we've recently handled any SLA
    }

    recentlyHandledSLAs.insert(task_id);

    if (recentlyHandledSLAs.size() > 100)
    {
        recentlyHandledSLAs.clear(); // Reset if too many entries
    }

    lastSLAMigrationTime = time;

    if (machine_with_task.find(task_id) == machine_with_task.end())
    {
        return; // Task not found, nothing to do
    }

    MachineId_t current_machine = machine_with_task[task_id];
    double task_load = CalculateTaskCPUUtilization(task_id);

    vector<MachineId_t> sortedMachines = SortMachinesByUtilization();

    bool migrated = false;
    for (auto target_machine : sortedMachines)
    {
        if (target_machine == current_machine)
            continue;

        MachineInfo_t targetInfo = Machine_GetInfo(target_machine);
        if (targetInfo.cpu != RequiredCPUType(task_id))
            continue;
        if (IsTaskGPUCapable(task_id) && !targetInfo.gpus)
            continue;

        double target_u = CalculateCPUUtilization(target_machine);
        if (target_u + task_load >= 0.95)
            continue; // Use threshold to avoid overloading

        VMId_t hosting_vm = 0;
        bool found = false;

        for (auto vm : vms)
        {
            VMInfo_t vmInfo = VM_GetInfo(vm);
            if (vmInfo.machine_id != current_machine)
                continue;

            for (auto t_id : vmInfo.active_tasks)
            {
                if (t_id == task_id)
                {
                    hosting_vm = vm;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }

        if (found && migrating_vms.find(hosting_vm) == migrating_vms.end())
        {
            if (targetInfo.s_state == S0)
            {
                MigrateVM(hosting_vm, target_machine);
                migrated = true;
                break;
            }
        }
    }

    if (!migrated)
    {
        int machinesChecked = 0;
        for (auto machine_id : machines)
        {
            if (machinesChecked >= 5)
                break; // Limit checks

            MachineInfo_t minfo = Machine_GetInfo(machine_id);
            if (minfo.s_state == S5 && minfo.cpu == RequiredCPUType(task_id))
            {
                if (IsTaskGPUCapable(task_id) && !minfo.gpus)
                    continue;

                Machine_SetState(machine_id, S0);
                break;
            }
            machinesChecked++;
        }
    }
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
