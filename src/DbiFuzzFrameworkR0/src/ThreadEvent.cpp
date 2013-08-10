/**
 * @file ThreadEvent.cpp
 * @author created by: Peter Hlavaty
 */

#include "stdafx.h"

#include "ThreadEvent.h"
#include "../../Common/FastCall/FastCall.h"
#include "../../Common/Kernel/Process.hpp"
#include "../../Common/Kernel/MemoryMapping.h"

#include "../../HyperVisor/Common/base/HVCommon.h"

EXTERN_C void syscall_instr_prologue();
EXTERN_C void syscall_instr_epilogue();

//need to call _dynamic_initializer_for__cSyscallSize_
//static const size_t cSyscallSize = (ULONG_PTR)syscall_instr_epilogue - (ULONG_PTR)syscall_instr_prologue;
#define cSyscallSize ((ULONG_PTR)syscall_instr_epilogue - (ULONG_PTR)syscall_instr_prologue)

EXTERN_C ULONG_PTR* get_ring3_rsp();

CThreadEvent::CThreadEvent() : 
	THREAD_INFO(PsGetCurrentThreadId(), NULL),
	m_currentThreadInfo(NULL),
	m_monitorThreadInfo(NULL)
{
	WaitForSyscallCallback = false;
}

CThreadEvent::CThreadEvent(
	__in HANDLE threadId, 
	__in HANDLE parentProcessId /* = NULL */
	) : THREAD_INFO(threadId, parentProcessId),
		m_currentThreadInfo(PsGetCurrentProcessId()),
		m_monitorThreadInfo(parentProcessId)
{
	WaitForSyscallCallback = false;
}

__checkReturn
	bool CThreadEvent::WaitForSyscallEpilogue()
{
	return WaitForSyscallCallback;
}

void CThreadEvent::SetCallbackEpilogue( 
	__in ULONG_PTR reg[REG_COUNT], 
	__in void* memory, 
	__in size_t size, 
	__in bool write, 
	__in_opt void* pageFault /*= NULL */ 
	)
{
	WaitForSyscallCallback = true;

	*GeneralPurposeContext = *reg;
	LastMemoryInfo.SetInfo(memory, size, write);

	//invoke SYSCALL again after syscall is finished!
	reg[RCX] -= cSyscallSize;
	/*
	ULONG_PTR* r3stack = get_ring3_rsp();
	//set return againt to SYSCALL instr
	*r3stack -= cSyscallSize;
	*/
}

void CThreadEvent::EpilogueProceeded()
{
	WaitForSyscallCallback = false;
}

//invoked from monitor
__checkReturn
bool CThreadEvent::MonitorFastCall( 
	__in ULONG_PTR reg[REG_COUNT] 
	)
{
	KeBreak();
	DbgPrint("\nMonitorFastCall\n");
	switch(reg[RCX])
	{
	case SYSCALL_TRACE_FLAG:
		break;
	case SYSCALL_INFO_FLAG:
		break;
	default:
		return false;
	}

	//m_monitorThreadInfo.SetContext(reg);
	return FlipSemaphore(m_currentThreadInfo);
}
#include "DbiMonitor.h"
__checkReturn
bool CThreadEvent::EventCallback( 
	__in ULONG_PTR reg[REG_COUNT] 
	)
{
	DbgPrint("\nEventCallback %p %p %p\n", reg[RCX], reg[RBX], reg);

	if (!CDbiMonitor::GetInstance().GetBranchStack().IsEmpty())
	{
		CMdl r_auto_context(reinterpret_cast<const void*>((PLATFORM_REG_TYPE)reg[DBI_REG_CONTEXT]), sizeof(PLATFORM_REG_TYPE) * CONTEXT_COUNT);
		PLATFORM_REG_TYPE* r_context = reinterpret_cast<PLATFORM_REG_TYPE*>(r_auto_context.Map());
		if (r_context)
		{
			switch(reg[DBI_ACTION])
			{
			case SYSCALL_TRACE_FLAG:
				{
					DbgPrint("\nr3 mapped regs %p | BTF ? %p\n", r_context, r_context[RBX]);

					BRANCH_INFO branch_i = CDbiMonitor::GetInstance().GetBranchStack().Pop();

					//TODO : disable trap flag in host when EIP is patched not here!!!!					
					/*
					 * switch return & flags for RETF in ring3
					 * set RETF to the DstEip
					 */
					r_context[RETURN] = (r_context[FLAGS] | TRAP);
					r_context[FLAGS] = (PLATFORM_REG_TYPE)branch_i.DstEip;

					//set info
					reg[RAX] = (PLATFORM_REG_TYPE)branch_i.SrcEip;

					DbgPrint("\n EventCallback <SYSCALL_TRACE_FLAG> : %p %p [-> %p] %p\n", branch_i.SrcEip, branch_i.DstEip, reg[DBI_R3TELEPORT], reg);

					m_currentThreadInfo.SetContext(r_context);//bad -> reg should go inside instead!!

					break;
				}
			case SYSCALL_PATCH_MEMORY:
				break;
			case SYSCALL_MAIN:
				//m_currentThreadInfo.SetContext(reg);
				return true;
			default:
				return false;
			}
		}		

		KeBreak();
		//if fastcall implemented by page fault handler!!
		IRET* iret = PPAGE_FAULT_IRET(reg);
		iret->Return = reinterpret_cast<const void*>(reg[DBI_R3TELEPORT]);
		return true;

		//m_currentThreadInfo.SetContext(reg);
		return FlipSemaphore(m_monitorThreadInfo);
	}
	return false;
}

__checkReturn 
bool CThreadEvent::FlipSemaphore( 
	__in const EVENT_THREAD_INFO& eventThreadInfo
	)
{
	CEProcess eprocess(eventThreadInfo.ProcessId);
	CAutoProcessAttach process(eprocess.GetEProcess());
	if (process.IsAttached())
	{
		CMdl event_semaphor(eventThreadInfo.EventSemaphor, sizeof(BYTE));
		volatile CHAR* semaphor = reinterpret_cast<volatile CHAR*>(event_semaphor.Map());
		if (semaphor)
		{
			InterlockedExchange8(semaphor, 1);
			return true;
		}
	}
	return false;
}