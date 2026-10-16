#include <pvz2native/elf32/elf32_loader.h>
#include <pvz2native/log.h>
#include <pvz2native/elf32/elf32_defs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PVZ2_SO_BASE, PVZ2_TRAMPOLINE_BASE and PVZ2_TRAMPOLINE_MAX are declared in
 * elf32_loader.h so runtime/guest_memmap.h can assert that the harness structures
 * placed between the trampolines and the image do not collide with either. */

/* Gap between consecutive modules. Only alignment matters for correctness --
 * the gap exists so a module's address range is visibly distinct in a log. */
#define PVZ2_MODULE_ALIGN 0x00010000u

/* Undefined symbols of type STT_OBJECT are DATA, not code: the guest reads
 * through them instead of calling them. libPVZ2.so has ten -- __sF,
 * __stack_chk_guard, the three bionic ctype tables (_ctype_, _tolower_tab_,
 * _toupper_tab_) and five OpenSLES SL_IID_* interface ids.
 *
 * Pointing those at an SVC trampoline means the guest reads the instruction
 * word (0xEF0000xx) as if it were the datum. For the ctype tables that
 * silently corrupts every isalpha/isdigit/tolower in the engine, with no
 * error anywhere -- so they get real, writable guest memory instead, which
 * the dependency modules then fill in. */
#define PVZ2_DATA_IMPORT_BASE 0x0000B000u
#define PVZ2_DATA_IMPORT_SLOT 1024u
#define PVZ2_DATA_IMPORT_MAX  16u

static uint32_t read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static char *dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* Finds (or creates) the trampoline stub for an imported symbol name and
 * returns its address in the emulated address space. Deliberately shared across
 * modules: libc++_shared.so and libPVZ2.so both import `malloc`, and they must
 * get the SAME stub or memory allocated by one could not be freed by the
 * other. */
static uint32_t get_or_create_trampoline(pvz2_elf_image_t *img, const char *name) {
    for (uint32_t i = 0; i < img->trampoline_count; ++i) {
        if (strcmp(img->trampoline_names[i], name) == 0) {
            return img->trampoline_base + i * 4;
        }
    }
    if (img->trampoline_count >= img->trampoline_capacity) {
        pvz2_log("[elf32] trampoline table full (max %u), cannot import '%s' -- "
                "raise PVZ2_TRAMPOLINE_MAX in elf32_loader.h", img->trampoline_capacity, name);
        img->import_overflow = 1; /* binds this symbol to 0; see the field's comment */
        return 0;
    }
    uint32_t idx = img->trampoline_count++;
    img->trampoline_names[idx] = dup_str(name);
    write32(img->mem + img->trampoline_base + idx * 4, 0xEF000000u | (idx & 0x00FFFFFFu)); /* SVC #idx */
    return img->trampoline_base + idx * 4;
}

/* Public wrapper -- see the header. Deduplication by name is what we want here
 * too: every OpenSL vtable slot gets its own unique name, so each ends up with
 * its own index, which is how the handler knows which method was called. */
uint32_t pvz2_elf_add_trampoline(pvz2_elf_image_t *img, const char *name) {
    if (img == NULL || name == NULL) return 0;
    return get_or_create_trampoline(img, name);
}

/* Gives an imported DATA symbol its own block of real guest memory (see
 * PVZ2_DATA_IMPORT_BASE). The block starts zeroed; dependency modules fill in
 * the ones whose contents matter. */
static uint32_t get_or_create_data_import(pvz2_elf_image_t *img, const char *name) {
    for (uint32_t i = 0; i < img->data_import_count; ++i) {
        if (strcmp(img->data_import_names[i], name) == 0) {
            return PVZ2_DATA_IMPORT_BASE + i * PVZ2_DATA_IMPORT_SLOT;
        }
    }
    if (img->data_import_count >= PVZ2_DATA_IMPORT_MAX) {
        pvz2_log("[elf32] data-import table full (max %u), cannot import '%s' -- "
                "raise PVZ2_DATA_IMPORT_MAX", (unsigned)PVZ2_DATA_IMPORT_MAX, name);
        img->import_overflow = 1;
        return 0;
    }
    uint32_t idx = img->data_import_count++;
    img->data_import_names[idx] = dup_str(name);
    img->data_import_addrs[idx] = PVZ2_DATA_IMPORT_BASE + idx * PVZ2_DATA_IMPORT_SLOT;
    return img->data_import_addrs[idx];
}

