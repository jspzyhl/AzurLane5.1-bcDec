/*
** String handling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_str_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_char.h"

#include "defs.h"

/* -- String helpers ------------------------------------------------------ */

/* Ordered compare of strings. Assumes string data is 4-byte aligned. */
int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b)
{
	MSize i, n = a->len > b->len ? b->len : a->len;
	for (i = 0; i < n; i += 4) {
		/* Note: innocuous access up to end of string + 3. */
		uint32_t va = *(const uint32_t *)(strdata(a) + i);
		uint32_t vb = *(const uint32_t *)(strdata(b) + i);
		if (va != vb) {
#if LJ_LE
			va = lj_bswap(va); vb = lj_bswap(vb);
#endif
			i -= n;
			if ((int32_t)i >= -3) {
				va >>= 32 + (i << 3); vb >>= 32 + (i << 3);
				if (va == vb) break;
			}
			return va < vb ? -1 : 1;
		}
	}
	return (int32_t)(a->len - b->len);
}

/* Fast string data comparison. Caveat: unaligned access to 1st string! */
static LJ_AINLINE int str_fastcmp(const char *a, const char *b, MSize len)
{
	MSize i = 0;
	lua_assert(len > 0);
	lua_assert((((uintptr_t)a + len - 1) & (LJ_PAGESIZE - 1)) <= LJ_PAGESIZE - 4);
	do {  /* Note: innocuous access up to end of string + 3. */
		uint32_t v = lj_getu32(a + i) ^ *(const uint32_t *)(b + i);
		if (v) {
			i -= len;
#if LJ_LE
			return (int32_t)i >= -3 ? (v << (32 + (i << 3))) : 1;
#else
			return (int32_t)i >= -3 ? (v >> (32 + (i << 3))) : 1;
#endif
		}
		i += 4;
	} while (i < len);
	return 0;
}

/* Find fixed string p inside string s. */
const char *lj_str_find(const char *s, const char *p, MSize slen, MSize plen)
{
	if (plen <= slen) {
		if (plen == 0) {
			return s;
		}
		else {
			int c = *(const uint8_t *)p++;
			plen--; slen -= plen;
			while (slen) {
				const char *q = (const char *)memchr(s, c, slen);
				if (!q) break;
				if (memcmp(q + 1, p, plen) == 0) return q;
				q++; slen -= (MSize)(q - s); s = q;
			}
		}
	}
	return NULL;
}

/* Check whether a string has a pattern matching character. */
int lj_str_haspattern(GCstr *s)
{
	const char *p = strdata(s), *q = p + s->len;
	while (p < q) {
		int c = *(const uint8_t *)p++;
		if (lj_char_ispunct(c) && strchr("^$*+?.([%-", c))
			return 1;  /* Found a pattern matching char. */
	}
	return 0;  /* No pattern matching chars found. */
}

/* -- String interning ---------------------------------------------------- */

/* Resize the string hash table (grow and shrink). */
void lj_str_resize(lua_State *L, MSize newmask)
{
	global_State *g = G(L);
	GCRef *newhash;
	MSize i;
	if (g->gc.state == GCSsweepstring || newmask >= LJ_MAX_STRTAB - 1)
		return;  /* No resizing during GC traversal or if already too big. */
	newhash = lj_mem_newvec(L, newmask + 1, GCRef);
	memset(newhash, 0, (newmask + 1) * sizeof(GCRef));
	for (i = g->strmask; i != ~(MSize)0; i--) {  /* Rehash old table. */
		GCobj *p = gcref(g->strhash[i]);
		while (p) {  /* Follow each hash chain and reinsert all strings. */
			MSize h = gco2str(p)->hash & newmask;
			GCobj *next = gcnext(p);
			/* NOBARRIER: The string table is a GC root. */
			setgcrefr(p->gch.nextgc, newhash[h]);
			setgcref(newhash[h], p);
			p = next;
		}
	}
	lj_mem_freevec(g, g->strhash, g->strmask + 1, GCRef);
	g->strmask = newmask;
	g->strhash = newhash;
}

