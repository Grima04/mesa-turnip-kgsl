/*
 * Copyright 2014-2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ac_rtld.h"

#include <gelf.h>
#include <libelf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_binary.h"
#include "util/u_math.h"

// Old distributions may not have this enum constant
#define MY_EM_AMDGPU 224

#ifndef R_AMDGPU_NONE
#define R_AMDGPU_NONE 0
#define R_AMDGPU_ABS32_LO 1
#define R_AMDGPU_ABS32_HI 2
#define R_AMDGPU_ABS64 3
#define R_AMDGPU_REL32 4
#define R_AMDGPU_REL64 5
#define R_AMDGPU_ABS32 6
#define R_AMDGPU_GOTPCREL 7
#define R_AMDGPU_GOTPCREL32_LO 8
#define R_AMDGPU_GOTPCREL32_HI 9
#define R_AMDGPU_REL32_LO 10
#define R_AMDGPU_REL32_HI 11
#define R_AMDGPU_RELATIVE64 13
#endif

/* For the UMR disassembler. */
#define DEBUGGER_END_OF_CODE_MARKER	0xbf9f0000 /* invalid instruction */
#define DEBUGGER_NUM_MARKERS		5

struct ac_rtld_section {
	bool is_rx : 1;
	bool is_pasted_text : 1;
	uint64_t offset;
	const char *name;
};

struct ac_rtld_part {
	Elf *elf;
	struct ac_rtld_section *sections;
	unsigned num_sections;
};

static void report_erroraf(const char *fmt, va_list va)
{
	char *msg;
	int ret = asprintf(&msg, fmt, va);
	if (ret < 0)
		msg = "(asprintf failed)";

	fprintf(stderr, "ac_rtld error: %s\n", msg);

	if (ret >= 0)
		free(msg);
}

static void report_errorf(const char *fmt, ...) PRINTFLIKE(1, 2);

static void report_errorf(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	report_erroraf(fmt, va);
	va_end(va);
}

static void report_elf_errorf(const char *fmt, ...) PRINTFLIKE(1, 2);

static void report_elf_errorf(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	report_erroraf(fmt, va);
	va_end(va);

	fprintf(stderr, "ELF error: %s\n", elf_errmsg(elf_errno()));
}

/**
 * Open a binary consisting of one or more shader parts.
 *
 * \param binary the uninitialized struct
 * \param num_parts number of shader parts
 * \param elf_ptrs pointers to the in-memory ELF objects for each shader part
 * \param elf_sizes sizes (in bytes) of the in-memory ELF objects
 */
