/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


#include "PrecompiledHeader.h"

#include "System.h"
#include "iR5900.h"
#include "Vif.h"
#include "VU.h"
#include "R3000A.h"

using namespace x86Emitter;

__tls_emit u8  *j8Ptr[32];
__tls_emit u32 *j32Ptr[32];

u16 g_x86AllocCounter = 0;
u16 g_xmmAllocCounter = 0;

EEINST* g_pCurInstInfo = NULL;

// used to make sure regs don't get changed while in recompiler
// use FreezeMMXRegs, FreezeXMMRegs
u32 g_recWriteback = 0;

_xmmregs xmmregs[iREGCNT_XMM], s_saveXMMregs[iREGCNT_XMM];

// X86 caching
_x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];

// XMM Caching
#define VU_VFx_ADDR(x)  (uptr)&VU->VF[x].UL[0]
#define VU_ACCx_ADDR    (uptr)&VU->ACC.UL[0]

static int s_xmmchecknext = 0;

// Clear current register mapping structure
// Clear allocation counter
void _initXMMregs() {
	memzero( xmmregs );
	g_xmmAllocCounter = 0;
	s_xmmchecknext = 0;
}

// Get a pointer to the physical register (GPR / FPU / VU etc..)
__fi void* _XMMGetAddr(int type, int reg, VURegs *VU)
{
	switch (type) {
		case XMMTYPE_VFREG:
			return (void*)VU_VFx_ADDR(reg);

		case XMMTYPE_ACC:
			return (void*)VU_ACCx_ADDR;

		case XMMTYPE_GPRREG:
			if( reg < 32 )
				pxAssert( !(g_cpuHasConstReg & (1<<reg)) || (g_cpuFlushedConstReg & (1<<reg)) );
			return &cpuRegs.GPR.r[reg].UL[0];

		case XMMTYPE_FPREG:
			return &fpuRegs.fpr[reg];

		case XMMTYPE_FPACC:
			return &fpuRegs.ACC.f;

		jNO_DEFAULT
	}

	return NULL;
}

// Get the index of a free register
// Step1: check any available register (inuse == 0)
// Step2: check registers that are not live (both EEINST_LIVE* are cleared)
// Step3: check registers that won't use SSE in the future (likely broken as EEINST_XMM isn't set properly)
// Step4: take a randome register
//
// Note: I don't understand why we don't check register that aren't useful anymore
// (i.e EEINST_USED is cleared)
int  _getFreeXMMreg()
{
	int i, tempi;
	u32 bestcount = 0x10000;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[(i+s_xmmchecknext)%iREGCNT_XMM].inuse == 0) {
			int ret = (s_xmmchecknext+i)%iREGCNT_XMM;
			s_xmmchecknext = (s_xmmchecknext+i+1)%iREGCNT_XMM;
			return ret;
		}
	}

	// check for dead regs
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].needed) continue;
		if (xmmregs[i].type == XMMTYPE_GPRREG ) {
			if (!(EEINST_ISLIVEXMM(xmmregs[i].reg))) {
				_freeXMMreg(i);
				return i;
			}
		}
	}

	// check for future xmm usage
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].needed) continue;
		if (xmmregs[i].type == XMMTYPE_GPRREG ) {
			if( !(g_pCurInstInfo->regs[xmmregs[i].reg] & EEINST_XMM) ) {
				_freeXMMreg(i);
				return i;
			}
		}
	}

	tempi = -1;
	bestcount = 0xffff;
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].needed) continue;
		if (xmmregs[i].type != XMMTYPE_TEMP) {

			if( xmmregs[i].counter < bestcount ) {
				tempi = i;
				bestcount = xmmregs[i].counter;
			}
			continue;
		}

		_freeXMMreg(i);
		return i;
	}

	if( tempi != -1 ) {
		_freeXMMreg(tempi);
		return tempi;
	}

	pxFailDev("*PCSX2*: XMM Reg Allocation Error in _getFreeXMMreg()!");
	throw Exception::FailedToAllocateRegister();
}

// Reserve a XMM register for temporary operation.
int _allocTempXMMreg(XMMSSEType type, int xmmreg) {
	if (xmmreg == -1)
		xmmreg = _getFreeXMMreg();
	else
		_freeXMMreg(xmmreg);

	xmmregs[xmmreg].inuse = 1;
	xmmregs[xmmreg].type = XMMTYPE_TEMP;
	xmmregs[xmmreg].needed = 1;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;
	g_xmmtypes[xmmreg] = type;

	return xmmreg;
}