/* --- symbol resolution ------------------------------------------------------ */

/* A defined symbol of this name in this module, or 0. */
static uint32_t module_lookup(const pvz2_elf_module_t *m, const char *name) {
    const Elf32_Sym *syms = (const Elf32_Sym *)m->dynsym;
    for (uint32_t i = 1; i < m->dynsym_count; ++i) { /* index 0 is the null symbol */
        if (syms[i].st_shndx == SHN_UNDEF) continue;
        if (strcmp(m->dynstr + syms[i].st_name, name) == 0) {
            return m->base + syms[i].st_value;
        }
    }
    return 0;
}

/* Resolves a symbol reference (relocation target) to an address in the emulated
 * address space: the referencing module's own definition, else a definition
 * exported by any other mapped module, else a host trampoline (code) or a block
 * of guest memory (data).
 *
 * `cache` memoises by symbol index. Without it this is the boot's worst hot
 * spot: libPVZ2.so has ~13000 symbol-bearing relocations over 514 distinct
 * undefined symbols, and every one of them would re-scan ~9600 dynamic symbols
 * across the module set. */
static uint32_t resolve_symbol(pvz2_elf_image_t *img, uint32_t mod_index, uint32_t symidx,
                               uint32_t *cache) {
    const pvz2_elf_module_t *m = &img->modules[mod_index];
    const Elf32_Sym *sym = (const Elf32_Sym *)(m->dynsym) + symidx;
    if (sym->st_shndx != SHN_UNDEF) {
        return m->base + sym->st_value;
    }
    if (cache != NULL && cache[symidx] != 0) {
        return cache[symidx];
    }

    const char *name = m->dynstr + sym->st_name;
    uint32_t addr = 0;
    for (uint32_t j = 0; j < img->module_count && addr == 0; ++j) {
        if (j == mod_index) continue;
        addr = module_lookup(&img->modules[j], name);
    }
    if (addr == 0) {
        addr = (ELF32_ST_TYPE(sym->st_info) == STT_OBJECT)
                   ? get_or_create_data_import(img, name)
                   : get_or_create_trampoline(img, name);
    }
    if (cache != NULL) cache[symidx] = addr;
    return addr;
}

static void apply_relocations(pvz2_elf_image_t *img, uint32_t mod_index, uint32_t rel_vaddr,
                              uint32_t rel_size, uint32_t *cache) {
    if (rel_vaddr == 0 || rel_size == 0) {
        return;
    }
    const uint32_t base = img->modules[mod_index].base;
    const Elf32_Rel *rel = (const Elf32_Rel *)(img->mem + base + rel_vaddr);
    uint32_t count = rel_size / (uint32_t)sizeof(Elf32_Rel);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = ELF32_R_TYPE(rel[i].r_info);
        uint32_t symidx = ELF32_R_SYM(rel[i].r_info);
        uint8_t *target = img->mem + base + rel[i].r_offset;

        switch (type) {
            case R_ARM_RELATIVE: {
                uint32_t addend = read32(target);
                write32(target, addend + base);
                break;
            }
            case R_ARM_ABS32: {
                uint32_t addend = read32(target);
                uint32_t sym_addr = resolve_symbol(img, mod_index, symidx, cache);
                write32(target, addend + sym_addr);
                break;
            }
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: {
                uint32_t sym_addr = resolve_symbol(img, mod_index, symidx, cache);
                write32(target, sym_addr);
                break;
            }
            case R_ARM_COPY:
                pvz2_log("[elf32] unexpected R_ARM_COPY relocation, skipping");
                break;
            default:
                pvz2_log("[elf32] unhandled relocation type %u at offset 0x%08x",
                        type, rel[i].r_offset);
                break;
        }
    }
}

/* --- mapping ---------------------------------------------------------------- */

/* Reads a whole file. Caller frees. */
static uint8_t *read_file(const char *path, long *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)file_size);
    if (!buf || fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = file_size;
    return buf;
}

