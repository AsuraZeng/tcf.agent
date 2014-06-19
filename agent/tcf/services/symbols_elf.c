/*******************************************************************************
 * Copyright (c) 2007, 2014 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Symbols service - ELF version.
 */

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <tcf/config.h>

#if SERVICE_Symbols && (!ENABLE_SymbolsProxy || ENABLE_SymbolsMux) && ENABLE_ELF

#if defined(_WRS_KERNEL)
#  include <symLib.h>
#  include <sysSymTbl.h>
#endif
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/exceptions.h>
#include <tcf/services/tcf_elf.h>
#include <tcf/services/dwarf.h>
#include <tcf/services/dwarfcache.h>
#include <tcf/services/dwarfexpr.h>
#include <tcf/services/dwarfecomp.h>
#include <tcf/services/dwarfframe.h>
#include <tcf/services/stacktrace.h>
#include <tcf/services/memorymap.h>
#include <tcf/services/funccall.h>
#include <tcf/services/pathmap.h>
#include <tcf/services/symbols.h>
#include <tcf/services/elf-symbols.h>
#include <tcf/services/vm.h>
#if ENABLE_SymbolsMux
#define SYM_READER_PREFIX elf_reader_
#include <tcf/services/symbols_mux.h>
#endif

struct Symbol {
#if ENABLE_SymbolsMux
    SymbolReader * reader;
#endif
    unsigned magic;
    ObjectInfo * obj;
    ObjectInfo * var; /* 'this' object if the symbol represents implicit 'this' reference */
    ELF_Section * tbl;
    int has_address;
    ContextAddress address;
    int sym_class;
    Context * ctx;
    int frame;
    unsigned index;
    unsigned dimension;
    unsigned cardinal;
    ContextAddress length;
    Symbol * base;
    /* Volatile fields, used for sorting */
    Symbol * next;
    unsigned level;
    unsigned pos;
    unsigned dup;
};

#define is_array_type_pseudo_symbol(s) (s->sym_class == SYM_CLASS_TYPE && s->obj == NULL && s->base != NULL)
#define is_int_type_pseudo_symbol(s) (s->sym_class == SYM_CLASS_TYPE && s->obj == NULL && s->base == NULL)
#define is_constant_pseudo_symbol(s) (s->sym_class == SYM_CLASS_VALUE && s->obj == NULL && s->base != NULL)

static Context * sym_ctx;
static int sym_frame;
static ContextAddress sym_ip;
static Symbol * find_symbol_list = NULL;

typedef struct {
    ELF_File * file;
    ELF_Section * section;
    ContextAddress lt_addr;
    ContextAddress rt_addr;
    CompUnit * unit;
} UnitAddress;

typedef long ConstantValueType;

static struct ConstantPseudoSymbol {
    const char * name;
    const char * type;
    ConstantValueType value;
} constant_pseudo_symbols[] = {
    { "false", "bool", 0 },
    { "true", "bool", 1 },
    { NULL },
};

static struct TypePseudoSymbol {
    const char * name;
    unsigned size;
    unsigned sign;
} type_pseudo_symbols[] = {
    { "uint8_t",  1, 0 },
    { "uint16_t", 2, 0 },
    { "uint32_t", 4, 0 },
    { "uint64_t", 8, 0 },
    { "int8_t",   1, 1 },
    { "int16_t",  2, 1 },
    { "int32_t",  4, 1 },
    { "int64_t",  8, 1 },
    { "char16_t", 2, 0 },
    { "char32_t", 4, 0 },
    { NULL },
};

static struct BaseTypeAlias {
    const char * name;
    const char * alias;
} base_types_aliases[] = {
    { "int", "signed int" },
    { "signed", "int" },
    { "signed int", "int" },
    { "unsigned", "unsigned int" },
    { "short", "short int" },
    { "signed short", "short int" },
    { "signed short int", "short int" },
    { "unsigned short", "unsigned short int" },
    { "long", "long int" },
    { "signed long", "long int" },
    { "signed long int", "long int" },
    { "unsigned long", "unsigned long int" },
    { "unsigned long", "long unsigned int" },
    { "long long", "long long int" },
    { "signed long long", "long long int" },
    { "signed long long int", "long long int" },
    { "unsigned long long", "unsigned long long int" },
    { "unsigned long long", "long long unsigned int" },
    { "char", "signed char" },
    { "char", "unsigned char" },
    { NULL, NULL }
};

#define SYMBOL_MAGIC 0x34875234

#define equ_symbol_names(x, y) (*x == *y && cmp_symbol_names(x, y) == 0)

/* This function is used for DWARF reader testing */
extern ObjectInfo * get_symbol_object(Symbol * sym);
ObjectInfo * get_symbol_object(Symbol * sym) {
    return sym->obj;
}

int elf_save_symbols_state(ELFSymbolsRecursiveCall * func, void * args) {
    Context * org_ctx = sym_ctx;
    int org_frame = sym_frame;
    ContextAddress org_ip = sym_ip;
    Symbol * org_symbol_list = find_symbol_list;
    Trap trap;

    if (set_trap(&trap)) {
        func(args);
        clear_trap(&trap);
    }

    find_symbol_list = org_symbol_list;
    sym_ctx = org_ctx;
    sym_frame = org_frame;
    sym_ip = org_ip;

    if (!trap.error) return 0;
    errno = trap.error;
    return -1;
}

static Symbol * alloc_symbol(void) {
    Symbol * s = (Symbol *)tmp_alloc_zero(sizeof(Symbol));
#if ENABLE_SymbolsMux
    s->reader = &symbol_reader;
#endif
    s->magic = SYMBOL_MAGIC;
    return s;
}

static int get_sym_context(Context * ctx, int frame, ContextAddress addr) {
    if (frame == STACK_NO_FRAME) {
        sym_ip = addr;
    }
    else if (is_top_frame(ctx, frame)) {
        if (!ctx->stopped) {
            errno = ERR_IS_RUNNING;
            return -1;
        }
        if (ctx->exited) {
            errno = ERR_ALREADY_EXITED;
            return -1;
        }
        sym_ip = get_regs_PC(ctx);
    }
    else {
        U8_T ip = 0;
        StackFrame * info = NULL;
        if (get_frame_info(ctx, frame, &info) < 0) return -1;
        if (read_reg_value(info, get_PC_definition(ctx), &ip) < 0) return -1;
        if (!info->is_top_frame && ip > 0) ip--;
        sym_ip = (ContextAddress)ip;
    }
    sym_ctx = ctx;
    sym_frame = frame;
    return 0;
}

int elf_symbol_address(Context * ctx, ELF_SymbolInfo * info, ContextAddress * address) {
    ELF_File * file = info->sym_section->file;
    ELF_Section * sec = NULL;
    U8_T value = info->value;

#ifdef ELF_SYMS_GET_ADDR
    ELF_SYMS_GET_ADDR;
#endif

    switch (info->type) {
    case STT_NOTYPE:
        /* Check if the NOTYPE symbol is for a section allocated in memory */
        if (info->section == NULL || (info->section->flags & SHF_ALLOC) == 0) break;
        /* fall through */
    case STT_OBJECT:
    case STT_FUNC:
        if (info->section_index == SHN_UNDEF) {
            set_errno(ERR_OTHER, "Cannot get address of ELF symbol: the symbol is undefined");
            return -1;
        }
        if (info->section_index == SHN_ABS) {
            *address = (ContextAddress)value;
            return 0;
        }
        if (info->section_index == SHN_COMMON) {
            set_errno(ERR_OTHER, "Cannot get address of ELF symbol: the symbol is a common block");
            return -1;
        }
        if (info->section != NULL) {
            if (file->type == ET_REL) {
                sec = info->section;
                value += sec->addr;
            }
            if (info->section->size > 0 && info->value == info->section->addr + info->section->size) {
                *address = elf_map_to_run_time_address(ctx, file, sec, (ContextAddress)value - 1);
                if (errno) return -1;
                *address += 1;
                return 0;
            }
        }
        *address = elf_map_to_run_time_address(ctx, file, sec, (ContextAddress)value);
        return errno ? -1 : 0;
    case STT_GNU_IFUNC:
        set_errno(ERR_OTHER, "Cannot get address of ELF symbol: indirect symbol");
        return -1;
    }
    set_errno(ERR_OTHER, "Cannot get address of ELF symbol: wrong symbol type");
    return -1;
}

int elf_tcf_symbol(Context * ctx, ELF_SymbolInfo * sym_info, Symbol ** symbol) {
    Symbol * sym = alloc_symbol();

    sym->frame = STACK_NO_FRAME;
    sym->ctx = context_get_group(ctx, CONTEXT_GROUP_SYMBOLS);
    sym->tbl = sym_info->sym_section;
    sym->index = sym_info->sym_index;

    switch (sym_info->type) {
    case STT_NOTYPE:
        /* Check if the NOTYPE symbol is for a section allocated in memory */
        if (sym_info->section == NULL || (sym_info->section->flags & SHF_ALLOC) == 0) {
            sym->sym_class = SYM_CLASS_VALUE;
            break;
        }
        /* fall through */
    case STT_FUNC:
    case STT_GNU_IFUNC:
        sym->sym_class = SYM_CLASS_FUNCTION;
        break;
    case STT_OBJECT:
        sym->sym_class = SYM_CLASS_REFERENCE;
        break;
    default:
        sym->sym_class = SYM_CLASS_VALUE;
        break;
    }
    *symbol = sym;
    return 0;
}

int elf_symbol_info(Symbol * sym, ELF_SymbolInfo * elf_sym) {
    Trap trap;

    assert (sym != NULL && sym->magic == SYMBOL_MAGIC && sym->tbl != NULL);

    if (!set_trap(&trap)) return -1;

    unpack_elf_symbol_info(sym->tbl, sym->index, elf_sym);

    clear_trap(&trap);
    return 0;
}

static void check_addr_and_size(void * args) {
    Symbol * sym = (Symbol *) args;
    ContextAddress addr = 0;
    ContextAddress size = 0;

    assert(sym->frame == STACK_NO_FRAME);

    if (get_symbol_size(sym, &size) < 0) {
         exception(errno);
    }

    if (sym->sym_class == SYM_CLASS_REFERENCE) {
        if (sym->obj->mTag == TAG_member || sym->obj->mTag == TAG_inheritance) {
            if (get_symbol_offset(sym, &addr) < 0) exception(errno);
        }
        else if (get_symbol_address(sym, &addr) < 0) {
            exception(errno);
        }
    }
}

/* Return 1 if evaluation of symbol properties requires a stack frame.
 * Return 0 otherwise.
 * In case of a doubt, should return 1. */
static int is_frame_based_object(Symbol * sym) {

    if (sym->var != NULL) return 1;
    if (sym->obj != NULL && (sym->obj->mFlags & DOIF_need_frame)) return 1;
    if (sym->obj != NULL && (sym->obj->mFlags & DOIF_pub_mark)) return 0;

    switch (sym->sym_class) {
    case SYM_CLASS_VALUE:
    case SYM_CLASS_BLOCK:
    case SYM_CLASS_NAMESPACE:
    case SYM_CLASS_COMP_UNIT:
    case SYM_CLASS_FUNCTION:
        return 0;
    case SYM_CLASS_TYPE:
        if (sym->obj != NULL) {
            ObjectInfo * obj = sym->obj;
            while (1) {
                switch (obj->mTag) {
                case TAG_typedef:
                case TAG_packed_type:
                case TAG_const_type:
                case TAG_volatile_type:
                case TAG_restrict_type:
                case TAG_shared_type:
                    if (obj->mType == NULL) break;
                    obj = obj->mType;
                    continue;
                case TAG_base_type:
                case TAG_fund_type:
                case TAG_class_type:
                case TAG_union_type:
                case TAG_structure_type:
                case TAG_enumeration_type:
                case TAG_subroutine_type:
                case TAG_pointer_type:
                case TAG_reference_type:
                case TAG_ptr_to_member_type:
                    return 0;
                }
                break;
            }
        }
        break;
    }

    if (sym->obj != NULL) {
        if (elf_save_symbols_state(check_addr_and_size, sym) < 0) return 1;
    }

    return 0;
}

/* Return 1 if evaluation of symbol properties requires a thread.
 * Return 0 otherwise.
 * In case of a doubt, should return 1. */
static int is_thread_based_object(Symbol * sym) {
    /* Variables can be thread local */
    if (sym->sym_class == SYM_CLASS_REFERENCE) return 1;
    return 0;
}

static void object2symbol(ObjectInfo * obj, Symbol ** res) {
    Symbol * sym = alloc_symbol();
    sym->obj = obj;
    switch (obj->mTag) {
    case TAG_global_subroutine:
    case TAG_inlined_subroutine:
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
    case TAG_mod_pointer:
    case TAG_mod_reference:
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
    case TAG_template_type_param:
        sym->sym_class = SYM_CLASS_TYPE;
        break;
    case TAG_global_variable:
    case TAG_inheritance:
    case TAG_member:
    case TAG_formal_parameter:
    case TAG_unspecified_parameters:
    case TAG_local_variable:
    case TAG_variable:
        sym->sym_class = SYM_CLASS_REFERENCE;
        break;
    case TAG_constant:
    case TAG_enumerator:
        sym->sym_class = SYM_CLASS_VALUE;
        break;
    case TAG_compile_unit:
    case TAG_partial_unit:
        sym->sym_class = SYM_CLASS_COMP_UNIT;
        break;
    case TAG_lexical_block:
    case TAG_with_stmt:
    case TAG_try_block:
    case TAG_catch_block:
        sym->sym_class = SYM_CLASS_BLOCK;
        break;
    case TAG_namespace:
        sym->sym_class = SYM_CLASS_NAMESPACE;
        break;
    }
    sym->frame = STACK_NO_FRAME;
    sym->ctx = context_get_group(sym_ctx, CONTEXT_GROUP_SYMBOLS);
    if (sym_frame != STACK_NO_FRAME && is_frame_based_object(sym)) {
        sym->frame = sym_frame;
        sym->ctx = sym_ctx;
    }
    else if (sym_ctx != sym->ctx && is_thread_based_object(sym)) {
        sym->ctx = sym_ctx;
    }
    *res = sym;
}

static ObjectInfo * get_object_type(ObjectInfo * obj) {
    if (obj != NULL) {
        switch (obj->mTag) {
        case TAG_compile_unit:
        case TAG_global_subroutine:
        case TAG_inlined_subroutine:
        case TAG_subroutine:
        case TAG_subprogram:
        case TAG_entry_point:
        case TAG_enumerator:
        case TAG_formal_parameter:
        case TAG_unspecified_parameters:
        case TAG_global_variable:
        case TAG_local_variable:
        case TAG_variable:
        case TAG_inheritance:
        case TAG_member:
        case TAG_constant:
            obj = obj->mType;
            break;
        }
    }
    return obj;
}

static int is_modified_type(ObjectInfo * obj) {
    if (obj != NULL) {
        switch (obj->mTag) {
        case TAG_subrange_type:
        case TAG_packed_type:
        case TAG_const_type:
        case TAG_volatile_type:
        case TAG_restrict_type:
        case TAG_shared_type:
        case TAG_typedef:
        case TAG_template_type_param:
            return 1;
        }
    }
    return 0;
}

