/*******************************************************************************
 * Copyright (c) 2007, 2010 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Symbols service - ELF version.
 */

#include <config.h>

#if SERVICE_Symbols && !ENABLE_SymbolsProxy && ENABLE_ELF

#if defined(_WRS_KERNEL)
#  include <symLib.h>
#  include <sysSymTbl.h>
#endif

#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <framework/errors.h>
#include <framework/myalloc.h>
#include <framework/events.h>
#include <framework/exceptions.h>
#include <services/tcf_elf.h>
#include <services/dwarf.h>
#include <services/dwarfcache.h>
#include <services/dwarfexpr.h>
#include <services/dwarfframe.h>
#include <services/stacktrace.h>
#include <services/symbols.h>
#if ENABLE_RCBP_TEST
#  include <main/test.h>
#endif

struct Symbol {
    unsigned magic;
    ObjectInfo * obj;
    SymbolSection * tbl;
    ContextAddress address;
    int sym_class;
    Context * ctx;
    int frame;
    unsigned index;
    unsigned dimension;
    ContextAddress size;
    Symbol * base;
};

#include <services/symbols_alloc.h>

static Context * sym_ctx;
static int sym_frame;
static ContextAddress sym_ip;

static int get_sym_context(Context * ctx, int frame) {
    if (frame == STACK_NO_FRAME) {
        ctx = context_get_group(ctx, CONTEXT_GROUP_PROCESS);
        sym_ip = 0;
    }
    else if (frame == STACK_TOP_FRAME) {
        sym_ip = get_regs_PC(ctx);
    }
    else {
        U8_T ip = 0;
        StackFrame * info = NULL;
        if (get_frame_info(ctx, frame, &info) < 0) return -1;
        if (read_reg_value(info, get_PC_definition(ctx), &ip) < 0) return -1;
        sym_ip = (ContextAddress)ip;
    }
    sym_ctx = ctx;
    sym_frame = frame;
    return 0;
}

/* Map ELF symbol table entry value to run-time address in given context address space */
static int syminfo2address(Context * ctx, SymbolInfo * info, ContextAddress * address) {
    switch (info->mType) {
    case STT_OBJECT:
    case STT_FUNC:
        {
            U8_T value = info->mValue;
            ELF_File * file = info->mSymSection->mFile;
            ELF_Section * sec = NULL;
            if (info->mSectionIndex == SHN_UNDEF) {
                errno = ERR_INV_ADDRESS;
                return -1;
            }
            if (file->type == ET_REL) {
                switch (info->mSectionIndex) {
                case SHN_ABS:
                    break;
                case SHN_COMMON:
                case SHN_UNDEF:
                    errno = ERR_INV_ADDRESS;
                    return -1;
                default:
                    sec = info->mSection;
                    value += sec->addr;
                    break;
                }
            }
            *address = elf_map_to_run_time_address(ctx, file, sec, (ContextAddress)value);
            return 0;
        }
    }
    errno = ERR_INV_ADDRESS;
    return -1;
}

static void object2symbol(ObjectInfo * obj, Symbol ** res) {
    Context * ctx = sym_ctx;
    Symbol * sym = alloc_symbol();
    sym->obj = obj;
    sym->frame = STACK_NO_FRAME;
    switch (obj->mTag) {
    case TAG_global_subroutine:
    case TAG_subroutine:
    case TAG_subprogram:
    case TAG_entry_point:
        sym->sym_class = SYM_CLASS_FUNCTION;
        break;
    case TAG_array_type:
    case TAG_class_type:
    case TAG_enumeration_type:
    case TAG_pointer_type:
    case TAG_reference_type:
    case TAG_string_type:
    case TAG_structure_type:
    case TAG_subroutine_type:
    case TAG_union_type:
    case TAG_ptr_to_member_type:
    case TAG_set_type:
    case TAG_subrange_type:
    case TAG_base_type:
    case TAG_fund_type:
    case TAG_file_type:
    case TAG_packed_type:
    case TAG_thrown_type:
    case TAG_const_type:
    case TAG_volatile_type:
    case TAG_restrict_type:
    case TAG_interface_type:
    case TAG_unspecified_type:
    case TAG_mutable_type:
    case TAG_shared_type:
    case TAG_typedef:
        sym->sym_class = SYM_CLASS_TYPE;
        break;
    case TAG_global_variable:
    case TAG_member:
        sym->sym_class = SYM_CLASS_REFERENCE;
        break;
    case TAG_formal_parameter:
    case TAG_local_variable:
    case TAG_variable:
        sym->sym_class = SYM_CLASS_REFERENCE;
        sym->frame = sym_frame;
        break;
    case TAG_constant:
    case TAG_enumerator:
        sym->sym_class = SYM_CLASS_VALUE;
        break;
    }
    if (sym->frame == STACK_NO_FRAME) ctx = context_get_group(ctx, CONTEXT_GROUP_PROCESS);
    sym->ctx = ctx;
    *res = sym;
}

static int get_num_prop(ObjectInfo * obj, int at, U8_T * res) {
    Trap trap;
    PropertyValue v;

    if (!set_trap(&trap)) return 0;
    read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, 0, obj, at, &v);
    *res = get_numeric_property_value(&v);
    clear_trap(&trap);
    return 1;
}

static int find_in_object_tree(ObjectInfo * list, const char * name, Symbol ** sym) {
    int found = 0;
    ObjectInfo * obj = list;
    while (obj != NULL) {
        if (obj->mName != NULL && strcmp(obj->mName, name) == 0) {
            object2symbol(obj, sym);
            found = 1;
        }
        switch (obj->mTag) {
        case TAG_enumeration_type:
            if (find_in_object_tree(obj->mChildren, name, sym)) found = 1;
            break;
        case TAG_global_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
        case TAG_lexical_block:
            {
                U8_T LowPC, HighPC;
                if (get_num_prop(obj, AT_low_pc, &LowPC) && get_num_prop(obj, AT_high_pc, &HighPC)) {
                    if (LowPC <= sym_ip && HighPC > sym_ip) {
                        if (find_in_object_tree(obj->mChildren, name, sym)) return 1;
                    }
                }
            }
            break;
        }
        obj = obj->mSibling;
    }
    return found;
}

