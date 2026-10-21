/*
    m68kmmu.h - PMMU implementation for 68851/68030/68040

    By R. Belmont

    Copyright Nicola Salmoria and the MAME Team.
    Visit http://mamedev.org for licensing and usage restrictions.
*/

#if M68K_EMULATE_PMMU
/*
	pmmu_translate_addr: perform 68851/68030-style PMMU address translation
*/
uint pmmu_translate_addr(uint addr_in)
{
	uint32 addr_out, tbl_entry = 0, tbl_entry2, tamode = 0, tbmode = 0, tcmode = 0;
	uint root_aptr, root_limit, tofs, is, abits, bbits, cbits;
	uint resolved, tptr, shift;

	resolved = 0;
	addr_out = addr_in;

	// if SRP is enabled and we're in supervisor mode, use it
	if ((m68ki_cpu.mmu_tc & 0x02000000) && (m68ki_get_sr() & 0x2000))
	{
		root_aptr = m68ki_cpu.mmu_srp_aptr;
		root_limit = m68ki_cpu.mmu_srp_limit;
	}
	else	// else use the CRP
	{
		root_aptr = m68ki_cpu.mmu_crp_aptr;
		root_limit = m68ki_cpu.mmu_crp_limit;
	}

	// get initial shift (# of top bits to ignore)
	is = (m68ki_cpu.mmu_tc>>16) & 0xf;
	abits = (m68ki_cpu.mmu_tc>>12)&0xf;
	bbits = (m68ki_cpu.mmu_tc>>8)&0xf;
	cbits = (m68ki_cpu.mmu_tc>>4)&0xf;

//	fprintf(stderr,"PMMU: tcr %08x limit %08x aptr %08x is %x abits %d bbits %d cbits %d\n", m68ki_cpu.mmu_tc, root_limit, root_aptr, is, abits, bbits, cbits);

	// get table A offset
	tofs = (addr_in<<is)>>(32-abits);

	// find out what format table A is
	switch (root_limit & 3)
	{
		case 0:	// invalid, should cause MMU exception
		case 1:	// page descriptor, should cause direct mapping
			fatalerror("680x0 PMMU: Unhandled root mode\n");
			break;

		case 2:	// valid 4 byte descriptors
			tofs *= 4;
//			fprintf(stderr,"PMMU: reading table A entry at %08x\n", tofs + (root_aptr & 0xfffffffc));
			tbl_entry = m68k_read_memory_32( tofs + (root_aptr & 0xfffffffc));
			tamode = tbl_entry & 3;
//			fprintf(stderr,"PMMU: addr %08x entry %08x mode %x tofs %x\n", addr_in, tbl_entry, tamode, tofs);
			break;

		case 3: // valid 8 byte descriptors
			tofs *= 8;
//			fprintf(stderr,"PMMU: reading table A entries at %08x\n", tofs + (root_aptr & 0xfffffffc));
			tbl_entry2 = m68k_read_memory_32( tofs + (root_aptr & 0xfffffffc));
			tbl_entry = m68k_read_memory_32( tofs + (root_aptr & 0xfffffffc)+4);
			tamode = tbl_entry2 & 3;
//			fprintf(stderr,"PMMU: addr %08x entry %08x entry2 %08x mode %x tofs %x\n", addr_in, tbl_entry, tbl_entry2, tamode, tofs);
			break;
	}

	// get table B offset and pointer
	tofs = (addr_in<<(is+abits))>>(32-bbits);
	tptr = tbl_entry & 0xfffffff0;

	// find out what format table B is, if any
	switch (tamode)
	{
		case 0: // invalid, should cause MMU exception
			fatalerror("680x0 PMMU: Unhandled Table A mode %d (addr_in %08x)\n", tamode, addr_in);
			break;

		case 2: // 4-byte table B descriptor
			tofs *= 4;
//			fprintf(stderr,"PMMU: reading table B entry at %08x\n", tofs + tptr);
			tbl_entry = m68k_read_memory_32( tofs + tptr);
			tbmode = tbl_entry & 3;
//			fprintf(stderr,"PMMU: addr %08x entry %08x mode %x tofs %x\n", addr_in, tbl_entry, tbmode, tofs);
			break;

		case 3: // 8-byte table B descriptor
			tofs *= 8;
//			fprintf(stderr,"PMMU: reading table B entries at %08x\n", tofs + tptr);
			tbl_entry2 = m68k_read_memory_32( tofs + tptr);
			tbl_entry = m68k_read_memory_32( tofs + tptr + 4);
			tbmode = tbl_entry2 & 3;
//			fprintf(stderr,"PMMU: addr %08x entry %08x entry2 %08x mode %x tofs %x\n", addr_in, tbl_entry, tbl_entry2, tbmode, tofs);
			break;

		case 1:	// early termination descriptor
			tbl_entry &= 0xffffff00;

			shift = is+abits;
			addr_out = ((addr_in<<shift)>>shift) + tbl_entry;
			resolved = 1;
			break;
	}

	// if table A wasn't early-out, continue to process table B
	if (!resolved)
	{
		// get table C offset and pointer
		tofs = (addr_in<<(is+abits+bbits))>>(32-cbits);
		tptr = tbl_entry & 0xfffffff0;

		switch (tbmode)
		{
			case 0:	// invalid, should cause MMU exception
				fatalerror("680x0 PMMU: Unhandled Table B mode %d (addr_in %08x PC %x)\n", tbmode, addr_in, REG_PC);
				break;

			case 2: // 4-byte table C descriptor
				tofs *= 4;
//				fprintf(stderr,"PMMU: reading table C entry at %08x\n", tofs + tptr);
				tbl_entry = m68k_read_memory_32(tofs + tptr);
				tcmode = tbl_entry & 3;
//				fprintf(stderr,"PMMU: addr %08x entry %08x mode %x tofs %x\n", addr_in, tbl_entry, tbmode, tofs);
				break;

			case 3: // 8-byte table C descriptor
				tofs *= 8;
//				fprintf(stderr,"PMMU: reading table C entries at %08x\n", tofs + tptr);
				tbl_entry2 = m68k_read_memory_32(tofs + tptr);
				tbl_entry = m68k_read_memory_32(tofs + tptr + 4);
				tcmode = tbl_entry2 & 3;
//				fprintf(stderr,"PMMU: addr %08x entry %08x entry2 %08x mode %x tofs %x\n", addr_in, tbl_entry, tbl_entry2, tbmode, tofs);
				break;

			case 1: // termination descriptor
				tbl_entry &= 0xffffff00;

				shift = is+abits+bbits;
				addr_out = ((addr_in<<shift)>>shift) + tbl_entry;
				resolved = 1;
				break;
		}
	}

	if (!resolved)
	{
		switch (tcmode)
		{
			case 0:	// invalid, should cause MMU exception
			case 2: // 4-byte ??? descriptor
			case 3: // 8-byte ??? descriptor
				fatalerror("680x0 PMMU: Unhandled Table B mode %d (addr_in %08x PC %x)\n", tbmode, addr_in, REG_PC);
				break;

			case 1: // termination descriptor
				tbl_entry &= 0xffffff00;

				shift = is+abits+bbits+cbits;
				addr_out = ((addr_in<<shift)>>shift) + tbl_entry;
				resolved = 1;
				break;
		}
	}


//	fprintf(stderr,"PMMU: [%08x] => [%08x]\n", addr_in, addr_out);

	return addr_out;
}
#endif /* M68K_EMULATE_PMMU */

