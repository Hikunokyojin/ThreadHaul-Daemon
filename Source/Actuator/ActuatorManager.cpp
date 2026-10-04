#include "ActuatorManager.h"

namespace actuator {

namespace {

// Demoting or suspending any of these can hang or crash Windows, so they are
// protected even if the configured list leaves them out.
const wchar_t* const kAlwaysProtected[] = {
    L"[System Process]", L"System", L"Registry", L"smss.exe", L"csrss.exe",
    L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe",
};

bool NamesEqualIgnoreCase(const std::wstring& a, const wchar_t* b) {
    return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}

}

ActuatorManager::ActuatorManager(StateMachineConfig config,
                                  TransitionCallback onTransition)
    : config_(config)
    , onTransition_(std::move(onTransition)) {}

bool ActuatorManager::AddBatchProcess(const std::wstring& processName, DWORD processId) {
    if (IsProtected(processName, processId)) {
        return false;
    }
    if (machines_.find(processId) != machines_.end()) {
        return true;
    }
    machines_[processId] = std::make_unique<ProcessStateMachine>(
        processName, processId, config_, onTransition_);
    return true;
}

void ActuatorManager::SetProtectedProcessNames(std::vector<std::wstring> names) {
    protectedNames_ = std::move(names);
    for (auto it = machines_.begin(); it != machines_.end();) {
        if (IsProtected(it->second->BatchProcessName(), it->first)) {
            it->second->ForceRecoverForShutdown();
            it = machines_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ActuatorManager::IsProtected(const std::wstring& processName, DWORD processId) const {
    if (processId == GetCurrentProcessId()) {
        return true;
    }
    for (const wchar_t* name : kAlwaysProtected) {
        if (NamesEqualIgnoreCase(processName, name)) {
            return true;
        }
    }
    for (const std::wstring& name : protectedNames_) {
        if (NamesEqualIgnoreCase(processName, name.c_str())) {
            return true;
        }
    }
    return false;
}

void ActuatorManager::RemoveBatchProcess(DWORD processId) {
    machines_.erase(processId);
}

void ActuatorManager::OnThresholdEvent(const ThresholdEvent& evt) {
    // See INTERFACES.md section 2: evt.criticalProcessId is informational
    // context only. The actual state-machine input is the observed CPU%,
    // applied to every batch process this manager already owns.
    UpdateAll(evt.observedCpuPercent);
}

void ActuatorManager::UpdateAll(double criticalProcessCpuPercent) {
    for (auto it = machines_.begin(); it != machines_.end();) {
        if (it->second->HasExited()) {
            it = machines_.erase(it);
            continue;
        }
        it->second->Update(criticalProcessCpuPercent);
        ++it;
    }
}

void ActuatorManager::ForceRecoverAllForShutdown() {
    for (auto& [pid, machine] : machines_) {
        machine->ForceRecoverForShutdown();
    }
}

}