static int find_in_dwarf(const char * name, Symbol ** sym) {
    UnitAddressRange * range = elf_find_unit(sym_ctx, sym_ip, sym_ip + 1, NULL);
    if (range != NULL) {
        CompUnit * unit = range->mUnit;
        if (find_in_object_tree(unit->mObject->mChildren, name, sym)) return 1;
        if (unit->mBaseTypes != NULL) {
            if (find_in_object_tree(unit->mBaseTypes->mObject->mChildren, name, sym)) return 1;
        }
    }
    return 0;
}

static int find_by_name_in_sym_table(DWARFCache * cache, char * name, Symbol ** res) {
    unsigned m = 0;
    unsigned h = calc_symbol_name_hash(name);
    unsigned cnt = 0;
    Context * prs = context_get_group(sym_ctx, CONTEXT_GROUP_PROCESS);
    while (m < cache->mSymSectionsCnt) {
        SymbolSection * tbl = cache->mSymSections[m];
        unsigned n = tbl->mSymbolHash[h];
        while (n) {
            SymbolInfo sym_info;
            unpack_elf_symbol_info(tbl, n, &sym_info);
            if (strcmp(name, sym_info.mName) == 0) {
                ContextAddress addr = 0;
                if (syminfo2address(prs, &sym_info, &addr) == 0 && addr != 0) {
                    int found = 0;
                    UnitAddressRange * range = elf_find_unit(sym_ctx, addr, addr + 1, NULL);
                    if (range != NULL) {
                        ObjectInfo * obj = range->mUnit->mObject->mChildren;
                        while (obj != NULL) {
                            switch (obj->mTag) {
                            case TAG_global_subroutine:
                            case TAG_global_variable:
                            case TAG_subroutine:
                            case TAG_subprogram:
                            case TAG_variable:
                                if (obj->mName != NULL && strcmp(obj->mName, name) == 0) {
                                    object2symbol(obj, res);
                                    found = 1;
                                    cnt++;
                                }
                                break;
                            }
                            obj = obj->mSibling;
                        }
                    }
                    if (!found) {
                        Symbol * sym = alloc_symbol();
                        sym->frame = STACK_NO_FRAME;
                        sym->ctx = prs;
                        sym->tbl = tbl;
                        sym->index = n;
                        switch (sym_info.mType) {
                        case STT_FUNC:
                            sym->sym_class = SYM_CLASS_FUNCTION;
                            break;
                        case STT_OBJECT:
                            sym->sym_class = SYM_CLASS_REFERENCE;
                            break;
                        }
                        *res = sym;
                        cnt++;
                    }
                }
            }
            n = tbl->mHashNext[n];
        }
        m++;
    }
    return cnt == 1;
}