/*
	m68881_mmu_ops: 68030 PMMU instruction handling (coprocessor ID 0).

	Instruction-level emulation only: PMOVE stores TC/SRP/CRP/TT0/TT1/MMUSR
	in CPU state, PLOAD/PFLUSH are no-ops (there is no ATC), and PTEST walks
	the translation tables in software to produce MMUSR and the optional
	descriptor address.  Unless M68K_EMULATE_PMMU is on, normal memory
	accesses are NOT translated (the host memory map treats the ROM's MMU
	setup as identity), so these instructions add zero cost to the hot path.
	Extension words are always consumed so the instruction length is right.
*/

/* Effective address of a memory operand (modes 2-7). size = operand bytes. */
static uint pmmu_ea_address(uint size)
{
	uint reg = REG_IR & 7;
	switch ((REG_IR >> 3) & 7)
	{
		case 2: return REG_A[reg];
		case 3: { uint a = REG_A[reg]; REG_A[reg] += size; return a; }
		case 4: REG_A[reg] -= size; return REG_A[reg];
		case 5: return EA_AY_DI_32();
		case 6: return EA_AY_IX_32();
		case 7:
			switch (reg)
			{
				case 0: return EA_AW_32();
				case 1: return EA_AL_32();
				case 2: return EA_PCDI_32();
				case 3: return EA_PCIX_32();
			}
			break;
	}
	return 0;
}

