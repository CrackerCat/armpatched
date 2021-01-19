#include "stdafx.h"
#include "cf_graph.h"
#include "bm_search.h"
#include "deriv.h"

extern int gSE;
extern int gUseLC;

int deriv_tests::add_module(const wchar_t *fname)
{
  arm64_pe_file *f = new arm64_pe_file(fname);
  if ( f->read(0) )
  {
    delete f;
    return 0;
  }
  if ( !f->map_pe(0) )
  {
    delete f;
    return 0;
  }
  inmem_import_holder ih;
  module_import *mimp = ih.add(fname, f);
  deriv_test obj{ f, std::move(ih), new deriv_hack(f, f->get_export_dict(), mimp) };
  mods.push_back(std::move(obj));
  return 1;
}

const path_item *path_edge::get_best_const() const
{
  const path_item *res = NULL;
  for ( const auto &c: list )
  {
    if ( c.type != ldr_off )
      continue;
    if ( !c.value_count )
      continue;
    if ( res == NULL )
    {
      res = &c;
      continue;
    }
    if ( res->value_count < c.value_count )
      continue;
    res = &c;
  }
  return res;
}

int path_edge::reduce()
{
  if ( !can_reduce() )
    return 0;
  std::list<path_item> new_list;
  int state = 0;
  int res = 0;
  for ( auto rc = list.crbegin(); rc != list.crend(); ++rc )
  {
    if ( rc->is_load_store() )
    {
      if ( state )
      {
        res++;
        continue;
      }
    }
    else
      state = 1;
    new_list.push_front(*rc);
  }
  list = std::move(new_list);
  return res;
}

int path_edge::can_reduce() const
{
  int state = 0;
  for ( auto rc = list.crbegin(); rc != list.crend(); ++rc )
  {
    if ( rc->is_load_store() )
    {
      if ( state )
        return 1;
    } else
      state = 1;
  }
  return 0;
}

int path_edge::has_const_count(int below) const
{
  return std::any_of(list.cbegin(), list.cend(), [=](const path_item &item) -> bool { return (item.type == ldr_off) && item.value_count && (item.value_count < below); });
}

int path_edge::contains_imp(std::string &name) const
{
  for ( const auto &c: list )
  {
    if ( c.type != call_imp )
      continue;
    if ( c.name == name )
      return 1;
  }
  return 0;
}

int path_edge::is_imp1_only(std::string &name) const
{
  int res = 0;
  for ( const auto &c: list )
  {
    if ( c.is_load_store() )
      continue;
    if ( c.type != call_imp )
      return 0;
    if ( ++res > 2 )
      return 0;
    name = c.name;
  }
  return (res == 1);
}

int path_item::is_load_store() const
{
  switch(type)
  {
    case load:
    case store:
    case ldrb:
    case ldrh:
    case strb:
    case strh:
      if ( name.empty() )
        return 1;
  }
  return 0;
}

void path_item::dump() const
{
  printf(" RVA %X", rva);
  switch(type)
  {
    case ldr_cookie:
        printf(" load_cookie\n");
      break;
    case load: 
       if ( name.empty() )
         printf(" load\n");
       else
         printf(" load exported %s\n", name.c_str());
      break;
    case store: 
       if ( name.empty() )
         printf(" store\n");
       else
         printf(" store exorted %s\n", name.c_str());
       break;
    case ldrb:
       if ( name.empty() )
         printf(" ldrb\n");
       else
         printf(" ldrb exorted %s\n", name.c_str());
       break;
    case ldrh:
       if ( name.empty() )
         printf(" ldrh\n");
       else
         printf(" ldrh exorted %s\n", name.c_str());
       break;
    case strb:
       if ( name.empty() )
         printf(" strb\n");
       else
         printf(" strb exorted %s\n", name.c_str());
       break;
    case strh:
       if ( name.empty() )
         printf(" strh\n");
       else
         printf(" strh exorted %s\n", name.c_str());
       break;
    case ldr_off:
         if ( value_count )
           printf(" const %X count %d\n", value, value_count);
         else
           printf(" const %X\n", value);
       break;
    case call_imp:
        printf(" call_imp %s\n", name.c_str());
       break;
    case call_exp:
        printf(" call_exp %s\n", name.c_str());
       break;
    case call_icall:
        printf(" call_icall\n");
       break;
    default:
        printf(" unknown type %d\n", type);
  }
}

