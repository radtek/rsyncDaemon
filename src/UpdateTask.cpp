//
// rsyncDaemon.cpp
//
// This sample demonstrates the ServerApplication class.
//
// Copyright (c) 2004-2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//

#include "UpdateTask.h"
#include "Poco/Util/ServerApplication.h"
#include "Poco/DateTimeFormatter.h"
#include "Poco/LogStream.h"
#include "Poco/Process.h"
#include "Poco/Debugger.h"
#include <iostream>


using namespace Reach;

using Poco::Util::Application;
using Poco::Task;
using Poco::DateTimeFormatter;
using Poco::LogStream;
using Poco::Process;
using Poco::ProcessHandle;
using Poco::Debugger;

UpdateTask::UpdateTask() 
	: Task("UpdateTask"), millisecond(5000 * 12),
	handle(0), hToken(0), hNewToken(0)
{
}

UpdateTask::~UpdateTask()
{
	if(hNewToken) CloseHandle(hNewToken);
	if(hToken) CloseHandle(hToken);
	if(handle) CloseHandle(handle);
}

void UpdateTask::runTask()
{
	Application& app = Application::instance();
	LogStream ls(app.logger());
	std::string name("updater.exe");
	std::string cmd;
	std::vector<std::string> args;
	args.push_back("-checkforupdates");

	std::string initialDirectory = app.config().getString("application.dir");
	int minute = app.config().getInt("updater.period.minute");
	int period = minute * millisecond;

	cmd.append(initialDirectory);
	cmd.append(name);

	while (!sleep(period))
	{
		createProcessAsUser(name, initialDirectory);
	}
}

/// CreateProcessAsUser is often used by Windows services
/// in case an executable is started that the user has specified
/// in order to run it with Medium Integrity Level 
/// to ensure privilege isolation.
/// Reference https://reverseengineering.stackexchange.com/questions/17010/how-is-createprocessasuser-impersonation-implemented-in-windows-services
#include "Windows.h"
#include "userenv.h"
DWORD ObtainExplorerProcessId();

void UpdateTask::createProcessAsUser(const std::string& name, const std::string& initialDirectory)
{
	// Call WTSGetActiveConsoleSessionId to retrieve session ID 
	// of the session attached to the physical console.
	DWORD sessionId = WTSGetActiveConsoleSessionId();
	// Obtain PID for a Administator process, like explorer.exe
	DWORD PID = ObtainExplorerProcessId();
	// Obtain a handle to this process by calling OpenProcess.
	DWORD dwDesiredAccess = MAXIMUM_ALLOWED;
	handle = OpenProcess(dwDesiredAccess, false, PID);
	if (!handle) throw Poco::SystemException("OpenProcess", GetLastError());
	// Using this handle, call OpenProcessToken to obtain a handle to
	// the process's access token.

	DWORD DesiredAccess = 
		TOKEN_ADJUST_PRIVILEGES | 
		TOKEN_QUERY |
		TOKEN_DUPLICATE |
		TOKEN_ASSIGN_PRIMARY |
		TOKEN_ADJUST_SESSIONID |
		TOKEN_READ |
		TOKEN_WRITE;

	if (!OpenProcessToken(handle, DesiredAccess, &hToken))
		throw Poco::SystemException("OpenProcessToken",GetLastError());

	TOKEN_PRIVILEGES tp;
	if (!LookupPrivilegeValue(0, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
		throw Poco::SystemException("LookupPrivilegeValue",GetLastError());

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	// Call DuplicateTokenEx specifying this handle and the user's session ID
	// in order to create a copy of the token.

	if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, 0, SecurityIdentification, TokenPrimary, &hNewToken))
		throw Poco::SystemException("DuplicateTokenEx",GetLastError());

	if (!SetTokenInformation(hNewToken, TokenSessionId, &sessionId, sizeof sessionId))
		throw Poco::SystemException("SetTokenInformation",GetLastError());

	if (!AdjustTokenPrivileges(hNewToken, false, &tp, sizeof TOKEN_PRIVILEGES, 0, 0))
		throw Poco::SystemException("AdjustTokenPrivileges", GetLastError());
	// Call CreateProcessAsUser with this duplicated token.

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	void* lpEnv = 0;
	DWORD dwCreationFlags = CREATE_NEW_CONSOLE |
		NORMAL_PRIORITY_CLASS;

	ZeroMemory(&si, sizeof(STARTUPINFO));
	si.cb = sizeof(STARTUPINFO);
	si.lpDesktop = L"Winsta0\\Default";
	ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

	if (CreateEnvironmentBlock(&lpEnv, hNewToken, true))
		dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;

	int wchars_num = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
	wchar_t* NAME = new wchar_t[wchars_num];
	MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NAME, wchars_num);

	wchars_num = MultiByteToWideChar(CP_UTF8, 0, initialDirectory.c_str(), -1, NULL, 0);
	wchar_t* INITIALDIRECTORY = new wchar_t[wchars_num];
	MultiByteToWideChar(CP_UTF8, 0, initialDirectory.c_str(), -1, INITIALDIRECTORY, wchars_num);

	if (!CreateProcessAsUserW(
		hNewToken,
		0,
		NAME,
		0, 0, false,
		dwCreationFlags,
		lpEnv,
		INITIALDIRECTORY,
		&si,&pi))
		throw Poco::SystemException("CreateProcessAsUser", GetLastError());

	CloseHandle(hNewToken);
	CloseHandle(hToken);
	CloseHandle(handle);
}

#include "tlhelp32.h"
#include "Poco/String.h"
#include "Poco/Path.h"

static DWORD ObtainExplorerProcessId()
{
	DWORD self = GetCurrentProcessId();
	std::string explorer("explorer.exe");

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		throw Poco::SystemException("CreateToolhelp32Snapshot", GetLastError());

	PROCESSENTRY32 ProcessEntry;
	if (!Process32First(hSnapshot, &ProcessEntry))
		throw Poco::SystemException("Process32First", GetLastError());

	do
	{
		if (ProcessEntry.th32ProcessID != self &&
			Poco::icompare(explorer, ProcessEntry.szExeFile) == 0 ||
			Poco::icompare(explorer, Poco::Path(ProcessEntry.szExeFile).getFileName()) == 0)
		{
			break;
		}

	} while (Process32Next(hSnapshot, &ProcessEntry));

	return ProcessEntry.th32ProcessID;
}