/* Read a 2/4/8-byte operand. For 8 bytes, *hi gets the first long. */
static void pmmu_read_ea(uint size, uint *hi, uint *lo)
{
	uint mode = (REG_IR >> 3) & 7, reg = REG_IR & 7, a;
	*hi = 0;
	if (mode == 0) { *lo = REG_D[reg]; return; }
	if (mode == 1) { *lo = REG_A[reg]; return; }
	if (mode == 7 && reg == 4)	/* #imm */
	{
		if (size == 2) { *lo = OPER_I_16(); return; }
		if (size == 8) *hi = OPER_I_32();
		*lo = OPER_I_32();
		return;
	}
	a = pmmu_ea_address(size);
	if (size == 2) { *lo = m68ki_read_16(a); return; }
	if (size == 8) { *hi = m68ki_read_32(a); a += 4; }
	*lo = m68ki_read_32(a);
}

static void pmmu_write_ea(uint size, uint hi, uint lo)
{
	uint mode = (REG_IR >> 3) & 7, reg = REG_IR & 7, a;
	if (mode == 0) { if (size == 2) REG_D[reg] = (REG_D[reg] & 0xffff0000) | (lo & 0xffff); else REG_D[reg] = lo; return; }
	if (mode == 1) { REG_A[reg] = lo; return; }
	a = pmmu_ea_address(size);
	if (size == 2) { m68ki_write_16(a, lo & 0xffff); return; }
	if (size == 8) { m68ki_write_32(a, hi); a += 4; }
	m68ki_write_32(a, lo);
}

/* Decode the 5-bit FC field of PLOAD/PFLUSH/PTEST. */
static uint pmmu_decode_fc(uint field)
{
	if (field == 0) return REG_SFC & 7;
	if (field == 1) return REG_DFC & 7;
	if ((field & 0x18) == 0x08) return REG_D[field & 7] & 7;
	return field & 7;	/* 10XXX: immediate */
}

static int pmmu_tt_match(uint tt, uint addr, uint fc)
{
	uint base, mask;
	if (!(tt & 0x8000)) return 0;
	base = (tt >> 24) & 0xff;
	mask = (tt >> 16) & 0xff;
	if (((addr >> 24) ^ base) & ~mask & 0xff) return 0;
	if (((fc ^ (tt >> 4)) & ~tt) & 7) return 0;	/* FC base bits 6-4, FC mask bits 2-0 */
	return 1;
}

/* Software table walk for PTEST. Returns MMUSR; *desc gets the address of
   the last descriptor fetched (0 if none). */
static uint pmmu_ptest_walk(uint addr, uint fc, uint maxlevel, uint *desc)
{
	uint tc = m68ki_cpu.mmu_tc;
	uint ti[4], shift, hi, lo, dt, table, dsz, lvl, d0, d1 = 0, next, mmusr = 0, n = 0;
	uint pmax = maxlevel ? maxlevel : 7;

	*desc = 0;
	if (pmmu_tt_match(m68ki_cpu.mmu_tt0, addr, fc) || pmmu_tt_match(m68ki_cpu.mmu_tt1, addr, fc))
		return 0x0040;	/* T */

	ti[0] = (tc >> 12) & 15; ti[1] = (tc >> 8) & 15; ti[2] = (tc >> 4) & 15; ti[3] = tc & 15;
	shift = (tc >> 16) & 15;	/* IS */

	if ((tc & 0x02000000) && (fc & 4)) { hi = m68ki_cpu.mmu_srp_limit; lo = m68ki_cpu.mmu_srp_aptr; }
	else                               { hi = m68ki_cpu.mmu_crp_limit; lo = m68ki_cpu.mmu_crp_aptr; }

	dt = hi & 3;
	if (dt == 0) return 0x0400;	/* I */
	if (dt == 1) return 0;		/* root is a page descriptor: early termination */
	table = lo & 0xfffffff0;
	dsz = (dt == 3) ? 8 : 4;

	for (lvl = 0; lvl < 4 && ti[lvl] && n < pmax; lvl++)
	{
		uint da = table + ((addr << shift) >> (32 - ti[lvl])) * dsz;
		shift += ti[lvl];
		n++;
		*desc = da;
		if (dsz == 8)
		{
			d0 = m68k_read_memory_32(ADDRESS_68K(da));
			d1 = m68k_read_memory_32(ADDRESS_68K(da + 4));
			if ((d0 & 0x100) && !(fc & 4)) mmusr |= 0x2000;	/* S */
			next = d1;
		}
		else
		{
			d0 = m68k_read_memory_32(ADDRESS_68K(da));
			next = d0;
		}
		if (d0 & 4) mmusr |= 0x0800;	/* WP */
		dt = d0 & 3;
		if (dt == 0) { mmusr |= 0x0400; break; }	/* invalid */
		if (dt == 1) { if (d0 & 0x10) mmusr |= 0x0200; break; }	/* page (or early-termination) */
		if (lvl == 3 || !ti[lvl + 1])
		{
			/* table-type descriptor at the last level = indirect descriptor */
			if (n >= pmax) break;
			da = next & ((dt == 3) ? 0xfffffff8 : 0xfffffffc);
			n++;
			*desc = da;
			d0 = m68k_read_memory_32(ADDRESS_68K(da));
			if (d0 & 4) mmusr |= 0x0800;
			if ((d0 & 3) != 1) mmusr |= 0x0400;
			else if (d0 & 0x10) mmusr |= 0x0200;
			break;
		}
		table = next & 0xfffffff0;
		dsz = (dt == 3) ? 8 : 4;
	}
	if (maxlevel) mmusr |= (n & 7);
	return mmusr;
}

