/*
 * WinMaliVm.c - VM handle table implementation.
 *
 * Fully wired: VmCreate builds a per-VM L0 root page table without consuming
 * a scarce hardware AS slot. VmBind maps/unmaps BO pages into the LPAE page
 * table. Execution leases a hardware AS just long enough to run the CSG and
 * returns it after parking the CSG on the permanent bring-up AS.
 */

#include "WinMaliKmd.h"
#include "WinMaliVm.h"

#define WMERR_OK          0
#define WMERR_EINVAL      -22
#define WMERR_ENOMEM      -12
#define WMERR_ENOENT      -2

/* Default user VA span when UMD passes 0 in VmCreate.UserVaRange. Must
   match what we publish in DRIVERCAPS.InternalGpuVirtualAddressRange* -
   that span is 0x10000 .. 0x3FFFFFFF (30-bit). The reserved low region
   (firmware sections under 0x04040000) is excluded by anchoring the user
   base above WINMALI_SYSMEM_GPU_BASE. */
#define WINMALI_VM_DEFAULT_USER_VA_BASE     WINMALI_SYSMEM_GPU_BASE
#define WINMALI_VM_DEFAULT_USER_VA_RANGE    (0x30000000ULL)  /* 768 MiB */

VOID
WinMaliVmTableInit(_Out_ PWINMALI_VM_TABLE Table)
{
    KeInitializeSpinLock(&Table->Lock);
    InitializeListHead(&Table->Head);
    Table->NextId = 1;
    Table->Count = 0;
}

static VOID
WinMaliVmFree_(_Inout_ PWINMALI_ADAPTER Adapter, _Inout_ PWINMALI_VM Vm)
{
    if (Vm != NULL) {
        /* Removing a VM from the table drops its owning reference. Worker or
           replay paths may still hold lookup references while a transient AS
           is executing, so wait before tearing down page tables. */
        if (InterlockedDecrement(&Vm->RefCount) != 0) {
            KeWaitForSingleObject(&Vm->RefZeroEvent, Executive, KernelMode,
                                  FALSE, NULL);
        }
        if (Vm->PtInitialized) {
            WinMaliMmuVmTeardown(Adapter, &Vm->Pt);
            Vm->PtInitialized = FALSE;
        }
        Vm->Magic = 0;
        ExFreePoolWithTag(Vm, WINMALI_POOL_TAG);
    }
}

VOID
WinMaliVmTableTeardown(_Inout_ PWINMALI_ADAPTER Adapter,
                       _Inout_ PWINMALI_VM_TABLE Table)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry;

    UNREFERENCED_PARAMETER(Adapter);
    InitializeListHead(&freeHead);

    KeAcquireSpinLock(&Table->Lock, &oldIrql);
    while (!IsListEmpty(&Table->Head)) {
        entry = RemoveHeadList(&Table->Head);
        InsertTailList(&freeHead, entry);
    }
    Table->Count = 0;
    KeReleaseSpinLock(&Table->Lock, oldIrql);

    while (!IsListEmpty(&freeHead)) {
        entry = RemoveHeadList(&freeHead);
        WinMaliVmFree_(Adapter, CONTAINING_RECORD(entry, WINMALI_VM, Link));
    }
}

