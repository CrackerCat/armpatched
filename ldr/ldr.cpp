#include "stdafx.h"
#include "pe_file.h"
#include "../source/armadillo.h"

void usage(const wchar_t *progname)
{
  printf("%S: [options] arm64_pe file(s)\n", progname);
  printf("Options:\n");
  printf(" -dlc - dump load_config\n");
  printf(" -de - dump exports\n");
  printf(" -dr - dump relocs\n");
  printf(" -ds - dump sections\n");
  printf(" -d  - dump all\n");
  exit(6);
}

typedef int (arm64_pe_file::*TDirGet)(DWORD &, DWORD &) const;

int wmain(int argc, wchar_t **argv)
{
   int dump_exp = 0;
   int dump_sects = 0;
   int dump_relocs = 0;
   int dump_lc = 0;
   std::pair<TDirGet, const char *> dir_get[] = { 
     std::make_pair(&arm64_pe_file::get_export, "export"),
     std::make_pair(&arm64_pe_file::get_import, "import"),
     std::make_pair(&arm64_pe_file::get_delayed_import, "delayed_import"),
     std::make_pair(&arm64_pe_file::get_bound_import, "get_bound"),
     std::make_pair(&arm64_pe_file::get_rsrc, "resources"),
     std::make_pair(&arm64_pe_file::get_security, "security"),
     std::make_pair(&arm64_pe_file::get_rel, "relocs"),
     std::make_pair(&arm64_pe_file::get_arch, "arch"),
     std::make_pair(&arm64_pe_file::get_gp, "GLOBALPTR"),
     std::make_pair(&arm64_pe_file::get_tls, "TLS"),
     std::make_pair(&arm64_pe_file::get_iat, "IAT"),
     std::make_pair(&arm64_pe_file::get_load_config, "load_config"),
     std::make_pair(&arm64_pe_file::get_exceptions, "exceptions"),
     std::make_pair(&arm64_pe_file::get_net, "COM")
   };
   if ( argc < 2 )
     usage(argv[0]);
   for ( int i = 1; i < argc; i++ )
   {
     if ( !wcscmp(argv[i], L"-de") )
     {
       dump_exp = 1;
       continue;
     }
     if ( !wcscmp(argv[i], L"-dr") )
     {
       dump_relocs = 1;
       continue;
     }
     if ( !wcscmp(argv[i], L"-dlc") )
     {
       dump_lc = 1;
       continue;
     }
     if ( !wcscmp(argv[i], L"-ds") )
     {
       dump_sects = 1;
       continue;
     }
     if ( !wcscmp(argv[i], L"-d") )
     {
       dump_exp = 1;
       dump_relocs = 1;
       dump_sects = 1;
       continue;
     }
     printf("%S:\n", argv[i]);
     arm64_pe_file f(argv[i]);
     if ( f.read(dump_sects) )
       continue;
     // dump PE directories
     printf("ImageBase: %I64X\n", f.image_base());
     for ( const auto &diter : dir_get )
     {
       DWORD addr = 0;
       DWORD size = 0;
       if ( ! (f.*(diter.first))(addr, size) )
         continue;
       printf("%s dir: rva %X size %X\n", diter.second, addr, size);
       const one_section *where = f.find_section_rva(addr);
       if ( NULL == where )
         continue;
       if (where->offset)
         printf(" in section %s, file offset %X\n", where->name, where->offset + addr - where->va);
       else
         printf(" in section %s\n", where->name);
     }
     // read exports
     exports_dict *ed = f.get_export_dict();
     if ( NULL != ed )
     {
       if ( dump_exp )
         ed->dump();
     }
     // dump relocs
     if ( dump_relocs )
     {
       DWORD r_size = 0;
       PBYTE rbody = f.read_relocs(r_size);
       if ( rbody != NULL )
       {
         PIMAGE_BASE_RELOCATION BaseReloc = (PIMAGE_BASE_RELOCATION)rbody;
         PBYTE  RelEnd = ((PBYTE)BaseReloc + r_size);
         while ((PBYTE)BaseReloc < RelEnd && BaseReloc->SizeOfBlock)
         {
           PRELOC Reloc = (PRELOC)((PBYTE)BaseReloc + sizeof(IMAGE_BASE_RELOCATION));
           for (DWORD i = 0;
                (i < (BaseReloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(RELOC)) &&
                ((PBYTE)&Reloc[i] < RelEnd);
                i++
               )
           {
             if ( !Reloc[i].Type ) // I don`t know WTF is absolute reloc means
               continue;
             const one_section *where = f.find_section_rva(BaseReloc->VirtualAddress + Reloc[i].Offset);
             if ( where == NULL )
               printf("reltype %d offset %X\n", Reloc[i].Type, BaseReloc->VirtualAddress + Reloc[i].Offset);
             else
               printf("reltype %d offset %X %s\n", Reloc[i].Type, BaseReloc->VirtualAddress + Reloc[i].Offset, where->name);
           }
           BaseReloc = (PIMAGE_BASE_RELOCATION)((PBYTE)BaseReloc + BaseReloc->SizeOfBlock);
         }
       }
       free(rbody);
     }
     // dump load_config
     if ( dump_lc )
     {
       DWORD lc_size = 0;
       Prfg_IMAGE_LOAD_CONFIG_DIRECTORY64 lc = (Prfg_IMAGE_LOAD_CONFIG_DIRECTORY64)f.read_load_config(lc_size);
       if ( lc != NULL && lc_size )
       {
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, SEHandlerTable) )
           printf("SecurityCookie: %I64X\n", lc->SecurityCookie);
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer) && lc->GuardCFCheckFunctionPointer )
           printf("GuardCFCheckFunctionPointer: %I64X\n", lc->GuardCFCheckFunctionPointer);
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFFunctionTable) && lc->GuardCFDispatchFunctionPointer )
           printf("GuardCFDispatchFunctionPointer: %I64X\n", lc->GuardCFDispatchFunctionPointer);
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, GuardRFFailureRoutineFunctionPointer) && lc->GuardRFFailureRoutine )
           printf("GuardRFFailureRoutine: %I64X\n", lc->GuardRFFailureRoutine);
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, DynamicValueRelocTableOffset) && lc->GuardRFFailureRoutineFunctionPointer )
           printf("GuardRFFailureRoutineFunctionPointer: %I64X\n", lc->GuardRFFailureRoutineFunctionPointer);
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, HotPatchTableOffset) && lc->GuardRFVerifyStackPointerFunctionPointer )
           printf("GuardRFVerifyStackPointerFunctionPointer: %I64X\n", lc->GuardRFVerifyStackPointerFunctionPointer);
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) && lc->DynamicValueRelocTable )
           printf("DynamicValueRelocTable: %I64X\n", lc->DynamicValueRelocTable);
         int has_rfg = 0;
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, DynamicValueRelocTableSection) && lc->DynamicValueRelocTableOffset )
         {
           printf("DynamicValueRelocTableOffset: %X\n", lc->DynamicValueRelocTableOffset);
           has_rfg = 1;
         }
         if ( lc_size >= offsetof(rfg_IMAGE_LOAD_CONFIG_DIRECTORY64, Reserved2) )
           printf("DynamicValueRelocTableSection: %X\n", lc->DynamicValueRelocTableSection);
         // dump RFG relocs
         if ( has_rfg )
           f.dump_rfg_relocs();
       }
     }
     if ( f.map_pe() && ed != NULL )
     {
       // apply relocs
       f.apply_relocs();
       // quick and dirty test
       const export_item *exp = ed->find("ExInitializeNPagedLookasideList");
       if (exp != NULL )
       {
         PBYTE addr = f.base_addr() + exp->rva;
         struct ad_insn arm_dis;
         for ( int i = 0; i < 3; i++, addr += 4 )
         {
           int res = ArmadilloDisassemble(*(PDWORD)addr, (uint64)(f.base_addr() + exp->rva + i * 4), &arm_dis);
           printf("res %d, %s\n", res, arm_dis.decoded);
           if ( res )
             break;
         }
       }
     }
     if ( ed != NULL )
       delete ed;
   }
   return 0;
}