static void alloc_int_type_pseudo_symbol(Context * ctx, unsigned size, unsigned sign, Symbol ** type) {
    Symbol * sym = alloc_symbol();
    sym->ctx = context_get_group(ctx, CONTEXT_GROUP_SYMBOLS);
    sym->frame = STACK_NO_FRAME;
    sym->sym_class = SYM_CLASS_TYPE;
    sym->cardinal = size;
    sym->dimension = sign;
    assert(is_int_type_pseudo_symbol(sym));
    *type = sym;
}

/* Get object original type, skipping typedefs and all modifications like const, volatile, etc. */
static ObjectInfo * get_original_type(ObjectInfo * obj) {
    obj = get_object_type(obj);
    while (obj != NULL && obj->mType != NULL && is_modified_type(obj)) obj = obj->mType;
    return obj;
}

static int get_num_prop(ObjectInfo * obj, U2_T at, U8_T * res) {
    Trap trap;
    PropertyValue v;

    if (!set_trap(&trap)) return 0;
    read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, at, &v);
    *res = get_numeric_property_value(&v);
    clear_trap(&trap);
    return 1;
}

/* Check link-time address 'addr' belongs to an object address range(s) */
static int check_in_range(ObjectInfo * obj, UnitAddress * addr) {
    if (obj->mFlags & DOIF_ranges) {
        Trap trap;
        if (set_trap(&trap)) {
            CompUnit * unit = obj->mCompUnit;
            DWARFCache * cache = get_dwarf_cache(unit->mFile);
            ELF_Section * debug_ranges = cache->mDebugRanges;
            if (debug_ranges != NULL) {
                ContextAddress base = unit->mObject->u.mCode.mLowPC;
                int res = 0;

#if 0
                U8_T entry_pc = 0;
                if (obj->mTag == TAG_inlined_subroutine &&
                    get_num_prop(obj, AT_entry_pc, &entry_pc))
                    base = (ContextAddress)entry_pc;
#endif

                dio_EnterSection(&unit->mDesc, debug_ranges, obj->u.mCode.mHighPC.mRanges);
                for (;;) {
                    ELF_Section * x_sec = NULL;
                    ELF_Section * y_sec = NULL;
                    U8_T x = dio_ReadAddress(&x_sec);
                    U8_T y = dio_ReadAddress(&y_sec);
                    if (x == 0 && y == 0) break;
                    if (x == ((U8_T)1 << unit->mDesc.mAddressSize * 8) - 1) {
                        base = (ContextAddress)y;
                    }
                    else {
                        if (x_sec == NULL) x_sec = unit->mTextSection;
                        if (y_sec == NULL) y_sec = unit->mTextSection;
                        if (x_sec == addr->section && y_sec == addr->section) {
                            x = base + x;
                            y = base + y;
                            if (x <= addr->lt_addr && addr->lt_addr < y) {
                                res = 1;
                                break;
                            }
                        }
                    }
                }
                dio_ExitSection();
                clear_trap(&trap);
                return res;
            }
            clear_trap(&trap);
        }
        return 0;
    }

    if (obj->u.mCode.mHighPC.mAddr > obj->u.mCode.mLowPC && obj->u.mCode.mSection == addr->section) {
        return addr->lt_addr >= obj->u.mCode.mLowPC && addr->lt_addr < obj->u.mCode.mHighPC.mAddr;
    }

    return 0;
}

static int cmp_object_profiles(ObjectInfo * x, ObjectInfo * y) {
    if (x == y) return 1;
    while (x != NULL) {
        switch (x->mTag) {
        case TAG_typedef:
        case TAG_const_type:
        case TAG_volatile_type:
            x = x->mType;
            continue;
        }
        break;
    }
    while (y != NULL) {
        switch (y->mTag) {
        case TAG_typedef:
        case TAG_const_type:
        case TAG_volatile_type:
            y = y->mType;
            continue;
        }
        break;
    }
    if (x == NULL || y == NULL) return 0;
    if (x->mTag != y->mTag) return 0;
    if (x->mName != y->mName) {
        if (x->mName == NULL || y->mName == NULL) return 0;
        if (strcmp(x->mName, y->mName) != 0) return 0;
    }
    if (!cmp_object_profiles(x->mType, y->mType)) return 0;
    switch (x->mTag) {
    case TAG_subprogram:
        {
            ObjectInfo * px = get_dwarf_children(x);
            ObjectInfo * py = get_dwarf_children(y);
            for (;;) {
                while (px != NULL) {
                    if (px->mTag == TAG_formal_parameter) break;
                    px = px->mSibling;
                }
                while (py != NULL) {
                    if (py->mTag == TAG_formal_parameter) break;
                    py = py->mSibling;
                }
                if (px == NULL || py == NULL) break;
                if (!cmp_object_profiles(px->mType, py->mType)) return 0;
                px = px->mSibling;
                py = py->mSibling;
            }
            if (x->mName != NULL && x->mName[0] == '~') break;
            if (px != NULL || py != NULL) return 0;
        }
        break;
    }
    return 1;
}

static const char * get_linkage_name(ObjectInfo * obj) {
    Trap trap;
    PropertyValue p;
    if ((obj->mFlags & DOIF_linkage_name) && set_trap(&trap)) {
        read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, AT_MIPS_linkage_name, &p);
        clear_trap(&trap);
        if (p.mAddr != NULL) return (char *)p.mAddr;
    }
    if ((obj->mFlags & DOIF_mangled_name) && set_trap(&trap)) {
        read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, AT_mangled, &p);
        clear_trap(&trap);
        if (p.mAddr != NULL) return (char *)p.mAddr;
    }
    return obj->mName;
}

static int cmp_object_linkage_names(ObjectInfo * x, ObjectInfo * y) {
    const char * xname = get_linkage_name(x);
    const char * yname = get_linkage_name(y);
    if (xname == yname) return 1;
    if (xname == NULL) return 0;
    if (yname == NULL) return 0;
    return strcmp(xname, yname) == 0;
}

static int symbol_priority(ObjectInfo * obj) {
    int p = 0;
    if (obj->mFlags & DOIF_external) p += 2;
    if (obj->mFlags & DOIF_declaration) p -= 4;
    if (obj->mFlags & DOIF_abstract_origin) p += 1;
    switch (obj->mTag) {
    case TAG_class_type:
    case TAG_structure_type:
    case TAG_union_type:
    case TAG_enumeration_type:
        p -= 1;
        break;
    }
    return p;
}

/* return 0 if symbol has no address, e.g. undef or common */
static int has_symbol_address(Symbol * sym) {
    if (sym->has_address) return 1;
    if (sym->tbl != NULL) {
        if (sym->tbl->index == SHN_UNDEF) return 0;
        if (sym->tbl->index == SHN_COMMON) return 0;
        return 1;
    }
    if (sym->obj != NULL) {
        if (sym->obj->mFlags & DOIF_location) {
            /* AT_location defined, so we have an address */
            return 1;
        }
        if (sym->obj->mFlags & DOIF_low_pc) {
            /* AT_low_pc defined, so we have an address */
            return 1;
        }
    }
    return 0;
}

/* Return 1 if find list has no location info: either common or undef */
static int has_symbol_list_no_location_info(void) {
    Symbol * s = find_symbol_list;
    while (s != NULL) {
        if (has_symbol_address(s)) return 0;
        s = s->next;
    }
    /* If we are here, no symbols has location info */
    return 1;
}

static int symbol_equ_comparator(const void * x, const void * y) {
    Symbol * sx = *(Symbol **)x;
    Symbol * sy = *(Symbol **)y;

    if (sx->obj < sy->obj) return -1;
    if (sx->obj > sy->obj) return +1;
    if (sx->var < sy->var) return -1;
    if (sx->var > sy->var) return +1;
    if (sx->tbl < sy->tbl) return -1;
    if (sx->tbl > sy->tbl) return +1;
    if (sx->index < sy->index) return -1;
    if (sx->index > sy->index) return +1;
    return 0;
}

static int symbol_prt_comparator(const void * x, const void * y) {
    Symbol * sx = *(Symbol **)x;
    Symbol * sy = *(Symbol **)y;
    int sx_addr = 0;
    int sy_addr = 0;

    /* symbols with no address have lower priority */

    if (has_symbol_address(sx))
        sx_addr = 1;
    if (has_symbol_address(sy))
        sy_addr = 1;

    if ((sx_addr) && (!sy_addr)) return +1;
    if ((sy_addr) && (!sx_addr)) return -1;

    /* Symbols order by priority, from low to high,
     * most likely match must be last */

    if (sx->level < sy->level) return -1;
    if (sx->level > sy->level) return +1;

    if (sx->obj == NULL && sy->obj != NULL) return -1;
    if (sx->obj != NULL && sy->obj == NULL) return +1;

    if (sx->obj != sy->obj) {
        int px = symbol_priority(sx->obj);
        int py = symbol_priority(sy->obj);
        if (px < py) return -1;
        if (px > py) return +1;
    }

    /* 'this' members have lower priority than local variables */
    if (sx->var == NULL && sy->var != NULL) return +1;
    if (sx->var != NULL && sy->var == NULL) return -1;

    /* First added to the results list has higher priority */
    if (sx->pos < sy->pos) return +1;
    if (sx->pos > sy->pos) return -1;

    return 0;
}

static void add_to_find_symbol_buf(Symbol * sym) {
    sym->next = find_symbol_list;
    find_symbol_list = sym;
}

static void add_obj_to_find_symbol_buf(ObjectInfo * obj, unsigned level) {
    Symbol * sym = NULL;
    object2symbol(obj, &sym);
    add_to_find_symbol_buf(sym);
    sym->level = level;
}

static void add_elf_to_find_symbol_buf(ELF_SymbolInfo * elf_sym) {
    Symbol * sym = NULL;
    elf_tcf_symbol(sym_ctx, elf_sym, &sym);
    add_to_find_symbol_buf(sym);
}

static void sort_find_symbol_buf(void) {
    /* Sort find_symbol_list:
     * 1. inner scope before parent scope
     * 2. DWARF symbols before ELF symbols
     * 3. 'extern' before 'static'
     * 3. definitions before declarations
     * 4. undef after symbols with addresses.
     * 5. etc.
     */
    unsigned cnt = 0;
    unsigned pos = 0;
    Symbol ** buf = NULL;
    Symbol * s = find_symbol_list;
    if (s == NULL) return;
    if (s->next == NULL) return;
    while (s != NULL) {
        s = s->next;
        cnt++;
    }
    pos = 0;
    s = find_symbol_list;
    buf = (Symbol **)tmp_alloc(sizeof(Symbol *) * cnt);
    while (s != NULL) {
        s->dup = 0;
        s->pos = cnt - pos;
        buf[pos++] = s;
        s = s->next;
    }
    find_symbol_list = NULL;
    /* Remove duplicate entries */
    qsort(buf, cnt, sizeof(Symbol *), symbol_equ_comparator);
    for (pos = 1; pos < cnt; pos++) {
        Symbol ** p = buf + pos;
        if (symbol_equ_comparator(p - 1, p)) continue;
        (*p)->dup = 1;
    }
    /* Final sort */
    qsort(buf, cnt, sizeof(Symbol *), symbol_prt_comparator);
    for (pos = 0; pos < cnt; pos++) {
        s = buf[pos];
        if (s->dup) continue;
        s->next = find_symbol_list;
        find_symbol_list = s;
    }
}

/* If 'decl' represents a declaration, replace it with definition - if possible */
static ObjectInfo * find_definition(ObjectInfo * decl) {
    while (decl != NULL) {
        int search_pub_names = 0;
        if (decl->mDefinition != NULL) {
            decl = decl->mDefinition;
            continue;
        }
        if (decl->mName == NULL) return decl;
        if ((decl->mFlags & DOIF_declaration) == 0) return decl;
        switch (decl->mTag) {
        case TAG_structure_type:
        case TAG_interface_type:
        case TAG_union_type:
        case TAG_class_type:
            search_pub_names = 1;
            break;
        default:
            search_pub_names = (decl->mFlags & DOIF_external) != 0;
            break;
        }
        if (search_pub_names) {
            ObjectInfo * def = NULL;
            DWARFCache * cache = get_dwarf_cache(get_dwarf_file(decl->mCompUnit->mFile));
            PubNamesTable * tbl = &cache->mPubNames;
            if (tbl->mHash != NULL) {
                unsigned n = tbl->mHash[calc_symbol_name_hash(decl->mName) % tbl->mHashSize];
                while (n != 0) {
                    ObjectInfo * obj = tbl->mNext[n].mObject;
                    n = tbl->mNext[n].mNext;
                    if (obj->mTag != decl->mTag) continue;
                    if (obj->mFlags & DOIF_declaration) continue;
                    if (obj->mFlags & DOIF_specification) continue;
                    if (!equ_symbol_names(obj->mName, decl->mName)) continue;
                    if (!cmp_object_profiles(decl, obj)) continue;
                    if (!cmp_object_linkage_names(decl, obj)) continue;
                    def = obj;
                    break;
                }
            }
            if (def != NULL) {
                decl->mDefinition = def;
                decl = def;
                continue;
            }
        }
        break;
    }
    return decl;
}

static void find_by_name_in_pub_names(DWARFCache * cache, const char * name) {
    PubNamesTable * tbl = &cache->mPubNames;
    if (tbl->mHash != NULL) {
        unsigned n = tbl->mHash[calc_symbol_name_hash(name) % tbl->mHashSize];
        while (n != 0) {
            ObjectInfo * obj = tbl->mNext[n].mObject;
            if (equ_symbol_names(obj->mName, name)) {
                add_obj_to_find_symbol_buf(obj, 1);
            }
            n = tbl->mNext[n].mNext;
        }
    }
}

