#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/CompileService.h"
#include "Interface/Core/OpcodeDispatcher.h"
#include "FEXCore/Debug/InternalThreadState.h"
#include "FEXCore/HLE/Linux/ThreadManagement.h"
#include "Interface/IR/PassManager.h"

#include <FEXCore/Core/CPUBackend.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Utils/Event.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Threads.h>

#include <memory>
#include <pthread.h>
#include <stdio.h>

namespace FEXCore {
  static void* ThreadHandler(void *Arg) {
    FEXCore::CompileService *This = reinterpret_cast<FEXCore::CompileService*>(Arg);
    This->ExecutionThread();
    return nullptr;
  }

  CompileService::CompileService(FEXCore::Context::Context *ctx, FEXCore::Core::InternalThreadState *Thread)
    : CTX {ctx}
    , ParentThread {Thread} {

    CompileThreadData = std::make_unique<FEXCore::Core::InternalThreadState>();
    CompileThreadData->IsCompileService = true;

    // We need a compiler for this work thread
    CTX->InitializeCompiler(CompileThreadData.get(), true);
    CompileThreadData->CPUBackend->CopyNecessaryDataForCompileThread(ParentThread->CPUBackend.get());

    uint64_t OldMask = FEXCore::Threads::SetSignalMask(~0ULL);
    WorkerThread = FEXCore::Threads::Thread::Create(ThreadHandler, this);
    FEXCore::Threads::SetSignalMask(OldMask);
  }

  void CompileService::Initialize() {
    // Share CompileService which = this
    CompileThreadData->CompileService = ParentThread->CompileService;
  }

  void CompileService::Shutdown() {
    ShuttingDown = true;
    // Kick the working thread
    StartWork.NotifyAll();
    WorkerThread->join(nullptr);
  }

  void CompileService::ClearCache(FEXCore::Core::InternalThreadState *Thread) {
    // On cache clear we need to spin down the execution thread to ensure it isn't trying to give us more work items
    if (CompileMutex.try_lock()) {
      // We can only clear these things if we pulled the compile mutex

      // Grab the work queue and clear it
      // We don't need to grab the queue mutex since this thread will no longer receive any work events
      // Threads are bounded 1:1
      while (!WorkQueue.empty()) {
        WorkQueue.pop();
      }

      // Go through the garbage collection array and clear it
      // It's safe to clear things that aren't marked safe since we are clearing cache
      GCArray.clear();

      LOGMAN_THROW_A_FMT(CompileThreadData->LocalIRCache.empty(), "Compile service must never have LocalIRCache");

      CompileMutex.unlock();
    }

    // Clear the inverse cache of what is calling us from the Context ClearCache routine
    auto SelectedThread = Thread->IsCompileService ? ParentThread : Thread;
    SelectedThread->LookupCache->ClearCache();
    SelectedThread->CPUBackend->ClearCache();
  }

  CompileService::WorkItem *CompileService::CompileCode(uint64_t RIP) {
    WorkItem* ResultItem = nullptr;

    {
      // Tell the worker thread to compile code for us
      auto Item = std::make_unique<WorkItem>();
      Item->RIP = RIP;

      // Fill the threads work queue
      std::scoped_lock lk(QueueMutex);
      ResultItem = WorkQueue.emplace(std::move(Item)).get();
    }

    // Notify the thread that it has more work
    StartWork.NotifyAll();

    return ResultItem;
  }

  void CompileService::ExecutionThread() {
    // Set our thread name so we can see its relation
    char ThreadName[16]{};
    snprintf(ThreadName, 16, "%ld-CS", ParentThread->ThreadManager.TID.load());
    pthread_setname_np(pthread_self(), ThreadName);

    while (true) {
      // Wait for work
      StartWork.Wait();
      if (ShuttingDown.load()) {
        break;
      }

      std::scoped_lock lk(CompileMutex);
      size_t WorkItems{};

      do {
        // Grab a work item
        std::unique_ptr<WorkItem> Item{};
        {
          std::scoped_lock lk(QueueMutex);
          WorkItems = WorkQueue.size();
          if (WorkItems != 0) {
            Item = std::move(WorkQueue.front());
            WorkQueue.pop();
          }
        }

        // If we had a work item then work on it
        if (Item) {
          // Make sure it's not in lookup cache by accident
          LOGMAN_THROW_A_FMT(CompileThreadData->LookupCache->FindBlock(Item->RIP) == 0, "Compile Service must never have entries in the LookupCache");

          // Code isn't in cache, compile now
          // Set our thread state's RIP
          CompileThreadData->CurrentFrame->State.rip = Item->RIP;

          auto [CodePtr, IRList, DebugData, RAData, Generated, StartAddr, Length] = CTX->CompileCode(CompileThreadData.get(), Item->RIP);

          LOGMAN_THROW_A_FMT(Generated == true, "Compile Service doesn't have IR Cache");

          if (!CodePtr) {
            // XXX: We currently have the expectation that compile service code will be significantly smaller than regular thread's code
            ERROR_AND_DIE_FMT("Couldn't compile code for thread at RIP: 0x{:x}", Item->RIP);
          }

          Item->CodePtr = CodePtr;
          Item->IRList = IRList;
          Item->DebugData = DebugData;
          Item->RAData = RAData;
          Item->StartAddr = StartAddr;
          Item->Length = Length;

          auto& GCItem = GCArray.emplace_back(std::move(Item));
          GCItem->ServiceWorkDone.NotifyAll();
        }
      } while (WorkItems != 0);

      // Clean up any safe entries in our GC array if we have any.
      std::erase_if(GCArray, [](const auto& Entry) {
        return Entry->SafeToClear.load(std::memory_order_relaxed);
      });
    }
  }
}