int find_symbol_by_name(Context * ctx, int frame, char * name, Symbol ** res) {
    int error = 0;
    int found = 0;

    assert(ctx != NULL);

#if defined(_WRS_KERNEL)
    {
        char * ptr;
        SYM_TYPE type;

        if (symFindByName(sysSymTbl, name, &ptr, &type) != OK) {
            error = errno;
            assert(error != 0);
            if (error == S_symLib_SYMBOL_NOT_FOUND) error = 0;
        }
        else {
            Symbol * sym = alloc_symbol();
            sym->ctx = context_get_group(ctx, CONTEXT_GROUP_PROCESS);
            sym->address = (ContextAddress)ptr;

            if (SYM_IS_TEXT(type)) {
                sym->sym_class = SYM_CLASS_FUNCTION;
            }
            else {
                sym->sym_class = SYM_CLASS_REFERENCE;
            }
            *res = sym;
            found = 1;
        }
    }
#endif

    if (error == 0 && !found && get_sym_context(ctx, frame) < 0) error = errno;

    if (error == 0 && !found && sym_ip != 0) {
        Trap trap;
        if (set_trap(&trap)) {
            found = find_in_dwarf(name, res);
            clear_trap(&trap);
        }
        else {
            error = trap.error;
        }
    }

    if (error == 0 && !found && sym_ip != 0) {
        ELF_File * file = elf_list_first(sym_ctx, sym_ip, sym_ip);
        if (file == NULL) error = errno;
        while (error == 0 && file != NULL) {
            Trap trap;
            if (set_trap(&trap)) {
                DWARFCache * cache = get_dwarf_cache(file);
                found = find_by_name_in_sym_table(cache, name, res);
                clear_trap(&trap);
            }
            else {
                error = trap.error;
                break;
            }
            if (found) break;
            file = elf_list_next(sym_ctx);
            if (file == NULL) error = errno;
        }
        elf_list_done(sym_ctx);
    }

    if (error == 0 && !found) {
        ELF_File * file = elf_list_first(sym_ctx, 0, ~(ContextAddress)0);
        if (file == NULL) error = errno;
        while (error == 0 && file != NULL) {
            Trap trap;
            if (set_trap(&trap)) {
                DWARFCache * cache = get_dwarf_cache(file);
                found = find_by_name_in_sym_table(cache, name, res);
                clear_trap(&trap);
            }
            else {
                error = trap.error;
                break;
            }
            if (found) break;
            file = elf_list_next(sym_ctx);
            if (file == NULL) error = errno;
        }
        elf_list_done(sym_ctx);
    }

    if (error == 0 && !found && sym_ip != 0) {
        Trap trap;
        if (set_trap(&trap)) {
            const char * s = NULL;
            if (strcmp(name, "signed") == 0) s = "int";
            else if (strcmp(name, "signed int") == 0) s = "int";
            else if (strcmp(name, "unsigned") == 0) s = "unsigned int";
            else if (strcmp(name, "short") == 0) s = "short int";
            else if (strcmp(name, "signed short") == 0) s = "short int";
            else if (strcmp(name, "signed short int") == 0) s = "short int";
            else if (strcmp(name, "unsigned short") == 0) s = "unsigned short int";
            else if (strcmp(name, "long") == 0) s = "long int";
            else if (strcmp(name, "signed long") == 0) s = "long int";
            else if (strcmp(name, "signed long int") == 0) s = "long int";
            else if (strcmp(name, "unsigned long") == 0) s = "unsigned long int";
            else if (strcmp(name, "long long") == 0) s = "long long int";
            else if (strcmp(name, "signed long long") == 0) s = "long long int";
            else if (strcmp(name, "signed long long int") == 0) s = "long long int";
            else if (strcmp(name, "unsigned long long") == 0) s = "unsigned long long int";
            else if (strcmp(name, "char") == 0) s = "signed char";
            if (s != NULL) found = find_in_dwarf(s, res);
            clear_trap(&trap);
        }
        else {
            error = trap.error;
        }
    }

#if ENABLE_RCBP_TEST
    if (!found) {
        int sym_class = 0;
        void * address = NULL;
        found = find_test_symbol(ctx, name, &address, &sym_class) >= 0;
        if (found) {
            Symbol * sym = alloc_symbol();
            sym->ctx = context_get_group(ctx, CONTEXT_GROUP_PROCESS);
            sym->address = (ContextAddress)address;
            sym->sym_class = sym_class;
            *res = sym;
        }
    }
#endif

    if (error == 0 && !found) error = ERR_SYM_NOT_FOUND;

    assert(error || (*res != NULL && (*res)->ctx != NULL));

    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static int find_by_addr_in_sym_table(DWARFCache * cache, ContextAddress addr, Symbol ** res) {
    /* TODO: need faster symbol by address search */
    unsigned m = 0;
    unsigned cnt = 0;
    Context * prs = context_get_group(sym_ctx, CONTEXT_GROUP_PROCESS);
    while (m < cache->mSymSectionsCnt) {
        SymbolSection * tbl = cache->mSymSections[m];
        unsigned n = 1;
        while (n < tbl->mSymCount) {
            ContextAddress sym_addr = 0;
            SymbolInfo sym_info;
            unpack_elf_symbol_info(tbl, n, &sym_info);
            if (syminfo2address(prs, &sym_info, &sym_addr) == 0 &&
                    (sym_addr == addr || sym_addr <= addr && sym_addr + (ContextAddress)sym_info.mSize > addr)) {
                Symbol * sym = alloc_symbol();
                sym->frame = STACK_NO_FRAME;
                sym->ctx = prs;
                sym->tbl = tbl;
                sym->index = n;
                switch (sym_info.mType) {
                case STT_FUNC:
                    sym->sym_class = SYM_CLASS_FUNCTION;
                    break;
                case STT_OBJECT:
                    sym->sym_class = SYM_CLASS_REFERENCE;
                    break;
                }
                *res = sym;
                cnt++;
            }
            n++;
        }
        m++;
    }
    return cnt == 1;
}

static int find_by_addr_in_unit(ObjectInfo * obj, int level, ContextAddress addr, Symbol ** res) {
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_global_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
        case TAG_lexical_block:
            {
                U8_T LowPC, HighPC;
                if (get_num_prop(obj, AT_low_pc, &LowPC) && get_num_prop(obj, AT_high_pc, &HighPC)) {
                    if (LowPC <= addr && HighPC > addr) {
                        object2symbol(obj, res);
                        return 1;
                    }
                    if (LowPC <= sym_ip && HighPC > sym_ip) {
                        return find_by_addr_in_unit(obj->mChildren, level + 1, addr, res);
                    }
                }
            }
            break;
        case TAG_formal_parameter:
        case TAG_local_variable:
            if (sym_frame == STACK_NO_FRAME) break;
        case TAG_variable:
            {
                U8_T lc = 0;
                U8_T sz = 0;
                if (!get_num_prop(obj, AT_location, &lc)) exception(errno);
                if (!get_num_prop(obj, AT_byte_size, &sz)) exception(errno);
                if (lc <= addr && lc + sz > addr) {
                    object2symbol(obj, res);
                    return 1;
                }
            }
            break;
        }
        obj = obj->mSibling;
    }
    return 0;
}

static int find_by_addr_in_sym_tables(ContextAddress addr, Symbol ** res) {
    int error = 0;
    int found = 0;
    ELF_File * file = elf_list_first(sym_ctx, addr, addr);
    if (file == NULL) error = errno;
    while (error == 0 && file != NULL) {
        Trap trap;
        if (set_trap(&trap)) {
            DWARFCache * cache = get_dwarf_cache(file);
            found = find_by_addr_in_sym_table(cache, addr, res);
            clear_trap(&trap);
        }
        else {
            error = trap.error;
            break;
        }
        if (found) break;
        file = elf_list_next(sym_ctx);
        if (file == NULL) error = errno;
    }
    elf_list_done(sym_ctx);
    if (error) exception(error);
    return found;
}

int find_symbol_by_addr(Context * ctx, int frame, ContextAddress addr, Symbol ** res) {
    Trap trap;
    int found = 0;
    UnitAddressRange * range = NULL;
    if (!set_trap(&trap)) return -1;
    if (frame == STACK_TOP_FRAME && (frame = get_top_frame(ctx)) < 0) exception(errno);
    if (get_sym_context(ctx, frame) < 0) exception(errno);
    range = elf_find_unit(sym_ctx, addr, addr + 1, NULL);
    if (range != NULL) found = find_by_addr_in_unit(range->mUnit->mObject->mChildren, 0, addr, res);
    if (!found) found = find_by_addr_in_sym_tables(addr, res);
    if (!found && sym_ip != 0) {
        /* Search in compilation unit that contains stack frame PC */
        range = elf_find_unit(sym_ctx, sym_ip, sym_ip + 1, NULL);
        if (range != NULL) found = find_by_addr_in_unit(range->mUnit->mObject->mChildren, 0, addr, res);
    }
    if (!found) exception(ERR_SYM_NOT_FOUND);
    clear_trap(&trap);
    return 0;
}

