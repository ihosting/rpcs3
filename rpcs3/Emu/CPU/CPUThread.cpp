#include "stdafx.h"
#include "CPUThread.h"

CPUThread* GetCurrentCPUThread()
{
	return (CPUThread*)GetCurrentNamedThread();
}

CPUThread::CPUThread(CPUThreadType type)
	: ThreadBase(true, "CPUThread")
	, m_type(type)
	, m_stack_size(0)
	, m_stack_addr(0)
	, m_offset(0)
	, m_prio(0)
	, m_sync_wait(false)
	, m_wait_thread_id(-1)
	, m_free_data(false)
	, m_dec(nullptr)
	, m_is_step(false)
	, m_is_branch(false)
{
}

CPUThread::~CPUThread()
{
	Close();
}

void CPUThread::Close()
{
	if(IsAlive())
	{
		m_free_data = true;
		ThreadBase::Stop(false);
	}
	else
	{
		delete m_dec;
		m_dec = nullptr;
	}
}

void CPUThread::Reset()
{
	CloseStack();

	m_sync_wait = 0;
	m_wait_thread_id = -1;

	SetPc(0);
	cycle = 0;
	m_is_branch = false;

	m_status = Stopped;
	m_error = 0;
	
	DoReset();
}

void CPUThread::CloseStack()
{
	if(m_stack_addr)
	{
		Memory.Free(m_stack_addr);
		m_stack_addr = 0;
	}

	m_stack_size = 0;
	m_stack_point = 0;
}

void CPUThread::SetId(const u32 id)
{
	m_id = id;
}

void CPUThread::SetName(const std::string& name)
{
	m_name = name;
}

void CPUThread::Wait(bool wait)
{
	wxCriticalSectionLocker lock(m_cs_sync);
	m_sync_wait = wait;
}

void CPUThread::Wait(const CPUThread& thr)
{
	wxCriticalSectionLocker lock(m_cs_sync);
	m_wait_thread_id = thr.GetId();
	m_sync_wait = true;
}

bool CPUThread::Sync()
{
	wxCriticalSectionLocker lock(m_cs_sync);

	return m_sync_wait;
}

int CPUThread::ThreadStatus()
{
	if(Emu.IsStopped())
	{
		return CPUThread_Stopped;
	}

	if(TestDestroy())
	{
		return CPUThread_Break;
	}

	if(m_is_step)
	{
		return CPUThread_Step;
	}

	if(Emu.IsPaused() || Sync())
	{
		return CPUThread_Sleeping;
	}

	return CPUThread_Running;
}

void CPUThread::SetEntry(const u64 pc)
{
	entry = pc;
}

void CPUThread::NextPc(u8 instr_size)
{
	if(m_is_branch)
	{
		m_is_branch = false;

		SetPc(nPC);
	}
	else
	{
		PC += instr_size;
	}
}

void CPUThread::SetBranch(const u64 pc, bool record_branch)
{
	if(!Memory.IsGoodAddr(m_offset + pc))
	{
		ConLog.Error("%s branch error: bad address 0x%llx #pc: 0x%llx", GetFName().mb_str(), m_offset + pc, m_offset + PC);
		Emu.Pause();
	}

	m_is_branch = true;
	nPC = pc;

	if(record_branch)
		CallStackBranch(pc);
}

void CPUThread::SetPc(const u64 pc)
{
	PC = pc;
}

void CPUThread::SetError(const u32 error)
{
	if(error == 0)
	{
		m_error = 0;
	}
	else
	{
		m_error |= error;
	}
}

wxArrayString CPUThread::ErrorToString(const u32 error)
{
	wxArrayString earr;

	if(error == 0) return earr;

	earr.Add("Unknown error");

	return earr;
}

void CPUThread::Run()
{
	if(IsRunning()) Stop();
	if(IsPaused())
	{
		Resume();
		return;
	}
	
#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_START_THREAD, this);
#endif

	m_status = Running;

	SetPc(entry);
	InitStack();
	InitRegs();
	DoRun();
	Emu.CheckStatus();

#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_STARTED_THREAD, this);
#endif
}

void CPUThread::Resume()
{
	if(!IsPaused()) return;

#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_RESUME_THREAD, this);
#endif

	m_status = Running;
	DoResume();
	Emu.CheckStatus();

	ThreadBase::Start();

#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_RESUMED_THREAD, this);
#endif
}

void CPUThread::Pause()
{
	if(!IsRunning()) return;

#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_PAUSE_THREAD, this);
#endif

	m_status = Paused;
	DoPause();
	Emu.CheckStatus();

	ThreadBase::Stop(false);
#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_PAUSED_THREAD, this);
#endif
}

void CPUThread::Stop()
{
	if(IsStopped()) return;

#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_STOP_THREAD, this);
#endif

	m_status = Stopped;
	ThreadBase::Stop(false);
	Reset();
	DoStop();
	Emu.CheckStatus();

#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_STOPED_THREAD, this);
#endif
}

void CPUThread::Exec()
{
	m_is_step = false;
#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_EXEC_THREAD, this);
#endif
	ThreadBase::Start();
}

void CPUThread::ExecOnce()
{
	m_is_step = true;
#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_EXEC_THREAD, this);
#endif
	ThreadBase::Start();
	if(!ThreadBase::Wait()) while(m_is_step) Sleep(1);
#ifndef QT_UI
	wxGetApp().SendDbgCommand(DID_PAUSE_THREAD, this);
	wxGetApp().SendDbgCommand(DID_PAUSED_THREAD, this);
#endif
}

void CPUThread::Task()
{
	//ConLog.Write("%s enter", CPUThread::GetFName());

	const Array<u64>& bp = Emu.GetBreakPoints();

	try
	{
		for(uint i=0; i<bp.GetCount(); ++i)
		{
			if(bp[i] == m_offset + PC)
			{
				Emu.Pause();
				break;
			}
		}

		while(true)
		{
			int status = ThreadStatus();

			if(status == CPUThread_Stopped || status == CPUThread_Break)
			{
				break;
			}

			if(status == CPUThread_Sleeping)
			{
				Sleep(1);
				continue;
			}

			Step();
			NextPc(m_dec->DecodeMemory(PC + m_offset));

			if(status == CPUThread_Step)
			{
				m_is_step = false;
				break;
			}

			for(uint i=0; i<bp.GetCount(); ++i)
			{
				if(bp[i] == PC)
				{
					Emu.Pause();
					break;
				}
			}
		}
	}
	catch(const wxString& e)
	{
		ConLog.Error("Exception: %s", e.mb_str());
	}
	catch(const char* e)
	{
		ConLog.Error("Exception: %s", e);
	}
	catch(int exitcode)
	{
		ConLog.Success("Exit Code: %d", exitcode);
		return;
	}

	//ConLog.Write("%s leave", CPUThread::GetFName());

	if(m_free_data)
	{
		delete m_dec;
		m_dec = nullptr;
		free(this);
	}
}
