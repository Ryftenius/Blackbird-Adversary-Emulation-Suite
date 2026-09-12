#include "..\common\bkaes_sample.h"

namespace {

constexpr wchar_t kRelayMode[] = L"--scope-pivot-relay";
constexpr wchar_t kLeafMode[] = L"--scope-pivot-leaf";
constexpr wchar_t kExternalTargetMode[] = L"--scope-pivot-external-target";
constexpr wchar_t kExternalResultName[] = L"scope-pivot-external.txt";
constexpr DWORD kAttachReadyBudgetMs = 15000;
constexpr DWORD kRelayPrePivotBudgetMs = 10000;
constexpr DWORD kLeafLifetimeMs = 30000;
constexpr DWORD kExternalGoBudgetMs = 60000;
constexpr DWORD kRelayGoBudgetMs = 30000;

bool LaunchMode(const wchar_t *mode, const wchar_t *arg1, const wchar_t *arg2,
                const wchar_t *arg3, PROCESS_INFORMATION *process) {
  STARTUPINFOW startup = {};
  std::wstring self = BkaesSelfPath();
  wchar_t commandLine[(MAX_PATH * 2) + 256] = {};

  if (mode == nullptr || process == nullptr || self.empty()) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  startup.cb = sizeof(startup);
  ZeroMemory(process, sizeof(*process));
  if (FAILED(StringCchPrintfW(commandLine, ARRAYSIZE(commandLine),
                              L"\"%ls\" %ls %ls %ls %ls", self.c_str(), mode,
                              arg1 != nullptr ? arg1 : L"-",
                              arg2 != nullptr ? arg2 : L"-",
                              arg3 != nullptr ? arg3 : L"-"))) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return false;
  }

  return CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        process) == TRUE;
}

bool InjectNoop(HANDLE process) {
  static const BYTE stub[] = {0x31, 0xC0, 0xC3};
  PVOID remote = nullptr;
  DWORD oldProtect = 0;
  SIZE_T written = 0;
  HANDLE thread = nullptr;
  bool ok = false;

  if (process == nullptr || process == INVALID_HANDLE_VALUE) {
    SetLastError(ERROR_INVALID_HANDLE);
    return false;
  }

  remote = VirtualAllocEx(process, nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT,
                          PAGE_READWRITE);
  if (remote != nullptr &&
      WriteProcessMemory(process, remote, stub, sizeof(stub), &written) &&
      written == sizeof(stub) &&
      VirtualProtectEx(process, remote, 0x1000, PAGE_EXECUTE_READ,
                       &oldProtect)) {
    thread = CreateRemoteThread(process, nullptr, 0,
                                reinterpret_cast<LPTHREAD_START_ROUTINE>(remote),
                                nullptr, 0, nullptr);
    if (thread != nullptr) {
      ok = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
    }
  }

  if (thread != nullptr) {
    CloseHandle(thread);
  }
  if (remote != nullptr) {
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  }
  return ok;
}

bool BuildExternalEventNames(DWORD targetPid, wchar_t *readyName,
                             size_t readyCount, wchar_t *goName,
                             size_t goCount) {
  return targetPid > 4 && readyName != nullptr && goName != nullptr &&
         SUCCEEDED(StringCchPrintfW(readyName, readyCount,
                                    L"Local\\BKAES_SCOPE_EXT_%lu_READY",
                                    targetPid)) &&
         SUCCEEDED(StringCchPrintfW(goName, goCount,
                                    L"Local\\BKAES_SCOPE_EXT_%lu_GO",
                                    targetPid));
}