static int validate_header(const Elf32_Ehdr *eh, long file_size, const char *path) {
    if ((size_t)file_size < sizeof(Elf32_Ehdr) ||
        eh->e_ident[EI_MAG0] != 0x7f || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L' || eh->e_ident[EI_MAG3] != 'F') {
        pvz2_log("[elf32] '%s' is not an ELF file", path);
        return -1;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM) {
        pvz2_log("[elf32] '%s' is not a 32-bit ARM ELF (class=%d machine=%d)",
                path, eh->e_ident[EI_CLASS], eh->e_machine);
        return -1;
    }
    if (eh->e_type != ET_DYN && eh->e_type != ET_EXEC) {
        pvz2_log("[elf32] '%s' has unsupported e_type %d", path, eh->e_type);
        return -1;
    }
    return 0;
}

/* The highest (vaddr + memsz) among a file's PT_LOAD segments, i.e. how much
 * address space mapping it will need -- read WITHOUT pulling the whole file in.
 *
 * pvz2_elf_load has to know this before it can allocate the address space, and
 * it used to get it by calling read_file() and throwing the buffer away, then
 * map_module() read the same file again from scratch. That is 18MB read twice for
 * 4.5.2 and 30MB twice for 9.6.1, on every single launch, to learn one number
 * that lives in the first few hundred bytes.
 *
 * Returns 0 on success. Diagnostics match read_file/validate_header so a bad file
 * reports the same way whichever path notices first. */
static int peek_load_span(const char *path, uint32_t *span_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        pvz2_log("[elf32] cannot open '%s'", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long file_size = ftell(f);

    Elf32_Ehdr eh;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(&eh, 1, sizeof(eh), f) != sizeof(eh) ||
        validate_header(&eh, file_size, path) != 0) {
        fclose(f);
        return -1;
    }

    const size_t phdr_bytes = (size_t)eh.e_phnum * sizeof(Elf32_Phdr);
    if (eh.e_phnum == 0 || (long)(eh.e_phoff + phdr_bytes) > file_size) {
        pvz2_log("[elf32] '%s' has no usable program headers", path);
        fclose(f);
        return -1;
    }
    Elf32_Phdr *phdrs = (Elf32_Phdr *)malloc(phdr_bytes);
    if (!phdrs || fseek(f, (long)eh.e_phoff, SEEK_SET) != 0 ||
        fread(phdrs, 1, phdr_bytes, f) != phdr_bytes) {
        free(phdrs);
        fclose(f);
        pvz2_log("[elf32] '%s' has a truncated program header table", path);
        return -1;
    }
    fclose(f);

    uint32_t span = 0;
    for (int i = 0; i < eh.e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint32_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > span) span = end;
    }
    free(phdrs);
    *span_out = span;
    return 0;
}

/* Maps one shared object at `base` and fills in modules[index]. Does NOT
 * relocate -- every module must be mapped before any is relocated, or a
 * cross-module reference would resolve to a host trampoline instead of the real
 * code. Returns 0 on success. */
