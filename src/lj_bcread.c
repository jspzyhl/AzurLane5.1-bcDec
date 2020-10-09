/*
** Bytecode reader.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_bcread_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_bc.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lualib.h"
#endif
#include "lj_lex.h"
#include "lj_bcdump.h"
#include "lj_state.h"
#include "lj_strfmt.h"

#include <emmintrin.h>
#include <tmmintrin.h>
#include "defs.h"

//	加载数组作为m128值
#define LD128(m128_array) _mm_loadu_si128((__m128i *)m128_array)

static const uint8_t xmmword_94570[] = { 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00 };
static const uint8_t xmmword_94560[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00 };
static const uint8_t xmmword_943A0[] = { 0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0C, 0x0D, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
static const uint8_t xmmword_943B0[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0C, 0x0D };
static const uint8_t xmmword_943C0[] = { 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
static const uint8_t xmmword_94580[] = { 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00 };
static const uint8_t xmmword_94320[] = { 0x0C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00 };
static const uint8_t xmmword_94590[] = { 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 };
static const uint8_t xmmword_945A0[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

//	opcode 对应关系
static const uint8_t op_map[] = { 12,13,14,15,16,17,39,40,41,42,43,44,77,78,79,80,81,82,83,84,85,86,87,88,0,1,2,3,4,5,6,7,8,9,10,11,65,66,67,68,69,70,71,72,18,19,20,21,73,74,75,76,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,64,60,61,62,63,96,89,90,91,92,93,94,95 };


/* Reuse some lexer fields for our own purposes. */
#define bcread_flags(ls)	ls->level
#define bcread_swap(ls) \
  ((bcread_flags(ls) & BCDUMP_F_BE) != LJ_BE*BCDUMP_F_BE)
#define bcread_oldtop(L, ls)	restorestack(L, ls->lastline)
#define bcread_savetop(L, ls, top) \
  ls->lastline = (BCLine)savestack(L, (top))

/* -- Input buffer handling ----------------------------------------------- */

/* Throw reader error. */
static LJ_NOINLINE void bcread_error(LexState *ls, ErrMsg em)
{
	lua_State *L = ls->L;
	const char *name = ls->chunkarg;
	if (*name == BCDUMP_HEAD1) name = "(binary)";
	else if (*name == '@' || *name == '=') name++;
	lj_strfmt_pushf(L, "%s: %s", name, err2msg(em));
	lj_err_throw(L, LUA_ERRSYNTAX);
}

/* Throw reader error. */
static void bcread_error_mod(lua_State *L, const char *chunkarg)
{
	const char *name = chunkarg;
	if (*name == BCDUMP_HEAD1) name = "(binary)";
	else if (*name == '@' || *name == '=') name++;
	lj_strfmt_pushf(L, "%s: %s", chunkarg);
	lj_err_throw(L, LUA_ERRSYNTAX);
}

/* Refill buffer. */
static LJ_NOINLINE void bcread_fill(LexState *ls, MSize len, int need)
{
	lua_assert(len != 0);
	if (len > LJ_MAX_BUF || ls->c < 0)
		bcread_error(ls, LJ_ERR_BCBAD);
	do {
		const char *buf;
		size_t sz;
		char *p = sbufB(&ls->sb);
		MSize n = (MSize)(ls->pe - ls->p);
		if (n) {  /* Copy remainder to buffer. */
			if (sbuflen(&ls->sb))
			{  /* Move down in buffer. */
				lua_assert(ls->pe == sbufP(&ls->sb));
				if (ls->p != p) memmove(p, ls->p, n);
			}
			else
			{  /* Copy from buffer provided by reader. */
				p = lj_buf_need(&ls->sb, len);
				memcpy(p, ls->p, n);
			}
			ls->p = p;
			ls->pe = p + n;
		}
		setsbufP(&ls->sb, p + n);
		buf = ls->rfunc(ls->L, ls->rdata, &sz);  /* Get more data from reader. */
		if (buf == NULL || sz == 0) {  /* EOF? */
			if (need) bcread_error(ls, LJ_ERR_BCBAD);
			ls->c = -1;  /* Only bad if we get called again. */
			break;
		}
		if (n) {  /* Append to buffer. */
			n += (MSize)sz;
			p = lj_buf_need(&ls->sb, n < len ? len : n);
			memcpy(sbufP(&ls->sb), buf, sz);
			setsbufP(&ls->sb, p + n);
			ls->p = p;
			ls->pe = p + n;
		}
		else {  /* Return buffer provided by reader. */
			ls->p = buf;
			ls->pe = buf + sz;
		}
	} while (ls->p + len > ls->pe);
}


/* Need a certain number of bytes. */
static LJ_AINLINE void bcread_need(LexState *ls, MSize len)
{
	if (LJ_UNLIKELY(ls->p + len > ls->pe))
		bcread_fill(ls, len, 1);
}

/* Want to read up to a certain number of bytes, but may need less. */
static LJ_AINLINE void bcread_want(LexState *ls, MSize len)
{
	if (LJ_UNLIKELY(ls->p + len > ls->pe))
		bcread_fill(ls, len, 0);
}

/* Return memory block from buffer. */
static LJ_AINLINE uint8_t *bcread_mem(LexState *ls, MSize len)
{
	uint8_t *p = (uint8_t *)ls->p;
	ls->p += len;
	lua_assert(ls->p <= ls->pe);
	return p;
}

/* Copy memory block from buffer. */
static void bcread_block(LexState *ls, void *q, MSize len)
{
	memcpy(q, bcread_mem(ls, len), len);
}

/* Read byte from buffer. */
static LJ_AINLINE uint32_t bcread_byte(LexState *ls)
{
	lua_assert(ls->p < ls->pe);
	return (uint32_t)(uint8_t)*ls->p++;
}

/* Read ULEB128 value from buffer. */
static LJ_AINLINE uint32_t bcread_uleb128(LexState *ls)
{
	uint32_t v = lj_buf_ruleb128(&ls->p);
	lua_assert(ls->p <= ls->pe);
	return v;
}

/* Read top 32 bits of 33 bit ULEB128 value from buffer. */
static uint32_t bcread_uleb128_33(LexState *ls)
{
	const uint8_t *p = (const uint8_t *)ls->p;
	uint32_t v = (*p++ >> 1);
	if (LJ_UNLIKELY(v >= 0x40)) {
		int sh = -1;
		v &= 0x3f;
		do {
			v |= ((*p & 0x7f) << (sh += 7));
		} while (*p++ >= 0x80);
	}
	ls->p = (char *)p;
	lua_assert(ls->p <= ls->pe);
	return v;
}

/* -- Bytecode reader ----------------------------------------------------- */