int RunExternalTarget() {
  const DWORD targetPid = GetCurrentProcessId();
  wchar_t readyName[96] = {};
  wchar_t goName[96] = {};
  HANDLE ready = nullptr;
  HANDLE go = nullptr;
  PROCESS_INFORMATION child = {};
  PROCESS_INFORMATION injected = {};
  bool childStarted = false;
  bool injectedStarted = false;
  bool injectedOk = false;
  char outcome[256] = {};
  int result = 1;

  if (!BuildExternalEventNames(targetPid, readyName, ARRAYSIZE(readyName),
                               goName, ARRAYSIZE(goName))) {
    return 1;
  }
  ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
  go = CreateEventW(nullptr, TRUE, FALSE, goName);
  if (ready == nullptr || go == nullptr || !SetEvent(ready) ||
      WaitForSingleObject(go, kExternalGoBudgetMs) != WAIT_OBJECT_0) {
    goto Exit;
  }

  childStarted = LaunchMode(kLeafMode, nullptr, nullptr, nullptr, &child);
  injectedStarted = LaunchMode(kLeafMode, nullptr, nullptr, nullptr, &injected);
  if (injectedStarted) {
    injectedOk = InjectNoop(injected.hProcess);
  }
  if (!childStarted || !injectedStarted || !injectedOk) {
    goto Exit;
  }

  if (FAILED(StringCchPrintfA(
          outcome, ARRAYSIZE(outcome),
          "scope-pivot-target relayPid=%lu childPid=%lu injectedPid=%lu\n",
          targetPid, child.dwProcessId, injected.dwProcessId)) ||
      !BkaesWriteTextFile(kExternalResultName, outcome)) {
    goto Exit;
  }
  // Dynamic SR71 attachment is fail-closed until the hook DLL has completed
  // registration. Keep both descendants alive through the bounded readiness
  // budget so this case measures recursive attachment, not process-exit timing.
  BkaesSettleTelemetry(kAttachReadyBudgetMs);
  result = 0;

Exit:
  if (childStarted) {
    BkaesCleanupProcess(&child, true, 2500);
  }
  if (injectedStarted) {
    BkaesCleanupProcess(&injected, true, 2500);
  }
  if (go != nullptr) {
    CloseHandle(go);
  }
  if (ready != nullptr) {
    CloseHandle(ready);
  }
  return result;
}

bool ReadExternalResult(const std::wstring &jobDirectory, DWORD targetPid,
                        DWORD *childPid, DWORD *injectedPid) {
  std::wstring path = BkaesJoinPath(jobDirectory, kExternalResultName);
  char content[512] = {};

  if (jobDirectory.empty() || childPid == nullptr || injectedPid == nullptr) {
    return false;
  }
  for (DWORD attempt = 0; attempt < 150; ++attempt) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD size = GetFileSize(file, nullptr);
      DWORD read = 0;
      bool validSize = size != INVALID_FILE_SIZE && size > 0 &&
                       size < ARRAYSIZE(content);
      bool readOk = validSize &&
                    ReadFile(file, content, size, &read, nullptr) &&
                    read == size;
      CloseHandle(file);
      if (readOk) {
        DWORD observedTarget = 0;
        content[read] = '\0';
        if (sscanf_s(content,
                     "scope-pivot-target relayPid=%lu childPid=%lu "
                     "injectedPid=%lu",
                     &observedTarget, childPid, injectedPid) == 3 &&
            observedTarget == targetPid && *childPid > 4 &&
            *injectedPid > 4 && *childPid != *injectedPid &&
            *childPid != targetPid && *injectedPid != targetPid) {
          return true;
        }
        return false;
      }
    }
    Sleep(100);
  }
  return false;
}

int RunExternalRoot(DWORD targetPid) {
  const DWORD rootPid = GetCurrentProcessId();
  std::wstring jobDirectory = BkaesGetEnvString(L"BKAES_AUDIT_JOB_DIR");
  wchar_t readyName[96] = {};
  wchar_t goName[96] = {};
  HANDLE ready = nullptr;
  HANDLE go = nullptr;
  HANDLE target = nullptr;
  DWORD childPid = 0;
  DWORD injectedPid = 0;
  char outcome[256] = {};
  int result = 1;

  if (!BuildExternalEventNames(targetPid, readyName, ARRAYSIZE(readyName),
                               goName, ARRAYSIZE(goName)) ||
      jobDirectory.empty()) {
    return 1;
  }
  ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
  go = CreateEventW(nullptr, TRUE, FALSE, goName);
  target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_CREATE_THREAD,
                       FALSE, targetPid);
  if (ready == nullptr || go == nullptr || target == nullptr ||
      GetProcessId(target) != targetPid ||
      WaitForSingleObject(target, 0) != WAIT_TIMEOUT ||
      WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0 ||
      !InjectNoop(target)) {
    goto Exit;
  }

  // Let the root-to-relay attach become ready before the relay creates the next
  // generation. Rapid-exit coverage is exercised separately by kernel process
  // and thread telemetry; this case is the end-to-end full-hook proof.
  BkaesSettleTelemetry(kRelayPrePivotBudgetMs);
  if (!SetEvent(go) ||
      !ReadExternalResult(jobDirectory, targetPid, &childPid, &injectedPid) ||
      FAILED(StringCchPrintfA(
          outcome, ARRAYSIZE(outcome),
          "scope-pivot rootPid=%lu relayPid=%lu childPid=%lu "
          "injectedPid=%lu\n",
          rootPid, targetPid, childPid, injectedPid))) {
    goto Exit;
  }

  BkaesWriteAuditText(L"bkaes-protection-outcome.txt", outcome);
  BkaesPrint("[OK] external scope pivot rootPid=%lu targetPid=%lu childPid=%lu "
             "injectedPid=%lu\n",
             rootPid, targetPid, childPid, injectedPid);
  result = 0;