#ifndef DISABLE_SVU
int _allocVFtoXMMreg(VURegs *VU, int xmmreg, int vfreg, int mode) {
	int i;
	int readfromreg = -1;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if ((xmmregs[i].inuse == 0)  || (xmmregs[i].type != XMMTYPE_VFREG) ||
		     (xmmregs[i].reg != vfreg) || (xmmregs[i].VU != XMM_CONV_VU(VU)))
			continue;

		if( xmmreg >= 0 ) {
			// requested specific reg, so return that instead
			if( i != xmmreg ) {
				if( xmmregs[i].mode & MODE_READ ) readfromreg = i;
				//if( xmmregs[i].mode & MODE_WRITE ) mode |= MODE_WRITE;
				mode |= xmmregs[i].mode&MODE_WRITE;
				xmmregs[i].inuse = 0;
				break;
			}
		}

		xmmregs[i].needed = 1;

		if( !(xmmregs[i].mode & MODE_READ) && (mode&MODE_READ) ) {
			xMOVAPS(xRegisterSSE(i), ptr[(void*)(VU_VFx_ADDR(vfreg))]);
			xmmregs[i].mode |= MODE_READ;
		}

		g_xmmtypes[i] = XMMT_FPS;
		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].mode|= mode;
		return i;
	}

	if (xmmreg == -1)
		xmmreg = _getFreeXMMreg();
	else
		_freeXMMreg(xmmreg);

	g_xmmtypes[xmmreg] = XMMT_FPS;
	xmmregs[xmmreg].inuse = 1;
	xmmregs[xmmreg].type = XMMTYPE_VFREG;
	xmmregs[xmmreg].reg = vfreg;
	xmmregs[xmmreg].mode = mode;
	xmmregs[xmmreg].needed = 1;
	xmmregs[xmmreg].VU = XMM_CONV_VU(VU);
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;
	if (mode & MODE_READ) {
		if( readfromreg >= 0 ) xMOVAPS(xRegisterSSE(xmmreg), xRegisterSSE(readfromreg));
		else xMOVAPS(xRegisterSSE(xmmreg), ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg))]);
	}

	return xmmreg;
}
#endif

// Search register "reg" of type "type" which is inuse
// If register doesn't have the read flag but mode is read
// then populate the register from the memory
// Note: There is a special HALF mode (to handle low 64 bits copy) but it seems to be unused
//
// So basically it is mostly used to set the mode of the register, and load value if we need to read it
int _checkXMMreg(int type, int reg, int mode)
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse && (xmmregs[i].type == (type&0xff)) && (xmmregs[i].reg == reg)) {

			if ( !(xmmregs[i].mode & MODE_READ) ) {
				if (mode & MODE_READ) {
					xMOVDQA(xRegisterSSE(i), ptr[_XMMGetAddr(xmmregs[i].type, xmmregs[i].reg, xmmregs[i].VU ? &VU1 : &VU0)]);
				}
				else if (mode & MODE_READHALF) {
					if( g_xmmtypes[i] == XMMT_INT )
						xMOVQZX(xRegisterSSE(i), ptr[(void*)(uptr)_XMMGetAddr(xmmregs[i].type, xmmregs[i].reg, xmmregs[i].VU ? &VU1 : &VU0)]);
					else
						xMOVL.PS(xRegisterSSE(i), ptr[(void*)(uptr)_XMMGetAddr(xmmregs[i].type, xmmregs[i].reg, xmmregs[i].VU ? &VU1 : &VU0)]);
				}
			}

			xmmregs[i].mode |= mode&~MODE_READHALF;
			xmmregs[i].counter = g_xmmAllocCounter++; // update counter
			xmmregs[i].needed = 1;
			return i;
		}
	}

	return -1;
}