/* Read debug info of a prototype. */
static void bcread_dbg(LexState *ls, GCproto *pt, MSize sizedbg)
{
	void *lineinfo = (void *)proto_lineinfo(pt);
	bcread_block(ls, lineinfo, sizedbg);
	/* Swap lineinfo if the endianess differs. */
	if (bcread_swap(ls) && pt->numline >= 256) {
		MSize i, n = pt->sizebc - 1;
		if (pt->numline < 65536) {
			uint16_t *p = (uint16_t *)lineinfo;
			for (i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
		}
		else {
			uint32_t *p = (uint32_t *)lineinfo;
			for (i = 0; i < n; i++) p[i] = lj_bswap(p[i]);
		}
	}
}


void bcread_dbg_mod(unsigned int ls, int pt, unsigned int sizedbg)
{
	unsigned int v3; // ecx
	char *v4; // esi
	__m128i *v5; // ebp
	int v6; // edi
	int v7; // edx
	unsigned int v8; // ecx
	unsigned int v9; // esi
	const __m128i *v10; // edx
	unsigned int v11; // edi
	__m128i v12; // xmm4
	__m128i v13; // xmm3
	__m128i v14; // xmm0
	int v15; // edx
	_WORD *v16; // edx
	int v17; // edx
	int v18; // edx
	unsigned int v19; // eax
	char v20; // dl
	__int16 v21; // dx
	int v22; // [esp-20h] [ebp-20h]

	__m128i xmmword_9B0B0 = _mm_set_epi8(0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x0D, 0x0C, 0x09, 0x08, 0x05, 0x04, 0x01, 0x00);
	__m128i xmmword_9B0C0 = _mm_set_epi8(0x0D, 0x0C, 0x09, 0x08, 0x05, 0x04, 0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80);


	v3 = sizedbg;
	v4 = *(char **)(ls + 24);
	v5 = *(__m128i **)(pt + 52);
	*(char **)(ls + 24) = &v4[sizedbg];
	v6 = (int)v5;
	v22 = pt;
	if (sizedbg >= 8)
	{
		if ((unsigned __int8)v5 & 1)
		{
			v20 = *v4;
			v6 = (int)v5->m128i_i32 + 1;
			++v4;
			v3 = sizedbg - 1;
			LOBYTE(v5->m128i_i32[0]) = v20;
		}
		if (v6 & 2)
		{
			v21 = *(_WORD *)v4;
			v6 += 2;
			v4 += 2;
			v3 -= 2;
			*(_WORD *)(v6 - 2) = v21;
		}
		if (v6 & 4)
		{
			v17 = *(_DWORD *)v4;
			v6 += 4;
			v4 += 4;
			v3 -= 4;
			*(_DWORD *)(v6 - 4) = v17;
		}
	}
	qmemcpy((void *)v6, v4, v3);
	if (*(_BYTE *)(ls + 108) & 1)
	{
		ls = *(_DWORD *)(v22 + 48);
		if (ls > 255)
		{
			v7 = *(_DWORD *)(v22 + 8);
			v8 = v7 - 1;
			if (ls > 0xFFFF)
			{
				v18 = (int)&v5->m128i_i32[v7 - 1];
				if (v8)
				{
					do
					{
						v19 = v5->m128i_i32[0];
						v5 = (__m128i *)((char *)v5 + 4);
						ls = _byteswap_ulong(v19);
						v5[-1].m128i_i32[3] = ls;
					} while (v5 != (__m128i *)v18);
				}
			}
			else
			{
				if (v7 == 1)
					return;
				v9 = ((unsigned int)(v7 - 9) >> 3) + 1;
				ls = 8 * v9;
				if ((unsigned int)(v7 - 2) <= 6)
				{
					ls = 0;
				}
				else
				{
					v10 = v5;
					v11 = 0;
					v12 = xmmword_9B0B0;
					v13 = xmmword_9B0C0;
					do
					{
						v14 = _mm_loadu_si128(v10);
						++v11;
						++v10;
						_mm_storeu_si128(
							(__m128i *)&v10[-1],
							_mm_or_si128(
								_mm_srli_epi16(v14, 8u),
								_mm_or_si128(
									_mm_shuffle_epi8(_mm_slli_epi32(_mm_unpackhi_epi16(v14, _mm_setzero_si128()), 8u), v13),
									_mm_shuffle_epi8(_mm_slli_epi32(_mm_unpacklo_epi16(v14, _mm_setzero_si128()), 8u), v12))));
					} while (v9 > v11);
					if (v8 == ls)
						return;
				}
				*((_WORD *)v5->m128i_i32 + ls) = (*((_WORD *)v5->m128i_i32 + ls) << 8) | (*((_WORD *)v5->m128i_i32 + ls) >> 8);
				if (v8 > ls + 1)
				{
					*((_WORD *)v5->m128i_i32 + ls + 1) = (*((_WORD *)v5->m128i_i32 + ls + 1) << 8) | (*((_WORD *)v5->m128i_i32
						+ ls
						+ 1) >> 8);
					if (v8 > ls + 2)
					{
						*((_WORD *)v5->m128i_i32 + ls + 2) = (*((_WORD *)v5->m128i_i32 + ls + 2) << 8) | (*((_WORD *)v5->m128i_i32
							+ ls
							+ 2) >> 8);
						if (v8 > ls + 3)
						{
							*((_WORD *)v5->m128i_i32 + ls + 3) = (*((_WORD *)v5->m128i_i32 + ls + 3) << 8) | (*((_WORD *)v5->m128i_i32
								+ ls
								+ 3) >> 8);
							if (v8 > ls + 4)
							{
								*((_WORD *)v5->m128i_i32 + ls + 4) = (*((_WORD *)v5->m128i_i32 + ls + 4) << 8) | (*((_WORD *)v5->m128i_i32
									+ ls
									+ 4) >> 8);
								v15 = ls + 5;
								if (v8 > ls + 5)
								{
									ls += 6;
									*((_WORD *)v5->m128i_i32 + v15) = (*((_WORD *)v5->m128i_i32 + v15) << 8) | (*((_WORD *)v5->m128i_i32
										+ v15) >> 8);
									if (v8 > ls)
									{
										v16 = (_WORD *)((char *)v5->m128i_i32 + 2 * ls);
										ls = ((unsigned __int16)*v16 << 8) | ((unsigned int)(unsigned __int16)*v16 >> 8);
										*v16 = ls;
									}
								}
							}
						}
					}
				}
			}
		}
	}
	return;
}

/* Find pointer to varinfo. */
static const void *bcread_varinfo(GCproto *pt)
{
	const uint8_t *p = proto_uvinfo(pt);
	MSize n = pt->sizeuv;
	if (n) while (*p++ || --n);
	return p;
}

/* Read a single constant key/value of a template table. */
static void bcread_ktabk(LexState *ls, TValue *o)
{
	MSize tp = bcread_uleb128(ls);
	if (tp >= BCDUMP_KTAB_STR) {
		MSize len = tp - BCDUMP_KTAB_STR;
		const char *p = (const char *)bcread_mem(ls, len);
		setstrV(ls->L, o, lj_str_new(ls->L, p, len));
	}
	else if (tp == BCDUMP_KTAB_INT) {
		setintV(o, (int32_t)bcread_uleb128(ls));
	}
	else if (tp == BCDUMP_KTAB_NUM) {
		o->u32.lo = bcread_uleb128(ls);
		o->u32.hi = bcread_uleb128(ls);
	}
	else {
		lua_assert(tp <= BCDUMP_KTAB_TRUE);
		setpriV(o, ~tp);
	}
}

double* bcread_ktabk_mod(int ls, double *o)
{
	int v2; // ebp
	char **v3; // esi
	unsigned int v4; // eax
	__m128i *v5; // esi
	size_t v6; // ecx
	unsigned int v7; // edi
	unsigned int v13; // eax
	__m128i v14; // xmm0
	__m128i v15; // xmm1
	int v16; // edx
	double *result; // eax
	int v18; // eax
	unsigned int v19; // edx
	double v20; // xmm0_8
	double *v21; // [esp+14h] [ebp-28h]
	unsigned int v22; // [esp+18h] [ebp-24h]
	__m128i v8;
	__m128i v9;
	__m128i v10;
	__m128i v11;
	__m128i v12;

	v2 = ls;
	v3 = (char **)(ls + 24);
	v21 = o;
	v4 = lj_buf_ruleb128((const char **)(ls + 24));
	if (v4 > 4)
	{
		v5 = *(__m128i **)(v2 + 24);
		v6 = v4 - 5;
		*(_DWORD *)(v2 + 24) = (uint32_t)((char *)v5 + v4 - 5);
		if (v4 != 5)
		{
			v22 = ((v4 - 21) >> 4) + 1;
			if (v4 - 6 <= 0xE)
			{
				v16 = 0;
			}
			else
			{
				v7 = 0;
				v8 = LD128(xmmword_94570);
				v9 = LD128(xmmword_94560);
				v10 = LD128(xmmword_943A0);
				v11 = LD128(xmmword_943B0);
				v12 = LD128(&xmmword_943C0);
				do
				{
					v13 = v7++;
					v14 = _mm_and_si128(
						_mm_or_si128(
							_mm_shuffle_epi8(_mm_add_epi32(_mm_load_si128((const __m128i *)&xmmword_94580), v9), v11),
							_mm_shuffle_epi8(v9, v10)),
						v12);
					v15 = _mm_or_si128(
						_mm_shuffle_epi8(_mm_add_epi32(_mm_load_si128((const __m128i *)&xmmword_94320), v9), v11),
						_mm_shuffle_epi8(_mm_add_epi32(_mm_load_si128((const __m128i *)&xmmword_94590), v9), v10));
					v9 = _mm_add_epi32(v9, v8);
					_mm_storeu_si128(
						&v5[v13],
						_mm_xor_si128(
							_mm_xor_si128(_mm_packus_epi16(v14, _mm_and_si128(v15, v12)), _mm_loadu_si128(&v5[v13])),
							LD128(xmmword_945A0)));
				} while (v22 > v7);
				v16 = 16 * v22;
				if (v6 == 16 * v22)
					goto LABEL_7;
			}
			v5[v16 / 0x10u].m128i_i8[0] = ~(v5[v16 / 0x10u].m128i_i8[0] ^ v16);
			if (v6 > (size_t)(v16 + 1))
			{
				v5[v16 / 0x10u].m128i_i8[1] = ~(v5[v16 / 0x10u].m128i_i8[1] ^ (v16 + 1));
				if (v6 > (size_t)(v16 + 2))
				{
					v5[v16 / 0x10u].m128i_i8[2] = ~(v5[v16 / 0x10u].m128i_i8[2] ^ (v16 + 2));
					if (v6 > (size_t)(v16 + 3))
					{
						v5[v16 / 0x10u].m128i_i8[3] = ~(v5[v16 / 0x10u].m128i_i8[3] ^ (v16 + 3));
						if (v6 > (size_t)(v16 + 4))
						{
							v5[v16 / 0x10u].m128i_i8[4] = ~(v5[v16 / 0x10u].m128i_i8[4] ^ (v16 + 4));
							if (v6 > (size_t)(v16 + 5))
							{
								v5[v16 / 0x10u].m128i_i8[5] = ~(v5[v16 / 0x10u].m128i_i8[5] ^ (v16 + 5));
								if (v6 > (size_t)(v16 + 6))
								{
									v5[v16 / 0x10u].m128i_i8[6] = ~(v5[v16 / 0x10u].m128i_i8[6] ^ (v16 + 6));
									if (v6 > (size_t)(v16 + 7))
									{
										v5[v16 / 0x10u].m128i_i8[7] = ~(v5[v16 / 0x10u].m128i_i8[7] ^ (v16 + 7));
										if (v6 > (size_t)(v16 + 8))
										{
											v5[v16 / 0x10u].m128i_i8[8] = ~(v5[v16 / 0x10u].m128i_i8[8] ^ (v16 + 8));
											if (v6 > (size_t)(v16 + 9))
											{
												v5[v16 / 0x10u].m128i_i8[9] = ~(v5[v16 / 0x10u].m128i_i8[9] ^ (v16 + 9));
												if (v6 > (size_t)(v16 + 10))
												{
													v5[v16 / 0x10u].m128i_i8[10] = ~(v5[v16 / 0x10u].m128i_i8[10] ^ (v16 + 10));
													if (v6 > (size_t)(v16 + 11))
													{
														v5[v16 / 0x10u].m128i_i8[11] = ~(v5[v16 / 0x10u].m128i_i8[11] ^ (v16 + 11));
														if (v6 > (size_t)(v16 + 12))
														{
															v5[v16 / 0x10u].m128i_i8[12] = ~(v5[v16 / 0x10u].m128i_i8[12] ^ (v16 + 12));
															v18 = v16 + 13;
															if (v6 > (size_t)(v16 + 13))
															{
																v19 = (uint32_t)(v16 + 14);
																*((_BYTE *)v5->m128i_i32 + v18) = ~(*((_BYTE *)v5->m128i_i32 + v18) ^ v18);
																if (v6 > v19)
																	*((_BYTE *)v5->m128i_i32 + v19) = ~(*((_BYTE *)v5->m128i_i32 + v19) ^ v19);
															}
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	LABEL_7:
		result = (double *)lj_str_new((lua_State*)*(_DWORD *)(v2 + 4), (char*)v5, v6);
		*((_DWORD *)v21 + 1) = -5;
		*(_DWORD *)v21 = (uint32_t)result;
		return result;
	}
	if (v4 == 3)
	{
		v20 = (double)(signed int)lj_buf_ruleb128((const char **)v3);
		result = v21;
		*v21 = v20;
	}
	else if (v4 == 4)
	{
		*(_DWORD *)v21 = lj_buf_ruleb128((const char **)v3);
		result = (double *)lj_buf_ruleb128((const char **)v3);
		*((_DWORD *)v21 + 1) = (uint32_t)result;
	}
	else
	{
		result = (double *)~v4;
		*((_DWORD *)v21 + 1) = (uint32_t)result;
	}
	return result;
}

/* Read a template table. */
static GCtab *bcread_ktab(LexState *ls)
{
	MSize narray = bcread_uleb128(ls);
	MSize nhash = bcread_uleb128(ls);
	GCtab *t = lj_tab_new(ls->L, narray, hsize2hbits(nhash));
	if (narray) {  /* Read array entries. */
		MSize i;
		TValue *o = tvref(t->array);
		for (i = 0; i < narray; i++, o++)
			bcread_ktabk(ls, o);
	}
	if (nhash) {  /* Read hash entries. */
		MSize i;
		for (i = 0; i < nhash; i++) {
			TValue key;
			bcread_ktabk(ls, &key);
			lua_assert(!tvisnil(&key));
			bcread_ktabk(ls, lj_tab_set(ls->L, t, &key));
		}
	}
	return t;
}

/* Read GC constants of a prototype. */
static void bcread_kgc(LexState *ls, GCproto *pt, MSize sizekgc)
{
	MSize i;
	GCRef *kr = mref(pt->k, GCRef) - (ptrdiff_t)sizekgc;
	for (i = 0; i < sizekgc; i++, kr++) {
		MSize tp = bcread_uleb128(ls);
		if (tp >= BCDUMP_KGC_STR)
		{
			MSize len = tp - BCDUMP_KGC_STR;
			const char *p = (const char *)bcread_mem(ls, len);
			setgcref(*kr, obj2gco(lj_str_new(ls->L, p, len)));
		}
		else if (tp == BCDUMP_KGC_TAB)
		{
			setgcref(*kr, obj2gco(bcread_ktab(ls)));
#if LJ_HASFFI
		}
		else if (tp != BCDUMP_KGC_CHILD)
		{
			CTypeID id = tp == BCDUMP_KGC_COMPLEX ? CTID_COMPLEX_DOUBLE :
				tp == BCDUMP_KGC_I64 ? CTID_INT64 : CTID_UINT64;
			CTSize sz = tp == BCDUMP_KGC_COMPLEX ? 16 : 8;
			GCcdata *cd = lj_cdata_new_(ls->L, id, sz);
			TValue *p = (TValue *)cdataptr(cd);
			setgcref(*kr, obj2gco(cd));
			p[0].u32.lo = bcread_uleb128(ls);
			p[0].u32.hi = bcread_uleb128(ls);
			if (tp == BCDUMP_KGC_COMPLEX)
			{
				p[1].u32.lo = bcread_uleb128(ls);
				p[1].u32.hi = bcread_uleb128(ls);
			}
#endif
		}
		else
		{
			lua_State *L = ls->L;
			lua_assert(tp == BCDUMP_KGC_CHILD);
			if (L->top <= bcread_oldtop(L, ls))  /* Stack underflow? */
				bcread_error(ls, LJ_ERR_BCBAD);
			L->top--;
			setgcref(*kr, obj2gco(protoV(L->top)));
		}
	}
}


/* Read number constants of a prototype. */
static void bcread_knum(LexState *ls, GCproto *pt, MSize sizekn)
{
	MSize i;
	TValue *o = mref(pt->k, TValue);
	for (i = 0; i < sizekn; i++, o++) {
		int isnum = (ls->p[0] & 1);
		uint32_t lo = bcread_uleb128_33(ls);
		if (isnum) {
			o->u32.lo = lo;
			o->u32.hi = bcread_uleb128(ls);
		}
		else {
			setintV(o, lo);
		}
	}
}


/* Read bytecode instructions. */
static void bcread_bytecode(LexState *ls, GCproto *pt, MSize sizebc)
{
	BCIns *bc = proto_bc(pt);
	bc[0] = BCINS_AD((pt->flags & PROTO_VARARG) ? BC_FUNCV : BC_FUNCF,
		pt->framesize, 0);
	bcread_block(ls, bc + 1, (sizebc - 1)*(MSize)sizeof(BCIns));
	/* Swap bytecode instructions if the endianess differs. */
	if (bcread_swap(ls)) {
		MSize i;
		for (i = 1; i < sizebc; i++) bc[i] = lj_bswap(bc[i]);
	}
}

/* Read upvalue refs. */
static void bcread_uv(LexState *ls, GCproto *pt, MSize sizeuv)
{
	if (sizeuv) {
		uint16_t *uv = proto_uv(pt);
		bcread_block(ls, uv, sizeuv * 2);
		/* Swap upvalue refs if the endianess differs. */
		if (bcread_swap(ls)) {
			MSize i;
			for (i = 0; i < sizeuv; i++)
				uv[i] = (uint16_t)((uv[i] >> 8) | (uv[i] << 8));
		}
	}
}

/* Read a prototype. */
GCproto *lj_bcread_proto(LexState *ls)
{
	GCproto *pt;
	MSize framesize, numparams, flags, sizeuv, sizekgc, sizekn, sizebc, sizept;
	MSize ofsk, ofsuv, ofsdbg;
	MSize sizedbg = 0;
	BCLine firstline = 0, numline = 0;

	/* Read prototype header. */
	flags = bcread_byte(ls);
	numparams = bcread_byte(ls);
	framesize = bcread_byte(ls);
	sizeuv = bcread_byte(ls);
	sizekgc = bcread_uleb128(ls);
	sizekn = bcread_uleb128(ls);
	sizebc = bcread_uleb128(ls) + 1;
	if (!(bcread_flags(ls) & BCDUMP_F_STRIP)) {
		sizedbg = bcread_uleb128(ls);
		if (sizedbg) {
			firstline = bcread_uleb128(ls);
			numline = bcread_uleb128(ls);
		}
	}

	/* Calculate total size of prototype including all colocated arrays. */
	sizept = (MSize)sizeof(GCproto) +
		sizebc*(MSize)sizeof(BCIns) +
		sizekgc*(MSize)sizeof(GCRef);
	sizept = (sizept + (MSize)sizeof(TValue) - 1) & ~((MSize)sizeof(TValue) - 1);
	ofsk = sizept; sizept += sizekn*(MSize)sizeof(TValue);
	ofsuv = sizept; sizept += ((sizeuv + 1)&~1) * 2;
	ofsdbg = sizept; sizept += sizedbg;

	/* Allocate prototype object and initialize its fields. */
	pt = (GCproto *)lj_mem_newgco(ls->L, (MSize)sizept);
	pt->gct = ~LJ_TPROTO;
	pt->numparams = (uint8_t)numparams;
	pt->framesize = (uint8_t)framesize;
	pt->sizebc = sizebc;
	setmref(pt->k, (char *)pt + ofsk);
	setmref(pt->uv, (char *)pt + ofsuv);
	pt->sizekgc = 0;  /* Set to zero until fully initialized. */
	pt->sizekn = sizekn;
	pt->sizept = sizept;
	pt->sizeuv = (uint8_t)sizeuv;
	pt->flags = (uint8_t)flags;
	pt->trace = 0;
	setgcref(pt->chunkname, obj2gco(ls->chunkname));

	/* Close potentially uninitialized gap between bc and kgc. */
	*(uint32_t *)((char *)pt + ofsk - sizeof(GCRef)*(sizekgc + 1)) = 0;

	/* Read bytecode instructions and upvalue refs. */
	bcread_bytecode(ls, pt, sizebc);
	bcread_uv(ls, pt, sizeuv);

	/* Read constants. */
	bcread_kgc(ls, pt, sizekgc);
	pt->sizekgc = sizekgc;
	bcread_knum(ls, pt, sizekn);

	/* Read and initialize debug info. */
	pt->firstline = firstline;
	pt->numline = numline;
	if (sizedbg) {
		MSize sizeli = (sizebc - 1) << (numline < 256 ? 0 : numline < 65536 ? 1 : 2);
		setmref(pt->lineinfo, (char *)pt + ofsdbg);
		setmref(pt->uvinfo, (char *)pt + ofsdbg + sizeli);
		bcread_dbg(ls, pt, sizedbg);
		setmref(pt->varinfo, bcread_varinfo(pt));
	}
	else {
		setmref(pt->lineinfo, NULL);
		setmref(pt->uvinfo, NULL);
		setmref(pt->varinfo, NULL);
	}
	return pt;
}

///* Read a prototype. */
int lj_bcread_proto_mod(int ls)
{
	char *ls_p; // eax
	char v2; // cl
	char v3; // cl
	char v4; // cl
	unsigned __int8 v5; // cl
	unsigned int ofsk; // esi
	int pt; // eax
	int pt_; // edx
	int v9; // edx
	char *v10; // esi
	int v11; // eax
	int v12; // edi
	char v13; // cl
	int v14; // edx
	int v15; // edi
	unsigned __int8 *v16; // esi
	unsigned __int8 v17; // al
	char *v18; // esi
	unsigned __int8 v19; // cl
	unsigned __int8 v20; // al
	signed int v21; // ecx
	unsigned int tp; // eax
	__m128i *v23; // esi
	size_t v24; // ecx
	unsigned int v25; // edi
	unsigned int v26; // eax
	unsigned int v28; // ecx
	__m128i v29; // xmm0
	__m128i v30; // xmm1
	int v31; // edx
	int result; // eax
	signed int v33; // ecx
	int v34; // edi
	char v35; // dl
	__int16 v36; // si
	int v37; // edi
	int v38; // eax
	unsigned int v39; // edx
	int v40; // eax
	unsigned int v41; // edx
	int v42; // eax
	int v43; // eax
	unsigned int v44; // edi
	double *v45; // esi
	double *v46; // edx
	unsigned int v47; // edi
	double *v48; // eax
	int v49; // esi
	int v50; // ecx
	int v51; // edx
	unsigned int v52; // esi
	int v53; // ecx
	_BYTE *v54; // edx
	char *v55; // eax
	unsigned int v56; // edi
	int v57; // ecx
	signed int v58; // esi
	unsigned int v59; // ecx
	int v60; // eax
	unsigned int v61; // edi
	const __m128i *v64; // esi
	__m128i v65; // xmm1
	unsigned int v66; // esi
	int v67; // edi
	int v68; // ecx
	unsigned int v69; // eax
	char v70; // cl
	__int16 v71; // cx
	__int16 v72; // si
	char v73; // cl
	unsigned int sizebc; // [esp+14h] [ebp-58h]
	char v87; // [esp+14h] [ebp-58h]
	int v88; // [esp+14h] [ebp-58h]
	__m128i *v89; // [esp+14h] [ebp-58h]
	char **var54; // [esp+18h] [ebp-54h]
	unsigned int sizekgc; // [esp+1Ch] [ebp-50h]
	unsigned int sizekn; // [esp+20h] [ebp-4Ch]
	int v91; // [esp+20h] [ebp-4Ch]
	unsigned int v92; // [esp+24h] [ebp-48h]
	int v93; // [esp+24h] [ebp-48h]
	_DWORD *t; // [esp+24h] [ebp-48h]
	unsigned int v95; // [esp+24h] [ebp-48h]
	unsigned __int8 sizeuv; // [esp+28h] [ebp-44h]
	int v99; // [esp+28h] [ebp-44h]
	int v100; // [esp+28h] [ebp-44h]
	unsigned int v101; // [esp+28h] [ebp-44h]
	char v102; // [esp+28h] [ebp-44h]
	size_t sizeuv_2; // [esp+2Ch] [ebp-40h]
	unsigned int v104; // [esp+2Ch] [ebp-40h]
	int pt__; // [esp+30h] [ebp-3Ch]
	unsigned int var38; // [esp+34h] [ebp-38h]
	char framesize; // [esp+38h] [ebp-34h]
	char numparams; // [esp+3Eh] [ebp-2Eh]
	char flags; // [esp+3Fh] [ebp-2Dh]
	double key[4]; // [esp+48h] [ebp-24h]
	__m128i v27;


	ls_p = *(char **)(ls + 24);
	*(_DWORD *)(ls + 24) = (uint32_t)(ls_p + 1);
	v2 = *ls_p;
	*(_DWORD *)(ls + 24) = (uint32_t)(ls_p + 2);
	framesize = v2;
	v3 = ls_p[1] ^ v2;
	*(_DWORD *)(ls + 24) = (uint32_t)(ls_p + 3);
	flags = v3;
	v4 = ls_p[2] ^ v3;
	*(_DWORD *)(ls + 24) = (uint32_t)(ls_p + 4);
	numparams = v4;
	v5 = ls_p[3] ^ v4;
	sizeuv_2 = v5;
	sizeuv = v5;
	var54 = (char **)(ls + 24);
	sizekn = lj_buf_ruleb128((const char **)(ls + 24));
	sizekgc = lj_buf_ruleb128((const char **)(ls + 24));
	v92 = lj_buf_ruleb128((const char **)(ls + 24));
	sizebc = v92 + 1;
	if (!(*(_BYTE *)(ls + 108) & 2) && lj_buf_ruleb128((const char **)var54))
	{
		*(_DWORD *)(ls + 24) += 4;
		lj_buf_ruleb128((const char **)var54);
		lj_buf_ruleb128((const char **)var54);
		lj_buf_ruleb128((const char **)var54);
		lj_buf_ruleb128((const char **)var54);
		lj_buf_ruleb128((const char **)var54);
		bcread_error_mod((lua_State*)*(_DWORD *)(ls + 4), *(const char **)(ls + 80));
	}
	ofsk = (4 * (sizebc + sizekgc + 18) + 7) & 0xFFFFFFF8;// sizept = (MSize)sizeof(GCproto) + sizebc*(MSize)sizeof(BCIns) + sizekgc*(MSize)sizeof(GCRef);
														  //       sizept = (sizept + (MSize)sizeof(TValue)-1) & ~((MSize)sizeof(TValue)-1);
	pt = (int)lj_mem_newgco(
		(lua_State*)*(_DWORD *)(ls + 4),
		ofsk + 8 * sizekn + ((2 * sizeuv_2 + 2) & 0x3FC));   // 调换了参数 lua_State *L 和 GCSize size的位置
	*(_BYTE *)(pt + 5) = 7;                       // pt->gct
	pt_ = pt;
	pt__ = pt;
	*(_DWORD *)(pt + 8) = sizebc;
	*(_DWORD *)(pt + 44) = ofsk + 8 * sizekn + ((2 * sizeuv_2 + 2) & 0x3FC);// pt->sizept
	*(_DWORD *)(pt + 20) = 0;                     // pt->sizekgc = 0;
	*(_BYTE *)(pt + 6) = framesize;
	*(_DWORD *)(pt + 36) = sizekn;
	*(_DWORD *)(pt + 40) = 0;                      // pt->trace = 0;  已改为4字节无符号整数
	*(_DWORD *)(pt + 28) = flags;				// 已改为4字节无符号整数
	*(_BYTE *)(pt + 7) = numparams;
	*(_DWORD *)(pt + 16) = sizeuv;				// 已改为4字节无符号整数
	*(_DWORD *)(pt_ + 32) = pt_ + ofsk;           // pt->k
	*(_DWORD *)(pt_ + 24) = pt_ + ofsk + 8 * sizekn;// pt->uv
	*(_DWORD *)(pt + 48) = *(_DWORD *)(ls + 76);  // chunkname
	*(_DWORD *)(pt + ofsk - (4 * sizekgc + 4)) = 0;// Close potentially uninitialized gap between bc and kgc.
	*(_DWORD *)(pt_ + 72) = ((*(_BYTE *)(pt_ + 28) & 2u) < 1 ? 90 : 93) | (*(unsigned __int8 *)(pt_ + 6) << 8);// bcread_bytecode_mod() begin
	v9 = 4 * v92;
	v10 = *(char **)(ls + 24);
	v11 = pt + 76;
	var38 = 4 * v92;
	v12 = v11;
	*(_DWORD *)(ls + 24) = (uint32_t)&v10[4 * v92];
	if (4 * v92 >= 8)
	{
		if (v11 & 1)
		{
			v70 = *v10++;
			*(_BYTE *)(pt__ + 76) = v70;
			v12 = pt__ + 77;
			var38 = v9 - 1;
		}
		if (v12 & 2)
		{
			v71 = *(_WORD *)v10;
			v12 += 2;
			v10 += 2;
			*(_WORD *)(v12 - 2) = v71;
			var38 -= 2;
		}
		if (v12 & 4)
		{
			v53 = *(_DWORD *)v10;
			v12 += 4;
			v10 += 4;
			*(_DWORD *)(v12 - 4) = v53;
			var38 -= 4;
		}
	}
	qmemcpy((void *)v12, v10, var38);
	if (*(_BYTE *)(ls + 108) & 1 && sizebc > 1)
	{
		v50 = pt__;
		v51 = pt__ + v9;
		do
		{
			v52 = *(_DWORD *)(v50 + 76);
			v50 += 4;
			*(_DWORD *)(v50 + 72) = _byteswap_ulong(v52);
		} while (v50 != v51);
	}
	else if (!v92)
	{
		goto LABEL_8;
	}
	v13 = 0;
	v14 = 0;
	do
	{
		*(_BYTE *)(v11 + 4 * v14 + 3) ^= v13++;
		*(_BYTE *)(v11 + 4 * v14 + 1) = ~*(_BYTE *)(v11 + 4 * v14 + 1);
		uint8_t obf_op = *(_BYTE *)(v11 + 4 * v14);
		*(_BYTE *)(v11 + 4 * v14) = op_map[obf_op];
		++v14;
	} while (v14 != v92);
LABEL_8:                                        // bcread_uv() begin
	if (!sizeuv)
		goto LABEL_9;
	v89 = *(__m128i **)(pt__ + 24);
	v54 = *(_BYTE **)(pt__ + 24);
	v55 = *(char **)(ls + 24);
	v56 = 2 * sizeuv_2;
	*(_DWORD *)(ls + 24) = (uint32_t)&v55[2 * sizeuv_2];
	v57 = (int)v54;
	if (2 * sizeuv_2 >= 8)
	{
		if ((unsigned __int8)v54 & 1)
		{
			v73 = *v55;
			--v56;
			++v55;
			*v54 = v73;
			v57 = (int)(v54 + 1);
		}
		if (v57 & 2)
		{
			v72 = *(_WORD *)v55;
			v57 += 2;
			v55 += 2;
			v56 -= 2;
			*(_WORD *)(v57 - 2) = v72;
		}
		if (v56 >= 8)
		{
			v95 = v56 & 0xFFFFFFF8;
			v66 = 0;
			v102 = v56;
			do
			{
				v67 = *(_DWORD *)&v55[v66];
				*(_DWORD *)(v57 + v66 + 4) = *(_DWORD *)&v55[v66 + 4];
				*(_DWORD *)(v57 + v66) = v67;
				v66 += 8;
			} while (v66 < v95);
			LOBYTE(v56) = v102;
			v57 += v66;
			v55 += v66;
		}
	}
	v58 = 0;
	if (v56 & 4)
	{
		*(_DWORD *)v57 = *(_DWORD *)v55;
		v58 = 4;
		if (!(v56 & 2))
		{
		LABEL_78:
			if (!(v56 & 1))
				goto LABEL_79;
			goto LABEL_86;
		}
	}
	else if (!(v56 & 2))
	{
		goto LABEL_78;
	}
	*(_WORD *)(v57 + v58) = *(_WORD *)&v55[v58];
	v58 += 2;
	if (!(v56 & 1))
	{
	LABEL_79:
		if (!(*(_BYTE *)(ls + 108) & 1))
			goto LABEL_9;
		goto LABEL_80;
	}
LABEL_86:
	*(_BYTE *)(v57 + v58) = v55[v58];
	if (!(*(_BYTE *)(ls + 108) & 1))
		goto LABEL_9;
LABEL_80:
	v59 = ((sizeuv_2 - 8) >> 3) + 1;
	v60 = 8 * v59;
	if (sizeuv_2 - 1 <= 6)
	{
		v60 = 0;
	}
	else
	{
		v61 = 0;
		v64 = v89;
		do
		{
			v65 = _mm_loadu_si128(v64);
			++v61;
			++v64;
			_mm_storeu_si128(
				(__m128i *)&v64[-1],
				_mm_or_si128(
					_mm_or_si128(
						_mm_shuffle_epi8(_mm_slli_epi32(_mm_unpackhi_epi16(v65, _mm_setzero_si128()), 8u), LD128(xmmword_943B0)),
						_mm_shuffle_epi8(_mm_slli_epi32(_mm_unpacklo_epi16(v65, _mm_setzero_si128()), 8u), LD128(xmmword_943A0))),
					_mm_srli_epi16(v65, 8u)));
		} while (v59 > v61);
		if (sizeuv_2 == v60)
			goto LABEL_9;
	}
	v89[v60 / 8u].m128i_i16[0] = (v89[v60 / 8u].m128i_i16[0] << 8) | ((unsigned __int16)v89[v60 / 8u].m128i_i16[0] >> 8);
	if (sizeuv_2 > (size_t)(v60 + 1))
	{
		*((_WORD *)v89->m128i_i32 + v60 + 1) = (*((_WORD *)v89->m128i_i32 + v60 + 1) << 8) | (*((_WORD *)v89->m128i_i32
			+ v60
			+ 1) >> 8);
		if (sizeuv_2 > (size_t)(v60 + 2))
		{
			*((_WORD *)v89->m128i_i32 + v60 + 2) = (*((_WORD *)v89->m128i_i32 + v60 + 2) << 8) | (*((_WORD *)v89->m128i_i32
				+ v60
				+ 2) >> 8);
			if (sizeuv_2 > (size_t)(v60 + 3))
			{
				*((_WORD *)v89->m128i_i32 + v60 + 3) = (*((_WORD *)v89->m128i_i32 + v60 + 3) << 8) | (*((_WORD *)v89->m128i_i32
					+ v60
					+ 3) >> 8);
				if (sizeuv_2 > (size_t)(v60 + 4))
				{
					*((_WORD *)v89->m128i_i32 + v60 + 4) = (*((_WORD *)v89->m128i_i32 + v60 + 4) << 8) | (*((_WORD *)v89->m128i_i32
						+ v60
						+ 4) >> 8);
					v68 = v60 + 5;
					if (sizeuv_2 > (size_t)(v60 + 5))
					{
						v69 = v60 + 6;
						*((_WORD *)v89->m128i_i32 + v68) = (*((_WORD *)v89->m128i_i32 + v68) << 8) | (*((_WORD *)v89->m128i_i32 + v68) >> 8);
						if (sizeuv_2 > v69)
							*((_WORD *)v89->m128i_i32 + v69) = (*((_WORD *)v89->m128i_i32 + v69) << 8) | (*((_WORD *)v89->m128i_i32
								+ v69) >> 8);
					}
				}
			}
		}
	}
LABEL_9:                                        // bcread_knum() begin
	v15 = 0;
	v93 = *(_DWORD *)(pt__ + 32);
	if (sizekn)
	{
		do
		{
			while (1)
			{
				v16 = *(unsigned __int8 **)(ls + 24);
				v17 = *v16;
				v18 = (char *)(v16 + 1);
				v19 = v17;
				v20 = v17 >> 1;
				v87 = v19 & 1;
				v21 = v20;
				if (v20 > 0x3Fu)
				{
					v33 = -1;
					v100 = v15;
					v34 = v20 & 0x3F;
					do
					{
						v35 = *v18;
						v33 += 7;
						++v18;
						v34 |= (v35 & 0x7F) << v33;
					} while (v35 < 0);
					v21 = v34;
					v15 = v100;
				}
				*(_DWORD *)(ls + 24) = (uint32_t)v18;
				if (v87)
					break;
				*(double *)(v93 + 8 * v15++) = (double)v21;
				if (v15 == sizekn)
					goto LABEL_15;
			}
			*(_DWORD *)(v93 + 8 * v15) = v21;
			*(_DWORD *)(v93 + 8 * v15++ + 4) = lj_buf_ruleb128((const char **)var54);
		} while (v15 != sizekn);
	LABEL_15:
		v93 = *(_DWORD *)(pt__ + 32);
	}
	v91 = v93 - 4 * sizekgc;                      // bcread_kgc() begin
	v88 = 0;
	if (sizekgc)
	{
		while (1)
		{
			while (1)
			{
				tp = lj_buf_ruleb128((const char **)var54);
				if (tp <= 4)
					break;
				v23 = *(__m128i **)(ls + 24);
				v24 = tp - 5;
				*(_DWORD *)(ls + 24) = (uint32_t)((char *)v23 + tp - 5);
				if (tp != 5)
				{
					v25 = ((tp - 21) >> 4) + 1;
					if (tp - 6 <= 0xE)
					{
						v31 = 0;
					LABEL_37:
						v23[v31 / 0x10u].m128i_i8[0] = ~(v23[v31 / 0x10u].m128i_i8[0] ^ v31);
						if (v24 > (size_t)(v31 + 1))
						{
							v23[v31 / 0x10u].m128i_i8[1] = ~(v23[v31 / 0x10u].m128i_i8[1] ^ (v31 + 1));
							if ((size_t)(v31 + 2) < v24)
							{
								v23[v31 / 0x10u].m128i_i8[2] = ~(v23[v31 / 0x10u].m128i_i8[2] ^ (v31 + 2));
								if (v24 > (size_t)(v31 + 3))
								{
									v23[v31 / 0x10u].m128i_i8[3] = ~(v23[v31 / 0x10u].m128i_i8[3] ^ (v31 + 3));
									if (v24 > (size_t)(v31 + 4))
									{
										v23[v31 / 0x10u].m128i_i8[4] = ~(v23[v31 / 0x10u].m128i_i8[4] ^ (v31 + 4));
										if (v24 > (size_t)(v31 + 5))
										{
											v23[v31 / 0x10u].m128i_i8[5] = ~(v23[v31 / 0x10u].m128i_i8[5] ^ (v31 + 5));
											if (v24 > (size_t)(v31 + 6))
											{
												v23[v31 / 0x10u].m128i_i8[6] = ~(v23[v31 / 0x10u].m128i_i8[6] ^ (v31 + 6));
												if (v24 > (size_t)(v31 + 7))
												{
													v23[v31 / 0x10u].m128i_i8[7] = ~(v23[v31 / 0x10u].m128i_i8[7] ^ (v31 + 7));
													if (v24 > (size_t)(v31 + 8))
													{
														v23[v31 / 0x10u].m128i_i8[8] = ~(v23[v31 / 0x10u].m128i_i8[8] ^ (v31 + 8));
														if (v24 > (size_t)(v31 + 9))
														{
															v23[v31 / 0x10u].m128i_i8[9] = ~(v23[v31 / 0x10u].m128i_i8[9] ^ (v31 + 9));
															if (v24 > (size_t)(v31 + 10))
															{
																v23[v31 / 0x10u].m128i_i8[10] = ~(v23[v31 / 0x10u].m128i_i8[10] ^ (v31 + 10));
																if (v24 > (size_t)(v31 + 11))
																{
																	v23[v31 / 0x10u].m128i_i8[11] = ~(v23[v31 / 0x10u].m128i_i8[11] ^ (v31 + 11));
																	if (v24 > (size_t)(v31 + 12))
																	{
																		v23[v31 / 0x10u].m128i_i8[12] = ~(v23[v31 / 0x10u].m128i_i8[12] ^ (v31 + 12));
																		v40 = v31 + 13;
																		if (v24 > (size_t)(v31 + 13))
																		{
																			v41 = v31 + 14;
																			*((_BYTE *)v23->m128i_i32 + v40) = ~(*((_BYTE *)v23->m128i_i32 + v40) ^ v40);
																			if (v24 > v41)
																				*((_BYTE *)v23->m128i_i32 + v41) = ~(*((_BYTE *)v23->m128i_i32 + v41) ^ v41);
																		}
																	}
																}
															}
														}
													}
												}
											}
										}
									}
								}
							}
						}
						goto LABEL_23;
					}
					v99 = tp - 5;
					v26 = 0;
					v27 = LD128(xmmword_94560);
					do
					{
						v28 = v26++;
						v29 = _mm_and_si128(
							_mm_or_si128(
								_mm_shuffle_epi8(
									_mm_add_epi32(LD128(xmmword_94580), v27),
									LD128(xmmword_943B0)),
								_mm_shuffle_epi8(v27, LD128(xmmword_943A0))),
							LD128(xmmword_943C0));
						v30 = _mm_or_si128(
							_mm_shuffle_epi8(
								_mm_add_epi32(LD128(xmmword_94320), v27),
								LD128(xmmword_943B0)),
							_mm_shuffle_epi8(
								_mm_add_epi32(LD128(xmmword_94590), v27),
								LD128(xmmword_943A0)));
						v27 = _mm_add_epi32(v27, LD128(xmmword_94570));
						_mm_storeu_si128(
							&v23[v28],
							_mm_xor_si128(
								_mm_packus_epi16(v29, _mm_and_si128(v30, LD128(xmmword_943C0))),
								_mm_xor_si128(_mm_loadu_si128(&v23[v28]), LD128(xmmword_945A0))));
					} while (v26 < v25);
					v24 = v99;
					v31 = 16 * v25;
					if (v99 != 16 * v25)
						goto LABEL_37;
				}
			LABEL_23:
				*(_DWORD *)(v91 + 4 * v88++) = (uint32_t)lj_str_new((lua_State*)*(_DWORD *)(ls + 4), (char*)v23, v24);
				if (v88 == sizekgc)
					goto LABEL_24;
			}
			if (tp != 1)
			{
				if (tp)
				{
					if (tp == 4)
					{
						v49 = (int)lj_mem_newgco((lua_State *)(ls + 4), 24);
						*(_BYTE *)(v49 + 5) = 10;
						*(_WORD *)(v49 + 6) = 16;
						*(_DWORD *)(v91 + 4 * v88) = v49;
						*(_DWORD *)(v49 + 8) = lj_buf_ruleb128((const char **)var54);
						*(_DWORD *)(v49 + 12) = lj_buf_ruleb128((const char **)var54);
						*(_DWORD *)(v49 + 16) = lj_buf_ruleb128((const char **)var54);
						*(_DWORD *)(v49 + 20) = lj_buf_ruleb128((const char **)var54);
					}
					else
					{
						v36 = (tp != 2) + 11;
						v37 = (int)lj_mem_newgco((lua_State *)(ls + 4), 16);
						*(_WORD *)(v37 + 6) = v36;
						*(_BYTE *)(v37 + 5) = 10;
						*(_DWORD *)(v91 + 4 * v88) = v37;
						*(_DWORD *)(v37 + 8) = lj_buf_ruleb128((const char **)var54);
						*(_DWORD *)(v37 + 12) = lj_buf_ruleb128((const char **)var54);
					}
				}
				else
				{
					v38 = *(_DWORD *)(ls + 4);
					v39 = *(_DWORD *)(v38 + 20);
					if (v39 <= *(_DWORD *)(ls + 72) + *(_DWORD *)(v38 + 28))
						bcread_error_mod((lua_State*)v38, *(const char **)(ls + 80));
					*(_DWORD *)(v38 + 20) = v39 - 8;
					*(_DWORD *)(v91 + 4 * v88) = *(_DWORD *)(v39 - 8);
				}
				goto LABEL_32;
			}
			v101 = lj_buf_ruleb128((const char **)var54);            // bcread_ktab() begin
			v104 = lj_buf_ruleb128((const char **)var54);
			if (!v101)
				break;
			if (v101 == 1)
			{
				v43 = 1;
			}
			else
			{
				_BitScanReverse((unsigned int *)&v42, v101 - 1);
				v43 = v42 + 1;
			}
			t = (uint32_t*)lj_tab_new((lua_State*)*(_DWORD *)(ls + 4), v104, v43);
			if (v104)
				goto LABEL_56;
		LABEL_59:
			v47 = 0;
			do
			{
				bcread_ktabk_mod(ls, key);
				++v47;
				v48 = (double *)lj_tab_set((lua_State*)*(_DWORD *)(ls + 4), (GCtab*)t, (cTValue*)key);
				bcread_ktabk_mod(ls, v48);
			} while (v101 > v47);
		LABEL_61:
			*(_DWORD *)(v91 + 4 * v88) = (uint32_t)t;
		LABEL_32:
			if (++v88 == sizekgc)
				goto LABEL_24;
		}
		t = (uint32_t*)lj_tab_new((lua_State*)*(_DWORD *)(ls + 4), v104, 0);
		if (!v104)
		{
			*(_DWORD *)(v91 + 4 * v88) = (uint32_t)t;
			goto LABEL_32;
		}
	LABEL_56:
		v44 = 0;
		v45 = (double *)t[2];
		do
		{
			v46 = v45;
			++v44;
			++v45;
			bcread_ktabk_mod(ls, v46);
		} while (v104 > v44);                       // bcread_ktab() end
		if (!v101)
			goto LABEL_61;
		goto LABEL_59;
	}
LABEL_24:
	result = pt__;
	*(_DWORD *)(pt__ + 52) = 0;
	*(_DWORD *)(pt__ + 20) = sizekgc;
	*(_DWORD *)(pt__ + 56) = 0;
	*(_DWORD *)(pt__ + 60) = 0;
	*(_DWORD *)(pt__ + 64) = 0;
	*(_DWORD *)(pt__ + 68) = 0;
	return result;
}

/* Read and check header of bytecode dump. */
static int bcread_header(LexState *ls)
{
	uint32_t flags;
	bcread_want(ls, 3 + 5 + 5);
	if (bcread_byte(ls) != BCDUMP_HEAD2 ||
		bcread_byte(ls) != BCDUMP_HEAD3 ||
		bcread_byte(ls) != BCDUMP_VERSION) return 0;
	bcread_flags(ls) = flags = bcread_uleb128(ls);
	if ((flags & ~(BCDUMP_F_KNOWN)) != 0) return 0;
	if ((flags & BCDUMP_F_FR2) != LJ_FR2*BCDUMP_F_FR2) return 0;
	if ((flags & BCDUMP_F_FFI)) {
#if LJ_HASFFI
		lua_State *L = ls->L;
		if (!ctype_ctsG(G(L))) {
			ptrdiff_t oldtop = savestack(L, L->top);
			luaopen_ffi(L);  /* Load FFI library on-demand. */
			L->top = restorestack(L, oldtop);
		}
#else
		return 0;
#endif
}
	if ((flags & BCDUMP_F_STRIP)) {
		ls->chunkname = lj_str_newz(ls->L, ls->chunkarg);
	}
	else {
		MSize len = bcread_uleb128(ls);
		bcread_need(ls, len);
		ls->chunkname = lj_str_new(ls->L, (const char *)bcread_mem(ls, len), len);
	}
	return 1;  /* Ok. */
	}

/* Read a bytecode dump. */
GCproto *lj_bcread(LexState *ls)
{
	lua_State *L = ls->L;
	lua_assert(ls->c == BCDUMP_HEAD1);
	bcread_savetop(L, ls, L->top);
	lj_buf_reset(&ls->sb);
	/* Check for a valid bytecode dump header. */
	if (!bcread_header(ls))
		bcread_error(ls, LJ_ERR_BCFMT);
	for (;;) {  /* Process all prototypes in the bytecode dump. */
		GCproto *pt;
		MSize len;
		const char *startp;
		/* Read length. */
		if (ls->p < ls->pe && ls->p[0] == 0) {  /* Shortcut EOF. */
			ls->p++;
			break;
		}
		bcread_want(ls, 5);
		len = bcread_uleb128(ls);
		if (!len) break;  /* EOF */
		bcread_need(ls, len);
		startp = ls->p;
		pt = lj_bcread_proto(ls);
		if (ls->p != startp + len)
			bcread_error(ls, LJ_ERR_BCBAD);
		setprotoV(L, L->top, pt);
		incr_top(L);
	}
	if ((int32_t)(2 * (uint32_t)(ls->pe - ls->p)) > 0 ||
		L->top - 1 != bcread_oldtop(L, ls))
		bcread_error(ls, LJ_ERR_BCBAD);
	/* Pop off last prototype. */
	L->top--;
	return protoV(L->top);
}

/* Read a bytecode dump. */
GCproto *lj_bcread_mod(LexState *ls)
{
	lua_State *L = ls->L;
	lua_assert(ls->c == BCDUMP_HEAD1);
	bcread_savetop(L, ls, L->top);
	lj_buf_reset(&ls->sb);
	/* Check for a valid bytecode dump header. */
	if (!bcread_header(ls))
		bcread_error(ls, LJ_ERR_BCFMT);
	for (;;) {  /* Process all prototypes in the bytecode dump. */
		GCproto *pt;
		MSize len;
		const char *startp;
		/* Read length. */
		if (ls->p < ls->pe && ls->p[0] == 0) {  /* Shortcut EOF. */
			ls->p++;
			break;
		}
		bcread_want(ls, 5);
		len = bcread_uleb128(ls);
		if (!len) break;  /* EOF */
		bcread_need(ls, len);
		startp = ls->p;
		pt = (GCproto *)lj_bcread_proto_mod((int)ls);
		if (ls->p != startp + len)
			bcread_error(ls, LJ_ERR_BCBAD);
		setprotoV(L, L->top, pt);
		incr_top(L);
	}
	if ((int32_t)(2 * (uint32_t)(ls->pe - ls->p)) > 0 ||
		L->top - 1 != bcread_oldtop(L, ls))
		bcread_error(ls, LJ_ERR_BCBAD);
	/* Pop off last prototype. */
	L->top--;
	return protoV(L->top);
}
