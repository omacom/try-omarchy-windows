#!/usr/bin/env python3
"""Execute one HLT instruction with Linux KVM. Run inside the Omarchy guest.

No disks, network, packages, sudo, or persistent changes. Exit 0 proves that
this user can execute a KVM vCPU; exit 1 explains the failed stage. Host-side
execution tests the probe, not nested virtualization through Windows/WHPX.
"""

import contextlib
import ctypes
import fcntl
import json
import mmap
import os
import platform
import signal
import struct
import sys


def probe():
    facts = {"architecture": platform.machine(), "kernel": platform.release(),
             "passed": False, "stage": "architecture"}
    try:
        if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
            raise RuntimeError("This probe requires x86_64 Linux inside Omarchy")
        facts["stage"] = "open /dev/kvm"
        with contextlib.ExitStack() as resources:
            kvm = os.open("/dev/kvm", os.O_RDWR | os.O_CLOEXEC)
            resources.callback(os.close, kvm)
            facts["stage"] = "KVM_GET_API_VERSION"
            version = fcntl.ioctl(kvm, 0xAE00, 0)
            facts["apiVersion"] = version
            if version != 12:
                raise RuntimeError(f"Expected KVM API 12, got {version}")
            facts["stage"] = "KVM_CREATE_VM"
            vm = fcntl.ioctl(kvm, 0xAE01, 0)
            resources.callback(os.close, vm)
            ram = resources.enter_context(mmap.mmap(-1, 4096))
            ram[0] = 0xF4  # HLT in real mode, with interrupts disabled.
            address = ctypes.addressof(ctypes.c_char.from_buffer(ram))
            facts["stage"] = "KVM_SET_USER_MEMORY_REGION"
            fcntl.ioctl(vm, 0x4020AE46, struct.pack("=IIQQQ", 0, 0, 0, 4096, address))
            facts["stage"] = "KVM_CREATE_VCPU"
            vcpu = fcntl.ioctl(vm, 0xAE41, 0)
            resources.callback(os.close, vcpu)
            run_size = fcntl.ioctl(kvm, 0xAE04, 0)
            run = resources.enter_context(mmap.mmap(vcpu, run_size))
            facts["stage"] = "KVM_SET_SREGS"
            sregs = bytearray(312)  # struct kvm_sregs, Linux x86 UAPI.
            fcntl.ioctl(vcpu, 0x8138AE83, sregs)
            struct.pack_into("=Q", sregs, 0, 0)  # cs.base
            struct.pack_into("=H", sregs, 12, 0)  # cs.selector
            fcntl.ioctl(vcpu, 0x4138AE84, sregs)
            regs = [0] * 18
            regs[17] = 2  # rflags: architecturally reserved bit 1.
            facts["stage"] = "KVM_SET_REGS"
            fcntl.ioctl(vcpu, 0x4090AE82, struct.pack("=18Q", *regs))
            facts["stage"] = "KVM_RUN"
            fcntl.ioctl(vcpu, 0xAE80, 0)
            reason = struct.unpack_from("=I", run, 8)[0]
            facts["exitReason"] = reason
            if reason != 5:  # KVM_EXIT_HLT
                raise RuntimeError(f"Expected KVM_EXIT_HLT (5), got {reason}")
            facts["passed"] = True
            facts["stage"] = "KVM_EXIT_HLT"
    except (OSError, RuntimeError) as error:
        facts["error"] = str(error)
    return facts


def timeout(_signum, _frame):
    raise RuntimeError("KVM probe exceeded 10 seconds")


if __name__ == "__main__":
    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(10)
    result = probe()
    signal.alarm(0)
    print(json.dumps(result, indent=2, sort_keys=True))
    sys.exit(0 if result["passed"] else 1)