void m68881_mmu_ops(void)
{
	uint modes, hi, lo;

	/* 1111 000 0xx: cpGEN only. cpScc/cpDBcc/cpTRAPcc/cpBcc forms do not exist on the 030 */
	if ((REG_IR & 0x01c0) != 0)
	{
		m68ki_exception_1111();
		return;
	}
	if (!FLAG_S)
	{
		m68ki_exception_privilege_violation();
		return;
	}

	modes = OPER_I_16();

	switch ((modes >> 13) & 7)
	{
		case 0:	/* PMOVE format 2: TT0/TT1 */
		case 2:	/* PMOVE format 1: TC/SRP/CRP */
		{
			uint preg = (modes >> 10) & 7;
			uint fmt1 = ((modes >> 13) & 7) == 2;
			uint size;
			uint *ph = 0, *pl = 0;
			if (modes & 0xff) break;
			if (fmt1)
			{
				switch (preg)
				{
					case 0: pl = &m68ki_cpu.mmu_tc; size = 4; break;
					case 2: ph = &m68ki_cpu.mmu_srp_limit; pl = &m68ki_cpu.mmu_srp_aptr; size = 8; break;
					case 3: ph = &m68ki_cpu.mmu_crp_limit; pl = &m68ki_cpu.mmu_crp_aptr; size = 8; break;
					default: goto bad;
				}
			}
			else
			{
				switch (preg)
				{
					case 2: pl = &m68ki_cpu.mmu_tt0; size = 4; break;
					case 3: pl = &m68ki_cpu.mmu_tt1; size = 4; break;
					default: goto bad;
				}
			}
			if (modes & 0x200)	/* MMU reg -> EA */
				pmmu_write_ea(size, ph ? *ph : 0, *pl);
			else
			{
				pmmu_read_ea(size, &hi, &lo);
				if (ph) *ph = hi;
				*pl = lo;
				if (pl == &m68ki_cpu.mmu_tc)
					m68ki_cpu.pmmu_enabled = (lo & 0x80000000) ? 1 : 0;
			}
			/* FD (bit 8) and any ATC flush: nothing to do without an ATC */
			return;
		}

		case 3:	/* PMOVE format 3: MMUSR */
			if ((modes & 0x1dff) != 0) break;
			if (modes & 0x200)
				pmmu_write_ea(2, 0, m68ki_cpu.mmu_sr);
			else
			{
				pmmu_read_ea(2, &hi, &lo);
				m68ki_cpu.mmu_sr = lo & 0xffff;
			}
			return;

		case 1:	/* PLOAD / PFLUSH / PFLUSHA */
		{
			uint pmode = (modes >> 10) & 7;
			if (pmode == 0)	/* PLOADR/PLOADW: 001 000 R 0000 FC */
			{
				if (modes & 0x01e0) break;
				(void)pmmu_decode_fc(modes & 0x1f);
				(void)pmmu_ea_address(4);
				return;
			}
			if (pmode == 1)	/* PFLUSHA */
				return;
			if (pmode == 4)	/* PFLUSH FC,#mask */
				return;
			if (pmode == 6)	/* PFLUSH FC,#mask,<ea> */
			{
				(void)pmmu_ea_address(4);
				return;
			}
			break;
		}

		case 4:	/* PTEST: 100 LEVEL R A REG FC */
		{
			uint level = (modes >> 10) & 7;
			uint fc = pmmu_decode_fc(modes & 0x1f);
			uint a = pmmu_ea_address(4);
			uint desc;
			m68ki_cpu.mmu_sr = (uint16)pmmu_ptest_walk(a, fc, level, &desc);
			if (modes & 0x100)
				REG_A[(modes >> 5) & 7] = desc;
			return;
		}
	}

bad:
	/* Unknown/invalid extension word: the 030 takes an F-line exception */
	m68ki_exception_1111();
}