static void enumerate_local_vars(ObjectInfo * obj, int level,
                                 EnumerateSymbolsCallBack * call_back, void * args) {
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_global_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
        case TAG_lexical_block:
            {
                U8_T LowPC, HighPC;
                if (get_num_prop(obj, AT_low_pc, &LowPC) && get_num_prop(obj, AT_high_pc, &HighPC)) {
                    if (LowPC <= sym_ip && HighPC > sym_ip) {
                        enumerate_local_vars(obj->mChildren, level + 1, call_back, args);
                    }
                }
            }
            break;
        case TAG_formal_parameter:
        case TAG_local_variable:
        case TAG_variable:
            if (level > 0 && obj->mName != NULL) {
                Symbol * sym = NULL;
                object2symbol(obj, &sym);
                call_back(args, sym);
            }
            break;
        }
        obj = obj->mSibling;
    }
}

int enumerate_symbols(Context * ctx, int frame, EnumerateSymbolsCallBack * call_back, void * args) {
    Trap trap;
    if (!set_trap(&trap)) return -1;
    if (frame == STACK_TOP_FRAME && (frame = get_top_frame(ctx)) < 0) exception(errno);
    if (get_sym_context(ctx, frame) < 0) exception(errno);
    if (sym_ip != 0) {
        UnitAddressRange * range = elf_find_unit(sym_ctx, sym_ip, sym_ip + 1, NULL);
        if (range != NULL) enumerate_local_vars(range->mUnit->mObject->mChildren, 0, call_back, args);
    }
    clear_trap(&trap);
    return 0;
}

const char * symbol2id(const Symbol * sym) {
    static char id[256];

    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        char base[256];
        assert(sym->ctx == sym->base->ctx);
        assert(sym->sym_class == SYM_CLASS_TYPE);
        strcpy(base, symbol2id(sym->base));
        snprintf(id, sizeof(id), "PTR%"PRIX64".%s", (uint64_t)sym->size, base);
    }
    else {
        ELF_File * file = NULL;
        uint64_t obj_index = 0;
        unsigned tbl_index = 0;
        int frame = sym->frame;
        if (sym->obj != NULL) file = sym->obj->mCompUnit->mFile;
        if (sym->tbl != NULL) file = sym->tbl->mFile;
        if (sym->obj != NULL) obj_index = sym->obj->mID;
        if (sym->tbl != NULL) tbl_index = sym->tbl->mIndex + 1;
        if (frame == STACK_TOP_FRAME) frame = get_top_frame(sym->ctx);
        snprintf(id, sizeof(id), "SYM%X.%lX.%lX.%"PRIX64".%"PRIX64".%X.%d.%X.%X.%"PRIX64".%s",
            sym->sym_class,
            file ? (unsigned long)file->dev : 0ul,
            file ? (unsigned long)file->ino : 0ul,
            file ? file->mtime : (int64_t)0,
            obj_index, tbl_index,
            frame, sym->index,
            sym->dimension, (uint64_t)sym->size,
            sym->ctx->id);
    }
    return id;
}

static uint64_t read_hex(const char ** s) {
    uint64_t res = 0;
    const char * p = *s;
    for (;;) {
        if (*p >= '0' && *p <= '9') res = (res << 4) | (*p - '0');
        else if (*p >= 'A' && *p <= 'F') res = (res << 4) | (*p - 'A' + 10);
        else break;
        p++;
    }
    *s = p;
    return res;
}

static int read_int(const char ** s) {
    int neg = 0;
    int res = 0;
    const char * p = *s;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    for (;;) {
        if (*p >= '0' && *p <= '9') res = res * 10 + (*p - '0');
        else break;
        p++;
    }
    *s = p;
    return neg ? -res : res;
}

int id2symbol(const char * id, Symbol ** res) {
    Symbol * sym = alloc_symbol();
    dev_t dev = 0;
    ino_t ino = 0;
    int64_t mtime;
    uint64_t obj_index = 0;
    unsigned tbl_index = 0;
    ELF_File * file = NULL;
    const char * p;
    Trap trap;

    *res = sym;
    if (id != NULL && id[0] == 'P' && id[1] == 'T' && id[2] == 'R') {
        p = id + 3;
        sym->size = (ContextAddress)read_hex(&p);
        if (*p == '.') p++;
        if (id2symbol(p, &sym->base)) return -1;
        sym->ctx = sym->base->ctx;
        sym->sym_class = SYM_CLASS_TYPE;
        return 0;
    }
    else if (id != NULL && id[0] == 'S' && id[1] == 'Y' && id[2] == 'M') {
        p = id + 3;
        sym->sym_class = (int)read_hex(&p);
        if (*p == '.') p++;
        dev = (dev_t)read_hex(&p);
        if (*p == '.') p++;
        ino = (ino_t)read_hex(&p);
        if (*p == '.') p++;
        mtime = (int64_t)read_hex(&p);
        if (*p == '.') p++;
        obj_index = read_hex(&p);
        if (*p == '.') p++;
        tbl_index = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->frame = read_int(&p);
        if (*p == '.') p++;
        sym->index = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->dimension = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->size = (ContextAddress)read_hex(&p);
        if (*p == '.') p++;
        sym->ctx = id2ctx(p);
        if (sym->ctx == NULL) {
            errno = ERR_INV_CONTEXT;
            return -1;
        }
        if (get_sym_context(sym->ctx, sym->frame) < 0) return -1;
        if (dev == 0 && ino == 0 && mtime == 0) return 0;
        file = elf_open_inode(sym_ctx, dev, ino, mtime);
        if (file == NULL) return -1;
        if (set_trap(&trap)) {
            DWARFCache * cache = get_dwarf_cache(file);
            if (obj_index) {
                sym->obj = find_object(cache, obj_index);
                if (sym->obj == NULL) exception(ERR_INV_CONTEXT);
            }
            if (tbl_index) {
                if (tbl_index > cache->mSymSectionsCnt) exception(ERR_INV_CONTEXT);
                sym->tbl = cache->mSymSections[tbl_index - 1];
            }
            clear_trap(&trap);
            return 0;
        }
    }
    else {
        errno = ERR_INV_CONTEXT;
    }
    return -1;
}