Exit:
  if (target != nullptr) {
    CloseHandle(target);
  }
  if (go != nullptr) {
    CloseHandle(go);
  }
  if (ready != nullptr) {
    CloseHandle(ready);
  }
  return result;
}

int RunRelay(const wchar_t *readyName, const wchar_t *goName,
             const wchar_t *rootPidText) {
  HANDLE ready = nullptr;
  HANDLE go = nullptr;
  PROCESS_INFORMATION child = {};
  PROCESS_INFORMATION injected = {};
  bool childStarted = false;
  bool injectedStarted = false;
  bool injectedOk = false;
  char outcome[256] = {};
  wchar_t *pidEnd = nullptr;
  unsigned long rootPid = 0;
  int result = 1;

  if (readyName == nullptr || goName == nullptr || rootPidText == nullptr ||
      wcsncmp(readyName, L"Local\\BKAES_SCOPE_", 18) != 0 ||
      wcsncmp(goName, L"Local\\BKAES_SCOPE_", 18) != 0) {
    BkaesPrint("[FAIL] scope pivot relay rejected event names\n");
    return 1;
  }
  rootPid = wcstoul(rootPidText, &pidEnd, 10);
  if (rootPid <= 4 || pidEnd == nullptr || pidEnd == rootPidText ||
      *pidEnd != L'\0') {
    BkaesPrint("[FAIL] scope pivot relay rejected root PID\n");
    return 1;
  }

  ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
  go = OpenEventW(SYNCHRONIZE, FALSE, goName);
  if (ready == nullptr || go == nullptr || !SetEvent(ready) ||
      WaitForSingleObject(go, kRelayGoBudgetMs) != WAIT_OBJECT_0) {
    BkaesPrint("[FAIL] scope pivot relay synchronization err=%lu\n",
               GetLastError());
    goto Exit;
  }

  childStarted = LaunchMode(kLeafMode, nullptr, nullptr, nullptr, &child);
  injectedStarted = LaunchMode(kLeafMode, nullptr, nullptr, nullptr, &injected);
  if (injectedStarted) {
    injectedOk = InjectNoop(injected.hProcess);
  }
  if (!childStarted || !injectedStarted || !injectedOk) {
    BkaesPrint(
        "[FAIL] scope pivot relay follow-on child=%u injected=%u injection=%u err=%lu\n",
        childStarted ? 1u : 0u, injectedStarted ? 1u : 0u,
        injectedOk ? 1u : 0u, GetLastError());
    goto Exit;
  }

  BkaesPrint(
      "[OK] scope pivot relayPid=%lu childPid=%lu injectedPid=%lu\n",
      GetCurrentProcessId(), child.dwProcessId, injected.dwProcessId);
  if (SUCCEEDED(StringCchPrintfA(
          outcome, ARRAYSIZE(outcome),
          "scope-pivot rootPid=%lu relayPid=%lu childPid=%lu injectedPid=%lu\n",
          rootPid, GetCurrentProcessId(), child.dwProcessId,
          injected.dwProcessId))) {
    BkaesWriteAuditText(L"bkaes-protection-outcome.txt", outcome);
  }
  BkaesSettleTelemetry(kAttachReadyBudgetMs);
  result = 0;

Exit:
  if (childStarted) {
    BkaesCleanupProcess(&child, true, 2500);
  }
  if (injectedStarted) {
    BkaesCleanupProcess(&injected, true, 2500);
  }
  if (go != nullptr) {
    CloseHandle(go);
  }
  if (ready != nullptr) {
    CloseHandle(ready);
  }
  return result;
}

} // namespace

