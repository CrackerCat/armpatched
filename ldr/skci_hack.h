#pragma once

#include "ndis_hack.h"

class skci_hack: public drv_hack
{
  public:
    skci_hack(arm64_pe_file *pe, exports_dict *ed, module_import *iat)
     : drv_hack(pe, ed, iat)
    {
      zero_data();
    }
    virtual ~skci_hack()
    { }
    int hack(int verbose);
    void dump() const;
  protected:
    void zero_data();
    int hack_gci(PBYTE);
    // output data
    PBYTE m_CipPolicyLock;
    PBYTE m_CiOptions;
    PBYTE m_CiDeveloperMode;
};