static int map_module(pvz2_elf_image_t *img, const char *path, uint32_t base, uint32_t index,
                      uint32_t *entry_out) {
    long file_size = 0;
    uint8_t *file_buf = read_file(path, &file_size);
    if (!file_buf) {
        pvz2_log("[elf32] cannot read '%s'", path);
        return -1;
    }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file_buf;
    if (validate_header(eh, file_size, path) != 0) {
        free(file_buf);
        return -1;
    }

    pvz2_elf_module_t *m = &img->modules[index];
    memset(m, 0, sizeof(*m));
    m->base = base;

    const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(file_buf + eh->e_phoff);
    uint32_t span = 0, dynamic_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_LOAD) {
            uint32_t end = ph->p_vaddr + ph->p_memsz;
            if (end > span) span = end;
            /* Executable and not writable: the range the JIT may treat as
             * constant (see text_vaddr in the header). Modules have exactly one
             * such segment in practice, but a union is taken rather than assumed
             * so a split layout degrades into a bigger-but-still-correct range
             * instead of silently covering only part of .text. */
            if ((ph->p_flags & PF_X) != 0 && (ph->p_flags & PF_W) == 0) {
                uint32_t lo = ph->p_vaddr;
                uint32_t hi = end;
                if (m->text_size == 0) {
                    m->text_vaddr = lo;
                    m->text_size = hi - lo;
                } else {
                    if (lo < m->text_vaddr) {
                        m->text_size += m->text_vaddr - lo;
                        m->text_vaddr = lo;
                    }
                    if (hi > m->text_vaddr + m->text_size) {
                        m->text_size = hi - m->text_vaddr;
                    }
                }
            }
        } else if (ph->p_type == PT_ARM_EXIDX) {
            /* The ARM exception index table. dl_unwind_find_exidx needs its
             * address and entry count to unwind C++ exceptions; without them
             * a throw cannot find its handler. */
            m->exidx_vaddr = ph->p_vaddr;
            m->exidx_size = ph->p_filesz;
        } else if (ph->p_type == PT_DYNAMIC) {
            dynamic_vaddr = ph->p_vaddr;
        }
    }
    if (dynamic_vaddr == 0) {
        pvz2_log("[elf32] '%s' has no PT_DYNAMIC segment", path);
        free(file_buf);
        return -1;
    }
    if (base + span > img->mem_size) {
        pvz2_log("[elf32] no room for '%s' at 0x%08x (needs 0x%x, space is 0x%08x)",
                path, base, span, img->mem_size);
        free(file_buf);
        return -1;
    }

    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        memcpy(img->mem + base + ph->p_vaddr, file_buf + ph->p_offset, ph->p_filesz);
        /* remaining bytes up to p_memsz (BSS) are already zero from calloc */
    }

    m->span = span;
    m->dynamic_vaddr = dynamic_vaddr;

    /* Walk PT_DYNAMIC for the pieces needed to look symbols up. Relocation
     * tables are re-read later, once every module is mapped. */
    uint32_t strtab_vaddr = 0, symtab_vaddr = 0, hash_vaddr = 0, soname_off = 0;
    uint32_t init_array_size = 0;
    const Elf32_Dyn *dyn = (const Elf32_Dyn *)(img->mem + base + dynamic_vaddr);
    for (uint32_t i = 0; dyn[i].d_tag != DT_NULL; ++i) {
        switch (dyn[i].d_tag) {
            case DT_STRTAB: strtab_vaddr = dyn[i].d_un.d_val; break;
            case DT_SYMTAB: symtab_vaddr = dyn[i].d_un.d_val; break;
            case DT_HASH:   hash_vaddr   = dyn[i].d_un.d_val; break;
            case DT_SONAME: soname_off   = dyn[i].d_un.d_val; break;
            case DT_INIT:   m->init_vaddr = dyn[i].d_un.d_val; break;
            case DT_INIT_ARRAY:   m->init_array_vaddr = dyn[i].d_un.d_val; break;
            case DT_INIT_ARRAYSZ: init_array_size = dyn[i].d_un.d_val; break;
            default: break;
        }
    }
    if (!strtab_vaddr || !symtab_vaddr) {
        pvz2_log("[elf32] '%s' missing DT_STRTAB/DT_SYMTAB", path);
        free(file_buf);
        return -1;
    }
    m->dynstr = (const char *)(img->mem + base + strtab_vaddr);
    m->dynsym = img->mem + base + symtab_vaddr;
    m->init_array_count = init_array_size / 4u;

    if (hash_vaddr) {
        const uint32_t *hash = (const uint32_t *)(img->mem + base + hash_vaddr);
        m->dynsym_count = hash[1]; /* nchain == number of dynamic symbols, by convention */
    } else {
        /* GNU-hash-only. Relocations are unaffected (they index .dynsym
         * directly), but nothing can be looked up BY NAME in this module -- so
         * it can neither export to another module nor be searched by
         * pvz2_elf_find_symbol. Worth saying out loud rather than behaving as
         * if the module exported nothing. */
        pvz2_log("[elf32] '%s' has no DT_HASH (GNU hash only) -- its exports are "
                        "invisible to symbol lookup", path);
        m->dynsym_count = 0;
    }

    /* Name: DT_SONAME if present, else the filename. */
    const char *soname = soname_off ? (m->dynstr + soname_off) : NULL;
    if (!soname || !*soname) {
        const char *slash = strrchr(path, '/');
        const char *back = strrchr(path, '\\');
        if (back > slash) slash = back;
        soname = slash ? slash + 1 : path;
    }
    snprintf(m->name, sizeof(m->name), "%s", soname);

    if (entry_out) *entry_out = base + eh->e_entry;
    free(file_buf);
    return 0;
}

/* --- DT_NEEDED discovery ----------------------------------------------------- */