int RunProcessScopePivotChain(int argc, wchar_t **argv) {
  if (argc >= 2 && _wcsicmp(argv[1], kLeafMode) == 0) {
    Sleep(kLeafLifetimeMs);
    return 0;
  }
  if (argc >= 2 && _wcsicmp(argv[1], kExternalTargetMode) == 0) {
    return RunExternalTarget();
  }
  if (argc == 5 && _wcsicmp(argv[1], kRelayMode) == 0) {
    return RunRelay(argv[2], argv[3], argv[4]);
  }

  std::wstring externalPidText = BkaesGetEnvString(L"BKAES_CHILD_PID");
  if (!externalPidText.empty()) {
    wchar_t *pidEnd = nullptr;
    unsigned long externalPid =
        wcstoul(externalPidText.c_str(), &pidEnd, 10);
    if (externalPid <= 4 || externalPid > MAXDWORD ||
        pidEnd == externalPidText.c_str() || pidEnd == nullptr ||
        *pidEnd != L'\0') {
      BkaesPrint("[FAIL] external scope pivot target PID is invalid\n");
      return 1;
    }
    return RunExternalRoot(static_cast<DWORD>(externalPid));
  }

  const DWORD rootPid = GetCurrentProcessId();
  wchar_t readyName[96] = {};
  wchar_t goName[96] = {};
  wchar_t rootPidText[32] = {};
  HANDLE ready = nullptr;
  HANDLE go = nullptr;
  PROCESS_INFORMATION relay = {};
  bool relayStarted = false;
  bool injected = false;
  DWORD relayExit = 1;
  int result = 1;

  if (FAILED(StringCchPrintfW(readyName, ARRAYSIZE(readyName),
                              L"Local\\BKAES_SCOPE_%lu_READY", rootPid)) ||
      FAILED(StringCchPrintfW(goName, ARRAYSIZE(goName),
                              L"Local\\BKAES_SCOPE_%lu_GO", rootPid)) ||
      FAILED(StringCchPrintfW(rootPidText, ARRAYSIZE(rootPidText), L"%lu",
                              rootPid))) {
    return 1;
  }

  ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
  go = CreateEventW(nullptr, TRUE, FALSE, goName);
  if (ready == nullptr || go == nullptr) {
    BkaesPrint("[FAIL] scope pivot root event create err=%lu\n", GetLastError());
    goto Exit;
  }

  relayStarted =
      LaunchMode(kRelayMode, readyName, goName, rootPidText, &relay);
  if (!relayStarted || WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0) {
    BkaesPrint("[FAIL] scope pivot relay launch err=%lu\n", GetLastError());
    goto Exit;
  }

  injected = InjectNoop(relay.hProcess);
  if (!injected) {
    BkaesPrint("[FAIL] scope pivot root injection targetPid=%lu err=%lu\n",
               relay.dwProcessId, GetLastError());
    goto Exit;
  }

  BkaesSettleTelemetry(2000);
  if (!SetEvent(go) || WaitForSingleObject(relay.hProcess, 15000) != WAIT_OBJECT_0) {
    BkaesPrint("[FAIL] scope pivot relay completion targetPid=%lu err=%lu\n",
               relay.dwProcessId, GetLastError());
    goto Exit;
  }

  if (!GetExitCodeProcess(relay.hProcess, &relayExit) || relayExit != 0) {
    BkaesPrint("[FAIL] scope pivot relay exit targetPid=%lu exit=%lu err=%lu\n",
               relay.dwProcessId, relayExit, GetLastError());
    goto Exit;
  }

  BkaesPrint(
      "[OK] scope pivot rootPid=%lu injectedRelayPid=%lu relaySpawnAndInjection=1\n",
      rootPid, relay.dwProcessId);
  result = 0;

Exit:
  if (relayStarted) {
    BkaesCleanupProcess(&relay, result != 0, 2500);
  }
  if (go != nullptr) {
    CloseHandle(go);
  }
  if (ready != nullptr) {
    CloseHandle(ready);
  }
  return result;
}