void funcs_holder_ts::add_processed(PBYTE addr)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_processed.insert(addr);
}

void funcs_holder_ts::add(PBYTE addr)
{
  if ( m_pe->inside_pdata(addr) )
    return;
  const std::lock_guard<std::mutex> lock(m_mutex);
  auto already = m_processed.find(addr);
  if ( already != m_processed.end() )
    return;
  m_current.insert(addr);
}

void funcs_holder::add_processed(PBYTE addr)
{
  m_processed.insert(addr);
}

void funcs_holder::add(PBYTE addr)
{
  if ( m_pe->inside_pdata(addr) )
    return;
  auto already = m_processed.find(addr);
  if ( already != m_processed.end() )
    return;
  m_current.insert(addr);
}

int funcs_holder::exchange(std::set<PBYTE> &outer)
{
  if ( empty() )
    return 0;
  outer.clear();
  outer = m_current;
  m_current.clear();
  return 1;
}

int funcs_holder_ts::exchange(std::set<PBYTE> &outer)
{
  if ( empty() )
    return 0;
  outer.clear();
  // bcs current set was filled in unpredictable times from several threads - it can contain already processed functions
  for ( const auto &c: m_current )
  {
    const auto p = m_processed.find(c);
    if ( p != m_processed.cend() )
      continue;
    outer.insert(c);
  }
  m_current.clear();
  return 1;
}

const char *deriv_hack::get_exported(PBYTE mz, PBYTE addr) const
{
  if ( m_ed == NULL )
    return NULL;
  DWORD rva = addr - mz;
  const export_item *ei = m_ed->find_exact(rva);
  if ( ei == NULL )
    return NULL;
  return ei->name;
}

#include <pshpack1.h>
struct fids_item
{
  DWORD rva;
  BYTE hz;
};
#include <poppack.h>

int deriv_hack::find_in_fids_table(PBYTE mz, PBYTE func) const
{
  if ( !gUseLC )
    return 0;
  DWORD lc_size = 0;
  Prfg_IMAGE_LOAD_CONFIG_DIRECTORY64 lc = (Prfg_IMAGE_LOAD_CONFIG_DIRECTORY64)m_pe->read_load_config(lc_size);
  if ( lc == NULL || !lc_size )
    return 0;
  if ( !lc->GuardCFFunctionTable || !lc->GuardCFFunctionCount )
    return 0;
  fids_item *fi = (fids_item *)(mz + (lc->GuardCFFunctionTable - m_pe->image_base()));
  for ( ULONGLONG i = 0; i < lc->GuardCFFunctionCount; i++, fi++ )
  {
    if ( fi->rva == func - mz )
      return 1;
  }
  return 0;
}

void deriv_hack::check_exported(PBYTE mz, found_xref &item) const
{
  DWORD rva = item.pfunc - mz;
  const one_section *s = m_pe->find_section_rva(rva);
  if ( s != NULL )
    item.section_name = s->name;
  if ( m_ed == NULL )
    return;
  const export_item *ei = m_ed->find_exact(rva);
  if ( ei == NULL )
    return;
  if ( ei->name == NULL )
    return;
  item.exported = ei->name;
}