static void find_in_object_tree(ObjectInfo * parent, unsigned level,
                                UnitAddress * ip, const char * name) {
    ObjectInfo * children = get_dwarf_children(parent);
    ObjectInfo * obj = NULL;
    ObjectInfo * sym_this = NULL;
    int obj_ptr_chk = 0;
    U8_T obj_ptr_id = 0;

    if (ip) {
        /* Search nested scope */
        obj = children;
        while (obj != NULL) {
            switch (obj->mTag) {
            case TAG_compile_unit:
            case TAG_partial_unit:
            case TAG_module:
            case TAG_global_subroutine:
            case TAG_inlined_subroutine:
            case TAG_lexical_block:
            case TAG_with_stmt:
            case TAG_try_block:
            case TAG_catch_block:
            case TAG_subroutine:
            case TAG_subprogram:
                if (!check_in_range(obj, ip)) break;
                find_in_object_tree(obj, level + 1, ip, name);
                break;
            }
            obj = obj->mSibling;
        }
    }

    /* Search current scope */
    obj = children;
    while (obj != NULL) {
        if (obj->mName != NULL && equ_symbol_names(obj->mName, name)) {
            add_obj_to_find_symbol_buf(find_definition(obj), level);
        }
        if (parent->mTag == TAG_subprogram && ip != 0) {
            if (!obj_ptr_chk) {
                get_num_prop(parent, AT_object_pointer, &obj_ptr_id);
                obj_ptr_chk = 1;
            }
            if (obj->mID == obj_ptr_id || (obj_ptr_id == 0 && obj->mTag == TAG_formal_parameter &&
                (obj->mFlags & DOIF_artificial) && obj->mName != NULL && strcmp(obj->mName, "this") == 0)) {
                sym_this = obj;
            }
        }
        obj = obj->mSibling;
    }

    if (sym_this != NULL) {
        /* Search in 'this' pointer */
        ObjectInfo * type = get_original_type(sym_this);
        if ((type->mTag == TAG_pointer_type || type->mTag == TAG_mod_pointer) && type->mType != NULL) {
            Trap trap;
            Symbol * this_list = NULL;
            Symbol * find_list = find_symbol_list;
            if (set_trap(&trap)) {
                find_symbol_list = NULL;
                type = get_original_type(type->mType);
                find_in_object_tree(type, level, NULL, name);
                sort_find_symbol_buf();
                this_list = find_symbol_list;
                clear_trap(&trap);
            }
            find_symbol_list = find_list;
            while (this_list != NULL) {
                Symbol * s = this_list;
                this_list = this_list->next;
                if (s->obj->mTag != TAG_subprogram) {
                    s->ctx = sym_ctx;
                    s->frame = sym_frame;
                    s->var = sym_this;
                }
                s->next = NULL;
                add_to_find_symbol_buf(s);
            }
        }
    }

    if (parent->mFlags & DOIF_extension) {
        /* If the parent is namespace extension, search in base namespace */
        PropertyValue p;
        ObjectInfo * name_space;
        read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, parent, AT_extension, &p);
        name_space = find_object(get_dwarf_cache(obj->mCompUnit->mFile), (ContextAddress)p.mValue);
        if (name_space != NULL) find_in_object_tree(name_space, level, NULL, name);
    }

    /* Search imported and inherited objects */
    obj = children;
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_enumeration_type:
            find_in_object_tree(obj, level, NULL, name);
            break;
        case TAG_inheritance:
            find_in_object_tree(obj->mType, level, NULL, name);
            break;
        case TAG_imported_declaration:
            if (obj->mName != NULL && equ_symbol_names(obj->mName, name)) {
                PropertyValue p;
                ObjectInfo * decl;
                read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, AT_import, &p);
                decl = find_object(get_dwarf_cache(obj->mCompUnit->mFile), (ContextAddress)p.mValue);
                if (decl != NULL) {
                    if (obj->mName != NULL || (decl->mName != NULL && equ_symbol_names(decl->mName, name))) {
                        add_obj_to_find_symbol_buf(find_definition(decl), level);
                    }
                }
            }
            break;
        case TAG_imported_module:
            find_in_object_tree(obj, level, NULL, name);
            {
                PropertyValue p;
                ObjectInfo * module;
                read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, AT_import, &p);
                module = find_object(get_dwarf_cache(obj->mCompUnit->mFile), (ContextAddress)p.mValue);
                if (module != NULL && (module->mFlags & DOIF_find_mark) == 0) {
                    Trap trap;
                    if (set_trap(&trap)) {
                        module->mFlags |= DOIF_find_mark;
                        find_in_object_tree(module, level, NULL, name);
                        clear_trap(&trap);
                        module->mFlags &= ~DOIF_find_mark;
                    }
                    else {
                        module->mFlags &= ~DOIF_find_mark;
                        exception(trap.error);
                    }
                }
            }
            break;
        }
        obj = obj->mSibling;
    }
}

static void find_unit(Context * ctx, ContextAddress addr, UnitAddress * unit) {
    ContextAddress rt_addr = 0;
    UnitAddressRange * range = elf_find_unit(ctx, addr, addr, &rt_addr);
    memset(unit, 0, sizeof(UnitAddress));
    if (range != NULL) {
        unit->file = range->mUnit->mFile;
        if (range->mSection) unit->section = unit->file->sections + range->mSection;
        unit->lt_addr = addr - rt_addr + range->mAddr;
        unit->rt_addr = addr;
        unit->unit = range->mUnit;
    }
}

static void find_in_dwarf(const char * name) {
    UnitAddress addr;
    find_unit(sym_ctx, sym_ip, &addr);
    if (addr.unit != NULL) {
        CompUnit * unit = addr.unit;
        find_in_object_tree(unit->mObject, 2, &addr, name);
        if (unit->mBaseTypes != NULL) find_in_object_tree(unit->mBaseTypes->mObject, 2, NULL, name);
    }
}

static void find_by_name_in_sym_table(ELF_File * file, const char * name, int globals) {
    unsigned m = 0;
    unsigned h = calc_symbol_name_hash(name);
    Context * prs = context_get_group(sym_ctx, CONTEXT_GROUP_SYMBOLS);

    for (m = 1; m < file->section_cnt; m++) {
        unsigned n;
        ELF_Section * tbl = file->sections + m;
        if (tbl->sym_names_hash == NULL) continue;
        n = tbl->sym_names_hash[h % tbl->sym_names_hash_size];
        while (n) {
            ELF_SymbolInfo sym_info;
            unpack_elf_symbol_info(tbl, n, &sym_info);
            if (equ_symbol_names(name, sym_info.name) && (!globals || sym_info.bind == STB_GLOBAL || sym_info.bind == STB_WEAK)) {
                ContextAddress addr = 0;
                if (sym_info.section_index != SHN_ABS && elf_symbol_address(prs, &sym_info, &addr) == 0) {
                    UnitAddressRange * range = elf_find_unit(sym_ctx, addr, addr, NULL);
                    if (range != NULL) {
                        ObjectInfo * obj = get_dwarf_children(range->mUnit->mObject);
                        while (obj != NULL) {
                            if (obj->mName != NULL && (!globals || (obj->mFlags & DOIF_external) != 0)) {
                                switch (obj->mTag) {
                                case TAG_global_subroutine:
                                case TAG_global_variable:
                                case TAG_subroutine:
                                case TAG_subprogram:
                                case TAG_variable:
                                    if (equ_symbol_names(obj->mName, name)) {
                                        add_obj_to_find_symbol_buf(obj, 0);
                                    }
                                    break;
                                }
                            }
                            obj = obj->mSibling;
                        }
                    }
                }

                add_elf_to_find_symbol_buf(&sym_info);
            }
            n = tbl->sym_names_next[n];
        }
    }
}

int find_symbol_by_name(Context * ctx, int frame, ContextAddress ip, const char * name, Symbol ** res) {
#define CONTINUE_SEARCH (error == 0 && find_symbol_list == NULL) || (has_symbol_list_no_location_info())
    int error = 0;
    ELF_File * curr_file = NULL;

    assert(ctx != NULL);
    find_symbol_list = NULL;
    *res = NULL;

    if (get_sym_context(ctx, frame, ip) < 0) error = errno;

    if (sym_ip != 0) {

        if (error == 0) {
            /* Search the name in the current compilation unit */
            Trap trap;
            if (set_trap(&trap)) {
                find_in_dwarf(name);
                clear_trap(&trap);
            }
            else {
                error = trap.error;
            }
        }

        if (CONTINUE_SEARCH) {
            /* Search in pub names of the current file */
            ELF_File * file = elf_list_first(sym_ctx, sym_ip, sym_ip);
            if (file == NULL) error = errno;
            while (error == 0 && file != NULL) {
                Trap trap;
                curr_file = file;
                if (set_trap(&trap)) {
                    DWARFCache * cache = get_dwarf_cache(get_dwarf_file(file));
                    find_by_name_in_pub_names(cache, name);
                    find_by_name_in_sym_table(file, name, 0);
                    clear_trap(&trap);
                }
                else {
                    error = trap.error;
                    break;
                }
                file = elf_list_next(sym_ctx);
                if (file == NULL) error = errno;
            }
            elf_list_done(sym_ctx);
        }

        if (CONTINUE_SEARCH) {
            /* Check if the name is one of well known C/C++ names */
            Trap trap;
            if (set_trap(&trap)) {
                unsigned i = 0;
                while (base_types_aliases[i].name) {
                    if (strcmp(name, base_types_aliases[i].name) == 0) {
                        find_in_dwarf(base_types_aliases[i].alias);
                        if (find_symbol_list != NULL) break;
                    }
                    i++;
                }
                if ((find_symbol_list == NULL) || (has_symbol_list_no_location_info())) {
                    i = 0;
                    while (constant_pseudo_symbols[i].name) {
                        if (strcmp(name, constant_pseudo_symbols[i].name) == 0) {
                            Trap trap;
                            Symbol * type = NULL;
                            Symbol * list = find_symbol_list;
                            if (set_trap(&trap)) {
                                find_symbol_list = NULL;
                                find_in_dwarf(constant_pseudo_symbols[i].type);
                                sort_find_symbol_buf();
                                type = find_symbol_list;
                                clear_trap(&trap);
                            }
                            find_symbol_list = list;
                            if (type != NULL) {
                                Symbol * sym = alloc_symbol();
                                sym->ctx = context_get_group(ctx, CONTEXT_GROUP_SYMBOLS);
                                sym->frame = STACK_NO_FRAME;
                                sym->sym_class = SYM_CLASS_VALUE;
                                sym->base = type;
                                sym->index = i;
                                assert(is_constant_pseudo_symbol(sym));
                                add_to_find_symbol_buf(sym);
                                break;
                            }
                        }
                        i++;
                    }
                }
                clear_trap(&trap);
            }
            else {
                error = trap.error;
            }
        }
    }

    if (CONTINUE_SEARCH)   {
        /* Search in pub names of all other files */
        ELF_File * file = elf_list_first(sym_ctx, 0, ~(ContextAddress)0);
        if (file == NULL) error = errno;

        while (error == 0 && file != NULL) {
            int no_loc_infos_in_list = 0;

            if (file != curr_file) {
                Trap trap;
                if (set_trap(&trap)) {
                    DWARFCache * cache = get_dwarf_cache(get_dwarf_file(file));
                    find_by_name_in_pub_names(cache, name);
                    find_by_name_in_sym_table(file, name, sym_ip != 0);
                    clear_trap(&trap);
                }
                else {
                    error = trap.error;
                    break;
                }

                no_loc_infos_in_list = has_symbol_list_no_location_info();

                /* If we have no address, continue */
                if (sym_ip != 0 && find_symbol_list != NULL && no_loc_infos_in_list == 0)
                    break;
            }
            file = elf_list_next(sym_ctx);
            if (file == NULL) error = errno;
        }
        elf_list_done(sym_ctx);
    }

    if (CONTINUE_SEARCH) {
        unsigned i = 0;
        while (type_pseudo_symbols[i].name) {
            if (strcmp(name, type_pseudo_symbols[i].name) == 0) {
                Symbol * type = NULL;
                alloc_int_type_pseudo_symbol(
                    context_get_group(ctx, CONTEXT_GROUP_SYMBOLS),
                    type_pseudo_symbols[i].size, type_pseudo_symbols[i].sign, &type);
                type->index = i + 1;
                add_to_find_symbol_buf(type);
                break;
            }
            i++;
        }
    }

#if defined(_WRS_KERNEL)
    if (error == 0 && find_symbol_list == NULL) {
        char * ptr;
        SYM_TYPE type;

        if (symFindByName(sysSymTbl, name, &ptr, &type) != OK) {
            error = errno;
            assert(error != 0);
            if (error == S_symLib_SYMBOL_NOT_FOUND) error = 0;
        }
        else {
            Symbol * sym = alloc_symbol();
            sym->ctx = context_get_group(ctx, CONTEXT_GROUP_SYMBOLS);
            sym->frame = STACK_NO_FRAME;
            sym->address = (ContextAddress)ptr;
            sym->has_address = 1;

            if (SYM_IS_TEXT(type)) {
                sym->sym_class = SYM_CLASS_FUNCTION;
            }
            else {
                sym->sym_class = SYM_CLASS_REFERENCE;
            }
            add_to_find_symbol_buf(sym);
        }
    }
#endif

    if (error == 0 && find_symbol_list == NULL) error = ERR_SYM_NOT_FOUND;

    if (error) {
        find_symbol_list = NULL;
    }
    else {
        sort_find_symbol_buf();
        *res = find_symbol_list;
        find_symbol_list = find_symbol_list->next;
    }

    assert(error || (*res != NULL && (*res)->ctx != NULL));

    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int find_symbol_in_scope(Context * ctx, int frame, ContextAddress ip, Symbol * scope, const char * name, Symbol ** res) {
    int error = 0;

    *res = NULL;
    find_symbol_list = NULL;
    if (get_sym_context(ctx, frame, ip) < 0) error = errno;

    if (!error && scope == NULL && sym_ip != 0) {
        Trap trap;
        if (set_trap(&trap)) {
            ELF_File * file = NULL;
            ELF_Section * sec = NULL;
            ContextAddress addr = elf_map_to_link_time_address(ctx, sym_ip, &file, &sec);
            if (file != NULL) {
                DWARFCache * cache = get_dwarf_cache(get_dwarf_file(file));
                UnitAddressRange * range = find_comp_unit_addr_range(cache, sec, addr, addr);
                if (range != NULL) {
                    find_in_object_tree(range->mUnit->mObject, 2, NULL, name);
                }
                find_by_name_in_sym_table(file, name, 0);
            }
            clear_trap(&trap);
        }
        else {
            error = trap.error;
        }
    }

    if (!error && find_symbol_list == NULL && scope != NULL && scope->obj != NULL) {
        Trap trap;
        if (set_trap(&trap)) {
            find_in_object_tree(scope->obj, 2, NULL, name);
            clear_trap(&trap);
        }
        else {
            error = trap.error;
        }
    }

    if (error == 0 && find_symbol_list == NULL) error = ERR_SYM_NOT_FOUND;

    if (error) {
        find_symbol_list = NULL;
    }
    else {
        sort_find_symbol_buf();
        *res = find_symbol_list;
        find_symbol_list = find_symbol_list->next;
    }

    assert(error || (*res != NULL && (*res)->ctx != NULL));

    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static int find_by_addr_in_unit(ObjectInfo * obj, int level, UnitAddress * addr, UnitAddress * ip, Symbol ** res) {
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_compile_unit:
        case TAG_partial_unit:
        case TAG_module:
        case TAG_global_subroutine:
        case TAG_inlined_subroutine:
        case TAG_lexical_block:
        case TAG_with_stmt:
        case TAG_try_block:
        case TAG_catch_block:
        case TAG_subroutine:
        case TAG_subprogram:
            if (check_in_range(obj, addr)) {
                object2symbol(obj, res);
                return 1;
            }
            if (check_in_range(obj, ip)) {
                return find_by_addr_in_unit(get_dwarf_children(obj), level + 1, addr, ip, res);
            }
            break;
        case TAG_formal_parameter:
        case TAG_unspecified_parameters:
        case TAG_local_variable:
            if (sym_frame == STACK_NO_FRAME) break;
        case TAG_variable:
            {
                U8_T lc = 0;
                /* Ignore location evaluation errors. For example, the error can be caused by
                 * the object not being mapped into the context memory */
                if (get_num_prop(obj, AT_location, &lc) && lc <= addr->rt_addr) {
                    U8_T sz = 0;
                    if (!get_num_prop(obj, AT_byte_size, &sz)) {
                        /* If object size unknown, continue search */
                        if (get_error_code(errno) == ERR_SYM_NOT_FOUND) break;
                        exception(errno);
                    }
                    if (lc + sz > addr->rt_addr) {
                        object2symbol(obj, res);
                        return 1;
                    }
                }
            }
            break;
        }
        obj = obj->mSibling;
    }
    return 0;
}