ContextAddress is_plt_section(Context * ctx, ContextAddress addr) {
    ContextAddress res = 0;
    ELF_File * file = elf_list_first(ctx, addr, addr);
    while (file != NULL) {
        unsigned idx;
        for (idx = 1; idx < file->section_cnt; idx++) {
            ELF_Section * sec = file->sections + idx;
            if (sec->name == NULL) continue;
            if (strcmp(sec->name, ".plt") != 0) continue;
            if (addr >= sec->addr && addr < sec->addr + sec->size) {
                res = (ContextAddress)sec->addr;
                break;
            }
        }
        if (res != 0) break;
        file = elf_list_next(ctx);
    }
    elf_list_done(ctx);
    return res;
}

int get_stack_tracing_info(Context * ctx, ContextAddress addr, StackTracingInfo ** info) {
    /* TODO: no debug info exists for linux-gate.so, need to read stack tracing information from the kernel  */
    /* TODO: support for separate debug info files */
    ELF_File * file = elf_list_first(ctx, addr, addr);
    int error = 0;

    *info = NULL;

    while (error == 0 && file != NULL) {
        Trap trap;
        if (set_trap(&trap)) {
            get_dwarf_stack_frame_info(ctx, file, addr);
            if (dwarf_stack_trace_fp->cmds_cnt > 0) {
                static StackTracingInfo buf;
                buf.addr = (ContextAddress)dwarf_stack_trace_addr;
                buf.size = (ContextAddress)dwarf_stack_trace_size;
                buf.fp = dwarf_stack_trace_fp;
                buf.regs = dwarf_stack_trace_regs;
                buf.reg_cnt = dwarf_stack_trace_regs_cnt;
                *info = &buf;
            }
            clear_trap(&trap);
        }
        else {
            error = trap.error;
        }
        if (error || *info != NULL) break;
        file = elf_list_next(ctx);
        if (file == NULL) error = errno;
    }
    elf_list_done(ctx);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int get_next_stack_frame(StackFrame * frame, StackFrame * down) {
    int error = 0;
    uint64_t ip = 0;
    Context * ctx = frame->ctx;
    StackTracingInfo * info = NULL;

    if (read_reg_value(frame, get_PC_definition(ctx), &ip) < 0) {
        if (frame->is_top_frame) error = errno;
    }
    else if (get_stack_tracing_info(ctx, ip, &info) < 0) {
        error = errno;
    }
    else if (info != NULL) {
        Trap trap;
        if (set_trap(&trap)) {
            int i;
            frame->fp = (ContextAddress)evaluate_stack_trace_commands(ctx, frame, info->fp);
            for (i = 0; i < info->reg_cnt; i++) {
                uint64_t v = evaluate_stack_trace_commands(ctx, frame, info->regs[i]);
                if (write_reg_value(down, info->regs[i]->reg, v) < 0) exception(errno);
            }
            clear_trap(&trap);
        }
        else {
            frame->fp = 0;
        }
    }
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

void ini_symbols_lib(void) {
}

/*************** Functions for retrieving symbol properties ***************************************/

static ELF_File * file;
static DWARFCache * cache;
static ObjectInfo * obj;
static SymbolSection * tbl;
static unsigned sym_index;
static unsigned dimension;
static SymbolInfo * sym_info;

static int unpack(const Symbol * sym) {
    assert(sym->base == NULL);
    assert(sym->size == 0);
    if (get_sym_context(sym->ctx, sym->frame) < 0) return -1;
    file = NULL;
    cache = NULL;
    obj = sym->obj;
    tbl = sym->tbl;
    sym_index = sym->index;
    dimension = sym->dimension;
    sym_info = NULL;
    if (obj != NULL) file = obj->mCompUnit->mFile;
    if (tbl != NULL) file = tbl->mFile;
    if (file != NULL) {
        cache = (DWARFCache *)file->dwarf_dt_cache;
        if (cache == NULL || cache->magic != DWARF_CACHE_MAGIC) {
            errno = ERR_INV_CONTEXT;
            return -1;
        }
        if (tbl != NULL) {
            static SymbolInfo info;
            unpack_elf_symbol_info(tbl, sym_index, &info);
            sym_info = &info;
        }
    }
    return 0;
}

static ObjectInfo * get_object_type(ObjectInfo * obj) {
    if (obj != NULL) {
        switch (obj->mTag) {
        case TAG_global_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
        case TAG_enumerator:
        case TAG_formal_parameter:
        case TAG_global_variable:
        case TAG_local_variable:
        case TAG_variable:
        case TAG_member:
        case TAG_constant:
            obj = obj->mType;
            break;
        }
    }
    return obj;
}

static int is_modified_type(ObjectInfo * obj) {
    if (obj != NULL && obj->mType != NULL) {
        switch (obj->mTag) {
        case TAG_subrange_type:
        case TAG_packed_type:
        case TAG_const_type:
        case TAG_volatile_type:
        case TAG_restrict_type:
        case TAG_shared_type:
        case TAG_typedef:
            return 1;
        }
    }
    return 0;
}

static ObjectInfo * get_original_type(ObjectInfo * obj) {
    obj = get_object_type(obj);
    while (is_modified_type(obj)) {
        obj = obj->mType;
    }
    return obj;
}

static U8_T get_object_length(ObjectInfo * obj) {
    U8_T x, y;

    if (get_num_prop(obj, AT_count, &x)) return x;
    if (get_num_prop(obj, AT_upper_bound, &x)) {
        if (get_num_prop(obj, AT_lower_bound, &y)) return x + 1 - y;
        return x + 1;
    }
    if (obj->mTag == TAG_enumeration_type) {
        ObjectInfo * c = obj->mChildren;
        x = 0;
        while (c != NULL) {
            x++;
            c = c->mSibling;
        }
        return x;
    }
    return 0;
}

int get_symbol_class(const Symbol * sym, int * sym_class) {
    assert(sym->magic == SYMBOL_MAGIC);
    *sym_class = sym->sym_class;
    return 0;
}

int get_symbol_type(const Symbol * sym, Symbol ** type) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base || sym->size) {
        *type = (Symbol *)sym;
        return 0;
    }
    if (sym->sym_class == SYM_CLASS_FUNCTION) {
        *type = alloc_symbol();
        (*type)->ctx = sym->ctx;
        (*type)->sym_class = SYM_CLASS_TYPE;
        (*type)->base = (Symbol *)sym;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    obj = sym->sym_class == SYM_CLASS_TYPE ?
        get_original_type(obj) : get_object_type(obj);
    if (obj == NULL) {
        *type = NULL;
    }
    else if (obj == sym->obj) {
        *type = (Symbol *)sym;
    }
    else {
        object2symbol(obj, type);
    }
    return 0;
}

int get_symbol_type_class(const Symbol * sym, int * type_class) {
    U8_T x;
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) *type_class = TYPE_CLASS_FUNCTION;
        else if (sym->size > 0) *type_class = TYPE_CLASS_ARRAY;
        else *type_class = TYPE_CLASS_POINTER;
        return 0;
    }
    if (sym->size) {
        *type_class = TYPE_CLASS_CARDINAL;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_global_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
        case TAG_subroutine_type:
            *type_class = TYPE_CLASS_FUNCTION;
            return 0;
        case TAG_array_type:
        case TAG_string_type:
            *type_class = TYPE_CLASS_ARRAY;
            return 0;
        case TAG_enumeration_type:
        case TAG_enumerator:
            *type_class = TYPE_CLASS_ENUMERATION;
            return 0;
        case TAG_pointer_type:
        case TAG_reference_type:
            *type_class = TYPE_CLASS_POINTER;
            return 0;
        case TAG_class_type:
        case TAG_structure_type:
        case TAG_union_type:
        case TAG_interface_type:
            *type_class = TYPE_CLASS_COMPOSITE;
            return 0;
        case TAG_base_type:
            if (get_num_prop(obj, AT_encoding, &x)) {
                switch ((int)x) {
                case ATE_address:
                    *type_class = TYPE_CLASS_POINTER;
                    return 0;
                case ATE_boolean:
                    *type_class = TYPE_CLASS_INTEGER;
                    return 0;
                case ATE_float:
                    *type_class = TYPE_CLASS_REAL;
                    return 0;
                case ATE_signed:
                case ATE_signed_char:
                    *type_class = TYPE_CLASS_INTEGER;
                    return 0;
                case ATE_unsigned:
                case ATE_unsigned_char:
                    *type_class = TYPE_CLASS_CARDINAL;
                    return 0;
                }
            }
            *type_class = TYPE_CLASS_UNKNOWN;
            return 0;
        case TAG_fund_type:
            switch (obj->mFundType) {
            case FT_boolean:
                *type_class = TYPE_CLASS_INTEGER;
                return 0;
            case FT_char:
                *type_class = TYPE_CLASS_INTEGER;
                return 0;
            case FT_dbl_prec_float:
            case FT_ext_prec_float:
            case FT_float:
                *type_class = TYPE_CLASS_REAL;
                return 0;
            case FT_signed_char:
            case FT_signed_integer:
            case FT_signed_long:
            case FT_signed_short:
            case FT_short:
            case FT_integer:
            case FT_long:
                *type_class = TYPE_CLASS_INTEGER;
                return 0;
            case FT_unsigned_char:
            case FT_unsigned_integer:
            case FT_unsigned_long:
            case FT_unsigned_short:
                *type_class = TYPE_CLASS_CARDINAL;
                return 0;
            case FT_pointer:
                *type_class = TYPE_CLASS_POINTER;
                return 0;
            case FT_void:
                *type_class = TYPE_CLASS_CARDINAL;
                return 0;
            case FT_label:
            case FT_complex:
            case FT_dbl_prec_complex:
            case FT_ext_prec_complex:
                break;
            }
            *type_class = TYPE_CLASS_UNKNOWN;
            return 0;
        case TAG_subrange_type:
        case TAG_packed_type:
        case TAG_volatile_type:
        case TAG_restrict_type:
        case TAG_shared_type:
        case TAG_const_type:
        case TAG_typedef:
        case TAG_formal_parameter:
        case TAG_global_variable:
        case TAG_local_variable:
        case TAG_variable:
        case TAG_member:
        case TAG_constant:
            obj = obj->mType;
            break;
        default:
            obj = NULL;
            break;
        }
    }
    if (sym_info != NULL && sym_info->mType == STT_FUNC) {
        *type_class = TYPE_CLASS_FUNCTION;
        return 0;
    }
    *type_class = TYPE_CLASS_UNKNOWN;
    return 0;
}