int deriv_hack::store_op(path_item_type t, const one_section *s, PBYTE pattern, PBYTE what, path_edge &edge)
{
  PBYTE mz = m_pe->base_addr();
  if ( pattern == what )
  {
    edge.last.type = t;
    edge.last.rva = m_psp - mz;
    return 1;
  }
  // check if this symbol is exported
  const char *exp_func = get_exported(mz, what);
  if ( exp_func != NULL )
  {
    path_item tmp;
    tmp.rva = m_psp - mz;
    tmp.type = t;
    tmp.name = exp_func;
    edge.list.push_back(tmp);
    return 0;
  }
  // check for security_cookie
  if ( gUseLC && what == m_cookie )
  {
    path_item tmp;
    tmp.type = ldr_cookie;
    tmp.rva = m_psp - mz;
    edge.list.push_back(tmp);
    return 0;
  }
  const one_section *other = m_pe->find_section_v(what - mz);
  if ( other == NULL )
    return 0;
  if ( other != s )
    return 0;
  path_item tmp;
  tmp.type = t;
  tmp.rva = m_psp - mz;
  edge.list.push_back(tmp);
  return 0;
}

int deriv_hack::resolve_section(DWORD rva, std::string &out_name) const
{
  PBYTE mz = m_pe->base_addr();
  const one_section *s = m_pe->find_section_v(rva);
  if ( s == NULL )
    return 0;
  out_name = s->name;
  return 1;
}

struct path_state
{
  std::list<path_item>::const_iterator iter;
  const path_item *s;
  int n;
  int last;

  bool operator<(const path_state& s) const
  {
    return n < s.n;
  }
  int next(path_edge &path)
  {
    if ( last )
      return 1;
    if ( ++iter == path.list.cend() )
    {
      s = &path.last;
      last = 1;
    }
    else
      s = &(*iter);
    n++;
    return 0;
  }
};

int deriv_hack::apply(found_xref &xref, path_edge &path, DWORD &found)
{
  const one_section *s = m_pe->find_section_by_name(path.symbol_section.c_str());
  if ( s == NULL )
  {
    printf("cannot find section %s\n", path.symbol_section.c_str());
    return 0;
  }
  if ( xref.exported != NULL )
  {
    const export_item *exp = m_ed->find(xref.exported);
    if ( exp == NULL )
    {
      printf("cannot find exported function %s\n", xref.exported);
      return 0;
    }
    return try_apply(s, m_pe->base_addr() + exp->rva, path, found);
  } else {
    const one_section *cs = m_pe->find_section_by_name(xref.section_name.c_str());
    if ( cs == NULL )
    {
      printf("cannot find functions section %s\n", xref.section_name.c_str());
      return 0;
    }
    const path_item *imm = path.get_best_const();
    if ( imm == NULL )
    {
      printf("cannot get_best_const\n");
      return 0;
    }
    PBYTE start = m_pe->base_addr() + cs->va;
    PBYTE end = start + cs->size;
    bm_search srch((const PBYTE)&imm->value, sizeof(imm->value));
    PBYTE curr = start;
    std::list<PBYTE> founds;
    while ( curr < end )
    {
      const PBYTE fres = srch.search(curr, end - curr);
      if ( NULL == fres )
        break;
      try
      {
        founds.push_back(fres);
      } catch(std::bad_alloc)
      { return 0; }
      curr = fres + sizeof(imm->value);
    }
    if ( founds.empty() )
    {
      printf("cannot find constant %X in section %s\n", imm->value, xref.section_name.c_str());
      return 0;
    }
    for ( auto citer = founds.cbegin(); citer != founds.cend(); ++citer )
    {
      PBYTE func = find_pdata(*citer);
      if ( NULL == func )
        continue;
      if ( try_apply(s, func, path, found) )
        return 1;
    }
  }
  return 0;
}