void lj_str_resize_mod(lua_State* L, MSize newmask)
{
	int newhash; // eax
	unsigned int size; // edx
	int newhash_v5; // ebp
	int v6; // edi
	int v7; // edx
	int v8; // edi
	_DWORD *v9; // eax
	_DWORD *v10; // ecx
	_DWORD *v11; // edx
	bool v12; // zf
	signed int v13; // eax
	int v14; // eax
	unsigned int v15; // ecx
	int v17; // [esp-18h] [ebp-18h]
	int *g; // [esp-10h] [ebp-10h]

	g = *(int **)(L + 8);                         // global_State *g = G(L);
	if (newmask > 0x3FFFFFE || *(_BYTE *)(*(_DWORD *)(L + 8) + 29) == 3)
		return;
	newhash = (int)lj_mem_realloc(L, 0, 0, 4 * newmask + 4);// GCRef *newhash;
	size = 4 * newmask + 4;                       // (newmask+1)*sizeof(GCRef)
	newhash_v5 = newhash;
	v6 = newhash;
	if (size >= 8)
	{
		if (newhash & 1)
		{
			*(_BYTE *)newhash = 0;                    // *newhash = 0
			v6 = newhash + 1;                         // v6 += 1
			size = 4 * newmask + 3;                   // size -= 1
		}
		if (v6 & 2)
		{
			*(_WORD *)v6 = 0;
			size -= 2;
			v6 += 2;
		}
		if (v6 & 4)
		{
			*(_DWORD *)v6 = 0;
			size -= 4;
			v6 += 4;
		}
		v15 = size;
		LOBYTE(size) = size & 3;
		v15 >>= 2;
		memset((void *)v6, 0, 4 * v15);
		v6 += 4 * v15;
		if (!(size & 4))
		{
		LABEL_5:
			if (!(size & 2))
				goto LABEL_6;
		LABEL_17:
			*(_WORD *)v6 = 0;
			v6 += 2;
			if (!(size & 1))
				goto LABEL_7;
			goto LABEL_16;
		}
	}
	else if (!(size & 4))
	{
		goto LABEL_5;
	}
	*(_DWORD *)v6 = 0;
	v6 += 4;
	if (size & 2)
		goto LABEL_17;
LABEL_6:
	if (size & 1)
		LABEL_16 :
		*(_BYTE *)v6 = 0;
LABEL_7:
	v17 = g[1];                                   // g->strmask
	v7 = *g;                                      // (int)g->strhash
	if (v17 == -1)
	{
		v13 = -1;
	}
	else
	{
		v8 = 4 * v17;                               // GCRef* v8 = (GCRef *)v17
		do
		{
			v9 = *(_DWORD **)(v7 + v8);
			if (v9)
			{
				do
				{
					v10 = (_DWORD *)*v9;
					v11 = (_DWORD *)(newhash_v5 + 4 * (newmask & v9[2]));
					v12 = *v9 == 0;
					*v9 = *v11;
					*v11 = (_DWORD)v9;
					v9 = v10;
				} while (!v12);
				v7 = *g;
			}
			--v17;
			v8 -= 4;
		} while (v17 != -1);
		v13 = g[1];
	}
	v14 = 4 * v13 + 4;                            // 以下三行合并为：lj_mem_freevec(g, g->strhash, g->strmask+1, GCRef);   -----
	g[5] -= v14;
	((int(*)(int, int, int, _DWORD))g[3])(g[4], v7, v14, 0);// g->allocf(g->allocd,v7,v14,0)  -----
	g[1] = newmask;                               // g->strmask = newmask;
	*g = newhash_v5;                              // g->strhash = newhash;
	return;
}