#ifndef DISABLE_SVU
int _allocACCtoXMMreg(VURegs *VU, int xmmreg, int mode) {
	int i;
	int readfromreg = -1;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_ACC) continue;
		if (xmmregs[i].VU != XMM_CONV_VU(VU) ) continue;

		if( xmmreg >= 0 ) {
			// requested specific reg, so return that instead
			if( i != xmmreg ) {
				if( xmmregs[i].mode & MODE_READ ) readfromreg = i;
				//if( xmmregs[i].mode & MODE_WRITE ) mode |= MODE_WRITE;
				mode |= xmmregs[i].mode&MODE_WRITE;
				xmmregs[i].inuse = 0;
				break;
			}
		}

		if( !(xmmregs[i].mode & MODE_READ) && (mode&MODE_READ)) {
			xMOVAPS(xRegisterSSE(i), ptr[(void*)(VU_ACCx_ADDR)]);
			xmmregs[i].mode |= MODE_READ;
		}

		g_xmmtypes[i] = XMMT_FPS;
		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		xmmregs[i].mode|= mode;
		return i;
	}

	if (xmmreg == -1)
		xmmreg = _getFreeXMMreg();
	else
		_freeXMMreg(xmmreg);

	g_xmmtypes[xmmreg] = XMMT_FPS;
	xmmregs[xmmreg].inuse = 1;
	xmmregs[xmmreg].type = XMMTYPE_ACC;
	xmmregs[xmmreg].mode = mode;
	xmmregs[xmmreg].needed = 1;
	xmmregs[xmmreg].VU = XMM_CONV_VU(VU);
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;
	xmmregs[xmmreg].reg = 0;

	if (mode & MODE_READ)
	{
		if( readfromreg >= 0 )
			xMOVAPS(xRegisterSSE(xmmreg), xRegisterSSE(readfromreg));
		else
			xMOVAPS(xRegisterSSE(xmmreg), ptr[(void*)(VU_ACCx_ADDR)]);
	}

	return xmmreg;
}
#endif

// Fully allocate a FPU register
// first trial:
//     search an already reserved reg then populate it if we read it
// Second trial:
//     reserve a new reg, then populate it if we read it
//
// Note: FPU are always in XMM register
int _allocFPtoXMMreg(int xmmreg, int fpreg, int mode) {
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_FPREG) continue;
		if (xmmregs[i].reg != fpreg) continue;

		if( !(xmmregs[i].mode & MODE_READ) && (mode & MODE_READ)) {
			xMOVSSZX(xRegisterSSE(i), ptr[&fpuRegs.fpr[fpreg].f]);
			xmmregs[i].mode |= MODE_READ;
		}

		g_xmmtypes[i] = XMMT_FPS;
		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		xmmregs[i].mode|= mode;
		return i;
	}

	if (xmmreg == -1)
		xmmreg = _getFreeXMMreg();

	g_xmmtypes[xmmreg] = XMMT_FPS;
	xmmregs[xmmreg].inuse = 1;
	xmmregs[xmmreg].type = XMMTYPE_FPREG;
	xmmregs[xmmreg].reg = fpreg;
	xmmregs[xmmreg].mode = mode;
	xmmregs[xmmreg].needed = 1;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;

	if (mode & MODE_READ)
		xMOVSSZX(xRegisterSSE(xmmreg), ptr[&fpuRegs.fpr[fpreg].f]);

	return xmmreg;
}

