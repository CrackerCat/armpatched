#pragma once

#include "hack.h"

class ntdll_hack: public arm64_hack
{
  public:
    ntdll_hack(arm64_pe_file *pe, exports_dict *ed)
     : arm64_hack(pe, ed)
    {
      zero_data();
    }
    virtual ~ntdll_hack()
    { }
    int hack(int verbose);
    void dump() const;
  protected:
    void zero_data();
    int hack_veh(PBYTE);
    // aux data
    PBYTE aux_RtlAcquireSRWLockExclusive;
    PBYTE aux_RtlAllocateHeap;
    PBYTE aux_LdrpMrdataLock; // not exported
    // output data
    PBYTE m_LdrpVectorHandlerList;
};