/* Intern a string and return string object. */
GCstr *lj_str_new(lua_State *L, const char *str, size_t lenx)
{
	global_State *g;
	GCstr *s;
	GCobj *o;
	MSize len = (MSize)lenx;
	MSize a, b, h = len;
	if (lenx >= LJ_MAX_STR)
		lj_err_msg(L, LJ_ERR_STROV);
	g = G(L);
	/* Compute string hash. Constants taken from lookup3 hash by Bob Jenkins. */
	if (len >= 4) {  /* Caveat: unaligned access! */
		a = lj_getu32(str);
		h ^= lj_getu32(str + len - 4);
		b = lj_getu32(str + (len >> 1) - 2);
		h ^= b; h -= lj_rol(b, 14);
		b += lj_getu32(str + (len >> 2) - 1);
	}
	else if (len > 0) {
		a = *(const uint8_t *)str;
		h ^= *(const uint8_t *)(str + len - 1);
		b = *(const uint8_t *)(str + (len >> 1));
		h ^= b; h -= lj_rol(b, 14);
	}
	else {
		return &g->strempty;
	}
	a ^= h; a -= lj_rol(h, 11);
	b ^= a; b -= lj_rol(a, 25);
	h ^= b; h -= lj_rol(b, 16);
	/* Check if the string has already been interned. */
	o = gcref(g->strhash[h & g->strmask]);
	if (LJ_LIKELY((((uintptr_t)str + len - 1) & (LJ_PAGESIZE - 1)) <= LJ_PAGESIZE - 4)) 
	{
		while (o != NULL) 
		{
			GCstr *sx = gco2str(o);
			if (sx->len == len && str_fastcmp(str, strdata(sx), len) == 0) {
				/* Resurrect if dead. Can only happen with fixstring() (keywords). */
				if (isdead(g, o)) flipwhite(o);
				return sx;  /* Return existing string. */
			}
			o = gcnext(o);
		}
	}
	else 
	{  /* Slow path: end of string is too close to a page boundary. */
		while (o != NULL) 
		{
			GCstr *sx = gco2str(o);
			if (sx->len == len && memcmp(str, strdata(sx), len) == 0) {
				/* Resurrect if dead. Can only happen with fixstring() (keywords). */
				if (isdead(g, o)) flipwhite(o);
				return sx;  /* Return existing string. */
			}
			o = gcnext(o);
		}
	}
	/* Nope, create a new string. */
	s = lj_mem_newt(L, sizeof(GCstr) + len + 1, GCstr);
	newwhite(g, s);
	s->gct = ~LJ_TSTR;
	s->len = len;
	s->hash = h;
	s->reserved = 0;
	memcpy(strdatawr(s), str, len);
	strdatawr(s)[len] = '\0';  /* Zero-terminate string. */
	/* Add it to string hash table. */
	h &= g->strmask;
	s->nextgc = g->strhash[h];
	/* NOBARRIER: The string table is a GC root. */
	setgcref(g->strhash[h], obj2gco(s));
	if (g->strnum++ > g->strmask)  /* Allow a 100% load factor. */
		lj_str_resize(L, (g->strmask << 1) + 1);  /* Grow string table. */
	return s;  /* Return newly interned string. */
}


int hash_str(unsigned __int8 *str, unsigned int len)
{
	unsigned __int8 *v2; // edi
	int v3; // esi
	int v4; // ecx
	unsigned int v5; // eax
	int v6; // ecx
	int v7; // ecx
	int result; // eax

	v2 = str;
	if (len > 3)
	{
		v3 = *(_DWORD *)str;
		v4 = *(_DWORD *)&str[(len >> 1) - 2];
		v5 = (len ^ v4 ^ *(_DWORD *)&str[len - 4]) - __ROL4__(v4, 14);
		v6 = *(_DWORD *)&v2[(len >> 2) - 1] + v4;
	LABEL_3:
		v7 = (((v5 ^ v3) - __ROL4__(v5, 11)) ^ v6) - __ROR4__((v5 ^ v3) - __ROL4__(v5, 11), 7);
		return (v7 ^ v5) - __ROL4__(v7, 16);
	}
	result = 0;
	if (len)
	{
		v3 = *v2;
		v6 = v2[len >> 1];
		v5 = (len ^ (unsigned __int8)(v2[len - 1] ^ v2[len >> 1])) - __ROL4__(v6, 14);
		goto LABEL_3;
	}
	return result;
}

