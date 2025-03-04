/*
$info$
tags: LinuxSyscalls|syscalls-shared
$end_info$
*/

#include "Tests/LinuxSyscalls/Syscalls.h"
#include "Tests/LinuxSyscalls/x64/Syscalls.h"
#include "Tests/LinuxSyscalls/x32/Syscalls.h"

#include <FEXCore/IR/IR.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/shm.h>

namespace FEX::HLE {
  void RegisterSHM() {
    using namespace FEXCore::IR;

    REGISTER_SYSCALL_IMPL_PASS_FLAGS(shmget, SyscallFlags::OPTIMIZETHROUGH | SyscallFlags::NOSYNCSTATEONENTRY,
      [](FEXCore::Core::CpuStateFrame *Frame, key_t key, size_t size, int shmflg) -> uint64_t {
      uint64_t Result = shmget(key, size, shmflg);
      SYSCALL_ERRNO();
    });

    // XXX: shmid_ds is definitely not correct for 32-bit
    REGISTER_SYSCALL_IMPL_PASS_FLAGS(shmctl, SyscallFlags::OPTIMIZETHROUGH | SyscallFlags::NOSYNCSTATEONENTRY,
      [](FEXCore::Core::CpuStateFrame *Frame, int shmid, int cmd, struct shmid_ds *buf) -> uint64_t {
      uint64_t Result = ::shmctl(shmid, cmd, buf);
      SYSCALL_ERRNO();
    });
  }
}