// In short try to allocate a GPR register. Code is an uterly mess
// due to XMM/MMX/X86 crazyness !
int _allocGPRtoXMMreg(int xmmreg, int gprreg, int mode)
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_GPRREG) continue;
		if (xmmregs[i].reg != gprreg) continue;

		pxAssert( _checkMMXreg(MMX_GPR|gprreg, mode) == -1 );

		g_xmmtypes[i] = XMMT_INT;

		if (!(xmmregs[i].mode & MODE_READ) && (mode & MODE_READ))
		{
			if (gprreg == 0 )
			{
				xPXOR(xRegisterSSE(i), xRegisterSSE(i));
			}
			else
			{
				//pxAssert( !(g_cpuHasConstReg & (1<<gprreg)) || (g_cpuFlushedConstReg & (1<<gprreg)) );
				_flushConstReg(gprreg);
				xMOVDQA(xRegisterSSE(i), ptr[&cpuRegs.GPR.r[gprreg].UL[0]]);
			}
			xmmregs[i].mode |= MODE_READ;
		}

		if  ((mode & MODE_WRITE) && (gprreg < 32))
		{
			g_cpuHasConstReg &= ~(1<<gprreg);
			//pxAssert( !(g_cpuHasConstReg & (1<<gprreg)) );
		}

		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		xmmregs[i].mode|= mode;
		return i;
	}

	// currently only gpr regs are const
	// fixme - do we really need to execute this both here and in the loop?
	if ((mode & MODE_WRITE) && gprreg < 32)
	{
		//pxAssert( !(g_cpuHasConstReg & (1<<gprreg)) );
		g_cpuHasConstReg &= ~(1<<gprreg);
	}

	if (xmmreg == -1)
		xmmreg = _getFreeXMMreg();

	g_xmmtypes[xmmreg] = XMMT_INT;
	xmmregs[xmmreg].inuse = 1;
	xmmregs[xmmreg].type = XMMTYPE_GPRREG;
	xmmregs[xmmreg].reg = gprreg;
	xmmregs[xmmreg].mode = mode;
	xmmregs[xmmreg].needed = 1;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;

	if (mode & MODE_READ)
	{
		if (gprreg == 0 )
		{
			xPXOR(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
		}
		else
		{
			// DOX86
			int mmxreg;

			if (mode & MODE_READ) _flushConstReg(gprreg);

			mmxreg = _checkMMXreg(MMX_GPR+gprreg, 0);

			if (mmxreg >= 0 )
			{
				// transfer
				SetMMXstate();
				xMOVQ(xRegisterSSE(xmmreg), xRegisterMMX(mmxreg));
				xPUNPCK.LQDQ(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
				xPUNPCK.HQDQ(xRegisterSSE(xmmreg), ptr[&cpuRegs.GPR.r[gprreg].UL[0]]);

				if (mmxregs[mmxreg].mode & MODE_WRITE )
				{
					// instead of setting to write, just flush to mem
					if  (!(mode & MODE_WRITE))
					{
						SetMMXstate();
						xMOVQ(ptr[&cpuRegs.GPR.r[gprreg].UL[0]], xRegisterMMX(mmxreg));
					}
					//xmmregs[xmmreg].mode |= MODE_WRITE;
				}

				// don't flush
				mmxregs[mmxreg].inuse = 0;
			}
			else
				xMOVDQA(xRegisterSSE(xmmreg), ptr[&cpuRegs.GPR.r[gprreg].UL[0]]);
		}
	}
	else
	_deleteMMXreg(MMX_GPR+gprreg, 0);

	return xmmreg;
}

// Same code as _allocFPtoXMMreg but for the FPU ACC register
// (seriously boy you could have factorized it)
int _allocFPACCtoXMMreg(int xmmreg, int mode)
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_FPACC) continue;

		if( !(xmmregs[i].mode & MODE_READ) && (mode&MODE_READ)) {
			xMOVSSZX(xRegisterSSE(i), ptr[&fpuRegs.ACC.f]);
			xmmregs[i].mode |= MODE_READ;
		}

		g_xmmtypes[i] = XMMT_FPS;
		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		xmmregs[i].mode|= mode;
		return i;
	}

	if (xmmreg == -1)
		xmmreg = _getFreeXMMreg();

	g_xmmtypes[xmmreg] = XMMT_FPS;
	xmmregs[xmmreg].inuse = 1;
	xmmregs[xmmreg].type = XMMTYPE_FPACC;
	xmmregs[xmmreg].mode = mode;
	xmmregs[xmmreg].needed = 1;
	xmmregs[xmmreg].reg = 0;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;

	if (mode & MODE_READ) {
		xMOVSSZX(xRegisterSSE(xmmreg), ptr[&fpuRegs.ACC.f]);
	}

	return xmmreg;
}

#ifndef DISABLE_SVU
void _addNeededVFtoXMMreg(int vfreg) {
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_VFREG) continue;
		if (xmmregs[i].reg != vfreg) continue;

		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
	}
}
#endif

// Mark reserved GPR reg as needed. It won't be evicted anymore.
// You must use _clearNeededXMMregs to clear the flag
void _addNeededGPRtoXMMreg(int gprreg)
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_GPRREG) continue;
		if (xmmregs[i].reg != gprreg) continue;

		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		break;
	}
}

#ifndef DISABLE_SVU
void _addNeededACCtoXMMreg() {
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_ACC) continue;

		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		break;
	}
}
#endif

// Mark reserved FPU reg as needed. It won't be evicted anymore.
// You must use _clearNeededXMMregs to clear the flag
void _addNeededFPtoXMMreg(int fpreg) {
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_FPREG) continue;
		if (xmmregs[i].reg != fpreg) continue;

		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		break;
	}
}

// Mark reserved FPU ACC reg as needed. It won't be evicted anymore.
// You must use _clearNeededXMMregs to clear the flag
void _addNeededFPACCtoXMMreg() {
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;
		if (xmmregs[i].type != XMMTYPE_FPACC) continue;

		xmmregs[i].counter = g_xmmAllocCounter++; // update counter
		xmmregs[i].needed = 1;
		break;
	}
}

// Clear needed flags of all registers
// Written register will set MODE_READ (aka data is valid, no need to load it)
void _clearNeededXMMregs() {
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {

		if( xmmregs[i].needed ) {

			// setup read to any just written regs
			if( xmmregs[i].inuse && (xmmregs[i].mode&MODE_WRITE) )
				xmmregs[i].mode |= MODE_READ;
			xmmregs[i].needed = 0;
		}

		if( xmmregs[i].inuse ) {
			pxAssert( xmmregs[i].type != XMMTYPE_TEMP );
		}
	}
}