static int is_valid_elf_symbol(ELF_SymbolInfo * info) {
    switch (info->type) {
    case STT_NOTYPE:
        if (info->name != NULL && info->name[0] == '$') break;
        return 1;
    case STT_FUNC:
    case STT_OBJECT:
        return 1;
    }
    return 0;
}

static int find_by_addr_in_sym_tables(ContextAddress addr, Symbol ** res) {
    ELF_File * file = NULL;
    ELF_Section * section = NULL;
    ELF_SymbolInfo sym_info;
    ContextAddress lt_addr = elf_map_to_link_time_address(sym_ctx, addr, &file, &section);
    if (section == NULL) return 0;
    elf_find_symbol_by_address(section, lt_addr, &sym_info);
    while (sym_info.sym_section != NULL) {
        if (is_valid_elf_symbol(&sym_info)) {
            ContextAddress sym_addr = (ContextAddress)sym_info.value;
            if (file->type == ET_REL) sym_addr += (ContextAddress)section->addr;
            assert(sym_addr <= lt_addr);
            /* Check if the searched address is part of symbol address range
             * or if symbol is a label (function + size of 0). */
            if (sym_addr + sym_info.size > lt_addr || sym_info.size == 0) {
                elf_tcf_symbol(sym_ctx, &sym_info, res);
                return 1;
            }
            return 0;
        }
        elf_prev_symbol_by_address(&sym_info);
    }
    if (section->name != NULL && strcmp(section->name, ".plt") == 0) {
        /* Create synthetic symbol for PLT section entry */
        unsigned first_size = 0;
        unsigned entry_size = 0;
        if (elf_get_plt_entry_size(file, &first_size, &entry_size) == 0 &&
                lt_addr >= section->addr + first_size && entry_size > 0) {
            Symbol * sym = alloc_symbol();
            sym->sym_class = SYM_CLASS_FUNCTION;
            sym->frame = STACK_NO_FRAME;
            sym->ctx = context_get_group(sym_ctx, CONTEXT_GROUP_SYMBOLS);
            sym->tbl = section;
            sym->index = (unsigned)((lt_addr - section->addr - first_size) / entry_size);
            sym->cardinal = first_size;
            sym->dimension = entry_size;
            *res = sym;
            return 1;
        }
    }
    return 0;
}

int find_symbol_by_addr(Context * ctx, int frame, ContextAddress addr, Symbol ** res) {
    Trap trap;
    int found = 0;
    UnitAddress loc;
    UnitAddress ip;

    find_symbol_list = NULL;
    if (!set_trap(&trap)) return -1;
    if (get_sym_context(ctx, frame, addr) < 0) exception(errno);
    find_unit(sym_ctx, addr, &loc);
    if (addr == sym_ip) ip = loc;
    else find_unit(sym_ctx, sym_ip, &ip);
    if (loc.unit != NULL) {
        found = find_by_addr_in_unit(
            get_dwarf_children(loc.unit->mObject),
            0, &loc, ip.unit ? &ip : NULL, res);
    }
    if (!found) found = find_by_addr_in_sym_tables(addr, res);
    if (!found && ip.unit != NULL) {
        /* Search in compilation unit that contains stack frame PC */
        if (loc.file == NULL) {
            loc.lt_addr = elf_map_to_link_time_address(sym_ctx, addr, &loc.file, &loc.section);
        }
        if (loc.file != NULL) {
            found = find_by_addr_in_unit(
                get_dwarf_children(ip.unit->mObject),
                0, &loc, &ip, res);
        }
    }

#ifdef ELF_SYMS_BY_ADDR
    ELF_SYMS_BY_ADDR;
#endif

    if (!found) exception(ERR_SYM_NOT_FOUND);
    clear_trap(&trap);
    return 0;
}

int find_next_symbol(Symbol ** sym) {
    if (find_symbol_list != NULL) {
        *sym = find_symbol_list;
        find_symbol_list = find_symbol_list->next;
        return 0;
    }
    errno = ERR_SYM_NOT_FOUND;
    return -1;
}

typedef struct LocalVarsCallBack {
    EnumerateSymbolsCallBack * call_back;
    void * args;
    Symbol * sym;
} LocalVarsCallBack;

static void local_vars_call_back(void * args) {
    LocalVarsCallBack * cb = (LocalVarsCallBack *)args;
    cb->call_back(cb->args, cb->sym);
}

static void enumerate_local_vars(ObjectInfo * obj, int level,
        UnitAddress * ip, LocalVarsCallBack * call_back) {
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_compile_unit:
        case TAG_partial_unit:
        case TAG_module:
        case TAG_global_subroutine:
        case TAG_inlined_subroutine:
        case TAG_lexical_block:
        case TAG_with_stmt:
        case TAG_try_block:
        case TAG_catch_block:
        case TAG_subroutine:
        case TAG_subprogram:
            if (check_in_range(obj, ip)) {
                enumerate_local_vars(get_dwarf_children(obj), level + 1, ip, call_back);
                if (level == 0) return;
            }
            break;
        case TAG_formal_parameter:
        case TAG_unspecified_parameters:
        case TAG_local_variable:
        case TAG_variable:
            if (level > 0) {
                call_back->sym = NULL;
                object2symbol(find_definition(obj), &call_back->sym);
                if (elf_save_symbols_state(local_vars_call_back, call_back) < 0) exception(errno);
            }
            break;
        }
        obj = obj->mSibling;
    }
}

int enumerate_symbols(Context * ctx, int frame, EnumerateSymbolsCallBack * call_back, void * args) {
    Trap trap;
    if (!set_trap(&trap)) return -1;
    if (get_sym_context(ctx, frame, 0) < 0) exception(errno);
    if (sym_ip != 0) {
        UnitAddress ip;
        find_unit(sym_ctx, sym_ip, &ip);
        if (ip.unit != NULL) {
            LocalVarsCallBack cb;
            memset(&cb, 0, sizeof(LocalVarsCallBack));
            cb.args = args;
            cb.call_back = call_back;
            enumerate_local_vars(get_dwarf_children(ip.unit->mObject), 0, &ip, &cb);
        }
    }
    clear_trap(&trap);
    return 0;
}

static char tmp_buf[256];
static size_t tmp_len = 0;

#define tmp_app_char(ch) { \
    if (tmp_len < sizeof(tmp_buf) - 1) tmp_buf[tmp_len++] = ch; \
}

static void tmp_app_str(char ch, const char * s) {
    tmp_app_char(ch);
    for (;;) {
        ch = *s++;
        if (ch == 0) break;
        tmp_app_char(ch);
    }
}