int
WinMaliVmCreate(_Inout_ PWINMALI_ADAPTER Adapter,
                _In_ ULONG Flags,
                _In_ UINT64 UserVaRange,
                _In_opt_ PVOID OwnerDevice,
                _Out_ ULONG* OutId,
                _Out_ UINT64* OutGrantedRange)
{
    PWINMALI_VM vm;
    KIRQL oldIrql;
    UINT64 granted;

    *OutId = 0;
    *OutGrantedRange = 0;

    WINMALI_TRACE("VmCreateDiag: ENTER flags=0x%x requested=0x%llx owner=%p",
                  Flags, (ULONGLONG)UserVaRange, OwnerDevice);
    granted = (UserVaRange != 0) ? UserVaRange : WINMALI_VM_DEFAULT_USER_VA_RANGE;
    /* Cap to the span we advertise. */
    if (granted > WINMALI_VM_DEFAULT_USER_VA_RANGE) {
        granted = WINMALI_VM_DEFAULT_USER_VA_RANGE;
    }

    vm = (PWINMALI_VM)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*vm),
                                            WINMALI_POOL_TAG);
    if (vm == NULL) {
        WINMALI_WARN("VmCreateDiag: VM allocation failed");
        return WMERR_ENOMEM;
    }
    WINMALI_TRACE("VmCreateDiag: VM allocated vm=%p granted=0x%llx",
                  vm, (ULONGLONG)granted);
    RtlZeroMemory(vm, sizeof(*vm));
    vm->Magic       = WINMALI_VM_MAGIC;
    vm->Flags       = Flags;
    vm->State       = WinMaliVmRun_Usable;
    vm->UserVaBase  = WINMALI_VM_DEFAULT_USER_VA_BASE;
    vm->UserVaSize  = granted;
    vm->RefCount    = 1;
    KeInitializeEvent(&vm->RefZeroEvent, NotificationEvent, FALSE);
    vm->OwnerDevice = OwnerDevice;
    /* KMD GPU-VA bump allocator starts just past the advertised user VA
       window (base + range), page-aligned. */
    vm->KmdVaNext   = (WINMALI_VM_DEFAULT_USER_VA_BASE + granted + 0xFFFFull)
                      & ~0xFFFFull;

    {
        WINMALI_TRACE("VmCreateDiag: calling MmuVmInit vm=%p", vm);
        NTSTATUS st = WinMaliMmuVmInit(Adapter, &vm->Pt);
        WINMALI_TRACE("VmCreateDiag: MmuVmInit returned 0x%08x resident=%u root=0x%llx",
                      st, vm->Pt.AsSlot < WINMALI_AS_SLOT_MAX,
                      (ULONGLONG)vm->Pt.RootPa.QuadPart);
        if (!NT_SUCCESS(st)) {
            ExFreePoolWithTag(vm, WINMALI_POOL_TAG);
            WINMALI_WARN("VmCreate: WinMaliMmuVmInit failed 0x%08x", st);
            return (st == STATUS_INSUFFICIENT_RESOURCES) ? WMERR_ENOMEM
                                                         : WMERR_EINVAL;
        }
        vm->PtInitialized = TRUE;
    }

    /* Cross-map the CSF ring/sync/shader into this escape-owned VM at the
       fixed KMD-reserved VAs so the MCU can execute in its own AS. Best-
       effort: if CSF isn't booted yet the submit path falls back to the NOP. */
    {
        WINMALI_TRACE("VmCreateDiag: calling CsfMapIntoVm root=0x%llx",
                      (ULONGLONG)vm->Pt.RootPa.QuadPart);
        NTSTATUS csfSt = WinMaliCsfMapIntoVm(Adapter, &vm->Pt);
        WINMALI_TRACE("VmCreateDiag: CsfMapIntoVm returned 0x%08x", csfSt);
        if (!NT_SUCCESS(csfSt) && csfSt != STATUS_DEVICE_NOT_READY) {
            WINMALI_WARN("VmCreate: CsfMapIntoVm failed 0x%08x (submits will "
                         "fall back to NOP for this VM)", csfSt);
        }
    }

    KeAcquireSpinLock(&Adapter->VmTable.Lock, &oldIrql);
    vm->Id = (ULONG)(Adapter->VmTable.NextId++);
    InsertTailList(&Adapter->VmTable.Head, &vm->Link);
    Adapter->VmTable.Count++;
    KeReleaseSpinLock(&Adapter->VmTable.Lock, oldIrql);

    *OutId = vm->Id;
    *OutGrantedRange = granted;
    WINMALI_TRACE("VmCreate: id=%u flags=0x%x base=0x%llx range=0x%llx",
                  vm->Id, vm->Flags, (ULONGLONG)vm->UserVaBase,
                  (ULONGLONG)vm->UserVaSize);
    return WMERR_OK;
}

static PWINMALI_VM
WinMaliVmTakeById_(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Id)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PWINMALI_VM found = NULL;

    KeAcquireSpinLock(&Adapter->VmTable.Lock, &oldIrql);
    for (entry = Adapter->VmTable.Head.Flink;
         entry != &Adapter->VmTable.Head;
         entry = entry->Flink) {
        PWINMALI_VM vm = CONTAINING_RECORD(entry, WINMALI_VM, Link);
        if (vm->Id == Id) {
            RemoveEntryList(&vm->Link);
            Adapter->VmTable.Count--;
            found = vm;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->VmTable.Lock, oldIrql);
    return found;
}