#ifndef DISABLE_SVU
void _deleteVFtoXMMreg(int reg, int vu, int flush)
{
	int i;
	VURegs *VU = vu ? &VU1 : &VU0;

	for (i=0; (uint)i<iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse && (xmmregs[i].type == XMMTYPE_VFREG) &&
		   (xmmregs[i].reg == reg) && (xmmregs[i].VU == vu))
		{
			switch(flush) {
				case 0:
					_freeXMMreg(i);
					break;
				case 1:
					if( xmmregs[i].mode & MODE_WRITE )
					{
						pxAssert( reg != 0 );

						if( xmmregs[i].mode & MODE_VUXYZ )
						{
							if( xmmregs[i].mode & MODE_VUZ )
							{
								// xyz, don't destroy w
								uint t0reg;

								for (t0reg = 0; t0reg < iREGCNT_XMM; ++t0reg)
								{
									if (!xmmregs[t0reg].inuse )
										break;
								}

								if (t0reg < iREGCNT_XMM )
								{
									xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(i));
									xMOVL.PS(ptr[(void*)(VU_VFx_ADDR(xmmregs[i].reg))], xRegisterSSE(i));
									xMOVSS(ptr[(void*)(VU_VFx_ADDR(xmmregs[i].reg)+8)], xRegisterSSE(t0reg));
								}
								else
								{
									// no free reg
									xMOVL.PS(ptr[(void*)(VU_VFx_ADDR(xmmregs[i].reg))], xRegisterSSE(i));
									xSHUF.PS(xRegisterSSE(i), xRegisterSSE(i), 0xc6);
									xMOVSS(ptr[(void*)(VU_VFx_ADDR(xmmregs[i].reg)+8)], xRegisterSSE(i));
									xSHUF.PS(xRegisterSSE(i), xRegisterSSE(i), 0xc6);
								}
							}
							else
							{
								// xy
								xMOVL.PS(ptr[(void*)(VU_VFx_ADDR(xmmregs[i].reg))], xRegisterSSE(i));
							}
						}
						else xMOVAPS(ptr[(void*)(VU_VFx_ADDR(xmmregs[i].reg))], xRegisterSSE(i));

						// get rid of MODE_WRITE since don't want to flush again
						xmmregs[i].mode &= ~MODE_WRITE;
						xmmregs[i].mode |= MODE_READ;
					}
					break;

				case 2:
					xmmregs[i].inuse = 0;
					break;
			}

			return;
		}
	}
}
#endif

#if 0
void _deleteACCtoXMMreg(int vu, int flush)
{
	int i;
	VURegs *VU = vu ? &VU1 : &VU0;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse && (xmmregs[i].type == XMMTYPE_ACC) && (xmmregs[i].VU == vu)) {

			switch(flush) {
				case 0:
					_freeXMMreg(i);
					break;
				case 1:
				case 2:
					if( xmmregs[i].mode & MODE_WRITE ) {

						if( xmmregs[i].mode & MODE_VUXYZ ) {

							if( xmmregs[i].mode & MODE_VUZ ) {
								// xyz, don't destroy w
								uint t0reg;
								for(t0reg = 0; t0reg < iREGCNT_XMM; ++t0reg ) {
									if( !xmmregs[t0reg].inuse ) break;
								}

								if( t0reg < iREGCNT_XMM ) {
									xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(i));
									xMOVL.PS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(i));
									xMOVSS(ptr[(void*)(VU_ACCx_ADDR+8)], xRegisterSSE(t0reg));
								}
								else {
									// no free reg
									xMOVL.PS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(i));
									xSHUF.PS(xRegisterSSE(i), xRegisterSSE(i), 0xc6);
									//xMOVHL.PS(xRegisterSSE(i), xRegisterSSE(i));
									xMOVSS(ptr[(void*)(VU_ACCx_ADDR+8)], xRegisterSSE(i));
									xSHUF.PS(xRegisterSSE(i), xRegisterSSE(i), 0xc6);
								}
							}
							else {
								// xy
								xMOVL.PS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(i));
							}
						}
						else xMOVAPS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(i));

						// get rid of MODE_WRITE since don't want to flush again
						xmmregs[i].mode &= ~MODE_WRITE;
						xmmregs[i].mode |= MODE_READ;
					}

					if( flush == 2 )
						xmmregs[i].inuse = 0;
					break;
			}

			return;
		}
	}
}
#endif