bool ac_rtld_open(struct ac_rtld_binary *binary, unsigned num_parts,
		  const char * const *elf_ptrs,
		  const size_t *elf_sizes)
{
	/* One of the libelf implementations
	 * (http://www.mr511.de/software/english.htm) requires calling
	 * elf_version() before elf_memory().
	 */
	elf_version(EV_CURRENT);

	memset(binary, 0, sizeof(*binary));
	binary->num_parts = num_parts;
	binary->parts = calloc(sizeof(*binary->parts), num_parts);
	if (!binary->parts)
		return false;

	uint64_t pasted_text_size = 0;
	uint64_t rx_align = 1;
	uint64_t rx_size = 0;

#define report_if(cond) \
	do { \
		if ((cond)) { \
			report_errorf(#cond); \
			goto fail; \
		} \
	} while (false)
#define report_elf_if(cond) \
	do { \
		if ((cond)) { \
			report_elf_errorf(#cond); \
			goto fail; \
		} \
	} while (false)

	/* First pass over all parts: open ELFs and determine the placement of
	 * sections in the memory image. */
	for (unsigned i = 0; i < num_parts; ++i) {
		struct ac_rtld_part *part = &binary->parts[i];
		part->elf = elf_memory((char *)elf_ptrs[i], elf_sizes[i]);
		report_elf_if(!part->elf);

		const Elf64_Ehdr *ehdr = elf64_getehdr(part->elf);
		report_elf_if(!ehdr);
		report_if(ehdr->e_machine != MY_EM_AMDGPU);

		size_t section_str_index;
		size_t num_shdrs;
		report_elf_if(elf_getshdrstrndx(part->elf, &section_str_index) < 0);
		report_elf_if(elf_getshdrnum(part->elf, &num_shdrs) < 0);

		part->num_sections = num_shdrs;
		part->sections = calloc(sizeof(*part->sections), num_shdrs);
		report_if(!part->sections);

		Elf_Scn *section = NULL;
		while ((section = elf_nextscn(part->elf, section))) {
			Elf64_Shdr *shdr = elf64_getshdr(section);
			struct ac_rtld_section *s = &part->sections[elf_ndxscn(section)];
			s->name = elf_strptr(part->elf, section_str_index, shdr->sh_name);
			report_elf_if(!s->name);

			/* Cannot actually handle linked objects yet */
			report_elf_if(shdr->sh_addr != 0);

			/* Alignment must be 0 or a power of two */
			report_elf_if(shdr->sh_addralign & (shdr->sh_addralign - 1));
			uint64_t sh_align = MAX2(shdr->sh_addralign, 1);

			if (shdr->sh_flags & SHF_ALLOC &&
			    shdr->sh_type != SHT_NOTE) {
				report_if(shdr->sh_flags & SHF_WRITE);

				s->is_rx = true;

				if (shdr->sh_flags & SHF_EXECINSTR) {
					report_elf_if(shdr->sh_size & 3);

					if (!strcmp(s->name, ".text"))
						s->is_pasted_text = true;
				}

				if (s->is_pasted_text) {
					s->offset = pasted_text_size;
					pasted_text_size += shdr->sh_size;
				} else {
					rx_align = align(rx_align, sh_align);
					rx_size = align(rx_size, sh_align);
					s->offset = rx_size;
					rx_size += shdr->sh_size;
				}
			}
		}
	}

	binary->rx_end_markers = pasted_text_size;
	pasted_text_size += 4 * DEBUGGER_NUM_MARKERS;

	/* Second pass: Adjust offsets of non-pasted text sections. */
	binary->rx_size = pasted_text_size;
	binary->rx_size = align(binary->rx_size, rx_align);

	for (unsigned i = 0; i < num_parts; ++i) {
		struct ac_rtld_part *part = &binary->parts[i];
		size_t num_shdrs;
		elf_getshdrnum(part->elf, &num_shdrs);

		for (unsigned j = 0; j < num_shdrs; ++j) {
			struct ac_rtld_section *s = &part->sections[j];
			if (s->is_rx && !s->is_pasted_text)
				s->offset += binary->rx_size;
		}
	}

	binary->rx_size += rx_size;

	return true;

#undef report_if
#undef report_elf_if

fail:
	ac_rtld_close(binary);
	return false;
}

void ac_rtld_close(struct ac_rtld_binary *binary)
{
	for (unsigned i = 0; i < binary->num_parts; ++i) {
		struct ac_rtld_part *part = &binary->parts[i];
		free(part->sections);
		elf_end(part->elf);
	}

	free(binary->parts);
	binary->parts = NULL;
	binary->num_parts = 0;
}

static bool get_section_by_name(struct ac_rtld_part *part, const char *name,
				const char **data, size_t *nbytes)
{
	for (unsigned i = 0; i < part->num_sections; ++i) {
		struct ac_rtld_section *s = &part->sections[i];
		if (s->name && !strcmp(name, s->name)) {
			Elf_Scn *target_scn = elf_getscn(part->elf, i);
			Elf_Data *target_data = elf_getdata(target_scn, NULL);
			if (!target_data) {
				report_elf_errorf("ac_rtld: get_section_by_name: elf_getdata");
				return false;
			}

			*data = target_data->d_buf;
			*nbytes = target_data->d_size;
			return true;
		}
	}
	return false;
}

bool ac_rtld_get_section_by_name(struct ac_rtld_binary *binary, const char *name,
				 const char **data, size_t *nbytes)
{
	assert(binary->num_parts == 1);
	return get_section_by_name(&binary->parts[0], name, data, nbytes);
}

bool ac_rtld_read_config(struct ac_rtld_binary *binary,
			 struct ac_shader_config *config)
{
	for (unsigned i = 0; i < binary->num_parts; ++i) {
		struct ac_rtld_part *part = &binary->parts[i];
		const char *config_data;
		size_t config_nbytes;

		if (!get_section_by_name(part, ".AMDGPU.config",
					 &config_data, &config_nbytes))
			return false;

		/* TODO: be precise about scratch use? */
		struct ac_shader_config c = {};
		ac_parse_shader_binary_config(config_data, config_nbytes, true, &c);

		config->num_sgprs = MAX2(config->num_sgprs, c.num_sgprs);
		config->num_vgprs = MAX2(config->num_vgprs, c.num_vgprs);
		config->spilled_sgprs = MAX2(config->spilled_sgprs, c.spilled_sgprs);
		config->spilled_vgprs = MAX2(config->spilled_vgprs, c.spilled_vgprs);
		config->scratch_bytes_per_wave = MAX2(config->scratch_bytes_per_wave,
						      c.scratch_bytes_per_wave);

		assert(i == 0 || config->float_mode == c.float_mode);
		config->float_mode = c.float_mode;

		/* SPI_PS_INPUT_ENA/ADDR can't be combined. Only the value from
		 * the main shader part is used. */
		assert(config->spi_ps_input_ena == 0 &&
		       config->spi_ps_input_addr == 0);
		config->spi_ps_input_ena = c.spi_ps_input_ena;
		config->spi_ps_input_addr = c.spi_ps_input_addr;

		/* TODO: consistently use LDS symbols for this */
		config->lds_size = MAX2(config->lds_size, c.lds_size);

		/* TODO: Should we combine these somehow? It's currently only
		 * used for radeonsi's compute, where multiple parts aren't used. */
		assert(config->rsrc1 == 0 && config->rsrc2 == 0);
		config->rsrc1 = c.rsrc1;
		config->rsrc2 = c.rsrc2;
	}

	return true;
}

static bool resolve_symbol(const struct ac_rtld_upload_info *u,
			   unsigned part_idx, const Elf64_Sym *sym,
			   const char *name, uint64_t *value)
{
	if (sym->st_shndx == SHN_UNDEF) {
		/* TODO: resolve from other parts */

		if (u->get_external_symbol(u->cb_data, name, value))
			return true;

		report_errorf("symbol %s: unknown", name);
		return false;
	}

	struct ac_rtld_part *part = &u->binary->parts[part_idx];
	if (sym->st_shndx >= part->num_sections) {
		report_errorf("symbol %s: section out of bounds", name);
		return false;
	}

	struct ac_rtld_section *s = &part->sections[sym->st_shndx];
	if (!s->is_rx) {
		report_errorf("symbol %s: bad section", name);
		return false;
	}

	uint64_t section_base = u->rx_va + s->offset;

	*value = section_base + sym->st_value;
	return true;
}

static bool apply_relocs(const struct ac_rtld_upload_info *u,
			 unsigned part_idx, const Elf64_Shdr *reloc_shdr,
			 const Elf_Data *reloc_data)
{
#define report_if(cond) \
	do { \
		if ((cond)) { \
			report_errorf(#cond); \
			return false; \
		} \
	} while (false)
#define report_elf_if(cond) \
	do { \
		if ((cond)) { \
			report_elf_errorf(#cond); \
			return false; \
		} \
	} while (false)

	struct ac_rtld_part *part = &u->binary->parts[part_idx];
	Elf_Scn *target_scn = elf_getscn(part->elf, reloc_shdr->sh_info);
	report_elf_if(!target_scn);

	Elf_Data *target_data = elf_getdata(target_scn, NULL);
	report_elf_if(!target_data);

	Elf_Scn *symbols_scn = elf_getscn(part->elf, reloc_shdr->sh_link);
	report_elf_if(!symbols_scn);

	Elf64_Shdr *symbols_shdr = elf64_getshdr(symbols_scn);
	report_elf_if(!symbols_shdr);
	uint32_t strtabidx = symbols_shdr->sh_link;

	Elf_Data *symbols_data = elf_getdata(symbols_scn, NULL);
	report_elf_if(!symbols_data);

	const Elf64_Sym *symbols = symbols_data->d_buf;
	size_t num_symbols = symbols_data->d_size / sizeof(Elf64_Sym);

	struct ac_rtld_section *s = &part->sections[reloc_shdr->sh_info];
	report_if(!s->is_rx);

	const char *orig_base = target_data->d_buf;
	char *dst_base = u->rx_ptr + s->offset;
	uint64_t va_base = u->rx_va + s->offset;

	Elf64_Rel *rel = reloc_data->d_buf;
	size_t num_relocs = reloc_data->d_size / sizeof(*rel);
	for (size_t i = 0; i < num_relocs; ++i, ++rel) {
		size_t r_sym = ELF64_R_SYM(rel->r_info);
		unsigned r_type = ELF64_R_TYPE(rel->r_info);

		const char *orig_ptr = orig_base + rel->r_offset;
		char *dst_ptr = dst_base + rel->r_offset;
		uint64_t va = va_base + rel->r_offset;

		uint64_t symbol;
		uint64_t addend;

		if (r_sym == STN_UNDEF) {
			symbol = 0;
		} else {
			report_elf_if(r_sym >= num_symbols);

			const Elf64_Sym *sym = &symbols[r_sym];
			const char *symbol_name =
				elf_strptr(part->elf, strtabidx, sym->st_name);
			report_elf_if(!symbol_name);

			if (!resolve_symbol(u, part_idx, sym, symbol_name, &symbol))
				return false;
		}

		/* TODO: Should we also support .rela sections, where the
		 * addend is part of the relocation record? */

		/* Load the addend from the ELF instead of the destination,
		 * because the destination may be in VRAM. */
		switch (r_type) {
		case R_AMDGPU_ABS32:
		case R_AMDGPU_ABS32_LO:
		case R_AMDGPU_ABS32_HI:
		case R_AMDGPU_REL32:
		case R_AMDGPU_REL32_LO:
		case R_AMDGPU_REL32_HI:
			addend = *(const uint32_t *)orig_ptr;
			break;
		case R_AMDGPU_ABS64:
		case R_AMDGPU_REL64:
			addend = *(const uint64_t *)orig_ptr;
			break;
		default:
			report_errorf("unsupported r_type == %u", r_type);
			return false;
		}

		uint64_t abs = symbol + addend;

		switch (r_type) {
		case R_AMDGPU_ABS32:
			assert((uint32_t)abs == abs);
		case R_AMDGPU_ABS32_LO:
			*(uint32_t *)dst_ptr = util_cpu_to_le32(abs);
			break;
		case R_AMDGPU_ABS32_HI:
			*(uint32_t *)dst_ptr = util_cpu_to_le32(abs >> 32);
			break;
		case R_AMDGPU_ABS64:
			*(uint64_t *)dst_ptr = util_cpu_to_le64(abs);
			break;
		case R_AMDGPU_REL32:
			assert((int64_t)(int32_t)(abs - va) == (int64_t)(abs - va));
		case R_AMDGPU_REL32_LO:
			*(uint32_t *)dst_ptr = util_cpu_to_le32(abs - va);
			break;
		case R_AMDGPU_REL32_HI:
			*(uint32_t *)dst_ptr = util_cpu_to_le32((abs - va) >> 32);
			break;
		case R_AMDGPU_REL64:
			*(uint64_t *)dst_ptr = util_cpu_to_le64(abs - va);
			break;
		default:
			unreachable("bad r_type");
		}
	}

	return true;

#undef report_if
#undef report_elf_if
}

/**
 * Upload the binary or binaries to the provided GPU buffers, including
 * relocations.
 */
bool ac_rtld_upload(struct ac_rtld_upload_info *u)
{
#define report_if(cond) \
	do { \
		if ((cond)) { \
			report_errorf(#cond); \
			return false; \
		} \
	} while (false)
#define report_elf_if(cond) \
	do { \
		if ((cond)) { \
			report_errorf(#cond); \
			return false; \
		} \
	} while (false)

	/* First pass: upload raw section data. */
	for (unsigned i = 0; i < u->binary->num_parts; ++i) {
		struct ac_rtld_part *part = &u->binary->parts[i];
		Elf_Scn *section = NULL;
		while ((section = elf_nextscn(part->elf, section))) {
			Elf64_Shdr *shdr = elf64_getshdr(section);
			struct ac_rtld_section *s = &part->sections[elf_ndxscn(section)];

			if (!s->is_rx)
				continue;

			report_if(shdr->sh_type != SHT_PROGBITS);

			Elf_Data *data = elf_getdata(section, NULL);
			report_elf_if(!data || data->d_size != shdr->sh_size);
			memcpy(u->rx_ptr + s->offset, data->d_buf, shdr->sh_size);
		}
	}

	if (u->binary->rx_end_markers) {
		uint32_t *dst = (uint32_t *)(u->rx_ptr + u->binary->rx_end_markers);
		for (unsigned i = 0; i < DEBUGGER_NUM_MARKERS; ++i)
			*dst++ = util_cpu_to_le32(DEBUGGER_END_OF_CODE_MARKER);
	}

	/* Second pass: handle relocations, overwriting uploaded data where
	 * appropriate. */
	for (unsigned i = 0; i < u->binary->num_parts; ++i) {
		struct ac_rtld_part *part = &u->binary->parts[i];
		Elf_Scn *section = NULL;
		while ((section = elf_nextscn(part->elf, section))) {
			Elf64_Shdr *shdr = elf64_getshdr(section);
			if (shdr->sh_type == SHT_REL) {
				Elf_Data *relocs = elf_getdata(section, NULL);
				report_elf_if(!relocs || relocs->d_size != shdr->sh_size);
				if (!apply_relocs(u, i, shdr, relocs))
					return false;
			} else if (shdr->sh_type == SHT_RELA) {
				report_errorf("SHT_RELA not supported");
				return false;
			}
		}
	}

	return true;

#undef report_if
#undef report_elf_if
}