int get_symbol_update_policy(const Symbol * sym, char ** id, int * policy) {
    assert(sym->magic == SYMBOL_MAGIC);
    *id = sym->ctx->id;
    *policy = context_has_state(sym->ctx) ? UPDATE_ON_EXE_STATE_CHANGES : UPDATE_ON_MEMORY_MAP_CHANGES;
    return 0;
}

int get_symbol_name(const Symbol * sym, char ** name) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base || sym->size) {
        *name = NULL;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    if (obj != NULL) {
        *name = obj->mName;
    }
    else if (sym_info != NULL) {
        *name = sym_info->mName;
    }
    else {
        *name = NULL;
    }
    return 0;
}

int get_symbol_size(const Symbol * sym, ContextAddress * size) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->size > 0) {
            if (get_symbol_size(sym->base, size)) return -1;
            *size *= sym->size;
        }
        else {
            Symbol * base = sym->base;
            while (base->obj == NULL && base->base != NULL) base = base->base;
            if (base->obj != NULL) *size = base->obj->mCompUnit->mDesc.mAddressSize;
            else *size = context_word_size(sym->ctx);
        }
        return 0;
    }
    if (sym->size) {
        *size = sym->size;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    *size = 0;
    if (obj != NULL) {
        Trap trap;
        int ok = 0;
        U8_T sz = 0;

        if (!set_trap(&trap)) return -1;
        if (dimension == 0) ok = get_num_prop(obj, AT_byte_size, &sz);
        if (!ok && sym->sym_class == SYM_CLASS_FUNCTION) {
            U8_T l, h;
            ok = get_num_prop(obj, AT_low_pc, &l) && get_num_prop(obj, AT_high_pc, &h);
            if (ok) sz = h - l;
        }
        else {
            if (!ok && sym->sym_class == SYM_CLASS_REFERENCE && obj->mType != NULL) {
                obj = obj->mType;
                if (dimension == 0) ok = get_num_prop(obj, AT_byte_size, &sz);
            }
            while (!ok && obj->mType != NULL) {
                if (!is_modified_type(obj) && obj->mTag != TAG_enumeration_type) break;
                obj = obj->mType;
                if (dimension == 0) ok = get_num_prop(obj, AT_byte_size, &sz);
            }
            if (!ok && obj->mTag == TAG_array_type) {
                U8_T length = 1;
                int i = dimension;
                ObjectInfo * idx = obj->mChildren;
                while (i > 0 && idx != NULL) {
                    idx = idx->mSibling;
                    i--;
                }
                if (idx == NULL) exception(ERR_INV_CONTEXT);
                while (idx != NULL) {
                    length *= get_object_length(idx);
                    idx = idx->mSibling;
                }
                if (obj->mType == NULL) exception(ERR_INV_CONTEXT);
                obj = obj->mType;
                ok = get_num_prop(obj, AT_byte_size, &sz);
                while (!ok && obj->mType != NULL) {
                    if (!is_modified_type(obj) && obj->mTag != TAG_enumeration_type) break;
                    obj = obj->mType;
                    ok = get_num_prop(obj, AT_byte_size, &sz);
                }
                if (ok) sz *= length;
            }
            if (!ok && obj->mTag == TAG_pointer_type) {
                sz = obj->mCompUnit->mDesc.mAddressSize;
                ok = sz > 0;
            }
        }
        if (!ok) str_exception(ERR_INV_DWARF, "Object has no size attribute");
        *size = (ContextAddress)sz;
        clear_trap(&trap);
    }
    else if (sym_info != NULL) {
        *size = (ContextAddress)sym_info->mSize;
    }
    return 0;
}