// Flush is 0: _freeXMMreg. Flush in memory if MODE_WRITE. Clear inuse
// Flush is 1: Flush in memory. But register is still valid
// Flush is 2: like 0 ...
void _deleteGPRtoXMMreg(int reg, int flush)
{
	int i;
	for (i=0; (uint)i<iREGCNT_XMM; i++) {

		if (xmmregs[i].inuse && xmmregs[i].type == XMMTYPE_GPRREG && xmmregs[i].reg == reg ) {

			switch(flush) {
				case 0:
					_freeXMMreg(i);
					break;
				case 1:
				case 2:
					if( xmmregs[i].mode & MODE_WRITE ) {
						pxAssert( reg != 0 );

						//pxAssert( g_xmmtypes[i] == XMMT_INT );
						xMOVDQA(ptr[&cpuRegs.GPR.r[reg].UL[0]], xRegisterSSE(i));

						// get rid of MODE_WRITE since don't want to flush again
						xmmregs[i].mode &= ~MODE_WRITE;
						xmmregs[i].mode |= MODE_READ;
					}

					if( flush == 2 )
						xmmregs[i].inuse = 0;
					break;
			}

			return;
		}
	}
}

// Flush is 0: _freeXMMreg. Flush in memory if MODE_WRITE. Clear inuse
// Flush is 1: Flush in memory. But register is still valid
// Flush is 2: drop register content
void _deleteFPtoXMMreg(int reg, int flush)
{
	int i;
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse && xmmregs[i].type == XMMTYPE_FPREG && xmmregs[i].reg == reg ) {
			switch(flush) {
				case 0:
					_freeXMMreg(i);
					return;

				case 1:
					if (xmmregs[i].mode & MODE_WRITE) {
						xMOVSS(ptr[&fpuRegs.fpr[reg].UL], xRegisterSSE(i));
						// get rid of MODE_WRITE since don't want to flush again
						xmmregs[i].mode &= ~MODE_WRITE;
						xmmregs[i].mode |= MODE_READ;
					}
					return;

				case 2:
					xmmregs[i].inuse = 0;
					return;
			}
		}
	}
}

// Free cached register
// Step 1: flush content in memory if MODE_WRITE
// Step 2: clear 'inuse' field
void _freeXMMreg(u32 xmmreg)
{
	pxAssert( xmmreg < iREGCNT_XMM );

	if (!xmmregs[xmmreg].inuse) return;

	if (xmmregs[xmmreg].mode & MODE_WRITE) {
	switch (xmmregs[xmmreg].type) {
		case XMMTYPE_VFREG:
		{
			const VURegs *VU = xmmregs[xmmreg].VU ? &VU1 : &VU0;
			if( xmmregs[xmmreg].mode & MODE_VUXYZ )
			{
				if( xmmregs[xmmreg].mode & MODE_VUZ )
				{
					// don't destroy w
					uint t0reg;
					for(t0reg = 0; t0reg < iREGCNT_XMM; ++t0reg ) {
						if( !xmmregs[t0reg].inuse ) break;
					}

					if( t0reg < iREGCNT_XMM )
					{
						xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(xmmreg));
						xMOVL.PS(ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg))], xRegisterSSE(xmmreg));
						xMOVSS(ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg)+8)], xRegisterSSE(t0reg));
					}
					else
					{
						// no free reg
						xMOVL.PS(ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg))], xRegisterSSE(xmmreg));
						xSHUF.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg), 0xc6);
						//xMOVHL.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
						xMOVSS(ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg)+8)], xRegisterSSE(xmmreg));
						xSHUF.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg), 0xc6);
					}
				}
				else
				{
					xMOVL.PS(ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg))], xRegisterSSE(xmmreg));
				}
			}
			else
			{
				xMOVAPS(ptr[(void*)(VU_VFx_ADDR(xmmregs[xmmreg].reg))], xRegisterSSE(xmmreg));
			}
		}
		break;

		case XMMTYPE_ACC:
		{
			const VURegs *VU = xmmregs[xmmreg].VU ? &VU1 : &VU0;
			if( xmmregs[xmmreg].mode & MODE_VUXYZ )
			{
				if( xmmregs[xmmreg].mode & MODE_VUZ )
				{
					// don't destroy w
					uint t0reg;

					for(t0reg = 0; t0reg < iREGCNT_XMM; ++t0reg ) {
						if( !xmmregs[t0reg].inuse ) break;
					}

					if( t0reg < iREGCNT_XMM )
					{
						xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(xmmreg));
						xMOVL.PS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(xmmreg));
						xMOVSS(ptr[(void*)(VU_ACCx_ADDR+8)], xRegisterSSE(t0reg));
					}
					else
					{
						// no free reg
						xMOVL.PS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(xmmreg));
						xSHUF.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg), 0xc6);
						//xMOVHL.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
						xMOVSS(ptr[(void*)(VU_ACCx_ADDR+8)], xRegisterSSE(xmmreg));
						xSHUF.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg), 0xc6);
					}
				}
				else
				{
					xMOVL.PS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(xmmreg));
				}
			}
			else
			{
				xMOVAPS(ptr[(void*)(VU_ACCx_ADDR)], xRegisterSSE(xmmreg));
			}
		}
		break;

		case XMMTYPE_GPRREG:
			pxAssert( xmmregs[xmmreg].reg != 0 );
			//pxAssert( g_xmmtypes[xmmreg] == XMMT_INT );
			xMOVDQA(ptr[&cpuRegs.GPR.r[xmmregs[xmmreg].reg].UL[0]], xRegisterSSE(xmmreg));
			break;

		case XMMTYPE_FPREG:
			xMOVSS(ptr[&fpuRegs.fpr[xmmregs[xmmreg].reg]], xRegisterSSE(xmmreg));
			break;

		case XMMTYPE_FPACC:
			xMOVSS(ptr[&fpuRegs.ACC.f], xRegisterSSE(xmmreg));
			break;

		default:
			break;
	}
	}
	xmmregs[xmmreg].mode &= ~(MODE_WRITE|MODE_VUXYZ);
	xmmregs[xmmreg].inuse = 0;
}

