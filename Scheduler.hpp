//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#ifndef Scheduler_hpp
#define Scheduler_hpp

#include <vector>
#include <map>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#include "Interfaces.h"

/**
 * Class that implements the full-power algorithm for cloud scheduling
 *
 * This scheduler maximizes performance and minimizes SLA violations with the following strategies:
 * 1. Keeps all machines powered on (S0 state) at all times
 * 2. Maintains VMs for all CPU types on each machine
 * 3. Prioritizes GPU machines for GPU-capable tasks
 * 4. Ensures CPU compatibility for task allocation
 *
 * The full-power algorithm serves as a baseline for minimal SLA violations
 * at the expense of higher energy consumption.
 */
class Scheduler
{
public:
    /**
     * Default constructor
     */
    Scheduler() {}

    /**
     * Initialize the scheduler
     *
     * Discovers all available machines, creates VMs for running tier machines,
     * and initializes the three-tier system.
     */
    void Init();

    /**
     * Handle VM migration completion
     *
     * Updates tracking data structures after a VM has been migrated.
     *
     * @param time Current simulation time
     * @param vm_id ID of the VM that completed migration
     */
    void MigrationComplete(Time_t time, VMId_t vm_id);

    /**
     * Handle new task arrival
     *
     * Allocates a new task to an appropriate machine based on
     * CPU compatibility and current tier status.
     *
     * @param now Current simulation time
     * @param task_id ID of the newly arrived task
     */
    void NewTask(Time_t now, TaskId_t task_id);

    /**
     * Perform periodic maintenance
     *
     * Adjusts tier sizes based on current system load and
     * activates/deactivates machines as needed.
     *
     * @param now Current simulation time
     */
    void PeriodicCheck(Time_t now);

    /**
     * Perform final cleanup and reporting
     *
     * Shuts down all VMs and reports final statistics.
     *
     * @param now Final simulation time
     */
    void Shutdown(Time_t now);

    /**
     * Handle task completion
     *
     * Updates machine loads and adjusts tiers based on
     * the new system state after task completion.
     *
     * @param now Current simulation time
     * @param task_id ID of the completed task
     */
    void TaskComplete(Time_t now, TaskId_t task_id);

    /**
     * Handle SLA warnings
     *
     * Responds to SLA violation warnings by attempting to
     * improve resource allocation for the affected task.
     *
     * @param time Current simulation time
     * @param task_id ID of the task with SLA warning
     */
    void SLAWarning(Time_t time, TaskId_t task_id);

    /**
     * Handle machine state change completion
     *
     * Processes pending operations after a machine state change.
     *
     * @param time Current simulation time
     * @param machine_id ID of the machine that changed state
     */
    void StateChangeComplete(Time_t time, MachineId_t machine_id);

    /**
     * Migrate a VM to another machine
     *
     * E-eco minimizes migrations, but this handles any necessary migrations.
     *
     * @param vm_id ID of the VM to migrate
     * @param target_machine ID of the target machine
     */
    void MigrateVM(VMId_t vm_id, MachineId_t target_machine);

    /**
     * Check if a VM is ready for use
     *
     * @param vm ID of the VM to check
     * @return True if the VM is ready, false otherwise
     */
    bool IsVMReady(VMId_t vm);

    /**
     * Calculate CPU utilization for a machine
     *
     * @param machine_id ID of the machine
     * @return CPU utilization as a value between 0.0 and 1.0
     */
    double CalculateCPUUtilization(MachineId_t machine_id);

    /**
     * Calculate memory utilization for a machine
     *
     * @param machine_id ID of the machine
     * @return Memory utilization as a value between 0.0 and 1.0
     */
    double CalculateMemoryUtilization(MachineId_t machine_id);

    /**
     * Calculate CPU utilization for a task
     *
     * @param task_id ID of the task
     * @return CPU utilization as a value between 0.0 and 1.0
     */
    double CalculateTaskCPUUtilization(TaskId_t task_id);

    /**
     * Calculate memory utilization for a task
     *
     * @param task_id ID of the task
     * @return Memory utilization as a value between 0.0 and 1.0
     */
    double CalculateTaskMemoryUtilization(TaskId_t task_id);

    /**
     * Calculate MIPS for a machine
     *
     * @param machine_id ID of the machine
     * @return MIPS value for the machine
     */
    unsigned CalculateMachineMIPS(MachineId_t machine_id);

    /**
     * Scheduler check function (for compatibility)
     */
    void SchedulerCheck(Time_t now) { PeriodicCheck(now); }

    /**
     * Sort machines by utilization
     *
     * @return Vector of machine IDs sorted by utilization (lowest to highest)
     */
    vector<MachineId_t> SortMachinesByUtilization();

    /**
     * Pending attachment structure for handling machine state changes
     */
    struct PendingAttachment
    {
        VMId_t vm;
        MachineId_t machine_id;
        TaskId_t task_id;
        Priority_t priority;
        double demand;
    };

private:
    // Host tier management
    enum HostTier
    {
        RUNNING,      // Active hosts running applications
        INTERMEDIATE, // Standby hosts ready to be activated
        SWITCHED_OFF  // Powered off hosts for energy saving
    };

    // Machine management
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
    vector<VMId_t> win;
    vector<VMId_t> aix;
    vector<VMId_t> linux;
    vector<VMId_t> linux_rt;
    unordered_set<VMId_t> migrating_vms;
    unordered_map<MachineId_t, float> mips_util_map;
    unordered_map<TaskId_t, MachineId_t> machine_with_task;

    // E-eco tier management
    map<MachineId_t, HostTier> machineTiers;             // Track which tier each machine belongs to
    map<MachineId_t, unsigned> machineLoads;             // Track current load for each machine
    map<CPUType_t, vector<MachineId_t>> cpuTypeMachines; // Group machines by CPU type

    // Tier calculation and management
    void CalculateTierSizes(unsigned totalMachines, unsigned activeWorkload,
                            unsigned &runningSize, unsigned &intermediateSize);
    void AdjustTiers(Time_t now);
    void ActivateMachine(MachineId_t machineId, Time_t now);
    void DeactivateMachine(MachineId_t machineId, Time_t now);
    MachineId_t FindCompatibleMachine(CPUType_t cpuType, bool includeIntermediate = false);

    // Load thresholds for tier transitions
    const double HIGH_LOAD_THRESHOLD = 0.7; // 70% utilization threshold for high load
    const double LOW_LOAD_THRESHOLD = 0.3;  // 30% utilization threshold for low load

    // Helper methods
    double GetSystemLoad();
    double GetMachineLoad(MachineId_t machineId);
    bool IsMachineSuitable(MachineId_t machineId, TaskId_t taskId);

    // Pending attachments for handling machine state changes
    vector<PendingAttachment> pendingAttachments;
    unordered_set<MachineId_t> transitioningMachines;
};

#endif /* Scheduler_hpp */