static void tmp_app_hex(char ch, uint64_t n) {
    char buf[32];
    unsigned i = 0;
    tmp_app_char(ch);
    do {
        int d = (int)(n & 0xf);
        buf[i++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        n = n >> 4;
    }
    while (n != 0);
    while (i > 0) {
        ch = buf[--i];
        tmp_app_char(ch);
    }
}

static void tmp_app_int(char ch, int n) {
    char buf[32];
    unsigned i = 0;
    tmp_app_char(ch);
    if (n < 0) {
        tmp_app_char('-');
        n = -n;
    }
    do {
        buf[i++] = '0' + n % 10;
        n = n / 10;
    }
    while (n != 0);
    while (i > 0) {
        ch = buf[--i];
        tmp_app_char(ch);
    }
}

const char * symbol2id(const Symbol * sym) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base) {
        char base[256];
        assert(sym->ctx == sym->base->ctx);
        assert(sym->frame == STACK_NO_FRAME);
        strlcpy(base, symbol2id(sym->base), sizeof(base));
        tmp_len = 0;
        tmp_app_char('@');
        tmp_app_hex('P', sym->sym_class);
        tmp_app_hex('.', sym->index);
        tmp_app_hex('.', sym->length);
        tmp_app_str('.', base);
    }
    else {
        ELF_File * file = NULL;
        uint64_t obj_index = 0;
        uint64_t var_index = 0;
        unsigned tbl_index = 0;
        int frame = sym->frame;
        if (sym->obj != NULL) file = sym->obj->mCompUnit->mFile;
        if (sym->tbl != NULL) file = sym->tbl->file;
        if (sym->obj != NULL) obj_index = sym->obj->mID;
        if (sym->var != NULL) var_index = sym->var->mID;
        if (sym->tbl != NULL) tbl_index = sym->tbl->index;
        if (frame == STACK_TOP_FRAME) frame = get_top_frame(sym->ctx);
        assert(sym->var == NULL || sym->var->mCompUnit->mFile == file);
        tmp_len = 0;
        tmp_app_char('@');
        tmp_app_hex('S', sym->sym_class);
        tmp_app_hex('.', file ? file->dev : (dev_t)0);
        tmp_app_hex('.', file ? file->ino : (ino_t)0);
        tmp_app_hex('.', file ? file->mtime : (int64_t)0);
        tmp_app_hex('.', obj_index);
        tmp_app_hex('.', var_index);
        tmp_app_hex('.', tbl_index);
        tmp_app_int('.', frame);
        tmp_app_hex('.', sym->index);
        tmp_app_hex('.', sym->dimension);
        tmp_app_hex('.', sym->cardinal);
        tmp_app_str('.', sym->ctx->id);
    }
    tmp_buf[tmp_len++] = 0;
    return tmp_buf;
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
    ContextAddress obj_index = 0;
    ContextAddress var_index = 0;
    unsigned tbl_index = 0;
    ELF_File * file = NULL;
    const char * p;
    Trap trap;

    *res = sym;
    if (id != NULL && id[0] == '@' && id[1] == 'P') {
        p = id + 2;
        sym->sym_class = (int)read_hex(&p);
        if (*p == '.') p++;
        sym->index = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->length = (ContextAddress)read_hex(&p);
        if (*p == '.') p++;
        if (id2symbol(p, &sym->base)) return -1;
        sym->ctx = sym->base->ctx;
        sym->frame = STACK_NO_FRAME;
        return 0;
    }
    else if (id != NULL && id[0] == '@' && id[1] == 'S') {
        p = id + 2;
        sym->sym_class = (int)read_hex(&p);
        if (*p == '.') p++;
        dev = (dev_t)read_hex(&p);
        if (*p == '.') p++;
        ino = (ino_t)read_hex(&p);
        if (*p == '.') p++;
        mtime = (int64_t)read_hex(&p);
        if (*p == '.') p++;
        obj_index = (ContextAddress)read_hex(&p);
        if (*p == '.') p++;
        var_index = (ContextAddress)read_hex(&p);
        if (*p == '.') p++;
        tbl_index = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->frame = read_int(&p);
        if (*p == '.') p++;
        sym->index = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->dimension = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->cardinal = (unsigned)read_hex(&p);
        if (*p == '.') p++;
        sym->ctx = id2ctx(p);
        if (sym->ctx == NULL) {
            errno = ERR_INV_CONTEXT;
            return -1;
        }
        if (dev == 0 && ino == 0 && mtime == 0) return 0;
        file = elf_open_inode(sym->ctx, dev, ino, mtime);
        if (file == NULL) return -1;
        if (set_trap(&trap)) {
            DWARFCache * cache = get_dwarf_cache(file);
            if (obj_index) {
                sym->obj = find_object(cache, obj_index);
                if (sym->obj == NULL) exception(ERR_INV_CONTEXT);
            }
            if (var_index) {
                sym->var = find_object(cache, var_index);
                if (sym->var == NULL) exception(ERR_INV_CONTEXT);
            }
            if (tbl_index) {
                if (tbl_index >= file->section_cnt) exception(ERR_INV_CONTEXT);
                sym->tbl = file->sections + tbl_index;
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
    ELF_File * file = NULL;
    ELF_Section * sec = NULL;
    ContextAddress res = 0;
    errno = 0;
    res = elf_map_to_link_time_address(ctx, addr, &file, &sec);
    if (res == 0 || sec == NULL) return 0;
    if (sec->name == NULL) return 0;
    if (strcmp(sec->name, ".plt") != 0) return 0;
    return (ContextAddress)sec->addr + (addr - res);
}

int get_context_isa(Context * ctx, ContextAddress ip, const char ** isa,
        ContextAddress * range_addr, ContextAddress * range_size) {
    ELF_File * file = NULL;
    ELF_Section * sec = NULL;
    ContextAddress lt_addr = elf_map_to_link_time_address(ctx, ip, &file, &sec);
    *isa = NULL;
    *range_addr = ip;
    *range_size = 1;
    if (sec != NULL && file->machine == EM_ARM) {
        /* TODO: faster handling of ARM mapping symbols */
        ELF_SymbolInfo sym_info;
        elf_find_symbol_by_address(sec, lt_addr, &sym_info);
        while (sym_info.sym_section != NULL) {
            assert(sym_info.section == sec);
            if (sym_info.name != NULL && *sym_info.name == '$') {
                if (strcmp(sym_info.name, "$a") == 0) *isa = "ARM";
                else if (strcmp(sym_info.name, "$t") == 0) *isa = "Thumb";
                else if (strcmp(sym_info.name, "$t.x") == 0) *isa = "ThumbEE";
                else if (strcmp(sym_info.name, "$d") == 0) *isa = "Data";
                else if (strcmp(sym_info.name, "$d.realdata") == 0) *isa = "Data";
                if (*isa) {
                    *range_addr = (ContextAddress)sym_info.value;
                    if (file->type == ET_REL) *range_addr += (ContextAddress)sec->addr;
                    for (;;) {
                        elf_next_symbol_by_address(&sym_info);
                        if (sym_info.sym_section == NULL) {
                            *range_size = (ContextAddress)(sec->addr + sec->size) - *range_addr;
                            return 0;
                        }
                        if (sym_info.name != NULL && *sym_info.name == '$') {
                            ContextAddress sym_addr = (ContextAddress)sym_info.value;
                            if (file->type == ET_REL) sym_addr += (ContextAddress)sec->addr;
                            *range_size = sym_addr - *range_addr;
                            return 0;
                        }
                    }
                }
            }
            elf_prev_symbol_by_address(&sym_info);
        }
    }
    else if (file != NULL) {
        switch (file->machine) {
        case EM_M32        : *isa = "M32"; break;
        case EM_SPARC      : *isa = "SPARC"; break;
        case EM_386        : *isa = "386"; break;
        case EM_68K        : *isa = "68K"; break;
        case EM_88K        : *isa = "88K"; break;
        case EM_860        : *isa = "860"; break;
        case EM_MIPS       : *isa = "MIPS"; break;
        case EM_PPC        : *isa = "PPC"; break;
        case EM_PPC64      : *isa = "PPC64"; break;
        case EM_SH         : *isa = "SH"; break;
        case EM_SPARCV9    : *isa = "SPARCV9"; break;
        case EM_IA_64      : *isa = "IA_64"; break;
        case EM_MIPS_X     : *isa = "MIPS_X"; break;
        case EM_COLDFIRE   : *isa = "COLDFIRE"; break;
        case EM_X86_64     : *isa = "X86_64"; break;
        case EM_MICROBLAZE : *isa = "MicroBlaze"; break;
        case EM_V850       : *isa = "V850"; break;
        }
    }
    {
        unsigned i;
        static MemoryMap map;
        ContextAddress size = 1 << 16;
        ContextAddress addr = ip & ~(size - 1);
        if (elf_get_map(ctx, addr, addr + size - 1, &map) == 0) {
            for (i = 0; i < map.region_cnt; i++) {
                MemoryRegion * r = map.regions + i;
                ContextAddress x = r->addr;
                ContextAddress y = r->addr + r->size;
                if (x > ip && x < addr + size) size = x - addr;
                if (y > ip && y < addr + size) size = y - addr;
                if (x <= ip && x > addr) {
                    size = addr + size - x;
                    addr = x;
                }
                if (y <= ip && y > addr) {
                    size = addr + size - y;
                    addr = y;
                }
            }
            assert(addr <= ip);
            assert(addr + size - 1 >= ip);
            *range_addr = addr;
            *range_size = size;
        }
    }
    return 0;
}

static int buf_sub_max = 0;

static void search_inlined_subroutine(ObjectInfo * obj, UnitAddress * addr, StackTracingInfo * buf) {
    ObjectInfo * o = get_dwarf_children(obj);
    while (o != NULL) {
        switch (o->mTag) {
        case TAG_compile_unit:
        case TAG_partial_unit:
        case TAG_module:
        case TAG_global_subroutine:
        case TAG_lexical_block:
        case TAG_with_stmt:
        case TAG_try_block:
        case TAG_catch_block:
        case TAG_subroutine:
        case TAG_subprogram:
            if (!check_in_range(o, addr)) break;
            search_inlined_subroutine(o, addr, buf);
            break;
        case TAG_inlined_subroutine:
            if (o->mFlags & DOIF_ranges) {
                DWARFCache * cache = get_dwarf_cache(addr->file);
                ELF_Section * debug_ranges = cache->mDebugRanges;
                if (debug_ranges != NULL) {
                    CompUnit * unit = addr->unit;
                    ContextAddress base = unit->mObject->u.mCode.mLowPC;
                    Symbol * sym = NULL;
                    U8_T call_file = 0;
                    CodeArea area;
                    object2symbol(o, &sym);
                    memset(&area, 0, sizeof(area));
                    if (get_num_prop(o, AT_call_file, &call_file)) {
                        U8_T call_line = 0;
                        load_line_numbers(unit);
                        area.directory = unit->mDir;
                        if (call_file < unit->mFilesCnt) {
                            FileInfo * file_info = unit->mFiles + (int)call_file;
                            if (is_absolute_path(file_info->mName) || file_info->mDir == NULL) {
                                area.file = file_info->mName;
                            }
                            else if (is_absolute_path(file_info->mDir)) {
                                area.directory = file_info->mDir;
                                area.file = file_info->mName;
                            }
                            else {
                                char buf[FILE_PATH_SIZE];
                                snprintf(buf, sizeof(buf), "%s/%s", file_info->mDir, file_info->mName);
                                area.file = tmp_strdup(buf);
                            }
                            area.file_mtime = file_info->mModTime;
                            area.file_size = file_info->mSize;
                        }
                        else {
                            area.file = unit->mObject->mName;
                        }
                        if (get_num_prop(o, AT_call_line, &call_line)) {
                            area.start_line = (int)call_line;
                            area.end_line = (int)call_line + 1;
                        }
                    }
                    dio_EnterSection(&unit->mDesc, debug_ranges, o->u.mCode.mHighPC.mRanges);
                    for (;;) {
                        ELF_Section * x_sec = NULL;
                        ELF_Section * y_sec = NULL;
                        U8_T x = dio_ReadAddress(&x_sec);
                        U8_T y = dio_ReadAddress(&y_sec);
                        if (x == 0 && y == 0) break;
                        if (x == ((U8_T)1 << unit->mDesc.mAddressSize * 8) - 1) {
                            base = (ContextAddress)y;
                        }
                        else {
                            if (x_sec == NULL) x_sec = unit->mTextSection;
                            if (y_sec == NULL) y_sec = unit->mTextSection;
                            if (x_sec == addr->section && y_sec == addr->section) {
                                StackFrameInlinedSubroutine * sub = (StackFrameInlinedSubroutine *)tmp_alloc(
                                        sizeof(StackFrameInlinedSubroutine));
                                sub->sym = sym;
                                sub->area = area;
                                sub->area.start_address = base + x;
                                sub->area.end_address = base + y;
                                if (buf->sub_cnt >= buf_sub_max) {
                                    buf_sub_max += 16;
                                    buf->subs = (StackFrameInlinedSubroutine **)tmp_realloc(
                                        buf->subs, sizeof(StackFrameInlinedSubroutine *) * buf_sub_max);
                                }
                                buf->subs[buf->sub_cnt++] = sub;
                            }
                        }
                    }
                    dio_ExitSection();
                }
            }
            break;
        }
        o = o->mSibling;
    }
}

int get_stack_tracing_info(Context * ctx, ContextAddress rt_addr, StackTracingInfo ** info) {
    /* TODO: no debug info exists for linux-gate.so, need to read stack tracing information from the kernel  */
    Trap trap;

    *info = NULL;

    if (set_trap(&trap)) {
        ELF_File * file = NULL;
        ELF_Section * sec = NULL;
        ContextAddress lt_addr = elf_map_to_link_time_address(ctx, rt_addr, &file, &sec);
        if (file != NULL) {
            get_dwarf_stack_frame_info(ctx, file, sec, lt_addr);
            if (dwarf_stack_trace_fp->cmds_cnt > 0) {
                static StackTracingInfo buf;
                memset(&buf, 0, sizeof(buf));
                assert(dwarf_stack_trace_addr <= lt_addr);
                assert(dwarf_stack_trace_addr + dwarf_stack_trace_size > lt_addr);
                buf.addr = (ContextAddress)dwarf_stack_trace_addr - lt_addr + rt_addr;
                buf.size = (ContextAddress)dwarf_stack_trace_size;
                buf.regs = dwarf_stack_trace_regs;
                buf.reg_cnt = dwarf_stack_trace_regs_cnt;
                buf.fp = dwarf_stack_trace_fp;
                if (get_sym_context(ctx, STACK_NO_FRAME, rt_addr) == 0) {
                    /* Search inlined functions info.
                     * Note: when debug info is a separate file,
                     * 'lt_addr' is not same as 'unit.lt_addr' */
                    UnitAddress unit;
                    find_unit(ctx, rt_addr, &unit);
                    if (unit.unit != NULL) {
                        buf_sub_max = 0;
                        search_inlined_subroutine(unit.unit->mObject, &unit, &buf);
                    }
                }
                *info = &buf;
            }
        }
        clear_trap(&trap);
        return 0;
    }
    return -1;
}

const char * get_symbol_file_name(Context * ctx, MemoryRegion * module) {
    int error = 0;
    ELF_File * file = NULL;
    if (module == NULL) {
        errno = 0;
        return NULL;
    }
    file = get_dwarf_file(elf_open_memory_region_file(module, &error));
    errno = error;
    if (file != NULL) return file->name;
    return NULL;
}

#if ENABLE_MemoryMap
static void event_map_changed(Context * ctx, void * args) {
    /* Make sure there is no stale data in the ELF cache */
    elf_invalidate();
}

static MemoryMapEventListener map_listener = {
    event_map_changed,
    NULL,
    NULL,
    event_map_changed,
};
#endif

void ini_symbols_lib(void) {
#if ENABLE_MemoryMap
    add_memory_map_event_listener(&map_listener, NULL);
#endif
#if ENABLE_SymbolsMux
    add_symbols_reader(&symbol_reader);
#endif
}

#if ENABLE_SymbolsMux
static int reader_is_valid(Context * ctx, ContextAddress addr) {
    ELF_File * file = NULL;
    ELF_Section * sec = NULL;
    elf_map_to_link_time_address(ctx, addr, &file, &sec);
    return file != NULL;
}
#endif

/*************** Functions for retrieving symbol properties ***************************************/

static int unpack(const Symbol * sym) {
    assert(sym->base == NULL);
    assert(!is_array_type_pseudo_symbol(sym));
    assert(!is_int_type_pseudo_symbol(sym));
    assert(!is_constant_pseudo_symbol(sym));
    assert(sym->obj == NULL || sym->obj->mTag != 0);
    assert(sym->obj == NULL || sym->obj->mCompUnit->mFile->dwarf_dt_cache != NULL);
    return get_sym_context(sym->ctx, sym->frame, 0);
}

static U8_T get_default_lower_bound(ObjectInfo * obj) {
    switch (obj->mCompUnit->mLanguage) {
    case LANG_ADA83:
    case LANG_ADA95:
    case LANG_COBOL74:
    case LANG_COBOL85:
    case LANG_FORTRAN77:
    case LANG_FORTRAN90:
    case LANG_FORTRAN95:
    case LANG_MODULA2:
    case LANG_PASCAL83:
    case LANG_PLI:
        return 1;
    }
    return 0;
}

static U8_T get_array_index_length(ObjectInfo * obj) {
    U8_T x, y;

    if (get_num_prop(obj, AT_count, &x)) return x;
    if (get_num_prop(obj, AT_upper_bound, &x)) {
        if (!get_num_prop(obj, AT_lower_bound, &y)) {
            y = get_default_lower_bound(obj);
        }
        return x + 1 - y;
    }
    if (obj->mTag == TAG_enumeration_type) {
        ObjectInfo * c = get_dwarf_children(obj);
        x = 0;
        while (c != NULL) {
            x++;
            c = c->mSibling;
        }
        return x;
    }
    return 0;
}

static int map_to_sym_table(ObjectInfo * obj, Symbol ** sym) {
    Trap trap;
    Symbol * list = find_symbol_list;
    if (set_trap(&trap)) {
        *sym = NULL;
        find_symbol_list = NULL;
        if (obj->mFlags & DOIF_external) {
            ELF_File * file = obj->mCompUnit->mFile;
            if (file->debug_info_file) {
                size_t n = strlen(file->name);
                if (n > 6 && strcmp(file->name + n - 6, ".debug") == 0) {
                    char * fnm = (char *)tmp_alloc_zero(n);
                    memcpy(fnm, file->name, n - 6);
                    fnm = canonicalize_file_name(fnm);
                    if (fnm != NULL) {
                        file = elf_open(fnm);
                        free(fnm);
                    }
                }
            }
            if (file != NULL) {
                find_by_name_in_sym_table(file, get_linkage_name(obj), 1);
            }
        }
        while (find_symbol_list != NULL) {
            Symbol * s = find_symbol_list;
            find_symbol_list = find_symbol_list->next;
            if (s->obj != obj) {
                *sym = s;
                break;
            }
        }
        clear_trap(&trap);
    }
    find_symbol_list = list;
    return *sym != NULL;
}

static U8_T read_string_length(ObjectInfo * obj);

static int get_object_size(ObjectInfo * obj, unsigned dimension, U8_T * byte_size, U8_T * bit_size) {
    U8_T n = 0, m = 0;
    obj = find_definition(obj);
    if (obj->mTag != TAG_string_type) {
        if (get_num_prop(obj, AT_byte_size, &n)) {
            *byte_size = n;
            return 1;
        }
        if (get_num_prop(obj, AT_bit_size, &n)) {
            *byte_size = (n + 7) / 8;
            *bit_size = n;
            return 1;
        }
    }
    switch (obj->mTag) {
    case TAG_enumerator:
    case TAG_formal_parameter:
    case TAG_unspecified_parameters:
    case TAG_template_type_param:
    case TAG_global_variable:
    case TAG_local_variable:
    case TAG_variable:
    case TAG_inheritance:
    case TAG_member:
    case TAG_constant:
    case TAG_const_type:
    case TAG_volatile_type:
    case TAG_restrict_type:
    case TAG_shared_type:
    case TAG_typedef:
        if (obj->mType == NULL) return 0;
        return get_object_size(obj->mType, 0, byte_size, bit_size);
    case TAG_compile_unit:
    case TAG_partial_unit:
    case TAG_module:
    case TAG_global_subroutine:
    case TAG_inlined_subroutine:
    case TAG_lexical_block:
    case TAG_with_stmt:
    case TAG_try_block:
    case TAG_catch_block:
    case TAG_subroutine:
    case TAG_subprogram:
        if ((obj->mFlags & DOIF_ranges) == 0 && (obj->mFlags & DOIF_low_pc) != 0 &&
                obj->u.mCode.mHighPC.mAddr >= obj->u.mCode.mLowPC) {
            *byte_size = obj->u.mCode.mHighPC.mAddr - obj->u.mCode.mLowPC;
            return 1;
        }
        return 0;
    case TAG_string_type:
        *byte_size = read_string_length(obj);
        return 1;
    case TAG_array_type:
        {
            unsigned i = 0;
            U8_T length = 1;
            ObjectInfo * idx = get_dwarf_children(obj);
            while (idx != NULL) {
                if (i++ >= dimension) length *= get_array_index_length(idx);
                idx = idx->mSibling;
            }
            if (get_num_prop(obj, AT_stride_size, &n)) {
                *byte_size = (n * length + 7) / 8;
                *bit_size = n * length;
                return 1;
            }
            if (obj->mType == NULL) return 0;
            if (!get_object_size(obj->mType, 0, &n, &m)) return 0;
            if (m != 0) {
                *byte_size = (m * length + 7) / 8;
                *bit_size = m * length;
            }
            *byte_size = n * length;
        }
        return 1;
    }
    return 0;
}

static void read_object_value(PropertyValue * v, void ** value, size_t * size, int * big_endian) {
    if (v->mPieces != NULL) {
        StackFrame * frame = NULL;
        if (get_frame_info(v->mContext, v->mFrame, &frame) < 0) exception(errno);
        read_location_pieces(v->mContext, frame, v->mPieces, v->mPieceCnt, v->mBigEndian, value, size);
        *big_endian = v->mBigEndian;
    }
    else if (v->mAddr != NULL) {
        *value = v->mAddr;
        *size = v->mSize;
        *big_endian = v->mBigEndian;
    }
    else {
        U1_T * bf = NULL;
        U8_T val_size = 0;
        U8_T bit_size = 0;

        if (v->mAttr == AT_string_length) {
            if (!get_num_prop(v->mObject, AT_byte_size, &val_size)) {
                val_size = v->mObject->mCompUnit->mDesc.mAddressSize;
            }
        }
        else if (!get_object_size(v->mObject, 0, &val_size, &bit_size)) {
            str_exception(ERR_INV_DWARF, "Unknown object size");
        }
        bf = (U1_T *)tmp_alloc((size_t)val_size);
        if (v->mForm == FORM_EXPR_VALUE) {
            if (context_read_mem(sym_ctx, (ContextAddress)v->mValue, bf, (size_t)val_size) < 0) exception(errno);
            *big_endian = v->mBigEndian;
        }
        else {
            U1_T * p = (U1_T *)&v->mValue;
            if (val_size > sizeof(v->mValue)) str_exception(ERR_INV_DWARF, "Unknown object size");
            if (big_endian_host()) p += sizeof(v->mValue) - (size_t)val_size;
            memcpy(bf, p, (size_t)val_size);
            *big_endian = big_endian_host();
        }
        if (bit_size % 8 != 0) bf[bit_size / 8] &= (1 << (bit_size % 8)) - 1;
        *size = (size_t)val_size;
        *value = bf;
    }
}

static U8_T read_cardinal_object_value(PropertyValue * v) {
    void * value = NULL;
    size_t size = 0;
    size_t i = 0;
    int big_endian = 0;
    U8_T n = 0;

    read_object_value(v, &value, &size, &big_endian);
    if (size > 8) str_exception(ERR_INV_DWARF, "Invalid object size");
    for (i = 0; i < size; i++) {
        n = (n << 8) | ((U1_T *)value)[big_endian ? i : size - i - 1];
    }
    return n;
}

static U8_T read_string_length(ObjectInfo * obj) {
    Trap trap;
    U8_T len = 0;

    assert(obj->mTag == TAG_string_type);
    if (set_trap(&trap)) {
        PropertyValue v;
        read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, AT_string_length, &v);
        len = read_cardinal_object_value(&v);
        clear_trap(&trap);
        return len;
    }
    else if (trap.error != ERR_SYM_NOT_FOUND) {
        exception(trap.error);
    }
    if (get_num_prop(obj, AT_byte_size, &len)) return len;
    str_exception(ERR_INV_DWARF, "Unknown length of a string type");
    return 0;
}

int get_symbol_class(const Symbol * sym, int * sym_class) {
    assert(sym->magic == SYMBOL_MAGIC);
    *sym_class = sym->sym_class;
    return 0;
}

int get_symbol_type(const Symbol * sym, Symbol ** type) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->sym_class == SYM_CLASS_TYPE && obj == NULL) {
        *type = (Symbol *)sym;
        return 0;
    }
    if (is_constant_pseudo_symbol(sym)) {
        *type = sym->base;
        return 0;
    }
    if (sym->sym_class == SYM_CLASS_FUNCTION) {
        if (obj == NULL) {
            *type = NULL;
        }
        else {
            *type = alloc_symbol();
            (*type)->ctx = sym->ctx;
            (*type)->frame = STACK_NO_FRAME;
            (*type)->sym_class = SYM_CLASS_TYPE;
            (*type)->base = (Symbol *)sym;
        }
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    if (is_modified_type(obj)) {
        obj = obj->mType;
    }
    else {
        obj = get_object_type(obj);
    }
    if (obj == NULL) {
        *type = NULL;
    }
    else if (obj == sym->obj) {
        *type = (Symbol *)sym;
    }
    else {
        object2symbol(find_definition(obj), type);
    }
    return 0;
}