// Return the number of inuse XMM register that have the MODE_WRITE flag
int _getNumXMMwrite()
{
	int num = 0, i;
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if( xmmregs[i].inuse && (xmmregs[i].mode&MODE_WRITE) ) ++num;
	}

	return num;
}

// Step1: check any available register (inuse == 0)
// Step2: check registers that are not live (both EEINST_LIVE* are cleared)
// Step3: check registers that are not useful anymore (EEINST_USED cleared)
u8 _hasFreeXMMreg()
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (!xmmregs[i].inuse) return 1;
	}

	// check for dead regs
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].needed) continue;
		if (xmmregs[i].type == XMMTYPE_GPRREG ) {
			if( !EEINST_ISLIVEXMM(xmmregs[i].reg) ) {
				return 1;
			}
		}
	}

	// check for dead regs
	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].needed) continue;
		if (xmmregs[i].type == XMMTYPE_GPRREG  ) {
			if( !(g_pCurInstInfo->regs[xmmregs[i].reg]&EEINST_USED) ) {
				return 1;
			}
		}
	}
	return 0;
}

#if 0
void _moveXMMreg(int xmmreg)
{
	int i;
	if( !xmmregs[xmmreg].inuse ) return;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse) continue;
		break;
	}

	if( i == iREGCNT_XMM ) {
		_freeXMMreg(xmmreg);
		return;
	}

	// move
	xmmregs[i] = xmmregs[xmmreg];
	xmmregs[xmmreg].inuse = 0;
	xMOVDQA(xRegisterSSE(i), xRegisterSSE(xmmreg));
}
#endif

// Flush in memory all inuse registers but registers are still valid
void _flushXMMregs()
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;

		pxAssert( xmmregs[i].type != XMMTYPE_TEMP );
		pxAssert( xmmregs[i].mode & (MODE_READ|MODE_WRITE) );

		_freeXMMreg(i);
		xmmregs[i].inuse = 1;
		xmmregs[i].mode &= ~MODE_WRITE;
		xmmregs[i].mode |= MODE_READ;
	}
}

// Flush in memory all inuse registers. All registers are invalid
void _freeXMMregs()
{
	int i;

	for (i=0; (uint)i<iREGCNT_XMM; i++) {
		if (xmmregs[i].inuse == 0) continue;

		pxAssert( xmmregs[i].type != XMMTYPE_TEMP );
		//pxAssert( xmmregs[i].mode & (MODE_READ|MODE_WRITE) );

		_freeXMMreg(i);
	}
}

