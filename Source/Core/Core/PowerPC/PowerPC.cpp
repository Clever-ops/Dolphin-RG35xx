// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/PowerPC.h"

#include <algorithm>
#include <cstring>
#include <istream>
#include <ostream>
#include <type_traits>
#include <vector>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FPURoundMode.h"
#include "Common/FloatUtils.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/Scripting/ScriptUtilities.h"
#include "Core/System.h"

namespace PowerPC
{
// STATE_TO_SAVE
PowerPCState ppcState;

static CPUCoreBase* s_cpu_core_base = nullptr;
static bool s_cpu_core_base_is_injected = false;
Interpreter* const s_interpreter = Interpreter::getInstance();
static CoreMode s_mode = CoreMode::Interpreter;

BreakPoints breakpoints;
MemChecks memchecks;
PPCDebugInterface debug_interface(Core::System::GetInstance());

static CoreTiming::EventType* s_invalidate_cache_thread_safe;

double PairedSingle::PS0AsDouble() const
{
  return Common::BitCast<double>(ps0);
}

double PairedSingle::PS1AsDouble() const
{
  return Common::BitCast<double>(ps1);
}

void PairedSingle::SetPS0(double value)
{
  ps0 = Common::BitCast<u64>(value);
}

void PairedSingle::SetPS1(double value)
{
  ps1 = Common::BitCast<u64>(value);
}

static void InvalidateCacheThreadSafe(Core::System& system, u64 userdata, s64 cyclesLate)
{
  ppcState.iCache.Invalidate(static_cast<u32>(userdata));
}

std::istream& operator>>(std::istream& is, CPUCore& core)
{
  std::underlying_type_t<CPUCore> val{};

  if (is >> val)
  {
    core = static_cast<CPUCore>(val);
  }
  else
  {
    // Upon failure, fall back to the cached interpreter
    // to ensure we always initialize our core reference.
    core = CPUCore::CachedInterpreter;
  }

  return is;
}

std::ostream& operator<<(std::ostream& os, CPUCore core)
{
  os << static_cast<std::underlying_type_t<CPUCore>>(core);
  return os;
}

void DoState(PointerWrap& p)
{
  // some of this code has been disabled, because
  // it changes registers even in Mode::Measure (which is suspicious and seems like it could cause
  // desyncs)
  // and because the values it's changing have been added to CoreTiming::DoState, so it might
  // conflict to mess with them here.

  // PowerPC::ppcState.spr[SPR_DEC] = SystemTimers::GetFakeDecrementer();
  // *((u64 *)&TL(PowerPC::ppcState)) = SystemTimers::GetFakeTimeBase(); //works since we are little
  // endian and TL comes first :)

  p.DoArray(ppcState.gpr);
  p.Do(ppcState.pc);
  p.Do(ppcState.npc);
  p.DoArray(ppcState.cr.fields);
  p.Do(ppcState.msr);
  p.Do(ppcState.fpscr);
  p.Do(ppcState.Exceptions);
  p.Do(ppcState.downcount);
  p.Do(ppcState.xer_ca);
  p.Do(ppcState.xer_so_ov);
  p.Do(ppcState.xer_stringctrl);
  p.DoArray(ppcState.ps);
  p.DoArray(ppcState.sr);
  p.DoArray(ppcState.spr);
  p.DoArray(ppcState.tlb);
  p.Do(ppcState.pagetable_base);
  p.Do(ppcState.pagetable_hashmask);

  p.Do(ppcState.reserve);
  p.Do(ppcState.reserve_address);

  ppcState.iCache.DoState(p);
  ppcState.dCache.DoState(p);

  if (p.IsReadMode())
  {
    if (!ppcState.m_enable_dcache)
    {
      INFO_LOG_FMT(POWERPC, "Flushing data cache");
      ppcState.dCache.FlushAll();
    }
    else
    {
      ppcState.dCache.Reset();
    }

    RoundingModeUpdated();
    IBATUpdated();
    DBATUpdated();
  }

  // SystemTimers::DecrementerSet();
  // SystemTimers::TimeBaseSet();

  JitInterface::DoState(p);
}

static void ResetRegisters()
{
  std::fill(std::begin(ppcState.ps), std::end(ppcState.ps), PairedSingle{});
  std::fill(std::begin(ppcState.sr), std::end(ppcState.sr), 0U);
  std::fill(std::begin(ppcState.gpr), std::end(ppcState.gpr), 0U);
  std::fill(std::begin(ppcState.spr), std::end(ppcState.spr), 0U);

  // Gamecube:
  // 0x00080200 = lonestar 2.0
  // 0x00088202 = lonestar 2.2
  // 0x70000100 = gekko 1.0
  // 0x00080100 = gekko 2.0
  // 0x00083203 = gekko 2.3a
  // 0x00083213 = gekko 2.3b
  // 0x00083204 = gekko 2.4
  // 0x00083214 = gekko 2.4e (8SE) - retail HW2
  // Wii:
  // 0x00087102 = broadway retail hw
  if (SConfig::GetInstance().bWii)
  {
    ppcState.spr[SPR_PVR] = 0x00087102;
  }
  else
  {
    ppcState.spr[SPR_PVR] = 0x00083214;
  }
  ppcState.spr[SPR_HID1] = 0x80000000;  // We're running at 3x the bus clock
  ppcState.spr[SPR_ECID_U] = 0x0d96e200;
  ppcState.spr[SPR_ECID_M] = 0x1840c00d;
  ppcState.spr[SPR_ECID_L] = 0x82bb08e8;

  ppcState.fpscr.Hex = 0;
  ppcState.pc = 0;
  ppcState.npc = 0;
  ppcState.Exceptions = 0;

  ppcState.reserve = false;
  ppcState.reserve_address = 0;

  for (auto& v : ppcState.cr.fields)
  {
    v = 0x8000000000000001;
  }
  ppcState.SetXER({});

  RoundingModeUpdated();
  DBATUpdated();
  IBATUpdated();

  TL(PowerPC::ppcState) = 0;
  TU(PowerPC::ppcState) = 0;
  SystemTimers::TimeBaseSet();

  // MSR should be 0x40, but we don't emulate BS1, so it would never be turned off :}
  ppcState.msr.Hex = 0;
  ppcState.spr[SPR_DEC] = 0xFFFFFFFF;
  SystemTimers::DecrementerSet();
}

static void InitializeCPUCore(CPUCore cpu_core)
{
  // We initialize the interpreter because
  // it is used on boot and code window independently.
  s_interpreter->Init();

  switch (cpu_core)
  {
  case CPUCore::Interpreter:
    s_cpu_core_base = s_interpreter;
    break;

  default:
    s_cpu_core_base = JitInterface::InitJitCore(cpu_core);
    if (!s_cpu_core_base)  // Handle Situations where JIT core isn't available
    {
      WARN_LOG_FMT(POWERPC, "CPU core {} not available. Falling back to default.",
                   static_cast<int>(cpu_core));
      s_cpu_core_base = JitInterface::InitJitCore(DefaultCPUCore());
    }
    break;
  }

  s_mode = s_cpu_core_base == s_interpreter ? CoreMode::Interpreter : CoreMode::JIT;
}

const std::vector<CPUCore>& AvailableCPUCores()
{
  static const std::vector<CPUCore> cpu_cores = {
#ifdef _M_X86_64
      CPUCore::JIT64,
#elif defined(_M_ARM_64)
      CPUCore::JITARM64,
#endif
      CPUCore::CachedInterpreter,
      CPUCore::Interpreter,
  };

  return cpu_cores;
}

CPUCore DefaultCPUCore()
{
#ifdef _M_X86_64
  return CPUCore::JIT64;
#elif defined(_M_ARM_64)
  return CPUCore::JITARM64;
#else
  return CPUCore::CachedInterpreter;
#endif
}

void Init(CPUCore cpu_core)
{
  s_invalidate_cache_thread_safe = Core::System::GetInstance().GetCoreTiming().RegisterEvent(
      "invalidateEmulatedCache", InvalidateCacheThreadSafe);

  Reset();

  InitializeCPUCore(cpu_core);
  ppcState.iCache.Init();
  ppcState.dCache.Init();

  ppcState.m_enable_dcache = Config::Get(Config::MAIN_ACCURATE_CPU_CACHE);

  if (Config::Get(Config::MAIN_ENABLE_DEBUGGING))
    breakpoints.ClearAllTemporary();
}

void Reset()
{
  ppcState.pagetable_base = 0;
  ppcState.pagetable_hashmask = 0;
  ppcState.tlb = {};

  ResetRegisters();
  ppcState.iCache.Reset();
  ppcState.dCache.Reset();
}

void ScheduleInvalidateCacheThreadSafe(u32 address)
{
  if (CPU::GetState() == CPU::State::Running && !Core::IsCPUThread())
  {
    Core::System::GetInstance().GetCoreTiming().ScheduleEvent(
        0, s_invalidate_cache_thread_safe, address, CoreTiming::FromThread::NON_CPU);
  }
  else
  {
    PowerPC::ppcState.iCache.Invalidate(static_cast<u32>(address));
  }
}

void Shutdown()
{
  InjectExternalCPUCore(nullptr);
  JitInterface::Shutdown();
  s_interpreter->Shutdown();
  s_cpu_core_base = nullptr;
}

CoreMode GetMode()
{
  return !s_cpu_core_base_is_injected ? s_mode : CoreMode::Interpreter;
}

static void ApplyMode()
{
  switch (s_mode)
  {
  case CoreMode::Interpreter:  // Switching from JIT to interpreter
    s_cpu_core_base = s_interpreter;
    break;

  case CoreMode::JIT:  // Switching from interpreter to JIT.
    // Don't really need to do much. It'll work, the cache will refill itself.
    s_cpu_core_base = JitInterface::GetCore();
    if (!s_cpu_core_base)  // Has a chance to not get a working JIT core if one isn't active on host
      s_cpu_core_base = s_interpreter;
    break;
  }
}

void SetMode(CoreMode new_mode)
{
  if (new_mode == s_mode)
    return;  // We don't need to do anything.

  s_mode = new_mode;

  // If we're using an external CPU core implementation then don't do anything.
  if (s_cpu_core_base_is_injected)
    return;

  ApplyMode();
}

const char* GetCPUName()
{
  return s_cpu_core_base->GetName();
}

void InjectExternalCPUCore(CPUCoreBase* new_cpu)
{
  // Previously injected.
  if (s_cpu_core_base_is_injected)
    s_cpu_core_base->Shutdown();

  // nullptr means just remove
  if (!new_cpu)
  {
    if (s_cpu_core_base_is_injected)
    {
      s_cpu_core_base_is_injected = false;
      ApplyMode();
    }
    return;
  }

  new_cpu->Init();
  s_cpu_core_base = new_cpu;
  s_cpu_core_base_is_injected = true;
}

void SingleStep()
{
  s_cpu_core_base->SingleStep();
}

void RunLoop()
{
  s_cpu_core_base->Run();
  Host_UpdateDisasmDialog();
}

u64 ReadFullTimeBaseValue()
{
  u64 value;
  std::memcpy(&value, &TL(PowerPC::ppcState), sizeof(value));
  return value;
}

void WriteFullTimeBaseValue(u64 value)
{
  std::memcpy(&TL(PowerPC::ppcState), &value, sizeof(value));
}

void UpdatePerformanceMonitor(u32 cycles, u32 num_load_stores, u32 num_fp_inst,
                              PowerPCState& ppc_state)
{
  switch (MMCR0(ppc_state).PMC1SELECT)
  {
  case 0:  // No change
    break;
  case 1:  // Processor cycles
    ppc_state.spr[SPR_PMC1] += cycles;
    break;
  default:
    break;
  }

  switch (MMCR0(ppc_state).PMC2SELECT)
  {
  case 0:  // No change
    break;
  case 1:  // Processor cycles
    ppc_state.spr[SPR_PMC2] += cycles;
    break;
  case 11:  // Number of loads and stores completed
    ppc_state.spr[SPR_PMC2] += num_load_stores;
    break;
  default:
    break;
  }

  switch (MMCR1(ppc_state).PMC3SELECT)
  {
  case 0:  // No change
    break;
  case 1:  // Processor cycles
    ppc_state.spr[SPR_PMC3] += cycles;
    break;
  case 11:  // Number of FPU instructions completed
    ppc_state.spr[SPR_PMC3] += num_fp_inst;
    break;
  default:
    break;
  }

  switch (MMCR1(ppc_state).PMC4SELECT)
  {
  case 0:  // No change
    break;
  case 1:  // Processor cycles
    ppc_state.spr[SPR_PMC4] += cycles;
    break;
  default:
    break;
  }

  if ((MMCR0(ppc_state).PMC1INTCONTROL && (ppc_state.spr[SPR_PMC1] & 0x80000000) != 0) ||
      (MMCR0(ppc_state).PMCINTCONTROL && (ppc_state.spr[SPR_PMC2] & 0x80000000) != 0) ||
      (MMCR0(ppc_state).PMCINTCONTROL && (ppc_state.spr[SPR_PMC3] & 0x80000000) != 0) ||
      (MMCR0(ppc_state).PMCINTCONTROL && (ppc_state.spr[SPR_PMC4] & 0x80000000) != 0))
  {
    ppc_state.Exceptions |= EXCEPTION_PERFORMANCE_MONITOR;
  }
}

void CheckExceptions()
{
  u32 exceptions = ppcState.Exceptions;

  // Example procedure:
  // Set SRR0 to either PC or NPC
  // SRR0 = NPC;
  //
  // Save specified MSR bits
  // SRR1 = MSR.Hex & 0x87C0FFFF;
  //
  // Copy ILE bit to LE
  // MSR.LE = MSR.ILE;
  //
  // Clear MSR as specified
  // MSR.Hex &= ~0x04EF36; // 0x04FF36 also clears ME (only for machine check exception)
  //
  // Set to exception type entry point
  // NPC = 0x00000x00;

  // TODO(delroth): Exception priority is completely wrong here: depending on
  // the instruction class, exceptions should be executed in a given order,
  // which is very different from the one arbitrarily chosen here. See §6.1.5
  // in 6xx_pem.pdf.

  if (exceptions & EXCEPTION_ISI)
  {
    SRR0(PowerPC::ppcState) = PowerPC::ppcState.npc;
    // Page fault occurred
    SRR1(PowerPC::ppcState) = (PowerPC::ppcState.msr.Hex & 0x87C0FFFF) | (1 << 30);
    PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
    PowerPC::ppcState.msr.Hex &= ~0x04EF36;
    PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000400;

    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_ISI");
    ppcState.Exceptions &= ~EXCEPTION_ISI;
  }
  else if (exceptions & EXCEPTION_PROGRAM)
  {
    SRR0(PowerPC::ppcState) = PowerPC::ppcState.pc;
    // SRR1 was partially set by GenerateProgramException, so bitwise or is used here
    SRR1(PowerPC::ppcState) |= PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
    PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
    PowerPC::ppcState.msr.Hex &= ~0x04EF36;
    PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000700;

    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_PROGRAM");
    ppcState.Exceptions &= ~EXCEPTION_PROGRAM;
  }
  else if (exceptions & EXCEPTION_SYSCALL)
  {
    SRR0(PowerPC::ppcState) = PowerPC::ppcState.npc;
    SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
    PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
    PowerPC::ppcState.msr.Hex &= ~0x04EF36;
    PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000C00;

    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_SYSCALL (PC={:08x})", PowerPC::ppcState.pc);
    ppcState.Exceptions &= ~EXCEPTION_SYSCALL;
  }
  else if (exceptions & EXCEPTION_FPU_UNAVAILABLE)
  {
    // This happens a lot - GameCube OS uses deferred FPU context switching
    SRR0(PowerPC::ppcState) = PowerPC::ppcState.pc;  // re-execute the instruction
    SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
    PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
    PowerPC::ppcState.msr.Hex &= ~0x04EF36;
    PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000800;

    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_FPU_UNAVAILABLE");
    ppcState.Exceptions &= ~EXCEPTION_FPU_UNAVAILABLE;
  }
  else if (exceptions & EXCEPTION_FAKE_MEMCHECK_HIT)
  {
    ppcState.Exceptions &= ~EXCEPTION_DSI & ~EXCEPTION_FAKE_MEMCHECK_HIT;
  }
  else if (exceptions & EXCEPTION_DSI)
  {
    SRR0(PowerPC::ppcState) = PowerPC::ppcState.pc;
    SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
    PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
    PowerPC::ppcState.msr.Hex &= ~0x04EF36;
    PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000300;
    // DSISR and DAR regs are changed in GenerateDSIException()

    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_DSI");
    ppcState.Exceptions &= ~EXCEPTION_DSI;
  }
  else if (exceptions & EXCEPTION_ALIGNMENT)
  {
    SRR0(PowerPC::ppcState) = PowerPC::ppcState.pc;
    SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
    PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
    PowerPC::ppcState.msr.Hex &= ~0x04EF36;
    PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000600;

    // TODO crazy amount of DSISR options to check out

    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_ALIGNMENT");
    ppcState.Exceptions &= ~EXCEPTION_ALIGNMENT;
  }

  // EXTERNAL INTERRUPT
  else
  {
    CheckExternalExceptions();
  }
}

void CheckExternalExceptions()
{
  u32 exceptions = ppcState.Exceptions;

  // EXTERNAL INTERRUPT
  // Handling is delayed until MSR.EE=1.
  if (exceptions && PowerPC::ppcState.msr.EE)
  {
    if (exceptions & EXCEPTION_EXTERNAL_INT)
    {
      // Pokemon gets this "too early", it hasn't a handler yet
      SRR0(PowerPC::ppcState) = PowerPC::ppcState.npc;
      SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
      PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
      PowerPC::ppcState.msr.Hex &= ~0x04EF36;
      PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000500;

      DEBUG_LOG_FMT(POWERPC, "EXCEPTION_EXTERNAL_INT");
      ppcState.Exceptions &= ~EXCEPTION_EXTERNAL_INT;

      DEBUG_ASSERT_MSG(POWERPC, (SRR1(PowerPC::ppcState) & 0x02) != 0,
                       "EXTERNAL_INT unrecoverable???");
    }
    else if (exceptions & EXCEPTION_PERFORMANCE_MONITOR)
    {
      SRR0(PowerPC::ppcState) = PowerPC::ppcState.npc;
      SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
      PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
      PowerPC::ppcState.msr.Hex &= ~0x04EF36;
      PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000F00;

      DEBUG_LOG_FMT(POWERPC, "EXCEPTION_PERFORMANCE_MONITOR");
      ppcState.Exceptions &= ~EXCEPTION_PERFORMANCE_MONITOR;
    }
    else if (exceptions & EXCEPTION_DECREMENTER)
    {
      SRR0(PowerPC::ppcState) = PowerPC::ppcState.npc;
      SRR1(PowerPC::ppcState) = PowerPC::ppcState.msr.Hex & 0x87C0FFFF;
      PowerPC::ppcState.msr.LE = PowerPC::ppcState.msr.ILE;
      PowerPC::ppcState.msr.Hex &= ~0x04EF36;
      PowerPC::ppcState.pc = PowerPC::ppcState.npc = 0x00000900;

      DEBUG_LOG_FMT(POWERPC, "EXCEPTION_DECREMENTER");
      ppcState.Exceptions &= ~EXCEPTION_DECREMENTER;
    }
    else
    {
      DEBUG_ASSERT_MSG(POWERPC, 0, "Unknown EXT interrupt: Exceptions == {:08x}", exceptions);
      ERROR_LOG_FMT(POWERPC, "Unknown EXTERNAL INTERRUPT exception: Exceptions == {:08x}",
                    exceptions);
    }
  }
}

void CheckBreakPoints()
{
  const TBreakPoint* bp = PowerPC::breakpoints.GetBreakpoint(PowerPC::ppcState.pc);

  if (!bp || !bp->is_enabled || !EvaluateCondition(bp->condition))
    return;

  Scripting::ScriptUtilities::RunOnInstructionHitCallbacks(PowerPC::ppcState.pc);

  if (bp->break_on_hit)
  {
    CPU::Break();
    if (GDBStub::IsActive())
      GDBStub::TakeControl();
  }
  if (bp->log_on_hit)
  {
    NOTICE_LOG_FMT(MEMMAP,
                   "BP {:08x} {}({:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} "
                   "{:08x}) LR={:08x}",
                   PowerPC::ppcState.pc, g_symbolDB.GetDescription(PowerPC::ppcState.pc),
                   PowerPC::ppcState.gpr[3], PowerPC::ppcState.gpr[4], PowerPC::ppcState.gpr[5],
                   PowerPC::ppcState.gpr[6], PowerPC::ppcState.gpr[7], PowerPC::ppcState.gpr[8],
                   PowerPC::ppcState.gpr[9], PowerPC::ppcState.gpr[10], PowerPC::ppcState.gpr[11],
                   PowerPC::ppcState.gpr[12], LR(PowerPC::ppcState));
  }
  if (PowerPC::breakpoints.IsTempBreakPoint(PowerPC::ppcState.pc))
    PowerPC::breakpoints.Remove(PowerPC::ppcState.pc);
}

void PowerPCState::SetSR(u32 index, u32 value)
{
  DEBUG_LOG_FMT(POWERPC, "{:08x}: MMU: Segment register {} set to {:08x}", pc, index, value);
  sr[index] = value;
}

// FPSCR update functions

void PowerPCState::UpdateFPRFDouble(double dvalue)
{
  fpscr.FPRF = Common::ClassifyDouble(dvalue);
}

void PowerPCState::UpdateFPRFSingle(float fvalue)
{
  fpscr.FPRF = Common::ClassifyFloat(fvalue);
}

void RoundingModeUpdated()
{
  // The rounding mode is separate for each thread, so this must run on the CPU thread
  ASSERT(Core::IsCPUThread());

  FPURoundMode::SetSIMDMode(PowerPC::ppcState.fpscr.RN, PowerPC::ppcState.fpscr.NI);
}

}  // namespace PowerPC