int deriv_hack::try_apply(const one_section *s, PBYTE psp, path_edge &path, DWORD &found)
{
  PBYTE mz = m_pe->base_addr();
  statefull_graph<PBYTE, path_state> cgraph;
  std::list<std::pair<PBYTE, path_state> > addr_list;
  int is_empty = path.list.empty();
  auto citer = path.list.cbegin();
  path_state state { citer, is_empty ? &path.last : &(*citer), 0, is_empty ? 1 : 0 };
  auto curr = std::make_pair(psp, state);
  addr_list.push_back(curr);
  int edge_gen = 0;
  int edge_n = 0;
  int res = 0;
#ifdef _DEBUG
  m_verbose = 1;
#endif
  while( edge_gen < 100 )
  {
    for ( auto iter = addr_list.begin(); iter != addr_list.end(); ++iter )
    {
      psp = iter->first;
      if ( cgraph.in_ranges(psp) )
        continue;
      if ( !setup(psp) )
        continue;      
      edge_n++;
      regs_pad used_regs;
      while( 1 )
      {
        if ( !disasm() || is_ret() )
          break;
        if ( check_jmps(cgraph, iter->second) )
          continue;
        PBYTE b_addr = NULL;
        if ( is_b_jimm(b_addr) )
        {
          if ( gSE )
          {
            if ( NULL != get_exported(mz, b_addr) )
              break;
          }
          cgraph.add(b_addr, iter->second);
          break;
        }
        // check for bl
        PBYTE caddr = NULL;
        if ( is_bl_jimm(caddr) && iter->second.s->type == call_exp )
        {
          const char *exp_func = get_exported(mz, caddr);
          if ( exp_func == NULL )
            continue;
          if ( !strcmp(exp_func, iter->second.s->name.c_str()) )
            iter->second.next(path);
          else
            break;
          continue;
        }
        if ( is_br_reg() )
        {
          PBYTE what = (PBYTE)used_regs.get(get_reg(0));
          if ( what != NULL && in_executable_section(what) )
             cgraph.add(what, iter->second);
          break;
        }
        if ( is_adrp(used_regs) )
          continue;
        // ldar
        if ( is_ldar(used_regs) )
          continue;
        // bl reg - usually call [IAT]
        if ( is_bl_reg() )
        {
          PBYTE what = (PBYTE)used_regs.get(get_reg(0));
          if ( iter->second.s->type == call_imp )
          {
            const char *name = get_iat_func(what);
            if ( name == NULL )
              continue;
            if ( !strcmp(name, iter->second.s->name.c_str()) )
              iter->second.next(path);
            else
              break;
          } else if ( iter->second.s->type == call_icall )
          {
            if ( what == m_GuardCFCheckFunctionPointer )
              iter->second.next(path);
          }
          continue;
        }
        // and now different variants of xref
        if ( is_add() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( iter->second.s->type != load )
            continue;
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        if ( is_ldr() && iter->second.s->type == ldr_cookie)
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( what == m_cookie )
            iter->second.next(path);
          continue;
        }
        if ( is_ldr() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( iter->second.s->type != load )
            continue;
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        if ( is_ldrb() && iter->second.s->type == ldrb )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        if ( is_ldrh() && iter->second.s->type == ldrh )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        if ( is_str() && iter->second.s->type == store )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        if ( is_strb() && iter->second.s->type == strb )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        if ( is_strh() && iter->second.s->type == strh )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( !iter->second.s->name.empty() )
          {
            const char *exp_name = get_exported(mz, what);
            if ( NULL == exp_name )
              continue;
            if ( strcmp(iter->second.s->name.c_str(), exp_name) )
              break;
          } else {
            const one_section *their = m_pe->find_section_v(what - mz);
            if ( their == NULL || their != s )
              continue;
          }
          if ( iter->second.next(path) )
          {
            found = what - mz;
            return 1;
          }
          continue;
        }
        // loading of constants
        if ( is_ldr_off() && iter->second.s->type == ldr_off )
        {
          if ( iter->second.s->value != *(PDWORD)m_dis.operands[1].op_imm.bits )
            break;
          iter->second.next(path);
          continue;
        }
      }
      cgraph.add_range(psp, m_psp - psp);
    }
    // prepare for next edge generation
    edge_gen++;
    if ( !cgraph.delete_ranges(&cgraph.ranges, &addr_list) )
      break;    
  }
  return 0;
}