int _signExtendXMMtoM(uptr to, x86SSERegType from, int candestroy)
{
	int t0reg;
	g_xmmtypes[from] = XMMT_INT;
	if( candestroy ) {
		if( g_xmmtypes[from] == XMMT_FPS ) xMOVSS(ptr[(void*)(to)], xRegisterSSE(from));
		else xMOVD(ptr[(void*)(to)], xRegisterSSE(from));

		xPSRA.D(xRegisterSSE(from), 31);
		xMOVD(ptr[(void*)(to+4)], xRegisterSSE(from));
		return 1;
	}
	else {
		// can't destroy and type is int
		pxAssert( g_xmmtypes[from] == XMMT_INT );


		if( _hasFreeXMMreg() ) {
			xmmregs[from].needed = 1;
			t0reg = _allocTempXMMreg(XMMT_INT, -1);
			xMOVDQA(xRegisterSSE(t0reg), xRegisterSSE(from));
			xPSRA.D(xRegisterSSE(from), 31);
			xMOVD(ptr[(void*)(to)], xRegisterSSE(t0reg));
			xMOVD(ptr[(void*)(to+4)], xRegisterSSE(from));

			// swap xmm regs.. don't ask
			xmmregs[t0reg] = xmmregs[from];
			xmmregs[from].inuse = 0;
		}
		else {
			xMOVD(ptr[(void*)(to+4)], xRegisterSSE(from));
			xMOVD(ptr[(void*)(to)], xRegisterSSE(from));
			xSAR(ptr32[(u32*)(to+4)], 31);
		}

		return 0;
	}

	pxAssume( false );
}

// Seem related to the mix between XMM/x86 in order to avoid a couple of move
// But it is quite obscure !!!
int _allocCheckGPRtoXMM(EEINST* pinst, int gprreg, int mode)
{
	if( pinst->regs[gprreg] & EEINST_XMM ) return _allocGPRtoXMMreg(-1, gprreg, mode);

	return _checkXMMreg(XMMTYPE_GPRREG, gprreg, mode);
}

// Seem related to the mix between XMM/x86 in order to avoid a couple of move
// But it is quite obscure !!!
int _allocCheckFPUtoXMM(EEINST* pinst, int fpureg, int mode)
{
	if( pinst->fpuregs[fpureg] & EEINST_XMM ) return _allocFPtoXMMreg(-1, fpureg, mode);

	return _checkXMMreg(XMMTYPE_FPREG, fpureg, mode);
}

int _allocCheckGPRtoX86(EEINST* pinst, int gprreg, int mode)
{
	if( pinst->regs[gprreg] & EEINST_USED )
        return _allocX86reg(xEmptyReg, X86TYPE_GPR, gprreg, mode);

	return _checkX86reg(X86TYPE_GPR, gprreg, mode);
}

void _recClearInst(EEINST* pinst)
{
	memzero( *pinst );
	memset8<EEINST_LIVE0|EEINST_LIVE2>( pinst->regs );
	memset8<EEINST_LIVE0>( pinst->fpuregs );
}

// returns nonzero value if reg has been written between [startpc, endpc-4]
u32 _recIsRegWritten(EEINST* pinst, int size, u8 xmmtype, u8 reg)
{
	u32  i, inst = 1;

	while(size-- > 0) {
		for(i = 0; i < ArraySize(pinst->writeType); ++i) {
			if ((pinst->writeType[i] == xmmtype) && (pinst->writeReg[i] == reg))
				return inst;
		}
		++inst;
		pinst++;
	}

	return 0;
}

#if 0
u32 _recIsRegUsed(EEINST* pinst, int size, u8 xmmtype, u8 reg)
{
	u32 i, inst = 1;
	while(size-- > 0) {
		for(i = 0; i < ArraySize(pinst->writeType); ++i) {
			if( pinst->writeType[i] == xmmtype && pinst->writeReg[i] == reg )
				return inst;
		}
		for(i = 0; i < ArraySize(pinst->readType); ++i) {
			if( pinst->readType[i] == xmmtype && pinst->readReg[i] == reg )
				return inst;
		}
		++inst;
		pinst++;
	}

	return 0;
}
#endif

void _recFillRegister(EEINST& pinst, int type, int reg, int write)
{
	u32 i = 0;
	if (write ) {
		for(i = 0; i < ArraySize(pinst.writeType); ++i) {
			if( pinst.writeType[i] == XMMTYPE_TEMP ) {
				pinst.writeType[i] = type;
				pinst.writeReg[i] = reg;
				return;
			}
		}
		pxAssume( false );
	}
	else {
		for(i = 0; i < ArraySize(pinst.readType); ++i) {
			if( pinst.readType[i] == XMMTYPE_TEMP ) {
				pinst.readType[i] = type;
				pinst.readReg[i] = reg;
				return;
			}
		}
		pxAssume( false );
	}
}

void SetMMXstate() {
	x86FpuState = MMX_STATE;
}