static int module_index_by_name(const pvz2_elf_image_t *img, const char *name) {
    for (uint32_t i = 0; i < img->module_count; ++i) {
        if (strcmp(img->modules[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Maps every DT_NEEDED library that ships beside the main .so, transitively.
 * `dir` is the main .so's directory including its trailing separator.
 *
 * Returns 0, or -1 if a library that IS shipped could not be mapped. Only one
 * thing is skipped quietly: a DT_NEEDED name with no file beside the .so, which
 * is every system library and is answered by the host shims. Anything else is
 * fatal -- see PVZ2_MAX_MODULES for why continuing is worse than refusing. */
static int map_dependencies(pvz2_elf_image_t *img, const char *dir, uint32_t *next_base) {
    /* Breadth-first over the mapped set: index `i` walks modules that exist,
     * appending any new ones it needs, so transitive dependencies are picked up
     * without recursion. */
    for (uint32_t i = 0; i < img->module_count; ++i) {
        pvz2_elf_module_t *m = &img->modules[i];
        const Elf32_Dyn *dyn = (const Elf32_Dyn *)(img->mem + m->base + m->dynamic_vaddr);
        for (uint32_t k = 0; dyn[k].d_tag != DT_NULL; ++k) {
            if (dyn[k].d_tag != DT_NEEDED) continue;
            const char *name = m->dynstr + dyn[k].d_un.d_val;

            int idx = module_index_by_name(img, name);
            if (idx < 0) {
                char path[1024];
                snprintf(path, sizeof(path), "%s%s", dir, name);
                if (!file_exists(path)) {
                    /* The normal case for libc.so, libm.so, libGLESv2.so and the
                     * rest of the system libraries: not shipped, and answered by
                     * the host shim layer instead. */
                    continue;
                }
                if (img->module_count >= PVZ2_MAX_MODULES) {
                    pvz2_log("[elf32] '%s' needs '%s', but the module table is full "
                            "(%u mapped, max %u) -- raise PVZ2_MAX_MODULES in elf32_loader.h",
                            m->name, name, img->module_count, (unsigned)PVZ2_MAX_MODULES);
                    return -1;
                }
                uint32_t base = (*next_base + PVZ2_MODULE_ALIGN - 1) & ~(PVZ2_MODULE_ALIGN - 1);
                uint32_t index = img->module_count;
                if (map_module(img, path, base, index, NULL) != 0) {
                    pvz2_log("[elf32] '%s' needs '%s', which ships at '%s' but could "
                            "not be mapped (see the error above)", m->name, name, path);
                    return -1;
                }
                img->module_count = index + 1;
                *next_base = base + img->modules[index].span;
                idx = (int)index;
                pvz2_log("[elf32] dependency '%s' mapped at 0x%08x..0x%08x (%u KB, %u exports)",
                       img->modules[index].name, base, base + img->modules[index].span,
                       img->modules[index].span >> 10, img->modules[index].dynsym_count);
                /* map_module may have reallocated nothing, but `m` points into
                 * img->modules which is a fixed array -- still valid. dyn/name
                 * point into img->mem, which never moves either. */
            }
            /* needed[] is PVZ2_MAX_MODULES long and idx is a mapped module, so
             * this can only fill up if one DT_NEEDED name is listed repeatedly.
             * Dropping a duplicate costs nothing: it is used for constructor
             * ordering, where an edge already recorded says the same thing. */
            if (m->needed_count < PVZ2_MAX_MODULES) {
                m->needed[m->needed_count++] = (uint32_t)idx;
            }
        }
    }
    return 0;
}

/* --- constructor ordering ---------------------------------------------------- */

/* Post-order over DT_NEEDED: a module is appended only after everything it
 * depends on, so libc++_shared's constructors run before libPVZ2's. */
static void order_inits(const pvz2_elf_image_t *img, uint32_t index, uint8_t *state,
                        uint32_t *order, uint32_t *count) {
    if (state[index] != 0) return; /* done, or on the stack (a cycle) */
    state[index] = 1;
    const pvz2_elf_module_t *m = &img->modules[index];
    for (uint32_t i = 0; i < m->needed_count; ++i) {
        order_inits(img, m->needed[i], state, order, count);
    }
    state[index] = 2;
    order[(*count)++] = index;
}

/* --- public API -------------------------------------------------------------- */

int pvz2_elf_load(const char *path, uint32_t space_size, pvz2_elf_image_t *out) {
    memset(out, 0, sizeof(*out));

    /* Peek at the main image's span before allocating, so "space too small" is
     * still reported up front rather than as a failed mapping. Headers only --
     * map_module reads the file properly a moment later. */
    uint32_t so_span = 0;
    if (peek_load_span(path, &so_span) != 0) return -1;

    uint32_t trampoline_bytes = PVZ2_TRAMPOLINE_MAX * 4;
    uint32_t required = PVZ2_SO_BASE + so_span;
    if (required < PVZ2_TRAMPOLINE_BASE + trampoline_bytes) {
        required = PVZ2_TRAMPOLINE_BASE + trampoline_bytes;
    }
    if (space_size < required) {
        pvz2_log("[elf32] space_size 0x%08x too small, need at least 0x%08x",
                space_size, required);
        return -1;
    }

    /* One page of slack past mem_size. The JIT reaches guest memory through a
     * page table (see build_page_table in dynarmic_config.cpp), which resolves an
     * access to `mem + vaddr` and reads the full width from there without
     * re-checking the end of the buffer -- so an unaligned 8-byte load in the
     * very last guest page would read a few bytes past the allocation. The
     * slack absorbs that; mem_size still describes the addressable space. */
    out->mem = (uint8_t *)calloc(1, (size_t)space_size + PVZ2_MEM_GUARD_SLACK);
    if (!out->mem) {
        pvz2_log("[elf32] failed to allocate %u bytes of emulated address space", space_size);
        return -1;
    }
    out->mem_size = space_size;
    out->trampoline_base = PVZ2_TRAMPOLINE_BASE;
    out->trampoline_capacity = PVZ2_TRAMPOLINE_MAX;
    out->trampoline_names = (char **)calloc(PVZ2_TRAMPOLINE_MAX, sizeof(char *));

    /* Reserve trampoline index 0 as a "$halt" sentinel: callers can point an
     * emulated LR at (trampoline_base + 0) to detect "the guest function
     * returned" via CallSVC(0), without it colliding with a real import. */
    out->trampoline_names[0] = dup_str("$halt");
    out->trampoline_count = 1;
    write32(out->mem + out->trampoline_base, 0xEF000000u);

    /* --- map the main image, then everything it needs that ships with it --- */
    uint32_t entry = 0;
    if (map_module(out, path, PVZ2_SO_BASE, 0, &entry) != 0) {
        pvz2_elf_free(out);
        return -1;
    }
    out->module_count = 1;
    out->so_entry = entry;

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    {
        char *slash = strrchr(dir, '/');
        char *back = strrchr(dir, '\\');
        if (back > slash) slash = back;
        if (slash) slash[1] = '\0';
        else dir[0] = '\0';
    }
    uint32_t next_base = PVZ2_SO_BASE + out->modules[0].span;
    if (map_dependencies(out, dir, &next_base) != 0) {
        pvz2_elf_free(out);
        return -1;
    }

    /* --- relocate every module, now that they can all see each other --- */
    for (uint32_t i = 0; i < out->module_count; ++i) {
        pvz2_elf_module_t *m = &out->modules[i];
        uint32_t rel_vaddr = 0, rel_size = 0, jmprel_vaddr = 0, jmprel_size = 0;
        const Elf32_Dyn *dyn = (const Elf32_Dyn *)(out->mem + m->base + m->dynamic_vaddr);
        for (uint32_t k = 0; dyn[k].d_tag != DT_NULL; ++k) {
            switch (dyn[k].d_tag) {
                case DT_REL:      rel_vaddr    = dyn[k].d_un.d_val; break;
                case DT_RELSZ:    rel_size     = dyn[k].d_un.d_val; break;
                case DT_JMPREL:   jmprel_vaddr = dyn[k].d_un.d_val; break;
                case DT_PLTRELSZ: jmprel_size  = dyn[k].d_un.d_val; break;
                default: break;
            }
        }
        uint32_t *cache = (uint32_t *)calloc(m->dynsym_count ? m->dynsym_count : 1, sizeof(uint32_t));
        apply_relocations(out, i, rel_vaddr, rel_size, cache);
        apply_relocations(out, i, jmprel_vaddr, jmprel_size, cache);
        free(cache);
    }

    /* An import table that filled up leaves symbols bound to 0, which a guest
     * call reads as success rather than as a fault -- refuse the image instead.
     * Checked here and not at the overflow itself so the log lists EVERY symbol
     * that lost its binding, which is what says how much bigger the table needs
     * to be. */
    if (out->import_overflow) {
        pvz2_log("[elf32] refusing to run '%s': the import tables overflowed and the "
                "symbols named above are bound to address 0", path);
        pvz2_elf_free(out);
        return -1;
    }

    /* --- constructor order, and where the address space ends --- */
    uint8_t state[PVZ2_MAX_MODULES];
    uint32_t count = 0;
    memset(state, 0, sizeof(state));
    order_inits(out, 0, state, out->init_order, &count);
    for (uint32_t i = 0; i < out->module_count; ++i) { /* anything unreachable from main */
        if (state[i] == 0) order_inits(out, i, state, out->init_order, &count);
    }

    out->images_end = 0;
    for (uint32_t i = 0; i < out->module_count; ++i) {
        uint32_t end = out->modules[i].base + out->modules[i].span;
        if (end > out->images_end) out->images_end = end;
    }

    /* Legacy mirrors of modules[0] -- see the header. */
    out->so_base = out->modules[0].base;
    out->so_span = out->modules[0].span;
    out->dynsym = out->modules[0].dynsym;
    out->dynsym_count = out->modules[0].dynsym_count;
    out->dynstr = out->modules[0].dynstr;
    out->exidx_vaddr = out->modules[0].exidx_vaddr;
    out->exidx_size = out->modules[0].exidx_size;
    out->text_vaddr = out->modules[0].text_vaddr;
    out->text_size = out->modules[0].text_size;
    out->init_array_vaddr = out->modules[0].init_array_vaddr;
    out->init_array_count = out->modules[0].init_array_count;

    pvz2_log("[elf32] loaded '%s' at base=0x%08x span=0x%x entry=0x%08x symbols=%u imports=%u init_array=%u",
           path, out->so_base, out->so_span, out->so_entry, out->dynsym_count, out->trampoline_count,
           out->init_array_count);
    pvz2_log("[elf32] read-only (R+X) range 0x%08x..0x%08x (%u KB), foldable at translation time",
           out->so_base + out->text_vaddr, out->so_base + out->text_vaddr + out->text_size,
           out->text_size >> 10);
    if (out->module_count > 1) {
        char order[512];
        size_t used = 0;
        order[0] = '\0';
        for (uint32_t i = 0; i < count; ++i) {
            const char *nm = out->modules[out->init_order[i]].name;
            int n = snprintf(order + used, sizeof(order) - used, "%s%s",
                             used ? " " : "", nm ? nm : "?");
            if (n < 0 || (size_t)n >= sizeof(order) - used) break;
            used += (size_t)n;
        }
        pvz2_log("[elf32] %u modules mapped, images end at 0x%08x; constructor order: %s",
                 out->module_count, out->images_end, order);
    }

    return 0;
}

uint32_t pvz2_elf_find_symbol(const pvz2_elf_image_t *img, const char *name) {
    for (uint32_t i = 0; i < img->module_count; ++i) {
        uint32_t addr = module_lookup(&img->modules[i], name);
        if (addr != 0) return addr;
    }
    return 0;
}

uint32_t pvz2_elf_find_symbol_in(const pvz2_elf_module_t *m, const char *name) {
    return module_lookup(m, name);
}

const pvz2_elf_module_t *pvz2_elf_module_for_pc(const pvz2_elf_image_t *img, uint32_t pc) {
    for (uint32_t i = 0; i < img->module_count; ++i) {
        const pvz2_elf_module_t *m = &img->modules[i];
        if (pc >= m->base && pc < m->base + m->span) return m;
    }
    return NULL;
}

void pvz2_elf_free(pvz2_elf_image_t *img) {
    if (img->trampoline_names) {
        for (uint32_t i = 0; i < img->trampoline_count; ++i) {
            free(img->trampoline_names[i]);
        }
        free(img->trampoline_names);
    }
    for (uint32_t i = 0; i < img->data_import_count; ++i) {
        free(img->data_import_names[i]);
    }
    free(img->mem);
    memset(img, 0, sizeof(*img));
}