int get_symbol_base_type(const Symbol * sym, Symbol ** base_type) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            if (sym->base->obj != NULL && sym->base->obj->mType != NULL) {
                if (unpack(sym->base) < 0) return -1;
                object2symbol(sym->base->obj->mType, base_type);
            }
            else {
                /* Function return type is 'void' */
                *base_type = alloc_symbol();
                (*base_type)->ctx = sym->ctx;
                (*base_type)->sym_class = SYM_CLASS_TYPE;
            }
            return 0;
        }
        *base_type = sym->base;
        return 0;
    }
    if (sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL) {
        if (obj->mTag == TAG_array_type) {
            int i = dimension;
            ObjectInfo * idx = obj->mChildren;
            while (i > 0 && idx != NULL) {
                idx = idx->mSibling;
                i--;
            }
            if (idx != NULL && idx->mSibling != NULL) {
                *base_type = alloc_symbol();
                **base_type = *sym;
                (*base_type)->dimension++;
                return 0;
            }
        }
        obj = obj->mType;
        if (obj != NULL) {
            object2symbol(obj, base_type);
            return 0;
        }
    }
    errno = ERR_UNSUPPORTED;
    return -1;
}

int get_symbol_index_type(const Symbol * sym, Symbol ** index_type) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            errno = ERR_INV_CONTEXT;
            return -1;
        }
        *index_type = alloc_symbol();
        (*index_type)->ctx = sym->ctx;
        (*index_type)->sym_class = SYM_CLASS_TYPE;
        (*index_type)->size = context_word_size(sym->ctx);
        return 0;
    }
    if (sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL && obj->mTag == TAG_array_type) {
        int i = dimension;
        ObjectInfo * idx = obj->mChildren;
        while (i > 0 && idx != NULL) {
            idx = idx->mSibling;
            i--;
        }
        if (idx != NULL) {
            object2symbol(idx, index_type);
            return 0;
        }
    }
    errno = ERR_UNSUPPORTED;
    return -1;
}

int get_symbol_length(const Symbol * sym, ContextAddress * length) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            errno = ERR_INV_CONTEXT;
            return -1;
        }
        *length = sym->size == 0 ? 1 : sym->size;
        return 0;
    }
    if (sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL && obj->mTag == TAG_array_type) {
        int i = dimension;
        ObjectInfo * idx = obj->mChildren;
        while (i > 0 && idx != NULL) {
            idx = idx->mSibling;
            i--;
        }
        if (idx != NULL) {
            Trap trap;
            if (!set_trap(&trap)) return -1;
            *length = (ContextAddress)get_object_length(idx);
            clear_trap(&trap);
            return 0;
        }
    }
    errno = ERR_UNSUPPORTED;
    return -1;
}