int
WinMaliVmDestroy(_Inout_ PWINMALI_ADAPTER Adapter, _In_ ULONG Id)
{
    PWINMALI_VM vm = WinMaliVmTakeById_(Adapter, Id);
    if (vm == NULL) {
        return WMERR_ENOENT;
    }
    WINMALI_TRACE("VmDestroy: id=%u", vm->Id);
    WinMaliVmFree_(Adapter, vm);
    return WMERR_OK;
}

ULONG
WinMaliVmRundownOwner(_Inout_ PWINMALI_ADAPTER Adapter, _In_ PVOID OwnerDevice)
{
    KIRQL oldIrql;
    LIST_ENTRY freeHead;
    PLIST_ENTRY entry, next;
    ULONG freed = 0;

    if (OwnerDevice == NULL) {
        return 0;
    }
    InitializeListHead(&freeHead);

    KeAcquireSpinLock(&Adapter->VmTable.Lock, &oldIrql);
    for (entry = Adapter->VmTable.Head.Flink;
         entry != &Adapter->VmTable.Head;
         entry = next) {
        PWINMALI_VM vm = CONTAINING_RECORD(entry, WINMALI_VM, Link);
        next = entry->Flink;
        if (vm->OwnerDevice == OwnerDevice) {
            RemoveEntryList(&vm->Link);
            Adapter->VmTable.Count--;
            InsertTailList(&freeHead, &vm->Link);
        }
    }
    KeReleaseSpinLock(&Adapter->VmTable.Lock, oldIrql);

    while (!IsListEmpty(&freeHead)) {
        PWINMALI_VM vm = CONTAINING_RECORD(RemoveHeadList(&freeHead),
                                           WINMALI_VM, Link);
        WINMALI_TRACE("VmRundown: id=%u (owner device died without VmDestroy)",
                      vm->Id);
        WinMaliVmFree_(Adapter, vm);
        ++freed;
    }
    return freed;
}

PWINMALI_VM
WinMaliVmGet(_In_ PWINMALI_ADAPTER Adapter, _In_ ULONG Id)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PWINMALI_VM found = NULL;

    KeAcquireSpinLock(&Adapter->VmTable.Lock, &oldIrql);
    for (entry = Adapter->VmTable.Head.Flink;
         entry != &Adapter->VmTable.Head;
         entry = entry->Flink) {
        PWINMALI_VM vm = CONTAINING_RECORD(entry, WINMALI_VM, Link);
        if (vm->Id == Id) {
            InterlockedIncrement(&vm->RefCount);
            found = vm;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->VmTable.Lock, oldIrql);
    return found;
}

VOID
WinMaliVmPut(_In_ PWINMALI_VM Vm)
{
    if (Vm != NULL) {
        LONG refs = InterlockedDecrement(&Vm->RefCount);
        if (refs == 0) {
            KeSetEvent(&Vm->RefZeroEvent, IO_NO_INCREMENT, FALSE);
        } else if (refs < 0) {
            WINMALI_ERROR("VmPut: refcount underflow vm=%p id=%u", Vm, Vm->Id);
        }
    }
}

int
WinMaliVmGetState(_In_ PWINMALI_ADAPTER Adapter,
                  _In_ ULONG Id,
                  _Out_ WINMALI_VM_RUN_STATE* OutState)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    int err = WMERR_ENOENT;

    *OutState = WinMaliVmRun_Unusable;
    KeAcquireSpinLock(&Adapter->VmTable.Lock, &oldIrql);
    for (entry = Adapter->VmTable.Head.Flink;
         entry != &Adapter->VmTable.Head;
         entry = entry->Flink) {
        PWINMALI_VM vm = CONTAINING_RECORD(entry, WINMALI_VM, Link);
        if (vm->Id == Id) {
            *OutState = vm->State;
            err = WMERR_OK;
            break;
        }
    }
    KeReleaseSpinLock(&Adapter->VmTable.Lock, oldIrql);
    return err;
}