int get_symbol_type_class(const Symbol * sym, int * type_class) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_constant_pseudo_symbol(sym)) return get_symbol_type_class(sym->base, type_class);
    if (is_array_type_pseudo_symbol(sym)) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) *type_class = TYPE_CLASS_FUNCTION;
        else if (sym->length > 0) *type_class = TYPE_CLASS_ARRAY;
        else *type_class = TYPE_CLASS_POINTER;
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym)) {
        *type_class = sym->dimension ? TYPE_CLASS_INTEGER : TYPE_CLASS_CARDINAL;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    while (obj != NULL) {
        switch (obj->mTag) {
        case TAG_global_subroutine:
        case TAG_inlined_subroutine:
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
        case TAG_mod_pointer:
        case TAG_mod_reference:
            *type_class = TYPE_CLASS_POINTER;
            return 0;
        case TAG_ptr_to_member_type:
            *type_class = TYPE_CLASS_MEMBER_PTR;
            return 0;
        case TAG_class_type:
        case TAG_structure_type:
        case TAG_union_type:
        case TAG_interface_type:
            *type_class = TYPE_CLASS_COMPOSITE;
            return 0;
        case TAG_base_type:
            switch (obj->u.mFundType) {
            case ATE_address:
                *type_class = TYPE_CLASS_POINTER;
                return 0;
            case ATE_boolean:
                *type_class = TYPE_CLASS_ENUMERATION;
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
            *type_class = TYPE_CLASS_UNKNOWN;
            return 0;
        case TAG_fund_type:
            switch (obj->u.mFundType) {
            case FT_boolean:
                *type_class = TYPE_CLASS_ENUMERATION;
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
        case TAG_unspecified_parameters:
        case TAG_global_variable:
        case TAG_local_variable:
        case TAG_variable:
        case TAG_inheritance:
        case TAG_member:
        case TAG_constant:
        case TAG_template_type_param:
            obj = obj->mType;
            break;
        default:
            obj = NULL;
            break;
        }
    }
    if (sym->tbl != NULL) {
        ELF_SymbolInfo info;
        if (sym->dimension != 0) {
            *type_class = TYPE_CLASS_FUNCTION;
            return 0;
        }
        unpack_elf_symbol_info(sym->tbl, sym->index, &info);
        if (info.type == STT_FUNC || info.type == STT_GNU_IFUNC || info.type == STT_NOTYPE) {
            *type_class = TYPE_CLASS_FUNCTION;
            return 0;
        }
    }
    *type_class = TYPE_CLASS_UNKNOWN;
    return 0;
}

int get_symbol_update_policy(const Symbol * sym, char ** id, int * policy) {
    assert(sym->magic == SYMBOL_MAGIC);
    *id = sym->ctx->id;
    *policy = sym->frame != STACK_NO_FRAME ? UPDATE_ON_EXE_STATE_CHANGES : UPDATE_ON_MEMORY_MAP_CHANGES;
    return 0;
}

int get_symbol_name(const Symbol * sym, char ** name) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_array_type_pseudo_symbol(sym)) {
        *name = NULL;
    }
    else if (is_int_type_pseudo_symbol(sym)) {
        *name = sym->index > 0 ? (char *)type_pseudo_symbols[sym->index - 1].name : NULL;
    }
    else if (is_constant_pseudo_symbol(sym)) {
        *name = (char *)constant_pseudo_symbols[sym->index].name;
    }
    else if (sym->obj != NULL) {
        *name = (char *)sym->obj->mName;
    }
    else if (sym->tbl != NULL) {
        ELF_SymbolInfo sym_info;
        if (sym->dimension == 0) {
            size_t i;
            unpack_elf_symbol_info(sym->tbl, sym->index, &sym_info);
            for (i = 0;; i++) {
                if (sym_info.name[i] == 0) {
                    *name = sym_info.name;
                    break;
                }
                if (sym_info.name[i] == '@' && sym_info.name[i + 1] == '@') {
                    *name = (char *)tmp_alloc_zero(i + 1);
                    memcpy(*name, sym_info.name, i);
                    break;
                }
            }
        }
        else {
            ContextAddress sym_offs = 0;
            if (elf_find_plt_dynsym(sym->tbl, sym->index, &sym_info, &sym_offs) < 0) return -1;
            if (sym_info.name != NULL) {
                *name = tmp_strdup2(sym_info.name, "@plt");
                if (sym_offs > 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "+0x%x", (unsigned)sym_offs);
                    *name = tmp_strdup2(*name, buf);
                }
            }
            else {
                *name = NULL;
            }
        }
    }
    else {
        *name = NULL;
    }
    return 0;
}

static int err_no_info(void) {
    set_errno(ERR_OTHER, "Debug info not available");
    return -1;
}

static int err_wrong_obj(void) {
    set_errno(ERR_OTHER, "Wrong object kind");
    return -1;
}

int get_symbol_size(const Symbol * sym, ContextAddress * size) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_constant_pseudo_symbol(sym)) return get_symbol_size(sym->base, size);
    if (is_array_type_pseudo_symbol(sym)) {
        if (sym->length > 0) {
            if (get_symbol_size(sym->base, size)) return -1;
            *size *= sym->length;
        }
        else {
            Symbol * base = sym->base;
            while (base->obj == NULL && base->base != NULL) base = base->base;
            if (base->obj != NULL) *size = base->obj->mCompUnit->mDesc.mAddressSize;
            else *size = context_word_size(sym->ctx);
        }
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym)) {
        *size = sym->cardinal;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    *size = 0;
    if (obj != NULL) {
        Trap trap;
        int ok = 0;
        U8_T sz = 0;
        U8_T n = 0;

        if (!set_trap(&trap)) return -1;
        ok = get_object_size(obj, sym->dimension, &sz, &n);
        clear_trap(&trap);
        if (!ok && sym->sym_class == SYM_CLASS_REFERENCE) {
            if (set_trap(&trap)) {
                PropertyValue v;
                read_and_evaluate_dwarf_object_property(sym_ctx, sym_frame, obj, AT_location, &v);
                if (v.mPieces) {
                    U4_T i = 0, j = 0;
                    while (i < v.mPieceCnt) {
                        LocationPiece * p = v.mPieces + i++;
                        if (p->bit_size) j += p->bit_size;
                        else sz += p->size;
                    }
                    sz += (j + 7) / 8;
                    ok = 1;
                }
                clear_trap(&trap);
            }
        }
        if (!ok && sym->sym_class == SYM_CLASS_REFERENCE) {
            Symbol * elf_sym = NULL;
            ContextAddress elf_sym_size = 0;
            if (map_to_sym_table(obj, &elf_sym) && get_symbol_size(elf_sym, &elf_sym_size) == 0) {
                sz = elf_sym_size;
                ok = 1;
            }
        }
        if (!ok) {
            set_errno(ERR_INV_DWARF, "Object has no size attribute");
            return -1;
        }
        *size = (ContextAddress)sz;
    }
    else if (sym->tbl != NULL) {
        if (sym->dimension == 0) {
            ELF_SymbolInfo info;
            unpack_elf_symbol_info(sym->tbl, sym->index, &info);
            switch (info.type) {
            case STT_NOTYPE:
            case STT_OBJECT:
            case STT_FUNC:
                *size = (ContextAddress)info.size;
                break;
            case STT_GNU_IFUNC:
                set_errno(ERR_OTHER, "Size not available: indirect symbol");
                return -1;
            default:
                *size = info.sym_section->file->elf64 ? 8 : 4;
                break;
            }
        }
        else {
            *size = sym->dimension;
        }
    }
    else {
        return err_no_info();
    }
    return 0;
}

int get_symbol_base_type(const Symbol * sym, Symbol ** base_type) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_array_type_pseudo_symbol(sym)) {
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            if (sym->base->obj != NULL && sym->base->obj->mType != NULL) {
                if (unpack(sym->base) < 0) return -1;
                object2symbol(sym->base->obj->mType, base_type);
                return 0;
            }
            return err_no_info();
        }
        if (sym->base->sym_class == SYM_CLASS_REFERENCE) {
            return err_no_info();
        }
        *base_type = sym->base;
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym) || is_constant_pseudo_symbol(sym)) {
        return err_wrong_obj();
    }
    if (unpack(sym) < 0) return -1;
    if (sym->sym_class == SYM_CLASS_FUNCTION) {
        if (sym->obj != NULL && sym->obj->mType != NULL) {
            object2symbol(sym->obj->mType, base_type);
            return 0;
        }
        return err_no_info();
    }
    obj = get_original_type(obj);
    if (obj != NULL) {
        if (obj->mTag == TAG_array_type) {
            int i = sym->dimension;
            ObjectInfo * idx = get_dwarf_children(obj);
            while (i > 0 && idx != NULL) {
                idx = idx->mSibling;
                i--;
            }
            if (idx != NULL && idx->mSibling != NULL) {
                object2symbol(obj, base_type);
                (*base_type)->dimension = sym->dimension + 1;
                return 0;
            }
        }
        obj = obj->mType;
        if (obj != NULL) {
            object2symbol(find_definition(obj), base_type);
            return 0;
        }
        return err_wrong_obj();
    }
    return err_no_info();
}

int get_symbol_index_type(const Symbol * sym, Symbol ** index_type) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_array_type_pseudo_symbol(sym)) {
        if (sym->base->sym_class != SYM_CLASS_TYPE) return err_wrong_obj();
        alloc_int_type_pseudo_symbol(sym->ctx, context_word_size(sym->ctx), 0, index_type);
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym) ||
            is_constant_pseudo_symbol(sym) ||
            sym->sym_class == SYM_CLASS_FUNCTION) {
        return err_wrong_obj();
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL) {
        if (obj->mTag == TAG_array_type) {
            int i = sym->dimension;
            ObjectInfo * idx = get_dwarf_children(obj);
            while (i > 0 && idx != NULL) {
                idx = idx->mSibling;
                i--;
            }
            if (idx != NULL) {
                object2symbol(idx, index_type);
                return 0;
            }
        }
        if (obj->mTag == TAG_string_type) {
            alloc_int_type_pseudo_symbol(sym->ctx, obj->mCompUnit->mDesc.mAddressSize, 0, index_type);
            return 0;
        }
        return err_wrong_obj();
    }
    return err_no_info();
}

int get_symbol_container(const Symbol * sym, Symbol ** container) {
    ObjectInfo * obj = sym->obj;
    while (is_array_type_pseudo_symbol(sym)) {
        sym = sym->base;
        obj = sym->obj;
    }
    if (obj != NULL) {
        U8_T origin = 0;
        U8_T spec = 0;
        ObjectInfo * parent = NULL;
        if (unpack(sym) < 0) return -1;
        if (sym->sym_class == SYM_CLASS_TYPE) {
            ObjectInfo * org = get_original_type(obj);
            if (org->mTag == TAG_ptr_to_member_type) {
                U8_T id = 0;
                if (get_num_prop(org, AT_containing_type, &id)) {
                    ObjectInfo * type = find_object(get_dwarf_cache(org->mCompUnit->mFile), (ContextAddress)id);
                    if (type != NULL) {
                        object2symbol(type, container);
                        return 0;
                    }
                }
                set_errno(ERR_INV_DWARF, "Invalid AT_containing_type attribute");
                return -1;
            }
        }
        parent = get_dwarf_parent(obj);
        if (parent != NULL && parent->mTag == TAG_compile_unit) {
            if ((obj->mFlags & DOIF_abstract_origin) && get_num_prop(obj, AT_abstract_origin, &origin)) {
                ObjectInfo * org = find_object(get_dwarf_cache(obj->mCompUnit->mFile), (ContextAddress)origin);
                if (org != NULL) obj = org;
            }
            if ((obj->mFlags & DOIF_specification) && get_num_prop(obj, AT_specification_v2, &spec)) {
                ObjectInfo * spc = find_object(get_dwarf_cache(obj->mCompUnit->mFile), (ContextAddress)spec);
                if (spc != NULL) obj = spc;
            }
            parent = get_dwarf_parent(obj);
        }
        if (parent != NULL) {
            object2symbol(parent, container);
            return 0;
        }
        if (obj->mTag >= TAG_fund_type && obj->mTag < TAG_fund_type + 0x100) {
            /* Virtual DWARF object that is created by the DWARF reader. */
            object2symbol(obj->mCompUnit->mObject, container);
            return 0;
        }
        return err_wrong_obj();
    }
    return err_no_info();
}