int get_symbol_lower_bound(const Symbol * sym, int64_t * value) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            errno = ERR_INV_CONTEXT;
            return -1;
        }
        *value = 0;
        return 0;
    }
    if (sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL && obj->mTag == TAG_array_type) {
        int i = dimension;
        ObjectInfo * idx = obj->mChildren;
        while (i > 0 && idx != NULL) {
            idx = idx->mSibling;
            i--;
        }
        if (idx != NULL) {
            if (get_num_prop(obj, AT_lower_bound, (U8_T *)value)) return 0;
            if (get_error_code(errno) != ERR_SYM_NOT_FOUND) return -1;
            *value = 0;
            return 0;
        }
    }
    errno = ERR_UNSUPPORTED;
    return -1;
}

int get_symbol_children(const Symbol * sym, Symbol *** children, int * count) {
    int n = 0;
    static Symbol ** buf = NULL;
    static int buf_len = 0;
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION && sym->base->obj == NULL) {
            *children = NULL;
            *count = 0;
            errno = ERR_SYM_NOT_FOUND;
            return -1;
        }
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            ObjectInfo * i = sym->base->obj->mChildren;
            if (unpack(sym->base) < 0) return -1;
            while (i != NULL) {
                if (i->mTag == TAG_formal_parameter) n++;
                i = i->mSibling;
            }
            if (buf_len < n) {
                buf = (Symbol **)loc_realloc(buf, sizeof(Symbol *) * n);
                buf_len = n;
            }
            n = 0;
            i = obj->mChildren;
            while (i != NULL) {
                if (i->mTag == TAG_formal_parameter) {
                    Symbol * x = NULL;
                    Symbol * y = NULL;
                    object2symbol(i, &x);
                    if (get_symbol_type(x, &y) <0) return -1;
                    buf[n++] = y;
                }
                i = i->mSibling;
            }
            *children = buf;
            *count = n;
            return 0;
        }
        *children = NULL;
        *count = 0;
        return 0;
    }
    if (sym->size) {
        *children = NULL;
        *count = 0;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL) {
        ObjectInfo * i = obj->mChildren;
        while (i != NULL) {
            i = i->mSibling;
            n++;
        }
        if (buf_len < n) {
            buf = (Symbol **)loc_realloc(buf, sizeof(Symbol *) * n);
            buf_len = n;
        }
        n = 0;
        i = obj->mChildren;
        while (i != NULL) {
            Symbol * x = NULL;
            object2symbol(i, &x);
            buf[n++] = x;
            i = i->mSibling;
        }
    }
    *children = buf;
    *count = n;
    return 0;
}

int get_symbol_offset(const Symbol * sym, ContextAddress * offset) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base || sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (unpack(sym) < 0) return -1;
    if (obj != NULL && obj->mTag == TAG_member) {
        U8_T v;
        if (!get_num_prop(obj, AT_data_member_location, &v)) return -1;
        *offset = (ContextAddress)v;
        return 0;
    }
    errno = ERR_INV_CONTEXT;
    return -1;
}

int get_symbol_value(const Symbol * sym, void ** value, size_t * size) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base || sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (unpack(sym) < 0) return -1;
    if (obj != NULL) {
        PropertyValue v;
        Trap trap;
        if (set_trap(&trap)) {
            read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, 0, obj, AT_const_value, &v);
            if (v.mAddr != NULL) {
                *size = v.mSize;
                *value = v.mAddr;
            }
            else {
                static U1_T bf[sizeof(v.mValue)];
                U8_T n = v.mValue;
                size_t i = 0;
                if (v.mAccessFunc != NULL) exception(ERR_INV_CONTEXT);
                for (i = 0; i < sizeof(bf); i++) {
                    bf[v.mBigEndian ? sizeof(bf) - i - 1 : i] = n & 0xffu;
                    n = n >> 8;
                }
                *size = sizeof(bf);
                *value = bf;
            }
            clear_trap(&trap);
            return 0;
        }
        else if (trap.error != ERR_SYM_NOT_FOUND) {
            return -1;
        }
        if (set_trap(&trap)) {
            read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, 0, obj, AT_location, &v);
            if (v.mAccessFunc == NULL) {
                exception(ERR_INV_CONTEXT);
            }
            else {
                static U1_T bf[sizeof(v.mValue)];
                U8_T n = 0;
                size_t i = 0;
                if (v.mAccessFunc(&v, 0, &n)) exception(errno);
                for (i = 0; i < sizeof(bf); i++) {
                    bf[v.mBigEndian ? sizeof(bf) - i - 1 : i] = n & 0xffu;
                    n = n >> 8;
                }
                *size = sizeof(bf);
                *value = bf;
            }
            clear_trap(&trap);
            return 0;
        }
        else if (trap.error != ERR_SYM_NOT_FOUND) {
            return -1;
        }
    }
    errno = ERR_INV_CONTEXT;
    return -1;
}

int get_symbol_address(const Symbol * sym, ContextAddress * address) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base || sym->size) {
        errno = ERR_INV_CONTEXT;
        return -1;
    }
    if (sym->address != 0) {
        *address = sym->address;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    if (obj != NULL && obj->mTag != TAG_member) {
        U8_T v;
        if (get_num_prop(obj, AT_location, &v)) {
            *address = (ContextAddress)v;
            return 0;
        }
        if (get_error_code(errno) != ERR_SYM_NOT_FOUND) return -1;
        if (get_num_prop(obj, AT_low_pc, &v)) {
            *address = (ContextAddress)v;
            return 0;
        }
        if (get_error_code(errno) != ERR_SYM_NOT_FOUND) return -1;
    }
    if (sym_info != NULL) {
        if (syminfo2address(sym_ctx, sym_info, address) == 0) return 0;
    }

    errno = ERR_INV_CONTEXT;
    return -1;
}

int get_array_symbol(const Symbol * sym, ContextAddress length, Symbol ** ptr) {
    assert(sym->magic == SYMBOL_MAGIC);
    *ptr = alloc_symbol();
    (*ptr)->ctx = sym->ctx;
    (*ptr)->sym_class = SYM_CLASS_TYPE;
    (*ptr)->base = (Symbol *)sym;
    (*ptr)->size = length;
    return 0;
}

#endif /* SERVICE_Symbols && ENABLE_ELF */