int lj_str_new_mod(int L, const char *str, size_t lenx)
{
	char *v3; // esi
	int s; // eax
	int o; // edi
	int v6; // ecx
	size_t v7; // eax
	int v8; // edx
	signed int v9; // eax
	char v10; // cl
	int s_buffer; // edi
	unsigned int v12; // ecx
	unsigned int v13; // ebp
	_DWORD *v14; // edx
	unsigned int v15; // edx
	int v16; // ST14_4
	int v17; // edx
	char v18; // dl
	__int16 v19; // dx
	int h; // [esp-28h] [ebp-28h]
	int g; // [esp-24h] [ebp-24h]

	v3 = (char*)(int)str;
	if (lenx > 0x7FFFFEFF)
		lj_err_msg((lua_State*)L, 56);
	g = *(_DWORD *)(L + 8);                       // global_State *g = G(L)
	s = g + 96;                                   // result = &g->strempty     //  GCstr* result
	if (lenx)
	{
		h = hash_str((unsigned __int8 *)str, lenx);
		o = *(_DWORD *)(*(_DWORD *)g + 4 * (h & *(_DWORD *)(g + 4)));// o = gcref(g->strhash[h & g->strmask]);
		if (((unsigned int)&str[lenx - 1] & 0xFFF) > 0xFFC)// if ((((uintptr_t)str+len-1) & (LJ_PAGESIZE-1)) <= LJ_PAGESIZE-4)
		{
			if (!o)
			{
			LABEL_19:
				s = (int)lj_mem_realloc((lua_State*)L, 0, 0, lenx + 17);// 内存申请？    // GCstr *s = lj_mem_newt(L, sizeof(GCstr)+len+1, GCstr);
				v10 = *(_BYTE *)(g + 28);               // g->gc.currentwhite   // 和下边 v10 & 3 合并为 newwhite(g, s)
				s_buffer = s + 16;                      // char* s_buffer = (char*)(s + 1)
				*(_BYTE *)(s + 5) = 4;                  // s->gct = ~LJ_TSTR;
				*(_BYTE *)(s + 4) = v10 & 3;            // newwhite(g, s);
				*(_DWORD *)(s + 12) = lenx;             // s->len = len;
				*(_DWORD *)(s + 8) = h;                 // s->hash = h;
				v12 = lenx;
				*(_BYTE *)(s + 6) = 0;                  // s->reserved = 0;
				if (lenx >= 8)
				{
					if (s_buffer & 1)
					{
						v18 = *v3;
						s_buffer = s + 17;                  // s_buffer += 1
						++v3;                               // v3 += 1
						v12 = lenx - 1;                     // v12 -= 1
						*(_BYTE *)(s + 16) = v18;
					}
					if (s_buffer & 2)
					{
						v19 = *(_WORD *)v3;
						s_buffer += 2;
						v3 += 2;
						v12 -= 2;
						*(_WORD *)(s_buffer - 2) = v19;
					}
					if (s_buffer & 4)
					{
						v17 = *(_DWORD *)v3;
						s_buffer += 4;
						v3 += 4;
						v12 -= 4;
						*(_DWORD *)(s_buffer - 4) = v17;
					}
				}
				qmemcpy((void *)s_buffer, v3, v12);
				*(_BYTE *)(s + lenx + 16) = 0;          // strdatawr(s)[len] = '\0';
				v13 = *(_DWORD *)(g + 4);               // 合并下边三行为：h &= g->strmask; s->nextgc = g->strhash[h];   ----
				v14 = (_DWORD *)(*(_DWORD *)g + 4 * (v13 & h));// v14 = g->strhash[h]
				*(_DWORD *)s = *v14;                    // s->nextgc = *v14      ----
				*v14 = s;                               // setgcref(g->strhash[h], obj2gco(s));
				v15 = *(_DWORD *)(g + 8);               // 合并下三行为：if (g->strnum++ > g->strmask)    -----
				*(_DWORD *)(g + 8) = v15 + 1;
				if (v13 < v15)                        // -----
				{
					v16 = s;
					lj_str_resize_mod((lua_State*)L, 2 * v13 + 1);
					s = v16;
				}
				return s;
			}
			while (lenx != *(_DWORD *)(o + 12) || *(_DWORD *)(o + 8) != h || memcmp(str, (const void *)(o + 16), lenx))
			{
				o = *(_DWORD *)o;
				if (!o)
				{
					v3 = (char*)(int)str;
					goto LABEL_19;
				}
			}
			s = o;
			if ((unsigned __int8)(*(_BYTE *)(g + 28) ^ 3) & *(_BYTE *)(o + 4) & 3)
				*(_BYTE *)(o + 4) ^= 3u;
		}
		else
		{
			if (!o)
				goto LABEL_19;
			while (lenx != *(_DWORD *)(o + 12) || *(_DWORD *)(o + 8) != h)
			{
			LABEL_6:
				o = *(_DWORD *)o;
				if (!o)
					goto LABEL_19;
			}
			v6 = o + 16;
			v7 = 0;
			while (1)
			{
				v8 = *(_DWORD *)&str[v7] ^ *(_DWORD *)(v6 + v7);
				if (*(_DWORD *)&str[v7] != *(_DWORD *)(v6 + v7))
					break;
				v7 += 4;
				if (lenx <= v7)
					goto LABEL_14;
			}
			v9 = v7 - lenx;
			if (v9 < -3 || v8 << (8 * v9 + 32))
				goto LABEL_6;
		LABEL_14:
			if ((unsigned __int8)(*(_BYTE *)(g + 28) ^ 3) & *(_BYTE *)(o + 4) & 3)
				*(_BYTE *)(o + 4) ^= 3u;
			s = o;
		}
	}
	return s;
}

void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s)
{
	g->strnum--;
	lj_mem_free(g, s, sizestring(s));
}

