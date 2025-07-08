#pragma once

// Include winsock2.h before windows.h to avoid macro redefinition warnings
#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif
#include <windows.h>

class CLockData
{
public:
    CRITICAL_SECTION m_Criti;
    CLockData() { InitializeCriticalSection(&m_Criti); }
    ~CLockData() { DeleteCriticalSection(&m_Criti); }
};

class CLock
{
    CLockData *m_pData;
public:
    CLock(CLockData &pData) { m_pData = &pData; EnterCriticalSection(&m_pData->m_Criti); }
    ~CLock() { LeaveCriticalSection(&m_pData->m_Criti); }
};