int deriv_hack::make_path(DWORD rva, PBYTE psp, path_edge &out_res)
{
  PBYTE mz = m_pe->base_addr();
  PBYTE psp_copy = psp;
  const one_section *s = m_pe->find_section_v(rva);
  if ( s == NULL )
    return 0;
  PBYTE pattern = mz + rva;
  statefull_graph<PBYTE, path_edge> cgraph;
  std::list<std::pair<PBYTE, path_edge> > addr_list;
  path_edge tmp;
  tmp.symbol_section = s->name;
  auto curr = std::make_pair(psp, tmp);
  addr_list.push_back(curr);
  int edge_gen = 0;
  int edge_n = 0;
  int res = 0;
  while( edge_gen < 100 )
  {
    for ( auto iter = addr_list.begin(); iter != addr_list.end(); ++iter )
    {
      psp = iter->first;
      if ( cgraph.in_ranges(psp) )
        continue;
      if ( !setup(psp) )
        continue;      
      edge_n++;
      regs_pad used_regs;
      while( 1 )
      {
        if ( !disasm() || is_ret() )
          break;
        if ( check_jmps(cgraph, iter->second) )
          continue;
        PBYTE b_addr = NULL;
        if ( is_b_jimm(b_addr) )
        {
          if ( gSE )
          {
            if ( NULL != get_exported(mz, b_addr) )
              break;
          }
          cgraph.add(b_addr, iter->second);
          break;
        }
        // check for bl
        PBYTE caddr = NULL;
        if ( is_bl_jimm(caddr) )
        {
          const char *exp_func = get_exported(mz, caddr);
          if ( exp_func != NULL )
          {
            path_item tmp;
            tmp.rva = m_psp - mz;
            tmp.name = exp_func;
            tmp.type = call_exp;
            iter->second.list.push_back(tmp);
          }
          continue;
        }
        if ( is_br_reg() )
        {
          PBYTE what = (PBYTE)used_regs.get(get_reg(0));
          if ( what != NULL && in_executable_section(what) )
             cgraph.add(what, iter->second);
          break;
        }
        if ( is_adrp(used_regs) )
          continue;
        // ldar
        if ( is_ldar(used_regs) )
          continue;
        // bl reg - usually call [IAT]
        if ( is_bl_reg() )
        {
          PBYTE what = (PBYTE)used_regs.get(get_reg(0));
          const char *name = get_iat_func(what);
          if ( name != NULL )
          {
            path_item tmp;
            tmp.rva = m_psp - mz;
            tmp.name = name;
            tmp.type = call_imp;
            iter->second.list.push_back(tmp);
          } else if ( gUseLC && what == m_GuardCFCheckFunctionPointer )
          {
            path_item tmp;
            tmp.rva = m_psp - mz;
            tmp.type = call_icall;
            iter->second.list.push_back(tmp);
          }
          continue;
        }
        // and now different variants of xref
        if ( is_add() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(load, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        if ( is_ldr() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(load, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        if ( is_ldrb() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(ldrb, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        if ( is_ldrh() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(ldrh, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        if ( is_str() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(store, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        if ( is_strb() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(strb, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        if ( is_strh() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          res = store_op(strh, s, pattern, what, iter->second);
          if ( res )
          {
            out_res = iter->second;
            goto end;
          }
          continue;
        }
        // loading of constants
        if ( is_ldr_off() )
        {
          path_item tmp;
          tmp.rva = m_psp - mz;
          tmp.type = ldr_off;
          tmp.value = *(PDWORD)m_dis.operands[1].op_imm.bits;
          if ( tmp.value ) // skip zero constants
          {
            tmp.value_count = 0;
            iter->second.list.push_back(tmp);
          }
          continue;
        }
      }
      cgraph.add_range(psp, m_psp - psp);
    }
    // prepare for next edge generation
    edge_gen++;
    if ( !cgraph.delete_ranges(&cgraph.ranges, &addr_list) )
      break;    
  }
end:
  if ( res )
    calc_const_count(psp_copy, out_res);
  return res;
}

void deriv_hack::calc_const_count(PBYTE func, path_edge &out_res)
{
  PBYTE mz = m_pe->base_addr();
  const one_section *s = m_pe->find_section_rva(func - mz);
  if ( s == NULL )
    return;
  for ( auto& item: out_res.list )
  {
    if ( item.type != ldr_off )
       continue;
    PBYTE start = mz + s->va;
    PBYTE end = start + s->size;
    bm_search srch((const PBYTE)&item.value, sizeof(item.value));
    PBYTE curr = start;
    while ( curr < end )
    {
      const PBYTE fres = srch.search(curr, end - curr);
      if ( NULL == fres )
        break;
      item.value_count++;
      curr = fres + sizeof(item.value);
    }
  }
}

// process one function (starting with psp) to find xref to what, add all newly discovered functions to fh
template <typename FH>
int deriv_hack::disasm_one_func(PBYTE psp, PBYTE pattern, FH &fh)
{
  cf_graph<PBYTE> cgraph;
  std::list<PBYTE> addr_list;
  addr_list.push_back(psp);
  int edge_n = 0;
  int edge_gen = 0;
  int res = 0;
  while( edge_gen < 100 )
  {
    for ( auto iter = addr_list.cbegin(); iter != addr_list.cend(); ++iter )
    {
      psp = *iter;
      if ( cgraph.in_ranges(psp) )
        continue;
      if ( !setup(psp) )
        continue;      
      edge_n++;
      regs_pad used_regs;
      while( 1 )
      {
        if ( !disasm() || is_ret() )
          break;
        if ( check_jmps(cgraph) )
          continue;
        PBYTE b_addr = NULL;
        if ( is_b_jimm(b_addr) )
        {
          if ( gSE )
          {
            if ( NULL != get_exported(m_pe->base_addr(), b_addr) )
              break;
          }
          cgraph.add(b_addr);
          break;
        }
        if ( is_adrp(used_regs) )
          continue;
        // check for bl
        PBYTE caddr = NULL;
        if ( is_bl_jimm(caddr) )
        {
          fh.add(caddr);
          continue;
        }
        if ( is_br_reg() )
        {
          PBYTE what = (PBYTE)used_regs.get(get_reg(0));
          if ( what != NULL && in_executable_section(what) )
             cgraph.add(what);
          break;
        }
        // and now different variants of xref
        if ( is_add() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( what == pattern )
            res++;
          continue;
        }
        // loading
        if ( is_ldr() || is_ldrb() || is_ldrh() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( what == pattern )
            res++;
          continue;
        }
        // storing
        if ( is_str() || is_strb() || is_strh() )
        {
          PBYTE what = (PBYTE)used_regs.add2(get_reg(0), get_reg(1), m_dis.operands[2].op_imm.bits);
          if ( what == pattern )
            res++;
          continue;
        }
      }
      cgraph.add_range(psp, m_psp - psp);
    }
    // prepare for next edge generation
    edge_gen++;
    if ( !cgraph.delete_ranges(&cgraph.ranges, &addr_list) )
      break;    
  }
  return res;
}

struct pdata_item
{
  DWORD off;
  DWORD seh_off;
};

int deriv_hack::find_xrefs(DWORD rva, std::list<found_xref> &out_res)
{
  if ( !has_pdata() )
    return 0;
  PBYTE mz = m_pe->base_addr();
  funcs_holder f(this);
  int res = 0;
  // process pdata first
  const pdata_item *first = (const pdata_item *)(mz + m_pdata_rva);
  const pdata_item *last = (const pdata_item *)(mz + m_pdata_rva + m_pdata_size);
  for ( ; first < last; first++ )
  {
    if ( disasm_one_func(mz + first->off, mz + rva, f) )
    {
      found_xref tmp { mz + first->off, 0 };
      tmp.in_fids_table = find_in_fids_table(mz, mz + first->off);
      check_exported(mz, tmp);
      out_res.push_back(tmp);
      res++;
    }
  }
  if ( f.empty() )
    return res;
  // now process newly discovered functions
  std::set<PBYTE> current_set;
  while( f.exchange(current_set) )
  {
    for ( auto c : current_set )
    {
      if ( disasm_one_func(c, mz + rva, f) )
      {
        found_xref tmp { c, 0 };
        tmp.in_fids_table = find_in_fids_table(mz, c);
        check_exported(mz, tmp);
        out_res.push_back(tmp);
        res++;
      }
      f.add_processed(c);
    }
  }
  return res;
}

int deriv_pool::find_xrefs(DWORD rva, std::list<found_xref> &out_res)
{
  deriv_hack *d = get_first();
  if ( !d->has_pdata() )
    return 0;
  const PBYTE mz = d->base_addr();
  funcs_holder_ts f(d);
  int res = 0;
  DWORD pdata_rva = 0,
        pdata_size = 0;
  d->get_pdata(pdata_rva, pdata_size);
  // process pdata first
  const pdata_item *first = (const pdata_item *)(mz + pdata_rva);
  const pdata_item *last = (const pdata_item *)(mz + pdata_rva + pdata_size);
  int tcsize = m_ders.size();
  std::vector<std::future<xref_res> > futures(tcsize);
  DWORD i = 0;
  for ( ; first < last; first++ )
  {
    if ( i >= tcsize )
    {
      // harvest results
      for ( DWORD j = 0; j < tcsize; j++ )
      {
        xref_res tres = futures[j].get();
        if ( tres.res )
        {
          res++;
          out_res.push_back(tres.xref);
        }
      }
      i = 0;
    }
    // put new task
    PBYTE addr = mz + first->off;
    std::packaged_task<xref_res()> job([&, i, addr] {
       xref_res task_res = { 0 };
       task_res.res = m_ders[i]->disasm_one_func(addr, mz + rva, f);
       if (task_res.res)
       {
         task_res.xref.pfunc = addr;
         task_res.xref.in_fids_table = m_ders[i]->find_in_fids_table(mz, addr);
         m_ders[i]->check_exported(mz, task_res.xref);
       }
       return task_res;
      }
    );
    futures[i++] = std::move(m_tpool.add(job));
  }
  // collect remaining results
  for ( DWORD j = 0; j < i; j++ )
  {
     xref_res tres = futures[j].get();
     if ( tres.res )
     {
       res++;
       out_res.push_back(tres.xref);
     }
  }

  if ( f.empty() )
    return res;
  std::set<PBYTE> current_set;
  while( f.exchange(current_set) )
  {
    i = 0;
    for ( auto c : current_set )
    {
      if ( i >= tcsize )
      {
        // harvest results
        for ( DWORD j = 0; j < tcsize; j++ )
        {
          xref_res tres = futures[j].get();
          if ( tres.res )
          {
            res++;
            out_res.push_back(tres.xref);
          }
        }
        i = 0;
      }
      // put new task
      std::packaged_task<xref_res()> job([&, i, c] {
         xref_res task_res = { 0 };
         task_res.res = m_ders[i]->disasm_one_func(c, mz + rva, f);
         if (task_res.res)
         {
           task_res.xref.pfunc = c;
           task_res.xref.in_fids_table = m_ders[i]->find_in_fids_table(mz, c);
           m_ders[i]->check_exported(mz, task_res.xref);
         }
         return task_res;
        }
      );
      futures[i++] = std::move(m_tpool.add(job));
      f.add_processed(c);
    }
    // collect remaining results
    for ( DWORD j = 0; j < i; j++ )
    {
       xref_res tres = futures[j].get();
       if ( tres.res )
       {
         res++;
         out_res.push_back(tres.xref);
       }
    }
  }
  return res;
}