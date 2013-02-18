// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "../../MemMap.h"
#include "../../Config.h"
#include "../MIPSAnalyst.h"

#include "Jit.h"
#include "../MIPSVFPUUtils.h"
#include "RegCache.h"

// VERY UNFINISHED

// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.

// #define CONDITIONAL_DISABLE { Comp_Generic(op); return; }
#define CONDITIONAL_DISABLE ;
#define DISABLE { Comp_Generic(op); return; }


#define _RS ((op>>21) & 0x1F)
#define _RT ((op>>16) & 0x1F)
#define _RD ((op>>11) & 0x1F)
#define _FS ((op>>11) & 0x1F)
#define _FT ((op>>16) & 0x1F)
#define _FD ((op>>6 ) & 0x1F)
#define _POS  ((op>>6 ) & 0x1F)
#define _SIZE ((op>>11 ) & 0x1F)

using namespace Gen;

namespace MIPSComp
{

static const float one = 1.0f;
static const float minus_one = -1.0f;
static const float zero = 0.0f;

const u32 GC_ALIGNED16( noSignMask[4] ) = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
const u32 GC_ALIGNED16( signBitLower[4] ) = {0x80000000, 0, 0, 0};

void Jit::Comp_VPFX(u32 op)
{
	CONDITIONAL_DISABLE;
	int data = op & 0xFFFFF;
	int regnum = (op >> 24) & 3;
	switch (regnum) {
	case 0:  // S
		js.prefixS = data;
		js.prefixSFlag = JitState::PREFIX_KNOWN_DIRTY;
		break;
	case 1:  // T
		js.prefixT = data;
		js.prefixTFlag = JitState::PREFIX_KNOWN_DIRTY;
		break;
	case 2:  // D
		js.prefixD = data;
		js.prefixDFlag = JitState::PREFIX_KNOWN_DIRTY;
		break;
	}
}


// TODO:  Got register value ownership issues. We need to be sure that if we modify input
// like this, it does NOT get written back!
void Jit::ApplyPrefixST(u8 *vregs, u32 prefix, VectorSize sz) {
	if (prefix == 0xE4) return;

	int n = GetNumVectorElements(sz);
	u8 origV[4];
	static const float constantArray[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f/3.f, 0.25f, 1.f/6.f};

	for (int i = 0; i < n; i++)
	{
		// TODO: This needs to be the original values, not the original regs. (e.g. [-x, |x|, x])
		origV[i] = vregs[i];
	}

	for (int i = 0; i < n; i++)
	{
		int regnum = (prefix >> (i*2)) & 3;
		int abs    = (prefix >> (8+i)) & 1;
		int negate = (prefix >> (16+i)) & 1;
		int constants = (prefix >> (12+i)) & 1;

		if (!constants) {
			// Prefix may say "z, z, z, z" but if this is a pair, we force to x.
			// TODO: But some ops seem to use const 0 instead?
			if (regnum > n) {
				regnum = 0;
			}
			vregs[i] = origV[regnum];
			if (abs) {
				ANDPS(fpr.VX(vregs[i]), M((void *)&noSignMask));
			}
		}	else {
			MOVSS(fpr.VX(vregs[i]), M((void *)&constantArray[regnum + (abs<<2)]));
		}

		if (negate)
			XORPS(fpr.VX(vregs[i]), M((void *)&signBitLower));
	}
}

void Jit::ApplyPrefixD(const u8 *vregs, u32 prefix, VectorSize sz, bool onlyWriteMask) {
	_assert_(js.prefixDFlag & JitState::PREFIX_KNOWN);
	if (!prefix) return;

	int n = GetNumVectorElements(sz);
	for (int i = 0; i < n; i++)
	{
		int mask = (prefix >> (8 + i)) & 1;
		js.writeMask[i] = mask ? true : false;
		if (onlyWriteMask)
			continue;
		if (!mask) {
			int sat = (prefix >> (i * 2)) & 3;
			if (sat == 1)
			{
				MAXSS(fpr.VX(vregs[i]), M((void *)&zero));
				MINSS(fpr.VX(vregs[i]), M((void *)&one));
			}
			else if (sat == 3)
			{
				MAXSS(fpr.VX(vregs[i]), M((void *)&minus_one));
				MINSS(fpr.VX(vregs[i]), M((void *)&one));
			}
		}
	}
}

// Vector regs can overlap in all sorts of swizzled ways.
// This does allow a single overlap in sregs[i].
bool IsOverlapSafeAllowS(int dreg, int di, int sn, u8 sregs[], int tn, u8 tregs[])
{
	for (int i = 0; i < sn; ++i)
	{
		if (sregs[i] == dreg && i != di)
			return false;
	}
	for (int i = 0; i < tn; ++i)
	{
		if (tregs[i] == dreg)
			return false;
	}

	// Hurray, no overlap, we can write directly.
	return true;
}

bool IsOverlapSafe(int dreg, int di, int sn, u8 sregs[], int tn, u8 tregs[])
{
	return IsOverlapSafeAllowS(dreg, di, sn, sregs, tn, tregs) && sregs[di] != dreg;
}

static u32 GC_ALIGNED16(ssLoadStoreTemp);

void Jit::Comp_SV(u32 op) {
	CONDITIONAL_DISABLE;

	s32 imm = (signed short)(op&0xFFFC);
	int vt = ((op >> 16) & 0x1f) | ((op & 3) << 5);
	int rs = _RS;

	switch (op >> 26)
	{
	case 50: //lv.s  // VI(vt) = Memory::Read_U32(addr);
		{
			gpr.BindToRegister(rs, true, false);
			fpr.MapRegV(vt, MAP_NOINIT);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg src;
			if (safe.PrepareRead(src))
			{
				MOVSS(fpr.VX(vt), safe.NextFastAddress(0));
			}
			if (safe.PrepareSlowRead((void *) &Memory::Read_U32))
			{
				MOV(32, M((void *)&ssLoadStoreTemp), R(EAX));
				MOVSS(fpr.VX(vt), M((void *)&ssLoadStoreTemp));
			}
			safe.Finish();

			gpr.UnlockAll();
			fpr.ReleaseSpillLocks();
		}
		break;

	case 58: //sv.s   // Memory::Write_U32(VI(vt), addr);
		{
			gpr.BindToRegister(rs, true, true);

			// Even if we don't use real SIMD there's still 8 or 16 scalar float registers.
			fpr.MapRegV(vt, 0);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg dest;
			if (safe.PrepareWrite(dest))
			{
				MOVSS(safe.NextFastAddress(0), fpr.VX(vt));
			}
			if (safe.PrepareSlowWrite())
			{
				MOVSS(M((void *)&ssLoadStoreTemp), fpr.VX(vt));
				safe.DoSlowWrite((void *) &Memory::Write_U32, M((void *)&ssLoadStoreTemp), 0);
			}
			safe.Finish();

			fpr.ReleaseSpillLocks();
			gpr.UnlockAll();
		}
		break;

	default:
		DISABLE;
	}
}

void Jit::Comp_SVQ(u32 op)
{
	CONDITIONAL_DISABLE;

	int imm = (signed short)(op&0xFFFC);
	int vt = (((op >> 16) & 0x1f)) | ((op&1) << 5);
	int rs = _RS;

	switch (op >> 26)
	{
	case 54: //lv.q
		{
			gpr.BindToRegister(rs, true, true);
	
			u8 vregs[4];
			GetVectorRegs(vregs, V_Quad, vt);
			fpr.MapRegsV(vregs, V_Quad, MAP_DIRTY | MAP_NOINIT);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg src;
			if (safe.PrepareRead(src))
			{
				// Just copy 4 words the easiest way while not wasting registers.
				for (int i = 0; i < 4; i++)
					MOVSS(fpr.VX(vregs[i]), safe.NextFastAddress(i * 4));
			}
			if (safe.PrepareSlowRead((void *) &Memory::Read_U32))
			{
				for (int i = 0; i < 4; i++)
				{
					safe.NextSlowRead((void *) &Memory::Read_U32, i * 4);
					MOV(32, M((void *)&ssLoadStoreTemp), R(EAX));
					MOVSS(fpr.VX(vregs[i]), M((void *)&ssLoadStoreTemp));
				}
			}
			safe.Finish();

			gpr.UnlockAll();
			fpr.ReleaseSpillLocks();
		}
		break;

	case 62: //sv.q
		{
			gpr.BindToRegister(rs, true, true);

			u8 vregs[4];
			GetVectorRegs(vregs, V_Quad, vt);
			// Even if we don't use real SIMD there's still 8 or 16 scalar float registers.
			fpr.MapRegsV(vregs, V_Quad, 0);

			JitSafeMem safe(this, rs, imm);
			safe.SetFar();
			OpArg dest;
			if (safe.PrepareWrite(dest))
			{
				for (int i = 0; i < 4; i++)
					MOVSS(safe.NextFastAddress(i * 4), fpr.VX(vregs[i]));
			}
			if (safe.PrepareSlowWrite())
			{
				for (int i = 0; i < 4; i++)
				{
					MOVSS(M((void *)&ssLoadStoreTemp), fpr.VX(vregs[i]));
					safe.DoSlowWrite((void *) &Memory::Write_U32, M((void *)&ssLoadStoreTemp), i * 4);
				}
			}
			safe.Finish();

			fpr.ReleaseSpillLocks();
			gpr.UnlockAll();
		}
		break;

	default:
		DISABLE;
		break;
	}
}

void Jit::Comp_VDot(u32 op) {
	CONDITIONAL_DISABLE;

	// No-op.
	if (js.writeMask[0]) {
		return;
	}

	// WARNING: No prefix support!
	if (js.MayHavePrefix()) {
		Comp_Generic(op);
		return;
	}

	int vd = _VD;
	int vs = _VS;
	int vt = _VT;
	VectorSize sz = GetVecSize(op);
	
	// TODO: Force read one of them into regs? probably not.
	u8 sregs[4], tregs[4], dregs[1];
	GetVectorRegs(sregs, sz, vs);
	GetVectorRegs(tregs, sz, vt);
	GetVectorRegs(dregs, V_Single, vd);

	// TODO: applyprefixST here somehow (shuffle, etc...)

	int n = GetNumVectorElements(sz);
	X64Reg tempxreg = XMM0;
	if (IsOverlapSafe(dregs[0], 0, n, sregs, n, tregs))
	{
		fpr.MapRegsV(dregs, V_Single, MAP_NOINIT);
		tempxreg = fpr.VX(dregs[0]);
	}

	// Need to start with +0.0f so it doesn't result in -0.0f.
	XORPS(tempxreg, R(tempxreg));
	for (int i = 0; i < n; i++)
	{
		// sum += s[i]*t[i];
		MOVSS(XMM1, fpr.V(sregs[i]));
		MULSS(XMM1, fpr.V(tregs[i]));
		ADDSS(tempxreg, R(XMM1));
	}

	if (!fpr.V(dregs[0]).IsSimpleReg(tempxreg))
	{
		fpr.MapRegsV(dregs, V_Single, MAP_NOINIT);
		MOVSS(fpr.V(dregs[0]), tempxreg);
	}

	// TODO: applyprefixD here somehow (write mask etc..)

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_VecDo3(u32 op) {
	CONDITIONAL_DISABLE;

	// WARNING: No prefix support!
	if (js.MayHavePrefix())
	{
		Comp_Generic(op);
		return;
	}

	int vd = _VD;
	int vs = _VS;
	int vt = _VT;
	VectorSize sz = GetVecSize(op);

	u8 sregs[4], tregs[4], dregs[4];
	GetVectorRegs(sregs, sz, vs);
	GetVectorRegs(tregs, sz, vt);
	GetVectorRegs(dregs, sz, vd);

	void (XEmitter::*xmmop)(X64Reg, OpArg) = NULL;
	switch (op >> 26)
	{
	case 24: //VFPU0
		switch ((op >> 23)&7)
		{
		case 0: // d[i] = s[i] + t[i]; break; //vadd
			xmmop = &XEmitter::ADDSS;
			break;
		case 1: // d[i] = s[i] - t[i]; break; //vsub
			xmmop = &XEmitter::SUBSS;
			break;
		case 7: // d[i] = s[i] / t[i]; break; //vdiv
			xmmop = &XEmitter::DIVSS;
			break;
		}
		break;
	case 25: //VFPU1
		switch ((op >> 23)&7)
		{
		case 0: // d[i] = s[i] * t[i]; break; //vmul
			xmmop = &XEmitter::MULSS;
			break;
		}
		break;
	}

	if (xmmop == NULL)
	{
		Comp_Generic(op);
		return;
	}

	int n = GetNumVectorElements(sz);

	X64Reg tempxregs[4];
	for (int i = 0; i < n; ++i)
	{
		if (!IsOverlapSafeAllowS(dregs[i], i, n, sregs, n, tregs))
		{
			// On 32-bit we only have 6 xregs for mips regs, use XMM0/XMM1 if possible.
			if (i < 2)
				tempxregs[i] = (X64Reg) (XMM0 + i);
			else
			{
				fpr.BindToRegister(TEMP0 + i, false, true);
				fpr.SpillLock(TEMP0 + i);
				tempxregs[i] = fpr.RX(TEMP0 + i);
			}
		}
		else
		{
			fpr.MapRegV(dregs[i], (dregs[i] == sregs[i] ? 0 : MAP_NOINIT) | MAP_DIRTY);
			fpr.SpillLockV(dregs[i]);
			tempxregs[i] = fpr.VX(dregs[i]);
		}
	}

	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(sregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(tempxregs[i], fpr.V(sregs[i]));
	}
	for (int i = 0; i < n; ++i)
		(this->*xmmop)(tempxregs[i], fpr.V(tregs[i]));
	for (int i = 0; i < n; ++i)
	{
		if (!fpr.V(dregs[i]).IsSimpleReg(tempxregs[i]))
			MOVSS(fpr.V(dregs[i]), tempxregs[i]);
	}

	fpr.ReleaseSpillLocks();
}

void Jit::Comp_Mftv(u32 op) {
	CONDITIONAL_DISABLE;

	int imm = op & 0xFF;
	int rt = _RT;
	switch ((op >> 21) & 0x1f)
	{
	case 3: //mfv / mfvc
		// rt = 0, imm = 255 appears to be used as a CPU interlock by some games.
		if (rt != 0) {
			if (imm < 128) {  //R(rt) = VI(imm);
				fpr.StoreFromRegisterV(imm);
				gpr.BindToRegister(rt, false, true);
				MOV(32, gpr.R(rt), fpr.V(imm));
			} else if (imm < 128 + VFPU_CTRL_MAX) { //mtvc
				// In case we have a saved prefix.
				FlushPrefixV();
				gpr.BindToRegister(rt, false, true);
				MOV(32, gpr.R(rt), M(&currentMIPS->vfpuCtrl[imm - 128]));
			} else {
				//ERROR - maybe need to make this value too an "interlock" value?
				_dbg_assert_msg_(CPU,0,"mfv - invalid register");
			}
		}
		break;

	case 7: //mtv
		if (imm < 128) {
			fpr.StoreFromRegisterV(imm);
			gpr.BindToRegister(rt, true, false);
			MOV(32, fpr.V(imm), gpr.R(rt));
			// VI(imm) = R(rt);
		} else if (imm < 128 + VFPU_CTRL_MAX) { //mtvc //currentMIPS->vfpuCtrl[imm - 128] = R(rt);
			gpr.BindToRegister(rt, true, false);
			MOV(32, M(&currentMIPS->vfpuCtrl[imm - 128]), gpr.R(rt));

			// TODO: Optimization if rt is Imm?
			if (imm - 128 == VFPU_CTRL_SPREFIX) {
				js.prefixSFlag = JitState::PREFIX_UNKNOWN;
			} else if (imm - 128 == VFPU_CTRL_TPREFIX) {
				js.prefixTFlag = JitState::PREFIX_UNKNOWN;
			} else if (imm - 128 == VFPU_CTRL_DPREFIX) {
				js.prefixDFlag = JitState::PREFIX_UNKNOWN;
			}
		} else {
			//ERROR
			_dbg_assert_msg_(CPU,0,"mtv - invalid register");
		}
		break;

	default:
		DISABLE;
	}
}

void Jit::Comp_Vmtvc(u32 op) {
	CONDITIONAL_DISABLE;
	int vs = _VS;
	int imm = op & 0xFF;
	if (imm >= 128 && imm < 128 + VFPU_CTRL_MAX) {
		fpr.MapRegV(vs, 0);
		MOVSS(M(&currentMIPS->vfpuCtrl[imm - 128]), fpr.RX(vs));
		fpr.ReleaseSpillLocks();

		if (imm - 128 == VFPU_CTRL_SPREFIX) {
			js.prefixSFlag = JitState::PREFIX_UNKNOWN;
		} else if (imm - 128 == VFPU_CTRL_TPREFIX) {
			js.prefixTFlag = JitState::PREFIX_UNKNOWN;
		} else if (imm - 128 == VFPU_CTRL_DPREFIX) {
			js.prefixDFlag = JitState::PREFIX_UNKNOWN;
		}
	}
}

}