int get_symbol_length(const Symbol * sym, ContextAddress * length) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_array_type_pseudo_symbol(sym)) {
        if (sym->base->sym_class != SYM_CLASS_TYPE) return err_wrong_obj();
        *length = sym->length == 0 ? 1 : sym->length;
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym) ||
            is_constant_pseudo_symbol(sym) ||
            sym->sym_class == SYM_CLASS_FUNCTION) {
        return err_wrong_obj();
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL) {
        if (obj->mTag == TAG_array_type) {
            int i = sym->dimension;
            ObjectInfo * idx = get_dwarf_children(obj);
            while (i > 0 && idx != NULL) {
                idx = idx->mSibling;
                i--;
            }
            if (idx != NULL) {
                Trap trap;
                if (!set_trap(&trap)) return -1;
                *length = (ContextAddress)get_array_index_length(idx);
                clear_trap(&trap);
                return 0;
            }
        }
        if (obj->mTag == TAG_string_type) {
            Trap trap;
            if (!set_trap(&trap)) return -1;
            *length = (ContextAddress)read_string_length(obj);
            clear_trap(&trap);
            return 0;
        }
        return err_wrong_obj();
    }
    return err_no_info();
}

int get_symbol_lower_bound(const Symbol * sym, int64_t * value) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_array_type_pseudo_symbol(sym)) {
        if (sym->base->sym_class != SYM_CLASS_TYPE) return err_wrong_obj();
        *value = 0;
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym) ||
            is_constant_pseudo_symbol(sym) ||
            sym->sym_class == SYM_CLASS_FUNCTION) {
        return err_wrong_obj();
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL) {
        if (obj->mTag == TAG_array_type) {
            int i = sym->dimension;
            ObjectInfo * idx = get_dwarf_children(obj);
            while (i > 0 && idx != NULL) {
                idx = idx->mSibling;
                i--;
            }
            if (idx != NULL) {
                if (get_num_prop(idx, AT_lower_bound, (U8_T *)value)) return 0;
                if (get_error_code(errno) != ERR_SYM_NOT_FOUND) return -1;
                *value = get_default_lower_bound(obj);
                return 0;
            }
        }
        if (obj->mTag == TAG_string_type) {
            *value = 0;
            return 0;
        }
        return err_wrong_obj();
    }
    return err_no_info();
}

static Symbol ** boolean_children(ObjectInfo * type_obj) {
    unsigned i = 0;
    unsigned n = 0;
    Symbol * type_sym = NULL;
    Symbol ** buf = (Symbol **)tmp_alloc(sizeof(Symbol *) * 2);
    object2symbol(type_obj, &type_sym);
    while (constant_pseudo_symbols[i].name != NULL) {
        if (n < 2 && strcmp(constant_pseudo_symbols[i].type, "bool") == 0) {
            Symbol * sym = alloc_symbol();
            sym->ctx = sym_ctx;
            sym->frame = STACK_NO_FRAME;
            sym->sym_class = SYM_CLASS_VALUE;
            sym->base = type_sym;
            sym->index = i;
            assert(is_constant_pseudo_symbol(sym));
            buf[n++] = sym;
        }
        i++;
    }
    assert(n == 2);
    return buf;
}

int get_symbol_children(const Symbol * sym, Symbol *** children, int * count) {
    ObjectInfo * obj = sym->obj;
    assert(sym->magic == SYMBOL_MAGIC);
    if (is_array_type_pseudo_symbol(sym)) {
        obj = sym->base->obj;
        if (sym->base->sym_class == SYM_CLASS_FUNCTION) {
            if (obj == NULL) {
                *children = NULL;
                *count = 0;
                errno = ERR_SYM_NOT_FOUND;
                return -1;
            }
            else {
                int n = 0;
                int buf_len = 0;
                Symbol ** buf = NULL;
                ObjectInfo * i = get_dwarf_children(obj);
                if (unpack(sym->base) < 0) return -1;
                while (i != NULL) {
                    if (i->mTag == TAG_formal_parameter || i->mTag == TAG_unspecified_parameters) {
                        Symbol * x = NULL;
                        Symbol * y = NULL;
                        object2symbol(i, &x);
                        if (get_symbol_type(x, &y) < 0) return -1;
                        if (y == NULL && i->mTag == TAG_unspecified_parameters) {
                            y = alloc_symbol();
                            y->ctx = sym->ctx;
                            y->frame = STACK_NO_FRAME;
                            y->sym_class = SYM_CLASS_TYPE;
                            y->base = (Symbol *)x;
                        }
                        if (y == NULL) {
                            set_errno(ERR_INV_DWARF, "Invalid function arguments info");
                            return -1;
                        }
                        if (buf_len <= n) {
                            buf_len += 16;
                            buf = (Symbol **)tmp_realloc(buf, sizeof(Symbol *) * buf_len);
                        }
                        buf[n++] = y;
                    }
                    i = i->mSibling;
                }
                *children = buf;
                *count = n;
                return 0;
            }
        }
        *children = NULL;
        *count = 0;
        return 0;
    }
    if (is_int_type_pseudo_symbol(sym) || is_constant_pseudo_symbol(sym)) {
        *children = NULL;
        *count = 0;
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    obj = get_original_type(obj);
    if (obj != NULL) {
        int n = 0;
        Symbol ** buf = NULL;
        if (obj->mTag == TAG_base_type) {
            if (obj->u.mFundType == ATE_boolean) {
                buf = boolean_children(obj);
                n = 2;
            }
        }
        else if (obj->mTag == TAG_fund_type) {
            if (obj->u.mFundType == FT_boolean) {
                buf = boolean_children(obj);
                n = 2;
            }
        }
        else {
            int buf_len = 0;
            ObjectInfo * i = get_dwarf_children(find_definition(obj));
            while (i != NULL) {
                Symbol * x = NULL;
                object2symbol(find_definition(i), &x);
                if (buf_len <= n) {
                    buf_len += 16;
                    buf = (Symbol **)tmp_realloc(buf, sizeof(Symbol *) * buf_len);
                }
                buf[n++] = x;
                i = i->mSibling;
            }
        }
        *children = buf;
        *count = n;
        return 0;
    }
    *children = NULL;
    *count = 0;
    return 0;
}

static void dwarf_location_operation(uint8_t op) {
    str_fmt_exception(ERR_UNSUPPORTED, "Unsupported location expression op 0x%02x", op);
}

static int dwarf_location_callback(LocationExpressionState * state) {
    state->client_op = dwarf_location_operation;
    return evaluate_vm_expression(state);
}

static LocationExpressionCommand * add_location_command(LocationInfo * l, int op) {
    LocationCommands * cmds = &l->value_cmds;
    LocationExpressionCommand * cmd = NULL;
    if (cmds->cnt >= cmds->max) {
        cmds->max += 4;
        cmds->cmds = (LocationExpressionCommand *)tmp_realloc(cmds->cmds,
            sizeof(LocationExpressionCommand) * cmds->max);
    }
    cmd = cmds->cmds + cmds->cnt++;
    memset(cmd, 0, sizeof(LocationExpressionCommand));
    cmd->cmd = op;
    return cmd;
}

static void add_dwarf_location_command(LocationInfo * l, PropertyValue * v) {
    DWARFExpressionInfo * info = NULL;
    LocationExpressionCommand * cmd = NULL;

    dwarf_get_expression_list(v, &info);
    dwarf_transform_expression(sym_ctx, v->mFrame, info);
    if (l->code_size == 0) {
        l->code_addr = info->code_addr;
        l->code_size = info->code_size;
    }
    else if (info->code_size > 0) {
        if (l->code_addr < info->code_addr) {
            assert(l->code_addr + l->code_size > info->code_addr);
            l->code_size = l->code_addr + l->code_size - info->code_addr;
            l->code_addr = info->code_addr;
        }
        if (l->code_addr + l->code_size > info->code_addr + info->code_size) {
            assert(l->code_addr < info->code_addr + info->code_size);
            l->code_size = info->code_addr + info->code_size - l->code_addr;
        }
    }
    /* Only create the command if no exception was thrown */
    cmd = add_location_command(l, SFT_CMD_LOCATION);
    cmd->args.loc.code_addr = info->expr_addr;
    cmd->args.loc.code_size = info->expr_size;
    cmd->args.loc.reg_id_scope = v->mObject->mCompUnit->mRegIdScope;
    cmd->args.loc.addr_size = v->mObject->mCompUnit->mDesc.mAddressSize;
    cmd->args.loc.func = dwarf_location_callback;
}

static void add_member_location_command(LocationInfo * info, ObjectInfo * obj) {
    U8_T bit_size = 0;
    U8_T bit_offs = 0;
    PropertyValue v;
    read_dwarf_object_property(sym_ctx, sym_frame, obj, AT_data_member_location, &v);
    switch (v.mForm) {
    case FORM_DATA1     :
    case FORM_DATA2     :
    case FORM_DATA4     :
    case FORM_DATA8     :
    case FORM_SDATA     :
    case FORM_UDATA     :
        add_location_command(info, SFT_CMD_ARG)->args.arg_no = 0;
        add_location_command(info, SFT_CMD_NUMBER)->args.num = get_numeric_property_value(&v);
        add_location_command(info, SFT_CMD_ADD);
        break;
    case FORM_BLOCK1    :
    case FORM_BLOCK2    :
    case FORM_BLOCK4    :
    case FORM_BLOCK     :
    case FORM_EXPRLOC   :
    case FORM_SEC_OFFSET:
        add_location_command(info, SFT_CMD_ARG)->args.arg_no = 0;
        add_dwarf_location_command(info, &v);
        break;
    default:
        str_fmt_exception(ERR_OTHER, "Invalid AT_data_member_location form 0x%04x", v.mForm);
        break;
    }
    if (get_num_prop(obj, AT_bit_size, &bit_size)) {
        LocationExpressionCommand * cmd = add_location_command(info, SFT_CMD_PIECE);
        cmd->args.piece.bit_size = (unsigned)bit_size;
        if (get_num_prop(obj, AT_bit_offset, &bit_offs)) {
            if (obj->mCompUnit->mFile->big_endian) {
                cmd->args.piece.bit_offs = (unsigned)bit_offs;
            }
            else {
                U8_T byte_size = 0;
                U8_T type_byte_size = 0;
                U8_T type_bit_size = 0;
                if (get_num_prop(obj, AT_byte_size, &byte_size)) {
                    cmd->args.piece.bit_offs = (unsigned)(byte_size * 8 - bit_offs - bit_size);
                }
                else if (obj->mType != NULL && get_object_size(obj->mType, 0, &type_byte_size, &type_bit_size)) {
                    cmd->args.piece.bit_offs = (unsigned)(type_byte_size * 8 - bit_offs - bit_size);
                }
                else {
                    str_exception(ERR_INV_DWARF, "Unknown field size");
                }
            }
        }
    }
}

static int add_member_location(LocationInfo * info, ObjectInfo * type, ObjectInfo * member) {
    ObjectInfo * obj = NULL;
    if (member->mParent == type) {
        add_member_location_command(info, member);
        return 1;
    }
    obj = get_dwarf_children(type);
    while (obj != NULL) {
        if (obj->mTag == TAG_inheritance) {
            unsigned cnt = info->value_cmds.cnt;
            add_member_location_command(info, obj);
            add_location_command(info, SFT_CMD_SET_ARG)->args.arg_no = 0;
            if (add_member_location(info, obj->mType, member)) return 1;
            info->value_cmds.cnt = cnt;
        }
        obj = obj->mSibling;
    }
    return 0;
}

int get_location_info(const Symbol * sym, LocationInfo ** res) {
    ObjectInfo * obj = sym->obj;
    LocationInfo * info = *res = (LocationInfo *)tmp_alloc_zero(sizeof(LocationInfo));

    assert(sym->magic == SYMBOL_MAGIC);

    if (sym->has_address) {
        info->big_endian = big_endian_host();
        add_location_command(info, SFT_CMD_NUMBER)->args.num = sym->address;
        return 0;
    }

    if (is_constant_pseudo_symbol(sym)) {
        void * value = NULL;
        ContextAddress size = 0;
        LocationExpressionCommand * cmd = add_location_command(info, SFT_CMD_PIECE);

        if (get_symbol_size(sym->base, &size) < 0) return -1;
        info->big_endian = big_endian_host();
        cmd->args.piece.bit_size = (unsigned)(size * 8);
        cmd->args.piece.value = tmp_alloc((size_t)size);
        value = &constant_pseudo_symbols[sym->index].value;
        if (big_endian_host() && size < sizeof(ConstantValueType)) {
            value = (uint8_t *)value + (sizeof(ConstantValueType) - size);
        }
        memcpy(cmd->args.piece.value, value, (size_t)size);
        return 0;
    }

    if (is_array_type_pseudo_symbol(sym) ||
            is_int_type_pseudo_symbol(sym)) {
        return err_wrong_obj();
    }

    if (unpack(sym) < 0) return -1;

    if (obj != NULL) {
        Trap trap;
        PropertyValue v;
        ObjectInfo * org_type;

        obj = find_definition(obj);
        org_type = obj;
        while (org_type != NULL && org_type->mType != NULL && is_modified_type(org_type)) org_type = org_type->mType;
        info->big_endian = obj->mCompUnit->mFile->big_endian;
        if ((obj->mFlags & DOIF_external) == 0 && sym->var != NULL) {
            /* The symbol represents a member of a class instance */
            LocationExpressionCommand * cmd = NULL;
            ObjectInfo * type = get_original_type(sym->var);
            if (!set_trap(&trap)) {
                if (errno == ERR_SYM_NOT_FOUND) set_errno(ERR_OTHER, "Location attribute not found");
                set_errno(errno, "Cannot evaluate location of 'this' pointer");
                return -1;
            }
            if ((type->mTag != TAG_pointer_type && type->mTag != TAG_mod_pointer) || type->mType == NULL) exception(ERR_INV_CONTEXT);
            read_dwarf_object_property(sym_ctx, sym_frame, sym->var, AT_location, &v);
            add_dwarf_location_command(info, &v);
            cmd = add_location_command(info, SFT_CMD_LOAD);
            cmd->args.mem.size = obj->mCompUnit->mDesc.mAddressSize;
            cmd->args.mem.big_endian = obj->mCompUnit->mFile->big_endian;
            add_location_command(info, SFT_CMD_SET_ARG)->args.arg_no = 0;
            type = get_original_type(type->mType);
            if (!add_member_location(info, type, obj)) exception(ERR_INV_CONTEXT);
            clear_trap(&trap);
            return 0;
        }
        if (org_type->mTag == TAG_ptr_to_member_type) {
            add_location_command(info, SFT_CMD_ARG)->args.arg_no = 1;
            add_location_command(info, SFT_CMD_ARG)->args.arg_no = 0;
            info->args_cnt = 2;
            if (set_trap(&trap)) {
                read_dwarf_object_property(sym_ctx, sym_frame, org_type, AT_use_location, &v);
                add_dwarf_location_command(info, &v);
                clear_trap(&trap);
                return 0;
            }
            else if (errno != ERR_SYM_NOT_FOUND) {
                set_errno(errno, "Cannot read member location expression");
                return -1;
            }
            add_location_command(info, SFT_CMD_ADD);
            return 0;
        }
        if (obj->mTag != TAG_inlined_subroutine) {
            if (set_trap(&trap)) {
                LocationExpressionCommand * cmd = NULL;
                read_dwarf_object_property(sym_ctx, sym_frame, obj, AT_const_value, &v);
                assert(v.mObject == obj);
                assert(v.mPieces == NULL);
                assert(v.mForm != FORM_EXPRLOC);
                if (v.mAddr != NULL) {
                    assert(v.mBigEndian == info->big_endian);
                    cmd = add_location_command(info, SFT_CMD_PIECE);
                    cmd->args.piece.bit_size = v.mSize * 8;
                    cmd->args.piece.value = v.mAddr;
                }
                else {
                    U1_T * bf = NULL;
                    U8_T val_size = 0;
                    U8_T bit_size = 0;
                    U1_T * p = (U1_T *)&v.mValue;
                    if (!get_object_size(obj, 0, &val_size, &bit_size)) {
                        str_exception(ERR_INV_DWARF, "Unknown object size");
                    }
                    assert(v.mForm != FORM_EXPR_VALUE);
                    if (val_size > sizeof(v.mValue)) str_exception(ERR_INV_DWARF, "Unknown object size");
                    bf = (U1_T *)tmp_alloc((size_t)val_size);
                    if (big_endian_host()) p += sizeof(v.mValue) - (size_t)val_size;
                    memcpy(bf, p, (size_t)val_size);
                    info->big_endian = big_endian_host();
                    if (bit_size % 8 != 0) bf[bit_size / 8] &= (1 << (bit_size % 8)) - 1;
                    cmd = add_location_command(info, SFT_CMD_PIECE);
                    cmd->args.piece.bit_size = (unsigned)(bit_size ? bit_size : val_size * 8);
                    cmd->args.piece.value = bf;
                }
                clear_trap(&trap);
                return 0;
            }
            else if (trap.error != ERR_SYM_NOT_FOUND) {
                return -1;
            }
        }
        if (obj->mTag == TAG_member || obj->mTag == TAG_inheritance) {
            if (set_trap(&trap)) {
                add_member_location_command(info, obj);
                info->args_cnt = 1;
                clear_trap(&trap);
                return 0;
            }
            else {
                if (errno != ERR_SYM_NOT_FOUND) set_errno(errno, "Cannot read member location expression");
                else set_errno(ERR_OTHER, "Member location info not avaiable");
                return -1;
            }
        }
#if 0
#if SERVICE_StackTrace || ENABLE_ContextProxy
        if (obj->mTag == TAG_formal_parameter) {
            /* Search call site info */
            if (set_trap(&trap)) {
                RegisterDefinition * reg_def = get_PC_definition(sym_ctx);
                if (reg_def != NULL) {
                    uint64_t addr = 0;
                    ContextAddress rt_addr = 0;
                    UnitAddressRange * range = NULL;
                    Symbol * caller = NULL;
                    StackFrame * info = NULL;
                    save_sym_context();
                    int frame = get_prev_frame(sym_ctx, sym_frame);
                    restore_sym_context();
                    if (get_frame_info(sym_ctx, frame, &info) < 0) exception(errno);
                    if (read_reg_value(info, reg_def, &addr) < 0) exception(errno);
                    range = elf_find_unit(sym_ctx, addr, addr, &rt_addr);
                    if (range != NULL) find_by_addr_in_unit(
                        get_dwarf_children(range->mUnit->mObject),
                        0, rt_addr - range->mAddr, addr, &caller);
                    if (caller != NULL && caller->obj != NULL) {
                        ObjectInfo * l = get_dwarf_children(caller->obj);
                        while (l != NULL) {
                            U8_T call_addr = 0;
                            if (l->mTag == TAG_GNU_call_site && get_num_prop(l, AT_low_pc, &call_addr)) {
                                call_addr += rt_addr - range->mAddr;
                                if (call_addr == addr) {
                                    /*
                                    clear_trap(&trap);
                                    return 0;
                                    */
                                }
                            }
                            l = l->mSibling;
                        }
                    }
                }
                exception(ERR_SYM_NOT_FOUND);
            }
        }
#endif
#endif
        {
            U8_T addr = 0;
            Symbol * s = NULL;
            if (set_trap(&trap)) {
                read_dwarf_object_property(sym_ctx, sym_frame, obj, AT_location, &v);
                add_dwarf_location_command(info, &v);
                clear_trap(&trap);
                return 0;
            }
            else if (errno != ERR_SYM_NOT_FOUND) {
                return -1;
            }
            switch (sym->sym_class) {
            case SYM_CLASS_FUNCTION:
            case SYM_CLASS_COMP_UNIT:
            case SYM_CLASS_BLOCK:
                if (get_num_prop(obj, AT_entry_pc, &addr)) {
                    add_location_command(info, SFT_CMD_NUMBER)->args.num = addr;
                    return 0;
                }
                else if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                    return -1;
                }
                if (get_num_prop(obj, AT_low_pc, &addr)) {
                    add_location_command(info, SFT_CMD_NUMBER)->args.num = addr;
                    return 0;
                }
                else if (get_error_code(errno) != ERR_SYM_NOT_FOUND) {
                    return -1;
                }
                break;
            }
            if (map_to_sym_table(obj, &s)) return get_location_info(s, res);
            set_errno(ERR_OTHER, "No object location info found in DWARF data");
            return -1;
        }
    }

    if (sym->tbl != NULL) {
        LocationExpressionCommand * cmd = NULL;
        ELF_SymbolInfo elf_sym_info;
        ContextAddress address = 0;
        info->big_endian = sym->tbl->file->big_endian;
        if (sym->dimension != 0) {
            /* @plt symbol */
            address = sym->tbl->addr + sym->cardinal + sym->index * sym->dimension;
            add_location_command(info, SFT_CMD_NUMBER)->args.num = address;
            return 0;
        }
        unpack_elf_symbol_info(sym->tbl, sym->index, &elf_sym_info);
        if (elf_sym_info.type == STT_GNU_IFUNC && elf_sym_info.name != NULL) {
            int error = 0;
            int found = 0;
            ELF_File * file = elf_list_first(sym_ctx, 0, ~(ContextAddress)0);
            if (file == NULL) error = errno;
            while (error == 0 && file != NULL) {
                ContextAddress got_addr = 0;
                if (elf_find_got_entry(file, elf_sym_info.name, &got_addr) < 0) {
                    error = errno;
                }
                else if (got_addr != 0) {
                    got_addr = elf_map_to_run_time_address(sym_ctx, file, NULL, got_addr);
                    if (got_addr != 0 && elf_read_memory_word(sym_ctx, file, got_addr, &address) == 0) {
                        found = 1;
                        break;
                    }
                }
                file = elf_list_next(sym_ctx);
                if (file == NULL) error = errno;
            }
            elf_list_done(sym_ctx);
            if (found) {
                add_location_command(info, SFT_CMD_NUMBER)->args.num = address;
                return 0;
            }
            if (error) {
                errno = error;
                return -1;
            }
        }
        switch (elf_sym_info.type) {
        case STT_NOTYPE:
            /* Check if the NOTYPE symbol is for a section allocated in memory */
            if (elf_sym_info.section == NULL || (elf_sym_info.section->flags & SHF_ALLOC) == 0) break;
            /* fall through */
        case STT_OBJECT:
        case STT_FUNC:
        case STT_GNU_IFUNC:
            if (elf_symbol_address(sym_ctx, &elf_sym_info, &address)) return -1;
            add_location_command(info, SFT_CMD_NUMBER)->args.num = address;
            return 0;
        }
        info->big_endian = big_endian_host();
        cmd = add_location_command(info, SFT_CMD_PIECE);
        if (elf_sym_info.sym_section->file->elf64) {
            static U8_T buf = 0;
            buf = elf_sym_info.value;
            cmd->args.piece.bit_size = 64;
            cmd->args.piece.value = &buf;
        }
        else {
            static U4_T buf = 0;
            buf = (U4_T)elf_sym_info.value;
            cmd->args.piece.bit_size = 32;
            cmd->args.piece.value = &buf;
        }
        return 0;
    }

    set_errno(ERR_OTHER, "Symbol does not have a location information");
    return -1;
}

int get_symbol_flags(const Symbol * sym, SYM_FLAGS * flags) {
    U8_T v = 0;
    ObjectInfo * obj = sym->obj;
    *flags = 0;
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->base || is_int_type_pseudo_symbol(sym)) {
        if (is_array_type_pseudo_symbol(sym) && sym->base->sym_class == SYM_CLASS_REFERENCE) {
            *flags |= SYM_FLAG_VARARG;
        }
        return 0;
    }
    if (unpack(sym) < 0) return -1;
    if (obj != NULL) {
        if (obj->mFlags & DOIF_external) *flags |= SYM_FLAG_EXTERNAL;
        if (obj->mFlags & DOIF_artificial) *flags |= SYM_FLAG_ARTIFICIAL;
        if (obj->mFlags & DOIF_private) *flags |= SYM_FLAG_PRIVATE;
        if (obj->mFlags & DOIF_protected) *flags |= SYM_FLAG_PROTECTED;
        if (obj->mFlags & DOIF_public) *flags |= SYM_FLAG_PUBLIC;
        if (obj->mFlags & DOIF_optional) *flags |= SYM_FLAG_OPTIONAL;
        switch (obj->mTag) {
        case TAG_subrange_type:
            *flags |= SYM_FLAG_SUBRANGE_TYPE;
            break;
        case TAG_packed_type:
            *flags |= SYM_FLAG_PACKET_TYPE;
            break;
        case TAG_const_type:
            *flags |= SYM_FLAG_CONST_TYPE;
            break;
        case TAG_volatile_type:
            *flags |= SYM_FLAG_VOLATILE_TYPE;
            break;
        case TAG_restrict_type:
            *flags |= SYM_FLAG_RESTRICT_TYPE;
            break;
        case TAG_shared_type:
            *flags |= SYM_FLAG_SHARED_TYPE;
            break;
        case TAG_typedef:
            *flags |= SYM_FLAG_TYPEDEF;
            break;
        case TAG_template_type_param:
            *flags |= SYM_FLAG_TYPE_PARAMETER;
            break;
        case TAG_reference_type:
        case TAG_mod_reference:
            *flags |= SYM_FLAG_REFERENCE;
            break;
        case TAG_union_type:
            *flags |= SYM_FLAG_UNION_TYPE;
            break;
        case TAG_class_type:
            *flags |= SYM_FLAG_CLASS_TYPE;
            break;
        case TAG_structure_type:
            *flags |= SYM_FLAG_STRUCT_TYPE;
            break;
        case TAG_string_type:
            *flags |= SYM_FLAG_STRING_TYPE;
            break;
        case TAG_enumeration_type:
            *flags |= SYM_FLAG_ENUM_TYPE;
            break;
        case TAG_interface_type:
            *flags |= SYM_FLAG_INTERFACE_TYPE;
            break;
        case TAG_unspecified_parameters:
            *flags |= SYM_FLAG_PARAMETER;
            *flags |= SYM_FLAG_VARARG;
            break;
        case TAG_formal_parameter:
        case TAG_variable:
        case TAG_constant:
        case TAG_base_type:
            if (obj->mTag == TAG_formal_parameter) {
                *flags |= SYM_FLAG_PARAMETER;
            }
            else if (obj->mTag == TAG_base_type) {
                if (obj->u.mFundType == ATE_boolean) *flags |= SYM_FLAG_BOOL_TYPE;
            }
            if (get_num_prop(obj, AT_endianity, &v)) {
                if (v == DW_END_big) *flags |= SYM_FLAG_BIG_ENDIAN;
                if (v == DW_END_little) *flags |= SYM_FLAG_LITTLE_ENDIAN;
            }
            break;
        case TAG_fund_type:
            if (obj->u.mFundType == FT_boolean) *flags |= SYM_FLAG_BOOL_TYPE;
            break;
        case TAG_inheritance:
            *flags |= SYM_FLAG_INHERITANCE;
            break;
        }
    }
    if (obj != NULL && sym->sym_class == SYM_CLASS_TYPE && !(*flags & (SYM_FLAG_BIG_ENDIAN|SYM_FLAG_LITTLE_ENDIAN))) {
        *flags |= obj->mCompUnit->mFile->big_endian ? SYM_FLAG_BIG_ENDIAN : SYM_FLAG_LITTLE_ENDIAN;
    }
    return 0;
}

int get_symbol_frame(const Symbol * sym, Context ** ctx, int * frame) {
    int n = sym->frame;
    if (n == STACK_TOP_FRAME) {
        n = get_top_frame(sym->ctx);
        if (n < 0) return -1;
    }
    *ctx = sym->ctx;
    *frame = n;
    return 0;
}

int get_array_symbol(const Symbol * sym, ContextAddress length, Symbol ** ptr) {
    assert(sym->magic == SYMBOL_MAGIC);
    if (sym->sym_class != SYM_CLASS_TYPE) return err_wrong_obj();
    assert(sym->frame == STACK_NO_FRAME);
    assert(sym->ctx == context_get_group(sym->ctx, CONTEXT_GROUP_SYMBOLS));
    *ptr = alloc_symbol();
    (*ptr)->ctx = sym->ctx;
    (*ptr)->frame = STACK_NO_FRAME;
    (*ptr)->sym_class = SYM_CLASS_TYPE;
    (*ptr)->base = (Symbol *)sym;
    (*ptr)->length = length;
    return 0;
}

int get_funccall_info(const Symbol * func,
        const Symbol ** args, unsigned args_cnt, FunctionCallInfo ** res) {
    if (func->obj != NULL) {
        FunctionCallInfo * info = (FunctionCallInfo *)tmp_alloc_zero(sizeof(FunctionCallInfo));
        info->ctx = func->ctx;
        info->func = func;
        info->scope = func->obj->mCompUnit->mRegIdScope;
        info->args_cnt = args_cnt;
        info->args = args;
        if (get_function_call_location_expression(info) < 0) return -1;
        *res = info;
        return 0;
    }
    set_errno(ERR_OTHER, "Func call injection info not available");
    return -1;
}

#endif /* SERVICE_Symbols && ENABLE_ELF */
