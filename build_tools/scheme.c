/* This version of TinyScheme was branched at 1.42 from
 * https://sourceforge.net/p/tinyscheme
 * There was reference to authors of MiniScheme from which TinyScheme
 * was created long ago, but nowadays it is somewhat misleading.
 * Many people put efforts to this thing, so please look at the
 * comprehensive list of contributors here:
 * http://tinyscheme.sourceforge.net/credits.html
 */

#define _SCHEME_SOURCE
#include "scm_priv.h"
#ifndef WIN32
#include <unistd.h>
#endif
#ifdef WIN32
#define snprintf _snprintf
#endif
#if USE_DL
#include "dynload.h"
#endif
#if USE_MATH
#include <math.h>
#endif

#include <limits.h>
#include <float.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#endif

/* Used for documentation purposes, to signal functions in 'interface' */
#define INTERFACE

#define TOK_EOF     (-1)
#define TOK_LPAREN  0
#define TOK_RPAREN  1
#define TOK_DOT     2
#define TOK_ATOM    3
#define TOK_QUOTE   4
#define TOK_COMMENT 5
#define TOK_DQUOTE  6
#define TOK_BQUOTE  7
#define TOK_COMMA   8
#define TOK_ATMARK  9
#define TOK_SHARP   10
#define TOK_SHARP_CONST 11
#define TOK_VEC     12

#define BACKQUOTE '`'
#define DELIMITERS  "()\";\f\t\v\n\r "

/*
 *  Basic memory allocation units
 */

#define OBJ_LIST_SIZE 461

#define VERSION "TinyScheme R7 (v21.03)"

#include <string.h>
#include <stdlib.h>

#if CASE_SENSITIVE
#define str_eq_to_lower(X,Y) (!strcmp((X),(Y)))
#define str_eq(X,Y) (!strcmp((X),(Y)))
#define str_to_maybe_lower(X) (X)
#else

static int str_eq_to_lower(const char *s1, const char *s2) {
  while (*s1) {
    if (tolower(*s1) != (*s2)) {
      return 0;
    }
    s1++, s2++;
  }
  return (*s2 == 0);
}

static int str_eq(const char *s1, const char *s2) {
  while (*s1) {
    if (tolower(*s1) != tolower(*s2)) {
      return 0;
    }
    s1++, s2++;
  }
  return (*s2 == 0);
}

static const char *str_to_maybe_lower(char *s) {
  const char *p = s;
  while (*s) {
    *s = tolower(*s);
    s++;
  }
  return p;
}

#endif // CASE_SENSITIVE

#ifndef prompt
#define prompt "ts> "
#endif

#ifndef InitFile
#define InitFile "build_tools/init.scm"
#endif

#ifndef FIRST_CELLSEGS
#define FIRST_CELLSEGS 3
#endif

enum scheme_types {
  T_STRING = 1,
  T_NUMBER = 2,
  T_SYMBOL = 3,
  T_PROC = 4,
  T_PAIR = 5,
  T_CLOSURE = 6,
  T_CONTINUATION = 7,
  T_FOREIGN = 8,
  T_CHARACTER = 9,
  T_PORT = 10,
  T_VECTOR = 11,
  T_MACRO = 12,
  T_PROMISE = 13,
  T_ENVIRONMENT = 14,
  T_BYTEVECTOR = 15
};

/* ADJ is enough slack to align cells in a TYPE_BITS-bit boundary */
#define ADJ 32
#define TYPE_BITS 5
#define T_MASKTYPE      31      /* 0000000000011111 */
#define T_SYNTAX      4096      /* 0001000000000000 */
#define T_IMMUTABLE   8192      /* 0010000000000000 */
#define T_ATOM       16384    /* 0100000000000000 */    /* only for gc */
#define CLRATOM      49151    /* 1011111111111111 */    /* only for gc */
#define MARK         32768      /* 1000000000000000 */
#define UNMARK       32767      /* 0111111111111111 */

// these may be overriden by env properties
static int cell_segsize = CELL_SEGSIZE;
static int cell_nsegment = CELL_NSEGMENT;
static long evalcnt = 0;
#ifdef EVAL_LIMIT
static long eval_limit;
#endif

static num num_add(num a, num b);
static num num_mul(num a, num b);
static num num_div(num a, num b);
static num num_sub(num a, num b);
static num num_rem(num a, num b);
static num num_mod(num a, num b);
static int num_eq(num a, num b);
static int num_gt(num a, num b);
static int num_ge(num a, num b);
static int num_lt(num a, num b);
static int num_le(num a, num b);

#if USE_MATH
static double round_per_R5RS(double x);
#endif
static INLINE int num_is_integer(pointer p) {
  return ((p)->_object._number.is_fixnum);
}

static num num_zero;
static num num_one;

/* macros for cell operations */
#define typeflag(p)      ((p)->_flag)
#define type(p)          (typeflag(p)&T_MASKTYPE)

INTERFACE INLINE int is_string(pointer p) {
  return (type(p) == T_STRING);
}

#define strvalue(p)      ((p)->_object._string._svalue)
#define strlength(p)        ((p)->_object._string._length)

INTERFACE static int is_list(scheme * sc, pointer p);
INTERFACE INLINE int is_vector(pointer p) {
  return (type(p) == T_VECTOR);
}

INTERFACE static void fill_vector(pointer vec, pointer obj);
INTERFACE static pointer vector_elem(pointer vec, int ielem);
INTERFACE static pointer set_vector_elem(pointer vec, int ielem, pointer a);

INTERFACE INLINE int is_bvector(pointer p) {
  return (type(p) == T_BYTEVECTOR);
}

INTERFACE INLINE int is_number(pointer p) {
  return (type(p) == T_NUMBER);
}
INTERFACE INLINE int is_integer(pointer p) {
  if (!is_number(p))
    return 0;
  if (num_is_integer(p) || (double) ivalue(p) == rvalue(p))
    return 1;
  return 0;
}

INTERFACE INLINE int is_real(pointer p) {
  return is_number(p) && (!(p)->_object._number.is_fixnum);
}

INTERFACE INLINE int is_character(pointer p) {
  return (type(p) == T_CHARACTER);
}
INTERFACE INLINE char *string_value(pointer p) {
  return strvalue(p);
}
INLINE num nvalue(pointer p) {
  return ((p)->_object._number);
}
INTERFACE long ivalue(pointer p) {
  return (num_is_integer(p) ? (p)->_object._number.value.ivalue : (long) (p)->
      _object._number.value.rvalue);
}
INTERFACE double rvalue(pointer p) {
  return (!num_is_integer(p) ? (p)->_object._number.
      value.rvalue : (double) (p)->_object._number.value.ivalue);
}

#define ivalue_unchecked(p)       ((p)->_object._number.value.ivalue)
#define rvalue_unchecked(p)       ((p)->_object._number.value.rvalue)
#define set_num_integer(p)   (p)->_object._number.is_fixnum=1;
#define set_num_real(p)      (p)->_object._number.is_fixnum=0;
INTERFACE long charvalue(pointer p) {
  return ivalue_unchecked(p);
}

INTERFACE INLINE int is_port(pointer p) {
  return (type(p) == T_PORT);
}
INTERFACE INLINE int is_inport(pointer p) {
  return is_port(p) && p->_object._port->kind & port_input;
}
INTERFACE INLINE int is_outport(pointer p) {
  return is_port(p) && p->_object._port->kind & port_output;
}

INTERFACE INLINE int is_pair(pointer p) {
  return (type(p) == T_PAIR);
}

#define car(p)           ((p)->_object._cons._car)
#define cdr(p)           ((p)->_object._cons._cdr)
INTERFACE pointer pair_car(pointer p) {
  return car(p);
}
INTERFACE pointer pair_cdr(pointer p) {
  return cdr(p);
}
INTERFACE pointer set_car(pointer p, pointer q) {
  return car(p) = q;
}
INTERFACE pointer set_cdr(pointer p, pointer q) {
  return cdr(p) = q;
}

INTERFACE INLINE int is_symbol(pointer p) {
  return (type(p) == T_SYMBOL);
}
INTERFACE INLINE char *symname(pointer p) {
  return strvalue(car(p));
}

#if USE_PLIST
SCHEME_EXPORT INLINE int hasprop(pointer p) {
  return (typeflag(p) & T_SYMBOL);
}

#define symprop(p)       cdr(p)
#endif

INTERFACE INLINE int is_syntax(pointer p) {
  return (typeflag(p) & T_SYNTAX);
}
INTERFACE INLINE int is_proc(pointer p) {
  return (type(p) == T_PROC);
}
INTERFACE INLINE int is_foreign(pointer p) {
  return (type(p) == T_FOREIGN);
}
INTERFACE INLINE char *syntaxname(pointer p) {
  return strvalue(car(p));
}

#define procnum(p)       ivalue(p)
static const char *procname(pointer x);

INTERFACE INLINE int is_closure(pointer p) {
  return (type(p) == T_CLOSURE);
}
INTERFACE INLINE int is_macro(pointer p) {
  return (type(p) == T_MACRO);
}
INTERFACE INLINE pointer closure_code(pointer p) {
  return car(p);
}
INTERFACE INLINE pointer closure_env(pointer p) {
  return cdr(p);
}

INTERFACE INLINE int is_continuation(pointer p) {
  return (type(p) == T_CONTINUATION);
}

#define cont_dump(p)     cdr(p)

/* To do: promise should be forced ONCE only */
INTERFACE INLINE int is_promise(pointer p) {
  return (type(p) == T_PROMISE);
}

INTERFACE INLINE int is_environment(pointer p) {
  return (type(p) == T_ENVIRONMENT);
}

#define setenvironment(p)    typeflag(p) = T_ENVIRONMENT

#define is_atom(p)       (typeflag(p)&T_ATOM)
#define setatom(p)       typeflag(p) |= T_ATOM
#define clratom(p)       typeflag(p) &= CLRATOM

#define is_mark(p)       (typeflag(p)&MARK)
#define setmark(p)       typeflag(p) |= MARK
#define clrmark(p)       typeflag(p) &= UNMARK

INTERFACE INLINE int is_immutable(pointer p) {
  return (typeflag(p) & T_IMMUTABLE);
}

/*#define setimmutable(p)  typeflag(p) |= T_IMMUTABLE*/
INTERFACE INLINE void setimmutable(pointer p) {
  typeflag(p) |= T_IMMUTABLE;
}

#define caar(p)          car(car(p))
#define cadr(p)          car(cdr(p))
#define cdar(p)          cdr(car(p))
#define cddr(p)          cdr(cdr(p))
#define cadar(p)         car(cdr(car(p)))
#define caddr(p)         car(cdr(cdr(p)))
#define cdaar(p)         cdr(car(car(p)))
#define cadaar(p)        car(cdr(car(car(p))))
#define cadddr(p)        car(cdr(cdr(cdr(p))))
#define cddddr(p)        cdr(cdr(cdr(cdr(p))))

#define IS_ASCII(C) (((C) & ~0x7F) == 0)

#if USE_CHAR_CLASSIFIERS
static INLINE int Cisalpha(int c) {
  return isalpha(c) && IS_ASCII(c);
}
static INLINE int Cisdigit(int c) {
  return isdigit(c) && IS_ASCII(c);
}
static INLINE int Cisspace(int c) {
  return isspace(c) && IS_ASCII(c);
}
static INLINE int Cisupper(int c) {
  return isupper(c) && IS_ASCII(c);
}
static INLINE int Cislower(int c) {
  return islower(c) && IS_ASCII(c);
}
#endif

static int file_push(scheme * sc, const char *fname);
static void file_pop(scheme * sc);
static int file_interactive(scheme * sc);
static INLINE int is_one_of(char *s, int c);
static int alloc_cellseg(scheme * sc, int n);
static INLINE pointer get_cell(scheme * sc, pointer a, pointer b);
static pointer _get_cell(scheme * sc, pointer a, pointer b);
static pointer get_consecutive_cells(scheme * sc, int n);
static pointer find_consecutive_cells(scheme * sc, int n);
static void finalize_cell(scheme * sc, pointer a);
static int count_consecutive_cells(pointer x, int needed);
static pointer find_slot_in_env(scheme * sc, pointer env, pointer sym,
    int all);
static pointer mk_number(scheme * sc, num n);
static pointer mk_vector(scheme * sc, int len);
static pointer mk_atom(scheme * sc, char *q);
static pointer mk_sharp_const(scheme * sc, char *name);
static pointer mk_port(scheme * sc, port * p);
static pointer port_from_filename(scheme * sc, const char *fn, int prop);
static pointer port_from_file(scheme * sc, FILE *, int prop);
static pointer port_from_string(scheme * sc, char *start, char *past_the_end,
    int prop);
static port *port_rep_from_filename(scheme * sc, const char *fn, int prop);
static port *port_rep_from_file(scheme * sc, FILE *, int prop);
static port *port_rep_from_string(scheme * sc, char *start,
    char *past_the_end, int prop);
static void port_close(scheme * sc, pointer p, int flag);
static void mark(pointer a);
static void gc(scheme * sc, pointer a, pointer b);
static int basic_inchar(port * pt);
static int inchar(scheme * sc);
static void backchar(scheme * sc, int c);
static char *readstr_upto(scheme * sc, char *delim);
static pointer readstrexp(scheme * sc);
static INLINE int skipspace(scheme * sc);
static int token(scheme * sc);
static void atom2str(scheme * sc, pointer l, int f, char **pp, int *plen);
static void printatom(scheme * sc, pointer l, int f);
static pointer mk_proc(scheme * sc, enum scheme_opcodes op);
static pointer mk_closure(scheme * sc, pointer c, pointer e);
static pointer mk_continuation(scheme * sc, pointer d);
static pointer reverse(scheme * sc, pointer a);
static pointer reverse_in_place(scheme * sc, pointer term, pointer list);
static pointer revappend(scheme * sc, pointer a, pointer b);
static void dump_stack_mark(scheme *);
static pointer opexe_0(scheme * sc, enum scheme_opcodes op);
static pointer opexe_1(scheme * sc, enum scheme_opcodes op);
static pointer opexe_2(scheme * sc, enum scheme_opcodes op);
static pointer opexe_3(scheme * sc, enum scheme_opcodes op);
static pointer opexe_4(scheme * sc, enum scheme_opcodes op);
static pointer opexe_5(scheme * sc, enum scheme_opcodes op);
static pointer opexe_6(scheme * sc, enum scheme_opcodes op);
static void Eval_Cycle(scheme * sc, enum scheme_opcodes op);
static void assign_syntax(scheme * sc, char *name);
static int syntaxnum(pointer p);
static void assign_proc(scheme * sc, enum scheme_opcodes, char *name);

#define num_ivalue(n)       (n.is_fixnum?(n).value.ivalue:(long)(n).value.rvalue)
#define num_rvalue(n)       (!n.is_fixnum?(n).value.rvalue:(double)(n).value.ivalue)

static num num_add(num a, num b) {
  num ret;
  ret.is_fixnum = a.is_fixnum && b.is_fixnum;
  if (ret.is_fixnum) {
    ret.value.ivalue = a.value.ivalue + b.value.ivalue;
  } else {
    ret.value.rvalue = num_rvalue(a) + num_rvalue(b);
  }
  return ret;
}

static num num_mul(num a, num b) {
  num ret;
  ret.is_fixnum = a.is_fixnum && b.is_fixnum;
  if (ret.is_fixnum) {
    ret.value.ivalue = a.value.ivalue * b.value.ivalue;
  } else {
    ret.value.rvalue = num_rvalue(a) * num_rvalue(b);
  }
  return ret;
}

static num num_div(num a, num b) {
  num ret;
  ret.is_fixnum =
      a.is_fixnum && b.is_fixnum
      && b.value.ivalue != 0 && a.value.ivalue % b.value.ivalue == 0;
  if (ret.is_fixnum) {
    ret.value.ivalue = a.value.ivalue / b.value.ivalue;
  } else {
    ret.value.rvalue = num_rvalue(a) / num_rvalue(b);
  }
  return ret;
}

static num num_sub(num a, num b) {
  num ret;
  ret.is_fixnum = a.is_fixnum && b.is_fixnum;
  if (ret.is_fixnum) {
    ret.value.ivalue = a.value.ivalue - b.value.ivalue;
  } else {
    ret.value.rvalue = num_rvalue(a) - num_rvalue(b);
  }
  return ret;
}

static num num_rem(num a, num b) {
  num ret;
  long e1, e2, res;
  ret.is_fixnum = a.is_fixnum && b.is_fixnum;
  e1 = num_ivalue(a);
  e2 = num_ivalue(b);
  res = e1 % e2;
  /* remainder should have same sign as first operand */
  if (res > 0) {
    if (e1 < 0) {
      res -= labs(e2);
    }
  } else if (res < 0) {
    if (e1 > 0) {
      res += labs(e2);
    }
  }
  if (ret.is_fixnum) {
    ret.value.ivalue = res;
  } else {
    ret.value.rvalue = res;
  }
  return ret;
}

static num num_mod(num a, num b) {
  num ret;
  long e1, e2, res;
  ret.is_fixnum = a.is_fixnum && b.is_fixnum;
  e1 = num_ivalue(a);
  e2 = num_ivalue(b);
  res = e1 % e2;
  /* modulo should have same sign as second operand */
  if ((res < 0) != (e2 < 0) && res) {
    res += e2;
  }
  if (ret.is_fixnum) {
    ret.value.ivalue = res;
  } else {
    ret.value.rvalue = res;
  }
  return ret;
}

static int num_eq(num a, num b) {
  int ret;
  int is_fixnum = a.is_fixnum && b.is_fixnum;
  if (is_fixnum) {
    ret = a.value.ivalue == b.value.ivalue;
  } else {
    ret = num_rvalue(a) == num_rvalue(b);
  }
  return ret;
}


static int num_gt(num a, num b) {
  int ret;
  int is_fixnum = a.is_fixnum && b.is_fixnum;
  if (is_fixnum) {
    ret = a.value.ivalue > b.value.ivalue;
  } else {
    ret = num_rvalue(a) > num_rvalue(b);
  }
  return ret;
}

static int num_ge(num a, num b) {
  return !num_lt(a, b);
}

static int num_lt(num a, num b) {
  int ret;
  int is_fixnum = a.is_fixnum && b.is_fixnum;
  if (is_fixnum) {
    ret = a.value.ivalue < b.value.ivalue;
  } else {
    ret = num_rvalue(a) < num_rvalue(b);
  }
  return ret;
}

static int num_le(num a, num b) {
  return !num_gt(a, b);
}

#if USE_MATH
/* Round to nearest. Round to even if midway */
static double round_per_R5RS(double x) {
  double fl = floor(x);
  double ce = ceil(x);
  double dfl = x - fl;
  double dce = ce - x;
  if (dfl > dce) {
    return ce;
  } else if (dfl < dce) {
    return fl;
  } else {
    if (fmod(fl, 2.0) == 0.0) { /* I imagine this holds */
      return fl;
    } else {
      return ce;
    }
  }
}
#endif

/* allocate new cell segment */
static int alloc_cellseg(scheme * sc, int n) {
  pointer newp;
  pointer last;
  pointer p;
  char *cp;
  long i;
  int k;
  int adj = ADJ;

  if (adj < sizeof(struct cell)) {
    adj = sizeof(struct cell);
  }
  for (k = 0; k < n; k++) {
    if (sc->last_cell_seg >= cell_nsegment - 1)
      return k;
    cp = (char *) sc->malloc(cell_segsize * sizeof(struct cell) + adj);
    if (cp == 0)
      return k;
    i = ++sc->last_cell_seg;
    sc->alloc_seg[i] = cp;
    /* adjust in TYPE_BITS-bit boundary */
    if (((unsigned long) cp) % adj != 0) {
      cp = (char *) (adj * ((unsigned long) cp / adj + 1));
    }
    /* insert new segment in address order */
    newp = (pointer) cp;
    sc->cell_seg[i] = newp;
    while (i > 0 && sc->cell_seg[i - 1] > sc->cell_seg[i]) {
      p = sc->cell_seg[i];
      sc->cell_seg[i] = sc->cell_seg[i - 1];
      sc->cell_seg[--i] = p;
    }
    sc->fcells += cell_segsize;
    last = newp + cell_segsize - 1;
    for (p = newp; p <= last; p++) {
      typeflag(p) = 0;
      cdr(p) = p + 1;
      car(p) = sc->NIL;
    }
    /* insert new cells in address order on free list */
    if (sc->free_cell == sc->NIL || p < sc->free_cell) {
      cdr(last) = sc->free_cell;
      sc->free_cell = newp;
    } else {
      p = sc->free_cell;
      while (cdr(p) != sc->NIL && newp > cdr(p))
        p = cdr(p);
      cdr(last) = cdr(p);
      cdr(p) = newp;
    }
  }
  return n;
}

static INLINE pointer get_cell_x(scheme * sc, pointer a, pointer b) {
  if (sc->free_cell != sc->NIL) {
    pointer x = sc->free_cell;
    sc->free_cell = cdr(x);
    --sc->fcells;
    return (x);
  }
  return _get_cell(sc, a, b);
}


/* get new cell.  parameter a, b is marked by gc. */
static pointer _get_cell(scheme * sc, pointer a, pointer b) {
  pointer x;

  if (sc->no_memory) {
    return sc->sink;
  }

  if (sc->free_cell == sc->NIL) {
    const int min_to_be_recovered = sc->last_cell_seg * 8;
    gc(sc, a, b);
    if (sc->fcells < min_to_be_recovered || sc->free_cell == sc->NIL) {
      /* if only a few recovered, get more to avoid fruitless gc's */
      if (!alloc_cellseg(sc, 1) && sc->free_cell == sc->NIL) {
        sc->no_memory = 1;
        return sc->sink;
      }
    }
  }
  x = sc->free_cell;
  sc->free_cell = cdr(x);
  --sc->fcells;
  return (x);
}

#if USE_INTERFACE
/* make sure that there is a given number of cells free */
static pointer reserve_cells(scheme * sc, int n) {
  if (sc->no_memory) {
    return sc->NIL;
  }

  /* Are there enough cells available? */
  if (sc->fcells < n) {
    /* If not, try gc'ing some */
    gc(sc, sc->NIL, sc->NIL);
    if (sc->fcells < n) {
      /* If there still aren't, try getting more heap */
      if (!alloc_cellseg(sc, 1)) {
        sc->no_memory = 1;
        return sc->NIL;
      }
    }
    if (sc->fcells < n) {
      /* If all fail, report failure */
      sc->no_memory = 1;
      return sc->NIL;
    }
  }
  return (sc->T);
}
#endif

static pointer get_consecutive_cells(scheme * sc, int n) {
  pointer x;

  if (sc->no_memory) {
    return sc->sink;
  }

  /* Are there any cells available? */
  x = find_consecutive_cells(sc, n);
  if (x != sc->NIL) {
    return x;
  }

  /* If not, try gc'ing some */
  gc(sc, sc->NIL, sc->NIL);
  x = find_consecutive_cells(sc, n);
  if (x != sc->NIL) {
    return x;
  }

  /* If there still aren't, try getting more heap */
  if (!alloc_cellseg(sc, 1)) {
    sc->no_memory = 1;
    return sc->sink;
  }

  x = find_consecutive_cells(sc, n);
  if (x != sc->NIL) {
    return x;
  }

  /* If all fail, report failure */
  sc->no_memory = 1;
  return sc->sink;
}

static int count_consecutive_cells(pointer x, int needed) {
  int n = 1;
  while (cdr(x) == x + 1) {
    x = cdr(x);
    n++;
    if (n > needed)
      return n;
  }
  return n;
}

static pointer find_consecutive_cells(scheme * sc, int n) {
  pointer *pp;
  int cnt;

  pp = &sc->free_cell;
  while (*pp != sc->NIL) {
    cnt = count_consecutive_cells(*pp, n);
    if (cnt >= n) {
      pointer x = *pp;
      *pp = cdr(*pp + n - 1);
      sc->fcells -= n;
      return x;
    }
    pp = &cdr(*pp + cnt - 1);
  }
  return sc->NIL;
}

/* To retain recent allocs before interpreter knows about them -
   Tehom */

static void push_recent_alloc(scheme * sc, pointer recent, pointer extra) {
  pointer holder = get_cell_x(sc, recent, extra);
  typeflag(holder) = T_PAIR | T_IMMUTABLE;
  car(holder) = recent;
  cdr(holder) = car(sc->sink);
  car(sc->sink) = holder;
}


static pointer get_cell(scheme * sc, pointer a, pointer b) {
  pointer cell = get_cell_x(sc, a, b);
  /* For right now, include "a" and "b" in "cell" so that gc doesn't
     think they are garbage. */
  /* Tentatively record it as a pair so gc understands it. */
  typeflag(cell) = T_PAIR;
  car(cell) = a;
  cdr(cell) = b;
  push_recent_alloc(sc, cell, sc->NIL);
  return cell;
}

static pointer get_vector_object(scheme * sc, int len, pointer init) {
  pointer cells = get_consecutive_cells(sc, len / 2 + len % 2 + 1);
  if (sc->no_memory) {
    return sc->sink;
  }
  /* Record it as a vector so that gc understands it. */
  typeflag(cells) = (T_VECTOR | T_ATOM);
  ivalue_unchecked(cells) = len;
  set_num_integer(cells);
  fill_vector(cells, init);
  push_recent_alloc(sc, cells, sc->NIL);
  return cells;
}

static INLINE void ok_to_freely_gc(scheme * sc) {
  car(sc->sink) = sc->NIL;
}


#if defined TSGRIND
static void check_cell_alloced(pointer p, int expect_alloced) {
  /* Can't use putstr(sc,str) because callers have no access to
     sc.  */
  if (typeflag(p) & !expect_alloced) {
    fprintf(stderr, "Cell is already allocated!\n");
  }
  if (!(typeflag(p)) & expect_alloced) {
    fprintf(stderr, "Cell is not allocated!\n");
  }

}
static void check_range_alloced(pointer p, int n, int expect_alloced) {
  int i;
  for (i = 0; i < n; i++) {
    (void) check_cell_alloced(p + i, expect_alloced);
  }
}

#endif

/* Medium level cell allocation */

/* get new cons cell */
pointer _cons(scheme * sc, pointer a, pointer b, int immutable) {
  pointer x = get_cell(sc, a, b);

  typeflag(x) = T_PAIR;
  if (immutable) {
    setimmutable(x);
  }
  car(x) = a;
  cdr(x) = b;
  return (x);
}

/* ========== oblist implementation  ========== */

static int hash_fn(const char *key, int table_size);

static pointer oblist_initial_value(scheme * sc) {
  return mk_vector(sc, OBJ_LIST_SIZE);
}

/* returns the new symbol */
static pointer oblist_add_by_name(scheme * sc, const char *name) {
  pointer x;
  int location;

  x = immutable_cons(sc, mk_string(sc, name), sc->NIL);
  typeflag(x) = T_SYMBOL;
  setimmutable(car(x));

  location = hash_fn(name, ivalue_unchecked(sc->oblist));
  set_vector_elem(sc->oblist, location,
      immutable_cons(sc, x, vector_elem(sc->oblist, location)));
  return x;
}

static INLINE pointer oblist_find_by_name(scheme * sc, const char *name) {
  int location;
  pointer x;
  char *s;

  location = hash_fn(name, ivalue_unchecked(sc->oblist));
  for (x = vector_elem(sc->oblist, location); x != sc->NIL; x = cdr(x)) {
    s = symname(car(x));
    if (str_eq_to_lower(name, s)) {
      return car(x);
    }
  }
  return sc->NIL;
}

static pointer oblist_all_symbols(scheme * sc) {
  int i;
  pointer x;
  pointer ob_list = sc->NIL;

  for (i = 0; i < ivalue_unchecked(sc->oblist); i++) {
    for (x = vector_elem(sc->oblist, i); x != sc->NIL; x = cdr(x)) {
      ob_list = cons(sc, x, ob_list);
    }
  }
  return ob_list;
}

static pointer mk_port(scheme * sc, port * p) {
  pointer x = get_cell(sc, sc->NIL, sc->NIL);

  typeflag(x) = T_PORT | T_ATOM;
  x->_object._port = p;
  return (x);
}

pointer mk_foreign_func(scheme * sc, foreign_func f) {
  pointer x = get_cell(sc, sc->NIL, sc->NIL);

  typeflag(x) = (T_FOREIGN | T_ATOM);
  x->_object._ff = f;
  return (x);
}

INTERFACE pointer mk_character(scheme * sc, int c) {
  pointer x = get_cell(sc, sc->NIL, sc->NIL);

  typeflag(x) = (T_CHARACTER | T_ATOM);
  ivalue_unchecked(x) = c;
  set_num_integer(x);
  return (x);
}

/* get number atom (integer) */
INTERFACE pointer mk_integer(scheme * sc, long num) {
  pointer x = get_cell(sc, sc->NIL, sc->NIL);

  typeflag(x) = (T_NUMBER | T_ATOM);
  ivalue_unchecked(x) = num;
  set_num_integer(x);
  return (x);
}

INTERFACE pointer mk_real(scheme * sc, double n) {
  pointer x = get_cell(sc, sc->NIL, sc->NIL);

  typeflag(x) = (T_NUMBER | T_ATOM);
  rvalue_unchecked(x) = n;
  set_num_real(x);
  return (x);
}

static pointer mk_number(scheme * sc, num n) {
  if (n.is_fixnum) {
    return mk_integer(sc, n.value.ivalue);
  } else {
    return mk_real(sc, n.value.rvalue);
  }
}

static int utf8_decode(const char *p) {
  int bytes;
  int c;
  unsigned char *s = (unsigned char*) p;
  if (IS_ASCII(*s)) {
    return *s;
  }
  bytes = (*s < 0xE0) ? 2 : ((*s < 0xF0) ? 3 : 4);
  c = *s & ((0x100 >> bytes) - 1);
  while (--bytes) {
    c = (c << 6) | (*(++s) & 0x3F);
  }
  return c;
}

#define UTFSTR_LEN_GET(S) ((*((int*)(S)) >> 8) & 0x7FFFFF)
#define UTFSTR_LEN_SET(LEN) ((((LEN) & 0x7FFFFF) << 8) | 0x80000080)

/* allocate name to string area */
static char *store_string(scheme * sc, int len, const char *str) {
  char *q;
  const char *end;
  int char_size = 1;
  int i;

  for (i = 0; i < len; i++) {
    if ((str[i] & 0x80) != 0) {
      char_size = 4;
      break;
    }
  }

  q = (char *) sc->malloc((len + 1) * char_size);
  if (q == 0) {
    sc->no_memory = 1;
    return sc->strbuff;
  }
  if (str != 0) {
    if (char_size == 1) {
      memcpy(q, str, len);
      q[len] = 0;
    } else {
      i = 1;
      end = str + len;
      while (str < end) {
        if (IS_ASCII(*str) || (*str & 0xE0) == 0xC0) {
          ((int*) q)[i++] = utf8_decode(str);
        }
        str++;
      }
      ((int*) q)[0] = UTFSTR_LEN_SET(i - 1);
    }
  }
  return (q);
}

/* get new string */
INTERFACE pointer mk_string(scheme * sc, const char *str) {
  return mk_counted_string(sc, str, strlen(str));
}

INTERFACE pointer mk_counted_string(scheme * sc, const char *str, int len) {
  char *s;
  pointer x = get_cell(sc, sc->NIL, sc->NIL);
  typeflag(x) = (T_STRING | T_ATOM);
  s = store_string(sc, len, str);
  strvalue(x) = s;
  strlength(x) = IS_ASCII(*s) ? len : UTFSTR_LEN_GET(s);
  return x;
}

static void upgrade_string(scheme *sc, pointer p) {
  int len = strlength(p);
  char *s = strvalue(p);
  int *sbig = (int*) sc->malloc((len + 1) * sizeof(int));
  int i;
  sbig[0] = UTFSTR_LEN_SET(len);
  for (i = 0; i < len; i++) {
    sbig[i + 1] = s[i];
  }
  sc->free(s);
  strvalue(p) = (char*) sbig;
}

INTERFACE static pointer mk_vector(scheme * sc, int len) {
  return get_vector_object(sc, len, sc->NIL);
}

INTERFACE static void fill_vector(pointer vec, pointer obj) {
  int i;
  int num = ivalue(vec) / 2 + ivalue(vec) % 2;
  for (i = 0; i < num; i++) {
    typeflag(vec + 1 + i) = T_PAIR;
    setimmutable(vec + 1 + i);
    car(vec + 1 + i) = obj;
    cdr(vec + 1 + i) = obj;
  }
}

INTERFACE static pointer vector_elem(pointer vec, int ielem) {
  int n = ielem / 2;
  if (ielem % 2 == 0) {
    return car(vec + 1 + n);
  } else {
    return cdr(vec + 1 + n);
  }
}

INTERFACE static pointer set_vector_elem(pointer vec, int ielem, pointer a) {
  int n = ielem / 2;
  if (ielem % 2 == 0) {
    return car(vec + 1 + n) = a;
  } else {
    return cdr(vec + 1 + n) = a;
  }
}

INTERFACE static pointer mk_bvector(scheme * sc, int len, int val) {
  char *s;
  pointer x = get_cell(sc, sc->NIL, sc->NIL);
  typeflag(x) = (T_BYTEVECTOR | T_ATOM);
  s = (char*) sc->malloc(len);
  if (val >= 0) {
    memset(s, val, len);
  }
  strvalue(x) = s;
  strlength(x) = len;
  return x;
}

/* get new symbol */
INTERFACE pointer mk_symbol(scheme * sc, const char *name) {
  pointer x;

  /* first check oblist */
  x = oblist_find_by_name(sc, name);
  if (x != sc->NIL) {
    return (x);
  } else {
    x = oblist_add_by_name(sc, name);
    return (x);
  }
}

INTERFACE pointer gensym(scheme * sc) {
  pointer x;
  char name[40];

  for (; sc->gensym_cnt < LONG_MAX; sc->gensym_cnt++) {
    snprintf(name, 40, "gensym-%ld", sc->gensym_cnt);

    /* first check oblist */
    x = oblist_find_by_name(sc, name);

    if (x != sc->NIL) {
      continue;
    } else {
      x = oblist_add_by_name(sc, name);
      return (x);
    }
  }

  return sc->NIL;
}

/* make symbol or number atom from string */
static pointer mk_atom(scheme * sc, char *q) {
  char c, *p;
  int has_dec_point = 0;
  int has_fp_exp = 0;

#if USE_COLON_HOOK
  if ((p = strstr(q, "::")) != 0) {
    *p = 0;
    return cons(sc, sc->COLON_HOOK,
        cons(sc,
            cons(sc,
                sc->QUOTE,
                cons(sc, mk_atom(sc, p + 2), sc->NIL)),
            cons(sc, mk_symbol(sc, str_to_maybe_lower(q)), sc->NIL)));
  }
#endif

  p = q;
  c = *p++;
  if ((c == '+') || (c == '-')) {
    if (str_eq(p, "inf.0")) {
      return mk_real(sc, (c == '+' ? 1 : -1) / 0.0);
    } else if (str_eq(p, "nan.0")) {
      return mk_real(sc, 0 / 0.0);
    }
    c = *p++;
    if (c == '.') {
      has_dec_point = 1;
      c = *p++;
    }
    if (!isdigit(c)) {
      return (mk_symbol(sc, str_to_maybe_lower(q)));
    }
  } else if (c == '.') {
    has_dec_point = 1;
    c = *p++;
    if (!isdigit(c)) {
      return (mk_symbol(sc, str_to_maybe_lower(q)));
    }
  } else if (!isdigit(c)) {
    return (mk_symbol(sc, str_to_maybe_lower(q)));
  }

  for (; (c = *p) != 0; ++p) {
    if (!isdigit(c)) {
      if (c == '.') {
        if (!has_dec_point) {
          has_dec_point = 1;
          continue;
        }
      } else if ((c == 'e') || (c == 'E')) {
        if (!has_fp_exp) {
          has_fp_exp = 1;
          has_dec_point = 1;    /* decimal point illegal further */
          p++;
          if ((*p == '-') || (*p == '+') || isdigit(*p)) {
            continue;
          }
        }
      }
      return (mk_symbol(sc, str_to_maybe_lower(q)));
    }
  }
  if (has_dec_point) {
    return mk_real(sc, atof(q));
  }
  return (mk_integer(sc, atol(q)));
}

static pointer mk_sharp_const(scheme * sc, char *name) {
  if (str_eq_to_lower(name, "t")) {
    return (sc->T);
  } else if (str_eq_to_lower(name, "f")) {
    return (sc->F);
  } else if (*name == '\\') {   /* #\w (character) */
    int c = 0;
    if (str_eq_to_lower(name + 1, "space")) {
      c = ' ';
    } else if (str_eq_to_lower(name + 1, "newline")) {
      c = '\n';
    } else if (str_eq_to_lower(name + 1, "return")) {
      c = '\r';
    } else if (str_eq_to_lower(name + 1, "tab")) {
      c = '\t';
    } else if (name[1] == 'x' && name[2] != 0) {
      int c1 = 0;
      if (sscanf(name + 2, "%x", (unsigned int *) &c1) == 1) {
        c = c1;
      } else {
        return sc->NIL;
      }
    } else if (name[2] == 0) {
      c = name[1];
    } else {
      return sc->NIL;
    }
    return mk_character(sc, c);
  } else if (*name == 'x') {    /* #x (hex) */
    return (mk_integer(sc, strtol(name + 1, 0, 16)));
  } else if (*name == 'b') {    /* #b (bin) */
    return (mk_integer(sc, strtol(name + 1, 0, 2)));
  } else if (*name == 'o') {    /* #o (oct) */
    return (mk_integer(sc, strtol(name + 1, 0, 8)));
  } else if (*name == 'd') {    /* #d (dec) */
    return (mk_integer(sc, strtol(name + 1, 0, 10)));
  } else {
    return (sc->NIL);
  }
}

/* ========== garbage collector ========== */

/*--
 *  We use algorithm E (Knuth, The Art of Computer Programming Vol.1,
 *  sec. 2.3.5), the Schorr-Deutsch-Waite link-inversion algorithm,
 *  for marking.
 */
static void mark(pointer a) {
  pointer t, q, p;

  t = (pointer) 0;
  p = a;
E2:setmark(p);
  if (is_vector(p)) {
    int i;
    int num = ivalue_unchecked(p) / 2 + ivalue_unchecked(p) % 2;
    for (i = 0; i < num; i++) {
      /* Vector cells will be treated like ordinary cells */
      mark(p + 1 + i);
    }
  }
  if (is_atom(p))
    goto E6;
  /* E4: down car */
  q = car(p);
  if (q && !is_mark(q)) {
    setatom(p);                 /* a note that we have moved car */
    car(p) = t;
    t = p;
    p = q;
    goto E2;
  }
E5:q = cdr(p);                 /* down cdr */
  if (q && !is_mark(q)) {
    cdr(p) = t;
    t = p;
    p = q;
    goto E2;
  }
E6:                            /* up.  Undo the link switching from steps E4 and E5. */
  if (!t)
    return;
  q = t;
  if (is_atom(q)) {
    clratom(q);
    t = car(q);
    car(q) = p;
    p = q;
    goto E5;
  } else {
    t = cdr(q);
    cdr(q) = p;
    p = q;
    goto E6;
  }
}

/* garbage collection. parameter a, b is marked. */
static void gc(scheme * sc, pointer a, pointer b) {
  pointer p;
  int i;

  if (sc->gc_verbose) {
    putstr(sc, "gc...");
  }

  /* mark system globals */
  mark(sc->oblist);
  mark(sc->global_env);

  /* mark current registers */
  mark(sc->args);
  mark(sc->envir);
  mark(sc->code);
  dump_stack_mark(sc);
  mark(sc->value);
  mark(sc->inport);
  mark(sc->save_inport);
  mark(sc->outport);
  mark(sc->loadport);

  /* Mark recent objects the interpreter doesn't know about yet. */
  mark(car(sc->sink));
  /* Mark any older stuff above nested C calls */
  mark(sc->c_nest);

  /* mark variables a, b */
  mark(a);
  mark(b);

  /* garbage collect */
  clrmark(sc->NIL);
  sc->fcells = 0;
  sc->free_cell = sc->NIL;
  /* free-list is kept sorted by address so as to maintain consecutive
     ranges, if possible, for use with vectors. Here we scan the cells
     (which are also kept sorted by address) downwards to build the
     free-list in sorted order.
   */
  for (i = sc->last_cell_seg; i >= 0; i--) {
    p = sc->cell_seg[i] + cell_segsize;
    while (--p >= sc->cell_seg[i]) {
      if (is_mark(p)) {
        clrmark(p);
      } else {
        /* reclaim cell */
        if (typeflag(p) != 0) {
          finalize_cell(sc, p);
          typeflag(p) = 0;
          car(p) = sc->NIL;
        }
        ++sc->fcells;
        cdr(p) = sc->free_cell;
        sc->free_cell = p;
      }
    }
  }

  if (sc->gc_verbose) {
    char msg[80];
    sprintf(msg, "done: %ld cells were recovered.\n", sc->fcells);
    putstr(sc, msg);
  }
}

static void finalize_cell(scheme * sc, pointer a) {
  if (is_string(a)) {
    sc->free(strvalue(a));
  } else if (is_port(a)) {
    if (a->_object._port->kind & port_file
        && a->_object._port->rep.stdio.closeit) {
      port_close(sc, a, port_input | port_output);
    }
    sc->free(a->_object._port);
  }
}

/* ========== Routines for Reading ========== */

static int file_push(scheme * sc, const char *fname) {
  FILE *fin = NULL;

  if (sc->file_i == MAXFIL - 1)
    return 0;
  fin = fopen(fname, "r");
  if (fin != 0) {
    sc->file_i++;
    sc->load_stack[sc->file_i].kind = port_file | port_input;
    sc->load_stack[sc->file_i].rep.stdio.file = fin;
    sc->load_stack[sc->file_i].rep.stdio.closeit = 1;
    sc->nesting_stack[sc->file_i] = 0;
    sc->loadport->_object._port = sc->load_stack + sc->file_i;

#if SHOW_ERROR_LINE
    sc->load_stack[sc->file_i].rep.stdio.curr_line = 0;
    if (fname)
      sc->load_stack[sc->file_i].rep.stdio.filename =
          store_string(sc, strlen(fname), fname);
#endif
  }
  return fin != 0;
}

static void file_pop(scheme * sc) {
  if (sc->file_i != 0) {
    sc->nesting = sc->nesting_stack[sc->file_i];
    port_close(sc, sc->loadport, port_input);
    sc->file_i--;
    sc->loadport->_object._port = sc->load_stack + sc->file_i;
  }
}

static int file_interactive(scheme * sc) {
  return sc->interactive_repl
      && sc->file_i == 0 && sc->load_stack[0].rep.stdio.file == stdin
      && sc->inport->_object._port->kind & port_file;
}

static port *port_rep_from_filename(scheme * sc, const char *fn, int prop) {
  FILE *f;
  char *rw;
  port *pt;
  if (prop == (port_input | port_output)) {
    rw = "a+";
  } else if (prop == port_output) {
    rw = "w";
  } else {
    rw = "r";
  }
  f = fopen(fn, rw);
  if (f == 0) {
    return 0;
  }
  pt = port_rep_from_file(sc, f, prop);
  pt->rep.stdio.closeit = 1;

#if SHOW_ERROR_LINE
  if (fn)
    pt->rep.stdio.filename = store_string(sc, strlen(fn), fn);

  pt->rep.stdio.curr_line = 0;
#endif
  return pt;
}

static pointer port_from_filename(scheme * sc, const char *fn, int prop) {
  port *pt;
  pt = port_rep_from_filename(sc, fn, prop);
  if (pt == 0) {
    return sc->NIL;
  }
  return mk_port(sc, pt);
}

static port *port_rep_from_file(scheme * sc, FILE * f, int prop) {
  port *pt;

  pt = (port *) sc->malloc(sizeof *pt);
  if (pt == NULL) {
    return NULL;
  }
  pt->kind = port_file | prop;
  pt->rep.stdio.file = f;
  pt->rep.stdio.closeit = 0;
  return pt;
}

static pointer port_from_file(scheme * sc, FILE * f, int prop) {
  port *pt;
  pt = port_rep_from_file(sc, f, prop);
  if (pt == 0) {
    return sc->NIL;
  }
  return mk_port(sc, pt);
}

static port *port_rep_from_string(scheme * sc, char *start,
    char *past_the_end, int prop) {
  port *pt;
  pt = (port *) sc->malloc(sizeof(port));
  if (pt == 0) {
    return 0;
  }
  pt->kind = port_string | prop;
  pt->rep.string.start = start;
  pt->rep.string.curr = start;
  pt->rep.string.past_the_end = past_the_end;
  return pt;
}

static pointer port_from_string(scheme * sc, char *start, char *past_the_end,
    int prop) {
  port *pt;
  pt = port_rep_from_string(sc, start, past_the_end, prop);
  if (pt == 0) {
    return sc->NIL;
  }
  return mk_port(sc, pt);
}

#define BLOCK_SIZE 256

static port *port_rep_from_scratch(scheme * sc) {
  port *pt;
  char *start;
  pt = (port *) sc->malloc(sizeof(port));
  if (pt == 0) {
    return 0;
  }
  start = sc->malloc(BLOCK_SIZE);
  if (start == 0) {
    return 0;
  }
  memset(start, ' ', BLOCK_SIZE - 1);
  start[BLOCK_SIZE - 1] = '\0';
  pt->kind = port_string | port_output | port_srfi6;
  pt->rep.string.start = start;
  pt->rep.string.curr = start;
  pt->rep.string.past_the_end = start + BLOCK_SIZE - 1;
  return pt;
}

static pointer port_from_scratch(scheme * sc) {
  port *pt;
  pt = port_rep_from_scratch(sc);
  if (pt == 0) {
    return sc->NIL;
  }
  return mk_port(sc, pt);
}

static void port_close(scheme * sc, pointer p, int flag) {
  port *pt = p->_object._port;
  pt->kind &= ~flag;
  if ((pt->kind & (port_input | port_output)) == 0) {
    if (pt->kind & port_file) {

#if SHOW_ERROR_LINE
      /* Cleanup is here so (close-*-port) functions could work too */
      pt->rep.stdio.curr_line = 0;

      if (pt->rep.stdio.filename)
        sc->free(pt->rep.stdio.filename);
#endif

      fclose(pt->rep.stdio.file);
    }
    pt->kind = port_free;
  }
}

static int utf8_inchar(port *pt) {
  int c = basic_inchar(pt);
  int bytes, i;
  char buf[4];
  if (c == EOF || c < 0x80) {
    return c;
  }
  buf[0] = (char) c;
  bytes = (c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4);
  for (i = 1; i < bytes; i++) {
    c = basic_inchar(pt);
    if (c == EOF) {
      return EOF;
    }
    buf[i] = (char) c;
  }
  return utf8_decode(buf);
}

/* get new character from input file */
static int inchar(scheme * sc) {
  int c;
  port *pt;
  if (sc->backchar >= 0) {
    c = sc->backchar;
    sc->backchar = -1;
    return c;
  }
  pt = sc->inport->_object._port;
  if (pt->kind & port_saw_EOF) {
    return EOF;
  }
  c = utf8_inchar(pt);
  if (c == EOF && sc->inport == sc->loadport) {
    /* Instead, set port_saw_EOF */
    pt->kind |= port_saw_EOF;
    return EOF;
  }
  return c;
}

static int inchar8(scheme * sc) {
  int c;
  port *pt;
  if (sc->backchar >= 0) {
    c = sc->backchar;
    sc->backchar = -1;
    return c;
  }
  pt = sc->inport->_object._port;
  if (pt->kind & port_saw_EOF) {
    return EOF;
  }
  c = basic_inchar(pt);
  if (c == EOF && sc->inport == sc->loadport) {
    /* Instead, set port_saw_EOF */
    pt->kind |= port_saw_EOF;
    return EOF;
  }
  return c;
}

static int basic_inchar(port * pt) {
  if (pt->kind & port_file) {
    return fgetc(pt->rep.stdio.file);
  } else {
    if (*pt->rep.string.curr == 0 ||
        pt->rep.string.curr == pt->rep.string.past_the_end) {
      return EOF;
    } else {
      return *pt->rep.string.curr++;
    }
  }
}

/* back character to input buffer */
static void backchar(scheme * sc, int c) {
  if (c == EOF)
    return;
  sc->backchar = c;
}

static int realloc_port_string(scheme * sc, port * p) {
  char *start = p->rep.string.start;
  size_t new_size = p->rep.string.past_the_end - start + 1 + BLOCK_SIZE;
  char *str = sc->malloc(new_size);
  if (str) {
    memset(str, ' ', new_size - 1);
    str[new_size - 1] = '\0';
    strcpy(str, start);
    p->rep.string.start = str;
    p->rep.string.past_the_end = str + new_size - 1;
    p->rep.string.curr -= start - str;
    sc->free(start);
    return 1;
  } else {
    return 0;
  }
}

static void char_to_utf8(int c, char *p, int *plen) {
  unsigned char *s = (unsigned char *) p;
  int bytes;
  if (c < 0 || c > 0x10FFFF) {
    c = '?';
  }
  if (c < 0x80) {
    *s++ = (unsigned char) c;
    *s = 0;
  } else {
    bytes = (c < 0x800) ? 2 : ((c < 0x10000) ? 3 : 4);
    s[bytes] = 0;
    s[0] = 0x80;
    while (--bytes) {
      s[0] |= (0x80 >> bytes);
      s[bytes] = (unsigned char) (0x80 | (c & 0x3F));
      c >>= 6;
    }
    s[0] |= (unsigned char) c;
  }
  *plen = strlen(p);
}

INTERFACE void putstr(scheme * sc, const char *s) {
  port *pt = sc->outport->_object._port;
  if (pt->kind & port_file) {
    fputs(s, pt->rep.stdio.file);
  } else {
    for (; *s; s++) {
      if (pt->rep.string.curr != pt->rep.string.past_the_end) {
        *pt->rep.string.curr++ = *s;
      } else if (pt->kind & port_srfi6 && realloc_port_string(sc, pt)) {
        *pt->rep.string.curr++ = *s;
      }
    }
  }
}

static void putchars(scheme * sc, const char *s, int len) {
  port *pt = sc->outport->_object._port;
  if (pt->kind & port_file) {
    fwrite(s, 1, len, pt->rep.stdio.file);
  } else {
    for (; len; len--) {
      if (pt->rep.string.curr != pt->rep.string.past_the_end) {
        *pt->rep.string.curr++ = *s++;
      } else if (pt->kind & port_srfi6 && realloc_port_string(sc, pt)) {
        *pt->rep.string.curr++ = *s++;
      }
    }
  }
}

INTERFACE void putcharacter(scheme * sc, int c) {
  port *pt = sc->outport->_object._port;
  if (pt->kind & port_file) {
    fputc(c, pt->rep.stdio.file);
  } else {
    if (pt->rep.string.curr != pt->rep.string.past_the_end) {
      *pt->rep.string.curr++ = c;
    } else if (pt->kind & port_srfi6 && realloc_port_string(sc, pt)) {
      *pt->rep.string.curr++ = c;
    }
  }
}

static int check_strbuff_size(scheme * sc, char **p) {
  char *t;
  int len = *p - sc->strbuff;
  if (len + 4 < sc->strbuff_size) {
    return 1;
  }
  sc->strbuff_size *= 2;
  if (sc->strbuff_size >= STRBUFF_MAX_SIZE) {
    sc->strbuff_size /= 2;
    return 0;
  }
  t = sc->malloc(sc->strbuff_size);
  memcpy(t, sc->strbuff, len);
  *p = t + len;
  sc->free(sc->strbuff);
  sc->strbuff = t;
  return 1;
}

/* read characters up to delimiter, but cater to character constants */
static char *readstr_upto(scheme * sc, char *delim) {
  char *p = sc->strbuff;
  int c, len;

  while (1) {
    c = inchar(sc);
    if (check_strbuff_size(sc, &p)) {
      char_to_utf8(c, p, &len);
      p += len;
    }
    if(is_one_of(delim, c)) {
      break;
    }
  }

  if (p == sc->strbuff + 2 && p[-2] == '\\') {
    *p = 0;
  } else {
    backchar(sc, p[-1]);
    *--p = '\0';
  }
  return sc->strbuff;
}

/* read string expression "xxx...xxx" */
static pointer readstrexp(scheme * sc) {
  char *p = sc->strbuff;
  int c, len;
  int c1 = 0;
  enum { st_ok, st_bsl, st_x1, st_x2, st_oct1, st_oct2 } state = st_ok;

  for (;;) {
    c = inchar(sc);
    if (c == EOF || !check_strbuff_size(sc, &p)) {
      return sc->F;
    }
    switch (state) {
    case st_ok:
      switch (c) {
      case '\\':
        state = st_bsl;
        break;
      case '"':
        *p = 0;
        return mk_counted_string(sc, sc->strbuff, p - sc->strbuff);
      default:
        char_to_utf8(c, p, &len);
        p += len;
        break;
      }
      break;
    case st_bsl:
      switch (c) {
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
        state = st_oct1;
        c1 = c - '0';
        break;
      case 'x':
      case 'X':
        state = st_x1;
        c1 = 0;
        break;
      case 'n':
        *p++ = '\n';
        state = st_ok;
        break;
      case 't':
        *p++ = '\t';
        state = st_ok;
        break;
      case 'r':
        *p++ = '\r';
        state = st_ok;
        break;
      case '"':
        *p++ = '"';
        state = st_ok;
        break;
      default:
        *p++ = c;
        state = st_ok;
        break;
      }
      break;
    case st_x1:
    case st_x2:
      c = toupper(c);
      if (c >= '0' && c <= 'F') {
        if (c <= '9') {
          c1 = (c1 << 4) + c - '0';
        } else {
          c1 = (c1 << 4) + c - 'A' + 10;
        }
        if (state == st_x1) {
          state = st_x2;
        } else {
          *p++ = c1;
          state = st_ok;
        }
      } else {
        return sc->F;
      }
      break;
    case st_oct1:
    case st_oct2:
      if (c < '0' || c > '7') {
        *p++ = c1;
        backchar(sc, c);
        state = st_ok;
      } else {
        if (state == st_oct2 && c1 >= 32)
          return sc->F;

        c1 = (c1 << 3) + (c - '0');

        if (state == st_oct1)
          state = st_oct2;
        else {
          *p++ = c1;
          state = st_ok;
        }
      }
      break;

    }
  }
}

/* check c is in chars */
static INLINE int is_one_of(char *s, int c) {
  if (c == EOF)
    return 1;
  while (*s)
    if (*s++ == c)
      return (1);
  return (0);
}

/* skip white characters */
static INLINE int skipspace(scheme * sc) {
  int c = 0, curr_line = 0;

  do {
    c = inchar(sc);
#if SHOW_ERROR_LINE
    if (c == '\n')
      curr_line++;
#endif
  } while (isspace(c));

/* record it */
#if SHOW_ERROR_LINE
  if (sc->load_stack[sc->file_i].kind & port_file)
    sc->load_stack[sc->file_i].rep.stdio.curr_line += curr_line;
#endif

  if (c != EOF) {
    backchar(sc, c);
    return 1;
  } else {
    return EOF;
  }
}

/* get token */
static int token(scheme * sc) {
  int c;
  c = skipspace(sc);
  if (c == EOF) {
    return (TOK_EOF);
  }
  switch (c = inchar(sc)) {
  case EOF:
    return (TOK_EOF);
  case '(':
    return (TOK_LPAREN);
  case ')':
    return (TOK_RPAREN);
  case '.':
    c = inchar(sc);
    if (is_one_of(" \n\t", c)) {
      return (TOK_DOT);
    } else {
      backchar(sc, c);
      backchar(sc, '.');
      return TOK_ATOM;
    }
  case '\'':
    return (TOK_QUOTE);
  case ';':
    while ((c = inchar(sc)) != '\n' && c != EOF) {
    }

#if SHOW_ERROR_LINE
    if (c == '\n' && sc->load_stack[sc->file_i].kind & port_file)
      sc->load_stack[sc->file_i].rep.stdio.curr_line++;
#endif

    if (c == EOF) {
      return (TOK_EOF);
    } else {
      return (token(sc));
    }
  case '"':
    return (TOK_DQUOTE);
  case BACKQUOTE:
    return (TOK_BQUOTE);
  case ',':
    if ((c = inchar(sc)) == '@') {
      return (TOK_ATMARK);
    } else {
      backchar(sc, c);
      return (TOK_COMMA);
    }
  case '#':
    c = inchar(sc);
    if (c == '(') {
      return (TOK_VEC);
    } else if (c == '!') {
      while ((c = inchar(sc)) != '\n' && c != EOF) {
      }

#if SHOW_ERROR_LINE
      if (c == '\n' && sc->load_stack[sc->file_i].kind & port_file)
        sc->load_stack[sc->file_i].rep.stdio.curr_line++;
#endif

      if (c == EOF) {
        return (TOK_EOF);
      } else {
        return (token(sc));
      }
    } else {
      backchar(sc, c);
      if (is_one_of(" tfodxb\\", c)) {
        return TOK_SHARP_CONST;
      } else {
        return (TOK_SHARP);
      }
    }
  default:
    backchar(sc, c);
    return (TOK_ATOM);
  }
}

/* ========== Routines for Printing ========== */
#define   ok_abbrev(x)   (is_pair(x) && cdr(x) == sc->NIL)

void long_to_str(long v, char *s, int base) {
  char *p;
  char c;
  if (v < 0) {
    *s++ = '-';
    v = -v;
  }
  p = s;
  if (v == 0) {
    *s++ = '0';
  }
  while (v > 0) {
    c = (char) (v % base);
    v /= base;
    if (c < 10) {
      c += '0';
    } else {
      c += 'A' - 10;
    }
    *s++ = c;
  }
  *s-- = '\0';
  while (s > p) {
    c = *p;
    *p++ = *s;
    *s-- = c;
  }
}


static void string_to_utf8(scheme * sc, char *s, int len) {
  int i, c, d, charsize;
  char *q = sc->strbuff;
  if (IS_ASCII(*s)) {
    charsize = 1;
  } else {
    charsize = 4;
    s += 4;
  }
  for (i = 0; i < len; i++) {
    if (charsize == 1) {
      c = *s++;
    } else {
      c = *((int*) s);
      s += 4;
    }
    if (check_strbuff_size(sc, &q)) {
      if (c != 0) {
        char_to_utf8(c, q, &d);
        q += d;
      } else {
        *q++ = (char) 0xC0;
        *q++ = (char) 0x80;
      }
    }
  }
  *q = 0;
}

static void printslashstring(scheme * sc, char *p, int len) {
  int i, c, d, charsize;
  char buf[5];
  unsigned char *s = (unsigned char *) p;
  putcharacter(sc, '"');
  if (IS_ASCII(*p)) {
    charsize = 1;
  } else {
    charsize = 4;
    s += 4;
  }
  for (i = 0; i < len; i++) {
    if (charsize == 1) {
      c = *s++;
    } else {
      c = *((int*) s);
      s += 4;
    }
    if (c == '"' || c < ' ' || c == '\\') {
      putcharacter(sc, '\\');
      switch (c) {
      case '"':
        putcharacter(sc, '"');
        break;
      case '\n':
        putcharacter(sc, 'n');
        break;
      case '\t':
        putcharacter(sc, 't');
        break;
      case '\r':
        putcharacter(sc, 'r');
        break;
      case '\\':
        putcharacter(sc, '\\');
        break;
      default:{
          d = c / 16;
          putcharacter(sc, 'x');
          if (d < 10) {
            putcharacter(sc, d + '0');
          } else {
            putcharacter(sc, d - 10 + 'A');
          }
          d = c % 16;
          if (d < 10) {
            putcharacter(sc, d + '0');
          } else {
            putcharacter(sc, d - 10 + 'A');
          }
        }
      }
    } else {
      char_to_utf8(c, buf, &d);
      for (c = 0; c < d; c++) {
        putcharacter(sc, buf[c]);
      }
    }
  }
  putcharacter(sc, '"');
}


/* print atoms */
static void printatom(scheme * sc, pointer l, int f) {
  char *p;
  int len;
  atom2str(sc, l, f, &p, &len);
  putchars(sc, p, len);
}


/* Uses internal buffer unless string pointer is already available */
static void atom2str(scheme * sc, pointer l, int f, char **pp, int *plen) {
  char *p;

  *plen = -1;
  if (l == sc->NIL) {
    p = "()";
  } else if (l == sc->T) {
    p = "#t";
  } else if (l == sc->F) {
    p = "#f";
  } else if (l == sc->EOF_OBJ) {
    p = "#<EOF>";
  } else if (is_port(l)) {
    p = "#<PORT>";
  } else if (is_number(l)) {
    p = sc->strbuff;
    if (f <= 1 || f == 10) {    /* f is the base for numbers if > 1 */
      if (num_is_integer(l)) {
        sprintf(p, "%ld", ivalue_unchecked(l));
      } else {
        if (rvalue_unchecked(l) * 0.0 != 0.0) { // is +/-inf or nan
          if (rvalue_unchecked(l) > 0) {
            strcpy(p, "+inf");
          } else if (rvalue_unchecked(l) < 0) {
            strcpy(p, "-inf");
          } else {
            strcpy(p, "+nan");
          }
        } else {
          sprintf(p, "%.10g", rvalue_unchecked(l));
        }
        /* r5rs says there must be a '.' (unless 'e'?) */
        f = strcspn(p, ".e");
        if (p[f] == 0) {
          p[f] = '.';           /* not found, so add '.0' at the end */
          p[f + 1] = '0';
          p[f + 2] = 0;
        }
      }
    } else {
      long v = ivalue(l);
      if (f >= 2 && f <= 36) {
        long_to_str(v, p, f);
      } else {
        *p = '\0';
      }
    }
  } else if (is_string(l)) {
    if (!f) {
      p = strvalue(l);
      if (IS_ASCII(*p)) {
        *plen = strlength(l);
      } else {
        string_to_utf8(sc, strvalue(l), strlength(l));
        p = sc->strbuff;
      }
    } else {                    /* Hack, uses the fact that printing is needed */
      *pp = sc->strbuff;
      *plen = 0;
      printslashstring(sc, strvalue(l), strlength(l));
      return;
    }
  } else if (is_character(l)) {
    int c = charvalue(l);
    p = sc->strbuff;
    if (!f) {
      char_to_utf8(c, p, plen);
    } else {
      switch (c) {
      case ' ':
        p = "#\\space";
        break;
      case '\n':
        p = "#\\newline";
        break;
      case '\r':
        p = "#\\return";
        break;
      case '\t':
        p = "#\\tab";
        break;
      default:
        if (c < 32 || c >= 0x80) {
          sprintf(p, "#\\x%x", c);
          break;
        }
        sprintf(p, "#\\%c", c);
        break;
      }
    }
  } else if (is_symbol(l)) {
    p = symname(l);
  } else if (is_proc(l)) {
    p = sc->strbuff;
    snprintf(p, sc->strbuff_size, "#<%s PROCEDURE %ld>", procname(l), procnum(l));
  } else if (is_macro(l)) {
    p = "#<MACRO>";
  } else if (is_closure(l)) {
    p = "#<CLOSURE>";
  } else if (is_promise(l)) {
    p = "#<PROMISE>";
  } else if (is_foreign(l)) {
    p = sc->strbuff;
    snprintf(p, sc->strbuff_size, "#<FOREIGN PROCEDURE %ld>", procnum(l));
  } else if (is_continuation(l)) {
    p = "#<CONTINUATION>";
  } else if (is_bvector(l)) {
    p = sc->strbuff;
    sprintf(p, "#u8(len=%d)", strlength(l));
  } else {
    p = "#<ERROR>";
  }
  *pp = p;
  if (*plen < 0) {
    *plen = strlen(p);
  }
}

/* ========== Routines for Evaluation Cycle ========== */

/* make closure. c is code. e is environment */
static pointer mk_closure(scheme * sc, pointer c, pointer e) {
  pointer x = get_cell(sc, c, e);

  typeflag(x) = T_CLOSURE;
  car(x) = c;
  cdr(x) = e;
  return (x);
}

/* make continuation. */
static pointer mk_continuation(scheme * sc, pointer d) {
  pointer x = get_cell(sc, sc->NIL, d);

  typeflag(x) = T_CONTINUATION;
  cont_dump(x) = d;
  return (x);
}

static pointer list_star(scheme * sc, pointer d) {
  pointer p, q;
  if (cdr(d) == sc->NIL) {
    return car(d);
  }
  p = cons(sc, car(d), cdr(d));
  q = p;
  while (cdr(cdr(p)) != sc->NIL) {
    d = cons(sc, car(p), cdr(p));
    if (cdr(cdr(p)) != sc->NIL) {
      p = cdr(d);
    }
  }
  cdr(p) = car(cdr(p));
  return q;
}

/* reverse list -- produce new list */
static pointer reverse(scheme * sc, pointer a) {
/* a must be checked by gc */
  pointer p = sc->NIL;

  for (; is_pair(a); a = cdr(a)) {
    p = cons(sc, car(a), p);
  }
  return (p);
}

/* reverse list --- in-place */
static pointer reverse_in_place(scheme * sc, pointer term, pointer list) {
  pointer p = list, result = term, q;

  while (p != sc->NIL) {
    q = cdr(p);
    cdr(p) = result;
    result = p;
    p = q;
  }
  return (result);
}

/* append list -- produce new list (in reverse order) */
static pointer revappend(scheme * sc, pointer a, pointer b) {
  pointer result = a;
  pointer p = b;

  while (is_pair(p)) {
    result = cons(sc, car(p), result);
    p = cdr(p);
  }

  if (p == sc->NIL) {
    return result;
  }

  return sc->F;                 /* signal an error */
}

/* equivalence of atoms */
int eqv(pointer a, pointer b) {
  if (is_string(a)) {
    if (is_string(b))
      return (strvalue(a) == strvalue(b));
    else
      return (0);
  } else if (is_number(a)) {
    if (is_number(b)) {
      if (num_is_integer(a) == num_is_integer(b))
        return num_eq(nvalue(a), nvalue(b));
    }
    return (0);
  } else if (is_character(a)) {
    if (is_character(b))
      return charvalue(a) == charvalue(b);
    else
      return (0);
  } else if (is_port(a)) {
    if (is_port(b))
      return a == b;
    else
      return (0);
  } else if (is_proc(a)) {
    if (is_proc(b))
      return procnum(a) == procnum(b);
    else
      return (0);
  } else {
    return (a == b);
  }
}

/* true or false value macro */
/* () is #t in R5RS */
#define is_true(p)       ((p) != sc->F)
#define is_false(p)      ((p) == sc->F)

/* ========== Environment implementation  ========== */

#if !defined(USE_ALIST_ENV) || !defined(USE_OBJECT_LIST)

static int hash_fn(const char *key, int table_size) {
  unsigned int hashed = 0;
  const char *c;
  int bits_per_int = sizeof(unsigned int) * 8;

  for (c = key; *c; c++) {
    /* letters have about 5 bits in them */
    hashed = (hashed << 5) | (hashed >> (bits_per_int - 5));
    hashed ^= *c;
  }
  return hashed % table_size;
}
#endif

#ifndef USE_ALIST_ENV

/*
 * In this implementation, each frame of the environment may be
 * a hash table: a vector of alists hashed by variable name.
 * In practice, we use a vector only for the initial frame;
 * subsequent frames are too small and transient for the lookup
 * speed to out-weigh the cost of making a new vector.
 */

static void new_frame_in_env(scheme * sc, pointer old_env) {
  pointer new_frame;

  /* The interaction-environment has about 300 variables in it. */
  if (old_env == sc->NIL) {
    new_frame = mk_vector(sc, 461);
  } else {
    new_frame = sc->NIL;
  }

  sc->envir = immutable_cons(sc, new_frame, old_env);
  setenvironment(sc->envir);
}

static INLINE void new_slot_spec_in_env(scheme * sc, pointer env,
    pointer variable, pointer value) {
  pointer slot = immutable_cons(sc, variable, value);

  if (is_vector(car(env))) {
    int location = hash_fn(symname(variable), ivalue_unchecked(car(env)));

    set_vector_elem(car(env), location,
        immutable_cons(sc, slot, vector_elem(car(env), location)));
  } else {
    car(env) = immutable_cons(sc, slot, car(env));
  }
}

static pointer find_slot_in_env(scheme * sc, pointer env, pointer hdl,
    int all) {
  pointer x, y;
  int location;

  for (x = env; x != sc->NIL; x = cdr(x)) {
    if (is_vector(car(x))) {
      location = hash_fn(symname(hdl), ivalue_unchecked(car(x)));
      y = vector_elem(car(x), location);
    } else {
      y = car(x);
    }
    for (; y != sc->NIL; y = cdr(y)) {
      if (caar(y) == hdl) {
        break;
      }
    }
    if (y != sc->NIL) {
      break;
    }
    if (!all) {
      return sc->NIL;
    }
  }
  if (x != sc->NIL) {
    return car(y);
  }
  return sc->NIL;
}

#else /* USE_ALIST_ENV */

static INLINE void new_frame_in_env(scheme * sc, pointer old_env) {
  sc->envir = immutable_cons(sc, sc->NIL, old_env);
  setenvironment(sc->envir);
}

static INLINE void new_slot_spec_in_env(scheme * sc, pointer env,
    pointer variable, pointer value) {
  car(env) =
      immutable_cons(sc, immutable_cons(sc, variable, value), car(env));
}

static pointer find_slot_in_env(scheme * sc, pointer env, pointer hdl,
    int all) {
  pointer x, y;
  for (x = env; x != sc->NIL; x = cdr(x)) {
    for (y = car(x); y != sc->NIL; y = cdr(y)) {
      if (caar(y) == hdl) {
        break;
      }
    }
    if (y != sc->NIL) {
      break;
    }
    if (!all) {
      return sc->NIL;
    }
  }
  if (x != sc->NIL) {
    return car(y);
  }
  return sc->NIL;
}

#endif /* USE_ALIST_ENV else */

static INLINE void new_slot_in_env(scheme * sc, pointer variable,
    pointer value) {
  new_slot_spec_in_env(sc, sc->envir, variable, value);
}

static INLINE void set_slot_in_env(pointer slot, pointer value) {
  cdr(slot) = value;
}

static INLINE pointer slot_value_in_env(pointer slot) {
  return cdr(slot);
}

/* ========== Evaluation Cycle ========== */


static pointer _Error_1(scheme * sc, const char *s, pointer a) {
  const char *str = s;
#if USE_ERROR_HOOK
  pointer x;
  pointer hdl = sc->ERROR_HOOK;
#endif

#if SHOW_ERROR_LINE
  char sbuf[AUXBUFF_SIZE];

  /* make sure error is not in REPL */
  if (sc->load_stack[sc->file_i].kind & port_file &&
      sc->load_stack[sc->file_i].rep.stdio.file != stdin) {
    int ln = sc->load_stack[sc->file_i].rep.stdio.curr_line;
    const char *fname = sc->load_stack[sc->file_i].rep.stdio.filename;

    /* should never happen */
    if (!fname)
      fname = "<unknown>";

    /* we started from 0 */
    ln++;
    snprintf(sbuf, AUXBUFF_SIZE, "(%s : %i) %s", fname, ln, s);

    str = (const char *) sbuf;
  }
#endif

#if USE_ERROR_HOOK
  x = find_slot_in_env(sc, sc->envir, hdl, 1);
  if (x != sc->NIL) {
    if (a != 0) {
      sc->code =
          cons(sc, cons(sc, sc->QUOTE, cons(sc, (a), sc->NIL)), sc->NIL);
    } else {
      sc->code = sc->NIL;
    }
    sc->code = cons(sc, mk_string(sc, str), sc->code);
    setimmutable(car(sc->code));
    sc->code = cons(sc, slot_value_in_env(x), sc->code);
    sc->op = (int) OP_EVAL;
    return sc->T;
  }
#endif

  if (a != 0) {
    sc->args = cons(sc, (a), sc->NIL);
  } else {
    sc->args = sc->NIL;
  }
  sc->args = cons(sc, mk_string(sc, str), sc->args);
  setimmutable(car(sc->args));
  sc->op = (int) OP_ERR0;
  return sc->T;
}

#define Error_1(sc,s, a) return _Error_1(sc,s,a)
#define Error_0(sc,s)    return _Error_1(sc,s,0)

/* Too small to turn into function */
#define  BEGIN     do {
#define  END  } while (0)
#define s_goto(sc,a) BEGIN                                  \
    sc->op = (int)(a);                                      \
    return sc->T; END

#define s_return(sc,a) return _s_return(sc,a)

#ifndef USE_SCHEME_STACK

/* this structure holds all the interpreter's registers */
struct dump_stack_frame {
  enum scheme_opcodes op;
  pointer args;
  pointer envir;
  pointer code;
};

#define STACK_GROWTH 3

static void s_save(scheme * sc, enum scheme_opcodes op, pointer args,
    pointer code) {
  int nframes = (int) sc->dump;
  struct dump_stack_frame *next_frame;

  /* enough room for the next frame? */
  if (nframes >= sc->dump_size) {
    sc->dump_size += STACK_GROWTH;
    /* alas there is no sc->realloc */
    sc->dump_base = realloc(sc->dump_base,
        sizeof(struct dump_stack_frame) * sc->dump_size);
  }
  next_frame = (struct dump_stack_frame *) sc->dump_base + nframes;
  next_frame->op = op;
  next_frame->args = args;
  next_frame->envir = sc->envir;
  next_frame->code = code;
  sc->dump = (pointer) (nframes + 1);
}

static pointer _s_return(scheme * sc, pointer a) {
  int nframes = (int) sc->dump;
  struct dump_stack_frame *frame;

  sc->value = (a);
  if (nframes <= 0) {
    return sc->NIL;
  }
  nframes--;
  frame = (struct dump_stack_frame *) sc->dump_base + nframes;
  sc->op = frame->op;
  sc->args = frame->args;
  sc->envir = frame->envir;
  sc->code = frame->code;
  sc->dump = (pointer) nframes;
  return sc->T;
}

static INLINE void dump_stack_reset(scheme * sc) {
  /* in this implementation, sc->dump is the number of frames on the stack */
  sc->dump = (pointer) 0;
}

static INLINE void dump_stack_initialize(scheme * sc) {
  sc->dump_size = 0;
  sc->dump_base = NULL;
  dump_stack_reset(sc);
}

static void dump_stack_free(scheme * sc) {
  free(sc->dump_base);
  sc->dump_base = NULL;
  sc->dump = (pointer) 0;
  sc->dump_size = 0;
}

static INLINE void dump_stack_mark(scheme * sc) {
  int nframes = (int) sc->dump;
  int i;
  for (i = 0; i < nframes; i++) {
    struct dump_stack_frame *frame;
    frame = (struct dump_stack_frame *) sc->dump_base + i;
    mark(frame->args);
    mark(frame->envir);
    mark(frame->code);
  }
}

#else

static INLINE void dump_stack_reset(scheme * sc) {
  sc->dump = sc->NIL;
}

static INLINE void dump_stack_initialize(scheme * sc) {
  dump_stack_reset(sc);
}

static void dump_stack_free(scheme * sc) {
  sc->dump = sc->NIL;
}

static pointer _s_return(scheme * sc, pointer a) {
  sc->value = (a);
  if (sc->dump == sc->NIL)
    return sc->NIL;
  sc->op = ivalue(car(sc->dump));
  sc->args = cadr(sc->dump);
  sc->envir = caddr(sc->dump);
  sc->code = cadddr(sc->dump);
  sc->dump = cddddr(sc->dump);
  return sc->T;
}

static void s_save(scheme * sc, enum scheme_opcodes op, pointer args,
    pointer code) {
  sc->dump = cons(sc, sc->envir, cons(sc, (code), sc->dump));
  sc->dump = cons(sc, (args), sc->dump);
  sc->dump = cons(sc, mk_integer(sc, (long) (op)), sc->dump);
}

static INLINE void dump_stack_mark(scheme * sc) {
  mark(sc->dump);
}
#endif

#define s_retbool(tf)    s_return(sc,(tf) ? sc->T : sc->F)

static pointer opexe_0(scheme * sc, enum scheme_opcodes op) {
  pointer x, y;

  switch (op) {
  case OP_LOAD:                /* load */
    if (file_interactive(sc)) {
      fprintf(sc->outport->_object._port->rep.stdio.file,
          "Loading %s\n", strvalue(car(sc->args)));
    }
    if (!file_push(sc, strvalue(car(sc->args)))) {
      Error_1(sc, "unable to open", car(sc->args));
    } else {
      sc->args = mk_integer(sc, sc->file_i);
      s_goto(sc, OP_T0LVL);
    }

  case OP_T0LVL:               /* top level */
    /* If we reached the end of file, this loop is done. */
    if (sc->loadport->_object._port->kind & port_saw_EOF) {
      if (sc->file_i == 0) {
        sc->args = sc->NIL;
        s_goto(sc, OP_QUIT);
      } else {
        file_pop(sc);
        s_return(sc, sc->value);
      }
      /* NOTREACHED */
    }

    /* If interactive, be nice to user. */
    if (file_interactive(sc)) {
      sc->envir = sc->global_env;
      dump_stack_reset(sc);
      putstr(sc, "\n");
      putstr(sc, prompt);
    }

    /* Set up another iteration of REPL */
    sc->nesting = 0;
    sc->save_inport = sc->inport;
    sc->inport = sc->loadport;
    s_save(sc, OP_T0LVL, sc->NIL, sc->NIL);
    s_save(sc, OP_VALUEPRINT, sc->NIL, sc->NIL);
    s_save(sc, OP_T1LVL, sc->NIL, sc->NIL);
    s_goto(sc, OP_READ_INTERNAL);

  case OP_T1LVL:               /* top level */
    sc->code = sc->value;
    sc->inport = sc->save_inport;
    s_goto(sc, OP_EVAL);

  case OP_READ_INTERNAL:       /* internal read */
    sc->tok = token(sc);
    if (sc->tok == TOK_EOF) {
      s_return(sc, sc->EOF_OBJ);
    }
    s_goto(sc, OP_RDSEXPR);

  case OP_GENSYM:
    s_return(sc, gensym(sc));

  case OP_VALUEPRINT:          /* print evaluation result */
    /* OP_VALUEPRINT is always pushed, because when changing from
       non-interactive to interactive mode, it needs to be
       already on the stack */
    if (sc->tracing) {
      putstr(sc, "\nGives: ");
    }
    if (file_interactive(sc)) {
      sc->print_flag = 1;
      sc->args = sc->value;
      s_goto(sc, OP_P0LIST);
    } else {
      s_return(sc, sc->value);
    }

  case OP_EVAL:                /* main part of evaluation */
    evalcnt += 1;
#ifdef EVAL_LIMIT
    if (evalcnt >= eval_limit) {
        fprintf(stderr, "Eval steps limit reached: %ld\n", evalcnt);
        exit(7);
    }
#endif
#if USE_TRACING
    if (sc->tracing) {
      /*s_save(sc,OP_VALUEPRINT,sc->NIL,sc->NIL); */
      s_save(sc, OP_REAL_EVAL, sc->args, sc->code);
      sc->args = sc->code;
      putstr(sc, "\nEval: ");
      s_goto(sc, OP_P0LIST);
    }
    /* fall through */
  case OP_REAL_EVAL:
#endif
    if (is_symbol(sc->code)) {  /* symbol */
      x = find_slot_in_env(sc, sc->envir, sc->code, 1);
      if (x != sc->NIL) {
        s_return(sc, slot_value_in_env(x));
      } else {
        Error_1(sc, "eval: unbound variable:", sc->code);
      }
    } else if (is_pair(sc->code)) {
      if (is_syntax(x = car(sc->code))) {       /* SYNTAX */
        sc->code = cdr(sc->code);
        s_goto(sc, syntaxnum(x));
      } else {                  /* first, eval top element and eval arguments */
        s_save(sc, OP_E0ARGS, sc->NIL, sc->code);
        /* If no macros => s_save(sc,OP_E1ARGS, sc->NIL, cdr(sc->code)); */
        sc->code = car(sc->code);
        s_goto(sc, OP_EVAL);
      }
    } else {
      s_return(sc, sc->code);
    }

  case OP_E0ARGS:              /* eval arguments */
    if (is_macro(sc->value)) {  /* macro expansion */
      s_save(sc, OP_DOMACRO, sc->NIL, sc->NIL);
      sc->args = cons(sc, sc->code, sc->NIL);
      sc->code = sc->value;
      s_goto(sc, OP_APPLY);
    } else {
      sc->code = cdr(sc->code);
      s_goto(sc, OP_E1ARGS);
    }

  case OP_E1ARGS:              /* eval arguments */
    sc->args = cons(sc, sc->value, sc->args);
    if (is_pair(sc->code)) {    /* continue */
      s_save(sc, OP_E1ARGS, sc->args, cdr(sc->code));
      sc->code = car(sc->code);
      sc->args = sc->NIL;
      s_goto(sc, OP_EVAL);
    } else {                    /* end */
      sc->args = reverse_in_place(sc, sc->NIL, sc->args);
      sc->code = car(sc->args);
      sc->args = cdr(sc->args);
      s_goto(sc, OP_APPLY);
    }

#if USE_TRACING
  case OP_TRACING:{
      int tr = sc->tracing;
      sc->tracing = ivalue(car(sc->args));
      s_return(sc, mk_integer(sc, tr));
    }
#endif

  case OP_APPLY:               /* apply 'code' to 'args' */
#if USE_TRACING
    if (sc->tracing) {
      s_save(sc, OP_REAL_APPLY, sc->args, sc->code);
      sc->print_flag = 1;
      /*  sc->args=cons(sc,sc->code,sc->args); */
      putstr(sc, "\nApply to: ");
      s_goto(sc, OP_P0LIST);
    }
    /* fall through */
  case OP_REAL_APPLY:
#endif
    if (is_proc(sc->code)) {
      s_goto(sc, procnum(sc->code));    /* PROCEDURE */
    } else if (is_foreign(sc->code)) {
      /* Keep nested calls from GC'ing the arglist */
      push_recent_alloc(sc, sc->args, sc->NIL);
      x = sc->code->_object._ff(sc, sc->args);
      s_return(sc, x);
    } else if (is_closure(sc->code) || is_macro(sc->code)
        || is_promise(sc->code)) {      /* CLOSURE */
      /* Should not accept promise */
      /* make environment */
      new_frame_in_env(sc, closure_env(sc->code));
      for (x = car(closure_code(sc->code)), y = sc->args;
          is_pair(x); x = cdr(x), y = cdr(y)) {
        if (y == sc->NIL) {
          Error_0(sc, "not enough arguments");
        } else {
          new_slot_in_env(sc, car(x), car(y));
        }
      }
      if (x == sc->NIL) {
                    /*--
                     * if (y != sc->NIL) {
                     *   Error_0(sc,"too many arguments");
                     * }
                     */
      } else if (is_symbol(x))
        new_slot_in_env(sc, x, y);
      else {
        Error_1(sc, "syntax error in closure: not a symbol:", x);
      }
      sc->code = cdr(closure_code(sc->code));
      sc->args = sc->NIL;
      s_goto(sc, OP_BEGIN);
    } else if (is_continuation(sc->code)) {     /* CONTINUATION */
      sc->dump = cont_dump(sc->code);
      s_return(sc, sc->args != sc->NIL ? car(sc->args) : sc->NIL);
    } else {
      Error_0(sc, "illegal function");
    }

  case OP_DOMACRO:             /* do macro */
    sc->code = sc->value;
    s_goto(sc, OP_EVAL);

#if 1
  case OP_LAMBDA:              /* lambda */
    /* If the hook is defined, apply it to sc->code, otherwise
       set sc->value fall thru */
    {
      pointer f = find_slot_in_env(sc, sc->envir, sc->COMPILE_HOOK, 1);
      if (f == sc->NIL) {
        sc->value = sc->code;
        /* Fallthru */
      } else {
        s_save(sc, OP_LAMBDA1, sc->args, sc->code);
        sc->args = cons(sc, sc->code, sc->NIL);
        sc->code = slot_value_in_env(f);
        s_goto(sc, OP_APPLY);
      }
    }

  case OP_LAMBDA1:
    s_return(sc, mk_closure(sc, sc->value, sc->envir));

#else
  case OP_LAMBDA:              /* lambda */
    s_return(sc, mk_closure(sc, sc->code, sc->envir));

#endif

  case OP_MKCLOSURE:           /* make-closure */
    x = car(sc->args);
    if (car(x) == sc->LAMBDA) {
      x = cdr(x);
    }
    if (cdr(sc->args) == sc->NIL) {
      y = sc->envir;
    } else {
      y = cadr(sc->args);
    }
    s_return(sc, mk_closure(sc, x, y));

  case OP_QUOTE:               /* quote */
    s_return(sc, car(sc->code));

  case OP_DEF0:                /* define */
    if (is_immutable(car(sc->code)))
      Error_1(sc, "define: unable to alter immutable", car(sc->code));

    if (is_pair(car(sc->code))) {
      x = caar(sc->code);
      sc->code =
          cons(sc, sc->LAMBDA, cons(sc, cdar(sc->code), cdr(sc->code)));
    } else {
      x = car(sc->code);
      sc->code = cadr(sc->code);
    }
    if (!is_symbol(x)) {
      Error_0(sc, "variable is not a symbol");
    }
    s_save(sc, OP_DEF1, sc->NIL, x);
    s_goto(sc, OP_EVAL);

  case OP_DEF1:                /* define */
    x = find_slot_in_env(sc, sc->envir, sc->code, 0);
    if (x != sc->NIL) {
      set_slot_in_env(x, sc->value);
    } else {
      new_slot_in_env(sc, sc->code, sc->value);
    }
    s_return(sc, sc->code);


  case OP_DEFP:                /* defined? */
    x = sc->envir;
    if (cdr(sc->args) != sc->NIL) {
      x = cadr(sc->args);
    }
    s_retbool(find_slot_in_env(sc, x, car(sc->args), 1) != sc->NIL);

  case OP_SET0:                /* set! */
    if (is_immutable(car(sc->code)))
      Error_1(sc, "set!: unable to alter immutable variable", car(sc->code));
    s_save(sc, OP_SET1, sc->NIL, car(sc->code));
    sc->code = cadr(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_SET1:                /* set! */
    y = find_slot_in_env(sc, sc->envir, sc->code, 1);
    if (y != sc->NIL) {
      set_slot_in_env(y, sc->value);
      s_return(sc, sc->value);
    } else {
      Error_1(sc, "set!: unbound variable:", sc->code);
    }


  case OP_BEGIN:               /* begin */
    if (!is_pair(sc->code)) {
      s_return(sc, sc->code);
    }
    if (cdr(sc->code) != sc->NIL) {
      s_save(sc, OP_BEGIN, sc->NIL, cdr(sc->code));
    }
    sc->code = car(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_IF0:                 /* if */
    s_save(sc, OP_IF1, sc->NIL, cdr(sc->code));
    sc->code = car(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_IF1:                 /* if */
    if (is_true(sc->value))
      sc->code = car(sc->code);
    else
      sc->code = cadr(sc->code);        /* (if #f 1) ==> () because
                                         * car(sc->NIL) = sc->NIL */
    s_goto(sc, OP_EVAL);

  case OP_LET0:                /* let */
    sc->args = sc->NIL;
    sc->value = sc->code;
    sc->code = is_symbol(car(sc->code)) ? cadr(sc->code) : car(sc->code);
    s_goto(sc, OP_LET1);

  case OP_LET1:                /* let (calculate parameters) */
    sc->args = cons(sc, sc->value, sc->args);
    if (is_pair(sc->code)) {    /* continue */
      if (!is_pair(car(sc->code)) || !is_pair(cdar(sc->code))) {
        Error_1(sc, "Bad syntax of binding spec in let :", car(sc->code));
      }
      s_save(sc, OP_LET1, sc->args, cdr(sc->code));
      sc->code = cadar(sc->code);
      sc->args = sc->NIL;
      s_goto(sc, OP_EVAL);
    } else {                    /* end */
      sc->args = reverse_in_place(sc, sc->NIL, sc->args);
      sc->code = car(sc->args);
      sc->args = cdr(sc->args);
      s_goto(sc, OP_LET2);
    }

  case OP_LET2:                /* let */
    new_frame_in_env(sc, sc->envir);
    for (x =
        is_symbol(car(sc->code)) ? cadr(sc->code) : car(sc->code), y =
        sc->args; y != sc->NIL; x = cdr(x), y = cdr(y)) {
      new_slot_in_env(sc, caar(x), car(y));
    }
    if (is_symbol(car(sc->code))) {     /* named let */
      for (x = cadr(sc->code), sc->args = sc->NIL; x != sc->NIL; x = cdr(x)) {
        if (!is_pair(x))
          Error_1(sc, "Bad syntax of binding in let :", x);
        if (!is_list(sc, car(x)))
          Error_1(sc, "Bad syntax of binding in let :", car(x));
        sc->args = cons(sc, caar(x), sc->args);
      }
      x = mk_closure(sc, cons(sc, reverse_in_place(sc, sc->NIL, sc->args),
              cddr(sc->code)), sc->envir);
      new_slot_in_env(sc, car(sc->code), x);
      sc->code = cddr(sc->code);
      sc->args = sc->NIL;
    } else {
      sc->code = cdr(sc->code);
      sc->args = sc->NIL;
    }
    s_goto(sc, OP_BEGIN);

  case OP_LET0AST:             /* let* */
    if (car(sc->code) == sc->NIL) {
      new_frame_in_env(sc, sc->envir);
      sc->code = cdr(sc->code);
      s_goto(sc, OP_BEGIN);
    }
    if (!is_pair(car(sc->code)) || !is_pair(caar(sc->code))
        || !is_pair(cdaar(sc->code))) {
      Error_1(sc, "Bad syntax of binding spec in let* :", car(sc->code));
    }
    s_save(sc, OP_LET1AST, cdr(sc->code), car(sc->code));
    sc->code = cadaar(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_LET1AST:             /* let* (make new frame) */
    new_frame_in_env(sc, sc->envir);
    s_goto(sc, OP_LET2AST);

  case OP_LET2AST:             /* let* (calculate parameters) */
    new_slot_in_env(sc, caar(sc->code), sc->value);
    sc->code = cdr(sc->code);
    if (is_pair(sc->code)) {    /* continue */
      s_save(sc, OP_LET2AST, sc->args, sc->code);
      sc->code = cadar(sc->code);
      sc->args = sc->NIL;
      s_goto(sc, OP_EVAL);
    } else {                    /* end */
      sc->code = sc->args;
      sc->args = sc->NIL;
      s_goto(sc, OP_BEGIN);
    }
  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);
  }
  return sc->T;
}

static pointer opexe_1(scheme * sc, enum scheme_opcodes op) {
  pointer x, y;

  switch (op) {
  case OP_LET0REC:             /* letrec */
    new_frame_in_env(sc, sc->envir);
    sc->args = sc->NIL;
    sc->value = sc->code;
    sc->code = car(sc->code);
    s_goto(sc, OP_LET1REC);

  case OP_LET1REC:             /* letrec (calculate parameters) */
    sc->args = cons(sc, sc->value, sc->args);
    if (is_pair(sc->code)) {    /* continue */
      if (!is_pair(car(sc->code)) || !is_pair(cdar(sc->code))) {
        Error_1(sc, "Bad syntax of binding spec in letrec :", car(sc->code));
      }
      s_save(sc, OP_LET1REC, sc->args, cdr(sc->code));
      sc->code = cadar(sc->code);
      sc->args = sc->NIL;
      s_goto(sc, OP_EVAL);
    } else {                    /* end */
      sc->args = reverse_in_place(sc, sc->NIL, sc->args);
      sc->code = car(sc->args);
      sc->args = cdr(sc->args);
      s_goto(sc, OP_LET2REC);
    }

  case OP_LET2REC:             /* letrec */
    for (x = car(sc->code), y = sc->args; y != sc->NIL;
        x = cdr(x), y = cdr(y)) {
      new_slot_in_env(sc, caar(x), car(y));
    }
    sc->code = cdr(sc->code);
    sc->args = sc->NIL;
    s_goto(sc, OP_BEGIN);

  case OP_COND0:               /* cond */
    if (!is_pair(sc->code)) {
      Error_0(sc, "syntax error in cond");
    }
    s_save(sc, OP_COND1, sc->NIL, sc->code);
    sc->code = caar(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_COND1:               /* cond */
    if (is_true(sc->value)) {
      if ((sc->code = cdar(sc->code)) == sc->NIL) {
        s_return(sc, sc->value);
      }
      if (!sc->code || car(sc->code) == sc->FEED_TO) {
        if (!is_pair(cdr(sc->code))) {
          Error_0(sc, "syntax error in cond");
        }
        x = cons(sc, sc->QUOTE, cons(sc, sc->value, sc->NIL));
        sc->code = cons(sc, cadr(sc->code), cons(sc, x, sc->NIL));
        s_goto(sc, OP_EVAL);
      }
      s_goto(sc, OP_BEGIN);
    } else {
      if ((sc->code = cdr(sc->code)) == sc->NIL) {
        s_return(sc, sc->NIL);
      } else {
        s_save(sc, OP_COND1, sc->NIL, sc->code);
        sc->code = caar(sc->code);
        s_goto(sc, OP_EVAL);
      }
    }

  case OP_DELAY:               /* delay */
    x = mk_closure(sc, cons(sc, sc->NIL, sc->code), sc->envir);
    typeflag(x) = T_PROMISE;
    s_return(sc, x);

  case OP_AND0:                /* and */
    if (sc->code == sc->NIL) {
      s_return(sc, sc->T);
    }
    s_save(sc, OP_AND1, sc->NIL, cdr(sc->code));
    sc->code = car(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_AND1:                /* and */
    if (is_false(sc->value)) {
      s_return(sc, sc->value);
    } else if (sc->code == sc->NIL) {
      s_return(sc, sc->value);
    } else {
      s_save(sc, OP_AND1, sc->NIL, cdr(sc->code));
      sc->code = car(sc->code);
      s_goto(sc, OP_EVAL);
    }

  case OP_OR0:                 /* or */
    if (sc->code == sc->NIL) {
      s_return(sc, sc->F);
    }
    s_save(sc, OP_OR1, sc->NIL, cdr(sc->code));
    sc->code = car(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_OR1:                 /* or */
    if (is_true(sc->value)) {
      s_return(sc, sc->value);
    } else if (sc->code == sc->NIL) {
      s_return(sc, sc->value);
    } else {
      s_save(sc, OP_OR1, sc->NIL, cdr(sc->code));
      sc->code = car(sc->code);
      s_goto(sc, OP_EVAL);
    }

  case OP_C0STREAM:            /* cons-stream */
    s_save(sc, OP_C1STREAM, sc->NIL, cdr(sc->code));
    sc->code = car(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_C1STREAM:            /* cons-stream */
    sc->args = sc->value;       /* save sc->value to register sc->args for gc */
    x = mk_closure(sc, cons(sc, sc->NIL, sc->code), sc->envir);
    typeflag(x) = T_PROMISE;
    s_return(sc, cons(sc, sc->args, x));

  case OP_MACRO0:              /* macro */
    if (is_pair(car(sc->code))) {
      x = caar(sc->code);
      sc->code =
          cons(sc, sc->LAMBDA, cons(sc, cdar(sc->code), cdr(sc->code)));
    } else {
      x = car(sc->code);
      sc->code = cadr(sc->code);
    }
    if (!is_symbol(x)) {
      Error_0(sc, "variable is not a symbol");
    }
    s_save(sc, OP_MACRO1, sc->NIL, x);
    s_goto(sc, OP_EVAL);

  case OP_MACRO1:              /* macro */
    typeflag(sc->value) = T_MACRO;
    x = find_slot_in_env(sc, sc->envir, sc->code, 0);
    if (x != sc->NIL) {
      set_slot_in_env(x, sc->value);
    } else {
      new_slot_in_env(sc, sc->code, sc->value);
    }
    s_return(sc, sc->code);

  case OP_CASE0:               /* case */
    s_save(sc, OP_CASE1, sc->NIL, cdr(sc->code));
    sc->code = car(sc->code);
    s_goto(sc, OP_EVAL);

  case OP_CASE1:               /* case */
    for (x = sc->code; x != sc->NIL; x = cdr(x)) {
      if (!is_pair(y = caar(x))) {
        break;
      }
      for (; y != sc->NIL; y = cdr(y)) {
        if (eqv(car(y), sc->value)) {
          break;
        }
      }
      if (y != sc->NIL) {
        break;
      }
    }
    if (x != sc->NIL) {
      if (is_pair(caar(x))) {
        sc->code = cdar(x);
        s_goto(sc, OP_BEGIN);
      } else {                  /* else */
        s_save(sc, OP_CASE2, sc->NIL, cdar(x));
        sc->code = caar(x);
        s_goto(sc, OP_EVAL);
      }
    } else {
      s_return(sc, sc->NIL);
    }

  case OP_CASE2:               /* case */
    if (is_true(sc->value)) {
      s_goto(sc, OP_BEGIN);
    } else {
      s_return(sc, sc->NIL);
    }

  case OP_PAPPLY:              /* apply */
    sc->code = car(sc->args);
    sc->args = list_star(sc, cdr(sc->args));
    /*sc->args = cadr(sc->args); */
    s_goto(sc, OP_APPLY);

  case OP_PEVAL:               /* eval */
    if (cdr(sc->args) != sc->NIL) {
      sc->envir = cadr(sc->args);
    }
    sc->code = car(sc->args);
    s_goto(sc, OP_EVAL);

  case OP_CONTINUATION:        /* call-with-current-continuation */
    sc->code = car(sc->args);
    sc->args = cons(sc, mk_continuation(sc, sc->dump), sc->NIL);
    s_goto(sc, OP_APPLY);

  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);
  }
  return sc->T;
}

static pointer opexe_2(scheme * sc, enum scheme_opcodes op) {
  pointer x, y;
  num v;
#if USE_MATH
  double dd;
#endif

  switch (op) {
#if USE_MATH
  case OP_INEX2EX:             /* exact */
    x = car(sc->args);
    if (num_is_integer(x)) {
      s_return(sc, x);
    } else if (modf(rvalue_unchecked(x), &dd) == 0.0) {
      s_return(sc, mk_integer(sc, ivalue(x)));
    } else {
      Error_1(sc, "argument not integral:", x);
    }

  case OP_EXP:
    x = car(sc->args);
    s_return(sc, mk_real(sc, exp(rvalue(x))));

  case OP_LOG:
    x = car(sc->args);
    s_return(sc, mk_real(sc, log(rvalue(x))));

  case OP_SIN:
    x = car(sc->args);
    s_return(sc, mk_real(sc, sin(rvalue(x))));

  case OP_COS:
    x = car(sc->args);
    s_return(sc, mk_real(sc, cos(rvalue(x))));

  case OP_TAN:
    x = car(sc->args);
    s_return(sc, mk_real(sc, tan(rvalue(x))));

  case OP_ASIN:
    x = car(sc->args);
    s_return(sc, mk_real(sc, asin(rvalue(x))));

  case OP_ACOS:
    x = car(sc->args);
    s_return(sc, mk_real(sc, acos(rvalue(x))));

  case OP_ATAN:
    x = car(sc->args);
    if (cdr(sc->args) == sc->NIL) {
      s_return(sc, mk_real(sc, atan(rvalue(x))));
    } else {
      y = cadr(sc->args);
      s_return(sc, mk_real(sc, atan2(rvalue(x), rvalue(y))));
    }

  case OP_SQRT:
    x = car(sc->args);
    s_return(sc, mk_real(sc, sqrt(rvalue(x))));

  case OP_EXPT:{
      double result;
      int real_result = 1;
      x = car(sc->args);
      y = cadr(sc->args);
      if (num_is_integer(x) && num_is_integer(y))
        real_result = 0;
      result = pow(rvalue(x), rvalue(y));
      /* Before returning integer result make sure we can. */
      /* If the test fails, result is too big for integer. */
      if (!real_result) {
        long result_as_long = (long) result;
        if (result != (double) result_as_long)
          real_result = 1;
      }
      if (real_result) {
        s_return(sc, mk_real(sc, result));
      } else {
        s_return(sc, mk_integer(sc, result));
      }
    }

  case OP_FLOOR:
    x = car(sc->args);
    s_return(sc, mk_real(sc, floor(rvalue(x))));

  case OP_CEILING:
    x = car(sc->args);
    s_return(sc, mk_real(sc, ceil(rvalue(x))));

  case OP_ROUND:
    x = car(sc->args);
    if (num_is_integer(x))
      s_return(sc, x);
    s_return(sc, mk_real(sc, round_per_R5RS(rvalue(x))));
#endif

  case OP_ADD:                 /* + */
    v = num_zero;
    for (x = sc->args; x != sc->NIL; x = cdr(x)) {
      v = num_add(v, nvalue(car(x)));
    }
    s_return(sc, mk_number(sc, v));

  case OP_MUL:                 /* * */
    v = num_one;
    for (x = sc->args; x != sc->NIL; x = cdr(x)) {
      v = num_mul(v, nvalue(car(x)));
    }
    s_return(sc, mk_number(sc, v));

  case OP_SUB:                 /* - */
    if (cdr(sc->args) == sc->NIL) {
      x = sc->args;
      v = num_zero;
    } else {
      x = cdr(sc->args);
      v = nvalue(car(sc->args));
    }
    for (; x != sc->NIL; x = cdr(x)) {
      v = num_sub(v, nvalue(car(x)));
    }
    s_return(sc, mk_number(sc, v));

  case OP_DIV:                 /* / */
    if (cdr(sc->args) == sc->NIL) {
      x = sc->args;
      v = num_one;
    } else {
      x = cdr(sc->args);
      v = nvalue(car(sc->args));
    }
    for (; x != sc->NIL; x = cdr(x)) {
      v = num_div(v, nvalue(car(x)));
    }
    s_return(sc, mk_number(sc, v));

  case OP_REM:                 /* remainder */
    v = nvalue(car(sc->args));
    x = cadr(sc->args);
    if (ivalue(x) != 0)
      v = num_rem(v, nvalue(x));
    else {
      Error_0(sc, "remainder: division by zero");
    }
    s_return(sc, mk_number(sc, v));

  case OP_MOD:                 /* modulo */
    v = nvalue(car(sc->args));
    x = cadr(sc->args);
    if (ivalue(x) != 0)
      v = num_mod(v, nvalue(x));
    else {
      Error_0(sc, "modulo: division by zero");
    }
    s_return(sc, mk_number(sc, v));

  case OP_CAR:                 /* car */
    s_return(sc, caar(sc->args));

  case OP_CDR:                 /* cdr */
    s_return(sc, cdar(sc->args));

  case OP_CONS:                /* cons */
    cdr(sc->args) = cadr(sc->args);
    s_return(sc, sc->args);

  case OP_SETCAR:              /* set-car! */
    if (!is_immutable(car(sc->args))) {
      caar(sc->args) = cadr(sc->args);
      s_return(sc, car(sc->args));
    } else {
      Error_0(sc, "set-car!: unable to alter immutable pair");
    }

  case OP_SETCDR:              /* set-cdr! */
    if (!is_immutable(car(sc->args))) {
      cdar(sc->args) = cadr(sc->args);
      s_return(sc, car(sc->args));
    } else {
      Error_0(sc, "set-cdr!: unable to alter immutable pair");
    }

  case OP_CHAR2INT:{           /* char->integer */
      char c;
      c = (char) ivalue(car(sc->args));
      s_return(sc, mk_integer(sc, (unsigned char) c));
    }

  case OP_INT2CHAR:{           /* integer->char */
      unsigned char c;
      c = (unsigned char) ivalue(car(sc->args));
      s_return(sc, mk_character(sc, (char) c));
    }

  case OP_CHARUPCASE:{
      unsigned char c;
      c = (unsigned char) ivalue(car(sc->args));
      c = toupper(c);
      s_return(sc, mk_character(sc, (char) c));
    }

  case OP_CHARDNCASE:{
      unsigned char c;
      c = (unsigned char) ivalue(car(sc->args));
      c = tolower(c);
      s_return(sc, mk_character(sc, (char) c));
    }

  case OP_STR2SYM:             /* string->symbol */
    s_return(sc, mk_symbol(sc, strvalue(car(sc->args))));

  case OP_STR2ATOM:            /* string->atom */  {
      char *s = strvalue(car(sc->args));
      long pf = 0;
      if (cdr(sc->args) != sc->NIL) {
        /* we know cadr(sc->args) is a natural number */
        /* see if it is 2, 8, 10, or 16, or error */
        pf = ivalue_unchecked(cadr(sc->args));
        if (pf < 2 || pf > 36) {
          pf = -1;
        }
      }
      if (pf < 0) {
        Error_1(sc, "string->atom: bad base:", cadr(sc->args));
      } else if (*s == '#') {   /* no use of base! */
        s_return(sc, mk_sharp_const(sc, s + 1));
      } else {
        if (pf == 0 || pf == 10) {
          s_return(sc, mk_atom(sc, s));
        } else {
          char *ep;
          long iv = strtol(s, &ep, (int) pf);
          if (*ep == 0) {
            s_return(sc, mk_integer(sc, iv));
          } else {
            s_return(sc, sc->F);
          }
        }
      }
    }

  case OP_SYM2STR:             /* symbol->string */
    x = mk_string(sc, symname(car(sc->args)));
    setimmutable(x);
    s_return(sc, x);

  case OP_ATOM2STR:            /* atom->string */  {
      long pf = 0;
      x = car(sc->args);
      y = cdr(sc->args);
      if (y != sc->NIL) {
        /* we know cadr(sc->args) is a natural number */
        /* see if it is 2, 8, 10, or 16, or error */
        y = car(y);
        pf = ivalue_unchecked(y);
        if (!is_number(x) || pf < 2 || pf > 36) {
          pf = -1;
        }
      }
      if (pf < 0) {
        Error_1(sc, "atom->string: bad base:", y);
      } else if (is_number(x) || is_character(x) || is_string(x)
          || is_symbol(x)) {
        char *p;
        int len;
        atom2str(sc, x, (int) pf, &p, &len);
        s_return(sc, mk_counted_string(sc, p, len));
      } else {
        Error_1(sc, "atom->string: not an atom:", x);
      }
    }

  case OP_MKSTRING:{           /* make-string */
      int fill = ' ';
      int len, i;
      char* s;
      pointer p;

      len = ivalue(car(sc->args));

      if (cdr(sc->args) != sc->NIL) {
        fill = charvalue(cadr(sc->args));
      }
      p = mk_counted_string(sc, "", len);
      s = strvalue(p);
      if (IS_ASCII(fill)) {
        memset(s, (char) fill, len);
        s[len] = 0;
      } else {
        upgrade_string(sc, p);
        s = strvalue(p);
        ((int*) s)[0] = UTFSTR_LEN_SET(len);
        for (i = 1; i <= len; i++) {
          ((int*) s)[i] = fill;
        }
      }
      s_return(sc, p);
    }

  case OP_STRLEN:              /* string-length */
    s_return(sc, mk_integer(sc, strlength(car(sc->args))));

  case OP_STRREF:{             /* string-ref */
      char *str;
      int index;

      str = strvalue(car(sc->args));

      x = cadr(sc->args);
      if (!is_integer(x)) {
        Error_1(sc, "string-ref: index must be exact:", x);
      }

      index = ivalue(x);
      if (index >= strlength(car(sc->args))) {
        Error_1(sc, "string-ref: out of bounds:", x);
      }

      s_return(sc, mk_character(sc,
        IS_ASCII(*str)
          ? ((unsigned char *) str)[index]
          : ((int*) str)[index + 1]));
    }

  case OP_STRSET:{             /* string-set! */
      char *str;
      int index;
      int c;

      x = car(sc->args);
      if (is_immutable(x)) {
        Error_1(sc, "string-set!: unable to alter immutable string:", x);
      }
      str = strvalue(x);

      y = cadr(sc->args);
      if (!is_integer(y)) {
        Error_1(sc, "string-set!: index must be exact:", y);
      }

      index = ivalue(y);
      if (index >= strlength(x)) {
        Error_1(sc, "string-set!: out of bounds:", y);
      }

      c = charvalue(caddr(sc->args));

      if (IS_ASCII(*str)) {
        if (IS_ASCII(c)) {
          str[index] = (char) c;
          s_return(sc, x);
        }
        upgrade_string(sc, x);
        str = strvalue(x);
      }
      ((int*) str)[index + 1] = c;
      s_return(sc, x);
    }

  case OP_STRAPPEND:{ /* string-append in core for speed*/
      int len = 0, isbig = 0    , i;
      pointer newstr;
      char *pos, *s;

      /* compute needed length for new string */
      for (x = sc->args; x != sc->NIL; x = cdr(x)) {
        len += strlength(car(x));
        if (!IS_ASCII(*strvalue(car(x)))) {
            isbig = 1;
        }
      }
      newstr = mk_counted_string(sc, "", len);
      if (isbig) {
        upgrade_string(sc, newstr);
      }
      /* store the contents of the argument strings into the new string */
      for (pos = strvalue(newstr) + (isbig ? 4 : 0), x = sc->args; x != sc->NIL;
          pos += strlength(car(x)) * (isbig ? 4 : 1), x = cdr(x)) {
        len = strlength(car(x));
        s = strvalue(car(x));
        if (isbig) {
          if (IS_ASCII(*s)) {
            for (i = 0; i < len; i++) {
              ((int*) pos)[i] = s[i];
            }
          } else {
            memcpy(pos, s + 4, len * 4);
          }
        } else {
          memcpy(pos, s, len);
          pos[len] = 0;
        }
      }
      s_return(sc, newstr);
    }

  case OP_SUBSTR:{             /* substring */
      char *str;
      int index0;
      int index1;
      int len;

      str = strvalue(car(sc->args));

      index0 = ivalue(cadr(sc->args));

      if (index0 > strlength(car(sc->args))) {
        Error_1(sc, "substring: start out of bounds:", cadr(sc->args));
      }

      if (cddr(sc->args) != sc->NIL) {
        index1 = ivalue(caddr(sc->args));
        if (index1 > strlength(car(sc->args)) || index1 < index0) {
          Error_1(sc, "substring: end out of bounds:", caddr(sc->args));
        }
      } else {
        index1 = strlength(car(sc->args));
      }

      len = index1 - index0;
      x = mk_counted_string(sc, "", len);

      if (IS_ASCII(*str)) {
        memcpy(strvalue(x), str + index0, len);
      } else { //todo - perhaps downgrade if al ascii in substr
        upgrade_string(sc, x);
        memcpy(strvalue(x) + 4, str + (index0 + 1) * 4, len * 4);
      }
      strvalue(x)[len] = 0;

      s_return(sc, x);
    }

  case OP_VECTOR:{             /* vector */
      int i;
      pointer vec;
      int len = list_length(sc, sc->args);
      if (len < 0) {
        Error_1(sc, "vector: not a proper list:", sc->args);
      }
      vec = mk_vector(sc, len);
      if (sc->no_memory) {
        s_return(sc, sc->sink);
      }
      for (x = sc->args, i = 0; is_pair(x); x = cdr(x), i++) {
        set_vector_elem(vec, i, car(x));
      }
      s_return(sc, vec);
    }

  case OP_MKVECTOR:{           /* make-vector */
      pointer fill = sc->NIL;
      int len;
      pointer vec;

      len = ivalue(car(sc->args));

      if (cdr(sc->args) != sc->NIL) {
        fill = cadr(sc->args);
      }
      vec = mk_vector(sc, len);
      if (sc->no_memory) {
        s_return(sc, sc->sink);
      }
      if (fill != sc->NIL) {
        fill_vector(vec, fill);
      }
      s_return(sc, vec);
    }

  case OP_VECLEN:              /* vector-length */
    s_return(sc, mk_integer(sc, ivalue(car(sc->args))));

  case OP_VECREF:{             /* vector-ref */
      int index;

      x = cadr(sc->args);
      if (!is_integer(x)) {
        Error_1(sc, "vector-ref: index must be exact:", x);
      }
      index = ivalue(x);

      if (index >= ivalue(car(sc->args))) {
        Error_1(sc, "vector-ref: out of bounds:", x);
      }

      s_return(sc, vector_elem(car(sc->args), index));
    }

  case OP_VECSET:{             /* vector-set! */
      int index;

      if (is_immutable(car(sc->args))) {
        Error_1(sc, "vector-set!: unable to alter immutable vector:",
            car(sc->args));
      }

      x = cadr(sc->args);
      if (!is_integer(x)) {
        Error_1(sc, "vector-set!: index must be exact:", x);
      }

      index = ivalue(x);
      if (index >= ivalue(car(sc->args))) {
        Error_1(sc, "vector-set!: out of bounds:", x);
      }

      set_vector_elem(car(sc->args), index, caddr(sc->args));
      s_return(sc, car(sc->args));
    }

  case OP_MKBVECTOR:{           /* make-bytevector */
      int fill = 0;
      int len;
      pointer vec;

      len = ivalue(car(sc->args));

      if (cdr(sc->args) != sc->NIL) {
        fill = ivalue(cadr(sc->args));
      }
      vec = mk_bvector(sc, len, fill);
      if (sc->no_memory) {
        s_return(sc, sc->sink);
      }
      s_return(sc, vec);
    }

  case OP_BVECREF:{             /* bytevector-u8-ref */
      int index;

      x = cadr(sc->args);
      if (!is_integer(x)) {
        Error_1(sc, "bytevector-u8-ref: index must be exact:", x);
      }
      index = ivalue(x);

      if (index >= ivalue(car(sc->args))) {
        Error_1(sc, "bytevector-u8-ref: out of bounds:", x);
      }

      s_return(sc, mk_integer(sc, ((unsigned char *) strvalue(car(sc->args)))[index]));
    }

  case OP_BVECSET:{             /* bytevector-u8-set! */
      int index;

      x = car(sc->args);
      if (is_immutable(x)) {
        Error_1(sc, "bytevector-u8-set!: unable to alter immutable data:", x);
      }

      y = cadr(sc->args);
      if (!is_integer(y)) {
        Error_1(sc, "bytevector-u8-set!: index must be exact:", y);
      }

      index = ivalue(y);
      if (index >= strlength(x)) {
        Error_1(sc, "bytevector-u8-set!: out of bounds:", y);
      }

      strvalue(x)[index] = (unsigned char) (ivalue(caddr(sc->args)));
      s_return(sc, x);
    }

  case OP_BVECLEN:              /* bytevector-length */
    s_return(sc, mk_integer(sc, strlength(car(sc->args))));

  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);
  }
  return sc->T;
}

static int is_list(scheme * sc, pointer a) {
  return list_length(sc, a) >= 0;
}

/* Result is:
   proper list: length
   circular list: -1
   not even a pair: -2
   dotted list: -2 minus length before dot
*/
int list_length(scheme * sc, pointer a) {
  int i = 0;
  pointer slow, fast;

  slow = fast = a;
  while (1) {
    if (fast == sc->NIL)
      return i;
    if (!is_pair(fast))
      return -2 - i;
    fast = cdr(fast);
    ++i;
    if (fast == sc->NIL)
      return i;
    if (!is_pair(fast))
      return -2 - i;
    ++i;
    fast = cdr(fast);

    /* Safe because we would have already returned if `fast'
       encountered a non-pair. */
    slow = cdr(slow);
    if (fast == slow) {
      /* the fast pointer has looped back around and caught up
         with the slow pointer, hence the structure is circular,
         not of finite length, and therefore not a list */
      return -1;
    }
  }
}

static pointer opexe_3(scheme * sc, enum scheme_opcodes op) {
  pointer x;
  num v;
  int (*comp_func) (num, num) = 0;

  switch (op) {
  case OP_NOT:                 /* not */
    s_retbool(is_false(car(sc->args)));
  case OP_BOOLP:               /* boolean? */
    s_retbool(car(sc->args) == sc->F || car(sc->args) == sc->T);
  case OP_EOFOBJP:             /* boolean? */
    s_retbool(car(sc->args) == sc->EOF_OBJ);
  case OP_NULLP:               /* null? */
    s_retbool(car(sc->args) == sc->NIL);
  case OP_NUMEQ:               /* = */
  case OP_LESS:                /* < */
  case OP_GRE:                 /* > */
  case OP_LEQ:                 /* <= */
  case OP_GEQ:                 /* >= */
    switch (op) {
    case OP_NUMEQ:
      comp_func = num_eq;
      break;
    case OP_LESS:
      comp_func = num_lt;
      break;
    case OP_GRE:
      comp_func = num_gt;
      break;
    case OP_LEQ:
      comp_func = num_le;
      break;
    case OP_GEQ:
      comp_func = num_ge;
      break;
    default:
      break;                    /* Quiet the compiler */
    }
    x = sc->args;
    v = nvalue(car(x));
    x = cdr(x);

    for (; x != sc->NIL; x = cdr(x)) {
      if (!comp_func(v, nvalue(car(x)))) {
        s_retbool(0);
      }
      v = nvalue(car(x));
    }
    s_retbool(1);
  case OP_SYMBOLP:             /* symbol? */
    s_retbool(is_symbol(car(sc->args)));
  case OP_NUMBERP:             /* number? */
    s_retbool(is_number(car(sc->args)));
  case OP_STRINGP:             /* string? */
    s_retbool(is_string(car(sc->args)));
  case OP_INTEGERP:            /* integer? */
    s_retbool(is_integer(car(sc->args)));
  case OP_REALP:               /* real? */
    s_retbool(is_number(car(sc->args)));        /* All numbers are real */
  case OP_CHARP:               /* char? */
    s_retbool(is_character(car(sc->args)));
#if USE_CHAR_CLASSIFIERS
  case OP_CHARAP:              /* char-alphabetic? */
    s_retbool(Cisalpha(ivalue(car(sc->args))));
  case OP_CHARNP:              /* char-numeric? */
    s_retbool(Cisdigit(ivalue(car(sc->args))));
  case OP_CHARWP:              /* char-whitespace? */
    s_retbool(Cisspace(ivalue(car(sc->args))));
  case OP_CHARUP:              /* char-upper-case? */
    s_retbool(Cisupper(ivalue(car(sc->args))));
  case OP_CHARLP:              /* char-lower-case? */
    s_retbool(Cislower(ivalue(car(sc->args))));
#endif
  case OP_PORTP:               /* port? */
    s_retbool(is_port(car(sc->args)));
  case OP_INPORTP:             /* input-port? */
    s_retbool(is_inport(car(sc->args)));
  case OP_OUTPORTP:            /* output-port? */
    s_retbool(is_outport(car(sc->args)));
  case OP_PROCP:               /* procedure? */
          /*--
              * continuation should be procedure by the example
              * (call-with-current-continuation procedure?) ==> #t
                 * in R^3 report sec. 6.9
              */
    s_retbool(is_proc(car(sc->args)) || is_closure(car(sc->args))
        || is_continuation(car(sc->args)) || is_foreign(car(sc->args)));
  case OP_PAIRP:               /* pair? */
    s_retbool(is_pair(car(sc->args)));
  case OP_LISTP:               /* list? */
    s_retbool(list_length(sc, car(sc->args)) >= 0);

  case OP_ENVP:                /* environment? */
    s_retbool(is_environment(car(sc->args)));
  case OP_VECTORP:             /* vector? */
    s_retbool(is_vector(car(sc->args)));
  case OP_BVECTORP:             /* bytevector? */
    s_retbool(is_bvector(car(sc->args)));
  case OP_EQ:                  /* eq? */
    s_retbool(car(sc->args) == cadr(sc->args));
  case OP_EQV:                 /* eqv? */
    s_retbool(eqv(car(sc->args), cadr(sc->args)));
  case OP_CURR_SEC:            /* current-second */
    v.is_fixnum = 0;
    v.value.rvalue = time(0);
    s_return(sc, mk_number(sc, v));
  case OP_EVAL_CNT:            /* eval-count */
    v.is_fixnum = 1;
    v.value.ivalue = evalcnt;
    s_return(sc, mk_number(sc, v));
  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);
  }
  return sc->T;
}

static pointer opexe_4(scheme * sc, enum scheme_opcodes op) {
  pointer x, y;

  switch (op) {
  case OP_FORCE:               /* force */
    sc->code = car(sc->args);
    if (is_promise(sc->code)) {
      /* Should change type to closure here */
      s_save(sc, OP_SAVE_FORCED, sc->NIL, sc->code);
      sc->args = sc->NIL;
      s_goto(sc, OP_APPLY);
    } else {
      s_return(sc, sc->code);
    }

  case OP_SAVE_FORCED:         /* Save forced value replacing promise */
    memcpy(sc->code, sc->value, sizeof(struct cell));
    s_return(sc, sc->value);

  case OP_WRITE:               /* write */
  case OP_DISPLAY:             /* display */
  case OP_WRITE_CHAR:          /* write-char */
    if (is_pair(cdr(sc->args))) {
      if (cadr(sc->args) != sc->outport) {
        x = cons(sc, sc->outport, sc->NIL);
        s_save(sc, OP_SET_OUTPORT, x, sc->NIL);
        sc->outport = cadr(sc->args);
      }
    }
    sc->args = car(sc->args);
    if (op == OP_WRITE) {
      sc->print_flag = 1;
    } else {
      sc->print_flag = 0;
    }
    s_goto(sc, OP_P0LIST);

  case OP_WRITE_U8:            /* write-u8 */
    if (is_pair(cdr(sc->args))) {
      if (cadr(sc->args) != sc->outport) {
        x = cons(sc, sc->outport, sc->NIL);
        s_save(sc, OP_SET_OUTPORT, x, sc->NIL);
        sc->outport = cadr(sc->args);
      }
    }
    putcharacter(sc, ivalue(car(sc->args)));
    s_return(sc, sc->T);

  case OP_NEWLINE:             /* newline */
    if (is_pair(sc->args)) {
      if (car(sc->args) != sc->outport) {
        x = cons(sc, sc->outport, sc->NIL);
        s_save(sc, OP_SET_OUTPORT, x, sc->NIL);
        sc->outport = car(sc->args);
      }
    }
    putstr(sc, "\n");
    s_return(sc, sc->T);

  case OP_ERR0:                /* error */
    sc->retcode = -1;
    if (!is_string(car(sc->args))) {
      sc->args = cons(sc, mk_string(sc, " -- "), sc->args);
      setimmutable(car(sc->args));
    }
    putstr(sc, "Error: ");
    putstr(sc, strvalue(car(sc->args)));
    sc->args = cdr(sc->args);
    s_goto(sc, OP_ERR1);

  case OP_ERR1:                /* error */
    putstr(sc, " ");
    if (sc->args != sc->NIL) {
      s_save(sc, OP_ERR1, cdr(sc->args), sc->NIL);
      sc->args = car(sc->args);
      sc->print_flag = 1;
      s_goto(sc, OP_P0LIST);
    } else {
      putstr(sc, "\n");
      if (sc->interactive_repl) {
        s_goto(sc, OP_T0LVL);
      } else {
        return sc->NIL;
      }
    }

  case OP_REVERSE:             /* reverse */
    s_return(sc, reverse(sc, car(sc->args)));

  case OP_LIST_STAR:           /* list* */
    s_return(sc, list_star(sc, sc->args));

  case OP_APPEND:              /* append */
    x = sc->NIL;
    y = sc->args;
    if (y == x) {
      s_return(sc, x);
    }

    /* cdr() in the while condition is not a typo. If car() */
    /* is used (append '() 'a) will return the wrong result. */
    while (cdr(y) != sc->NIL) {
      x = revappend(sc, x, car(y));
      y = cdr(y);
      if (x == sc->F) {
        Error_0(sc, "non-list argument to append");
      }
    }

    s_return(sc, reverse_in_place(sc, car(y), x));

#if USE_PLIST
  case OP_PUT:                 /* put */
    if (!hasprop(car(sc->args)) || !hasprop(cadr(sc->args))) {
      Error_0(sc, "illegal use of put");
    }
    for (x = symprop(car(sc->args)), y = cadr(sc->args); x != sc->NIL;
        x = cdr(x)) {
      if (caar(x) == y) {
        break;
      }
    }
    if (x != sc->NIL)
      cdar(x) = caddr(sc->args);
    else
      symprop(car(sc->args)) = cons(sc, cons(sc, y, caddr(sc->args)),
          symprop(car(sc->args)));
    s_return(sc, sc->T);

  case OP_GET:                 /* get */
    if (!hasprop(car(sc->args)) || !hasprop(cadr(sc->args))) {
      Error_0(sc, "illegal use of get");
    }
    for (x = symprop(car(sc->args)), y = cadr(sc->args); x != sc->NIL;
        x = cdr(x)) {
      if (caar(x) == y) {
        break;
      }
    }
    if (x != sc->NIL) {
      s_return(sc, cdar(x));
    } else {
      s_return(sc, sc->NIL);
    }
#endif /* USE_PLIST */
  case OP_QUIT:                /* quit */
    if (is_pair(sc->args)) {
      sc->retcode = ivalue(car(sc->args));
    }
    return (sc->NIL);

  case OP_GC:                  /* gc */
    gc(sc, sc->NIL, sc->NIL);
    s_return(sc, sc->T);

  case OP_GCVERB:              /* gc-verbose */
    {
      int was = sc->gc_verbose;

      sc->gc_verbose = (car(sc->args) != sc->F);
      s_retbool(was);
    }

  case OP_NEWSEGMENT:          /* new-segment */
    if (!is_pair(sc->args) || !is_number(car(sc->args))) {
      Error_0(sc, "new-segment: argument must be a number");
    }
    alloc_cellseg(sc, (int) ivalue(car(sc->args)));
    s_return(sc, sc->T);

  case OP_OBLIST:              /* oblist */
    s_return(sc, oblist_all_symbols(sc));

  case OP_CURR_INPORT:         /* current-input-port */
    s_return(sc, sc->inport);

  case OP_CURR_OUTPORT:        /* current-output-port */
    s_return(sc, sc->outport);

  case OP_OPEN_INFILE:         /* open-input-file */
  case OP_OPEN_OUTFILE:        /* open-output-file */
  case OP_OPEN_INOUTFILE:      /* open-input-output-file */  {
      int prop = 0;
      pointer p;
      switch (op) {
      case OP_OPEN_INFILE:
        prop = port_input;
        break;
      case OP_OPEN_OUTFILE:
        prop = port_output;
        break;
      case OP_OPEN_INOUTFILE:
        prop = port_input | port_output;
        break;
      default:
        break;                  /* Quiet the compiler */
      }
      p = port_from_filename(sc, strvalue(car(sc->args)), prop);
      if (p == sc->NIL) {
        s_return(sc, sc->F);
      }
      s_return(sc, p);
    }

#if USE_STRING_PORTS
  case OP_OPEN_INSTRING:       /* open-input-string */
  case OP_OPEN_INOUTSTRING:    /* open-input-output-string */  {
      int prop = 0;
      pointer p;
      switch (op) {
      case OP_OPEN_INSTRING:
        prop = port_input;
        break;
      case OP_OPEN_INOUTSTRING:
        prop = port_input | port_output;
        break;
      default:
        break;                  /* Quiet the compiler */
      }
      p = port_from_string(sc, strvalue(car(sc->args)),
          strvalue(car(sc->args)) + strlength(car(sc->args)), prop);
      if (p == sc->NIL) {
        s_return(sc, sc->F);
      }
      s_return(sc, p);
    }
  case OP_OPEN_OUTSTRING:      /* open-output-string */  {
      pointer p;
      if (car(sc->args) == sc->NIL) {
        p = port_from_scratch(sc);
        if (p == sc->NIL) {
          s_return(sc, sc->F);
        }
      } else {
        p = port_from_string(sc, strvalue(car(sc->args)),
            strvalue(car(sc->args)) + strlength(car(sc->args)), port_output);
        if (p == sc->NIL) {
          s_return(sc, sc->F);
        }
      }
      s_return(sc, p);
    }
  case OP_GET_OUTSTRING:       /* get-output-string */  {
      port *p;

      if ((p = car(sc->args)->_object._port)->kind & port_string) {
        int size;
        char *str;

        size = p->rep.string.curr - p->rep.string.start + 1;
        str = sc->malloc(size);
        if (str != NULL) {
          pointer s;

          memcpy(str, p->rep.string.start, size - 1);
          str[size - 1] = '\0';
          s = mk_counted_string(sc, str, size - 1);
          sc->free(str);
          s_return(sc, s);
        }
      }
      s_return(sc, sc->F);
    }
#endif

  case OP_CLOSE_INPORT:        /* close-input-port */
    port_close(sc, car(sc->args), port_input);
    s_return(sc, sc->T);

  case OP_CLOSE_OUTPORT:       /* close-output-port */
    port_close(sc, car(sc->args), port_output);
    s_return(sc, sc->T);

  case OP_INT_ENV:             /* interaction-environment */
    s_return(sc, sc->global_env);

  case OP_CURR_ENV:            /* current-environment */
    s_return(sc, sc->envir);

  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);
  }
  return sc->T;
}

static pointer opexe_5(scheme * sc, enum scheme_opcodes op) {
  pointer x;

  if (sc->nesting != 0) {
    int n = sc->nesting;
    sc->nesting = 0;
    sc->retcode = -1;
    Error_1(sc, "unmatched parentheses:", mk_integer(sc, n));
  }

  switch (op) {
    /* ========== reading part ========== */
  case OP_READ:
    if (!is_pair(sc->args)) {
      s_goto(sc, OP_READ_INTERNAL);
    }
    if (!is_inport(car(sc->args))) {
      Error_1(sc, "read: not an input port:", car(sc->args));
    }
    if (car(sc->args) == sc->inport) {
      s_goto(sc, OP_READ_INTERNAL);
    }
    x = sc->inport;
    sc->inport = car(sc->args);
    x = cons(sc, x, sc->NIL);
    s_save(sc, OP_SET_INPORT, x, sc->NIL);
    s_goto(sc, OP_READ_INTERNAL);

  case OP_READ_CHAR:           /* read-char */
  case OP_PEEK_CHAR:           /* peek-char */  {
      int c;
      if (is_pair(sc->args)) {
        if (car(sc->args) != sc->inport) {
          x = sc->inport;
          x = cons(sc, x, sc->NIL);
          s_save(sc, OP_SET_INPORT, x, sc->NIL);
          sc->inport = car(sc->args);
        }
      }
      c = inchar(sc);
      if (c == EOF) {
        s_return(sc, sc->EOF_OBJ);
      }
      if (sc->op == OP_PEEK_CHAR) {
        backchar(sc, c);
      }
      s_return(sc, mk_character(sc, c));
    }

  case OP_READ_U8:           /* read-u8 */
  case OP_PEEK_U8:           /* peek-u8 */  {
      int c;
      if (is_pair(sc->args)) {
        if (car(sc->args) != sc->inport) {
          x = sc->inport;
          x = cons(sc, x, sc->NIL);
          s_save(sc, OP_SET_INPORT, x, sc->NIL);
          sc->inport = car(sc->args);
        }
      }
      c = inchar8(sc);
      if (c == EOF) {
        s_return(sc, sc->EOF_OBJ);
      }
      if (sc->op == OP_PEEK_U8) {
        backchar(sc, c);
      }
      s_return(sc, mk_integer(sc, c));
    }

  case OP_CHAR_READY:          /* char-ready? */  {
      pointer p = sc->inport;
      int res;
      if (is_pair(sc->args)) {
        p = car(sc->args);
      }
      res = p->_object._port->kind & port_string;
      s_retbool(res);
    }

  case OP_SET_INPORT:          /* set-input-port */
    sc->inport = car(sc->args);
    s_return(sc, sc->value);

  case OP_SET_OUTPORT:         /* set-output-port */
    sc->outport = car(sc->args);
    s_return(sc, sc->value);

  case OP_RDSEXPR:
    switch (sc->tok) {
    case TOK_EOF:
      s_return(sc, sc->EOF_OBJ);
    case TOK_VEC:
      s_save(sc, OP_RDVEC, sc->NIL, sc->NIL);
      /* fall through */
    case TOK_LPAREN:
      sc->tok = token(sc);
      if (sc->tok == TOK_RPAREN) {
        s_return(sc, sc->NIL);
      } else if (sc->tok == TOK_DOT) {
        Error_0(sc, "syntax error: illegal dot expression");
      } else {
        sc->nesting_stack[sc->file_i]++;
        s_save(sc, OP_RDLIST, sc->NIL, sc->NIL);
        s_goto(sc, OP_RDSEXPR);
      }
    case TOK_QUOTE:
      s_save(sc, OP_RDQUOTE, sc->NIL, sc->NIL);
      sc->tok = token(sc);
      s_goto(sc, OP_RDSEXPR);
    case TOK_BQUOTE:
      sc->tok = token(sc);
      if (sc->tok == TOK_VEC) {
        s_save(sc, OP_RDQQUOTEVEC, sc->NIL, sc->NIL);
        sc->tok = TOK_LPAREN;
        s_goto(sc, OP_RDSEXPR);
      } else {
        s_save(sc, OP_RDQQUOTE, sc->NIL, sc->NIL);
      }
      s_goto(sc, OP_RDSEXPR);
    case TOK_COMMA:
      s_save(sc, OP_RDUNQUOTE, sc->NIL, sc->NIL);
      sc->tok = token(sc);
      s_goto(sc, OP_RDSEXPR);
    case TOK_ATMARK:
      s_save(sc, OP_RDUQTSP, sc->NIL, sc->NIL);
      sc->tok = token(sc);
      s_goto(sc, OP_RDSEXPR);
    case TOK_ATOM:
      s_return(sc, mk_atom(sc, readstr_upto(sc, DELIMITERS)));
    case TOK_DQUOTE:
      x = readstrexp(sc);
      if (x == sc->F) {
        Error_0(sc, "Error reading string");
      }
      setimmutable(x);
      s_return(sc, x);
    case TOK_SHARP:{
        pointer f = find_slot_in_env(sc, sc->envir, sc->SHARP_HOOK, 1);
        if (f == sc->NIL) {
          Error_0(sc, "undefined sharp expression");
        } else {
          sc->code = cons(sc, slot_value_in_env(f), sc->NIL);
          s_goto(sc, OP_EVAL);
        }
      }
    case TOK_SHARP_CONST:
      if ((x = mk_sharp_const(sc, readstr_upto(sc, DELIMITERS))) == sc->NIL) {
        Error_0(sc, "undefined sharp expression");
      } else {
        s_return(sc, x);
      }
    default:
      Error_0(sc, "syntax error: illegal token");
    }
    break;

  case OP_RDLIST:{
      sc->args = cons(sc, sc->value, sc->args);
      sc->tok = token(sc);
      if (sc->tok == TOK_EOF) {
        s_return(sc, sc->EOF_OBJ);
      } else if (sc->tok == TOK_RPAREN) {
        int c = inchar(sc);
        if (c != '\n')
          backchar(sc, c);
#if SHOW_ERROR_LINE
        else if (sc->load_stack[sc->file_i].kind & port_file)
          sc->load_stack[sc->file_i].rep.stdio.curr_line++;
#endif
        sc->nesting_stack[sc->file_i]--;
        s_return(sc, reverse_in_place(sc, sc->NIL, sc->args));
      } else if (sc->tok == TOK_DOT) {
        s_save(sc, OP_RDDOT, sc->args, sc->NIL);
        sc->tok = token(sc);
        s_goto(sc, OP_RDSEXPR);
      } else {
        s_save(sc, OP_RDLIST, sc->args, sc->NIL);;
        s_goto(sc, OP_RDSEXPR);
      }
    }

  case OP_RDDOT:
    if (token(sc) != TOK_RPAREN) {
      Error_0(sc, "syntax error: illegal dot expression");
    } else {
      sc->nesting_stack[sc->file_i]--;
      s_return(sc, reverse_in_place(sc, sc->value, sc->args));
    }

  case OP_RDQUOTE:
    s_return(sc, cons(sc, sc->QUOTE, cons(sc, sc->value, sc->NIL)));

  case OP_RDQQUOTE:
    s_return(sc, cons(sc, sc->QQUOTE, cons(sc, sc->value, sc->NIL)));

  case OP_RDQQUOTEVEC:
    s_return(sc, cons(sc, mk_symbol(sc, "apply"),
            cons(sc, mk_symbol(sc, "vector"),
                cons(sc, cons(sc, sc->QQUOTE,
                        cons(sc, sc->value, sc->NIL)), sc->NIL))));

  case OP_RDUNQUOTE:
    s_return(sc, cons(sc, sc->UNQUOTE, cons(sc, sc->value, sc->NIL)));

  case OP_RDUQTSP:
    s_return(sc, cons(sc, sc->UNQUOTESP, cons(sc, sc->value, sc->NIL)));

  case OP_RDVEC:
    /*sc->code=cons(sc,mk_proc(sc,OP_VECTOR),sc->value);
       s_goto(sc,OP_EVAL); Cannot be quoted */
    /*x=cons(sc,mk_proc(sc,OP_VECTOR),sc->value);
       s_return(sc,x); Cannot be part of pairs */
    /*sc->code=mk_proc(sc,OP_VECTOR);
       sc->args=sc->value;
       s_goto(sc,OP_APPLY); */
    sc->args = sc->value;
    s_goto(sc, OP_VECTOR);

    /* ========== printing part ========== */
  case OP_P0LIST:
    if (is_vector(sc->args)) {
      putstr(sc, "#(");
      sc->args = cons(sc, sc->args, mk_integer(sc, 0));
      s_goto(sc, OP_PVECFROM);
    } else if (is_environment(sc->args)) {
      putstr(sc, "#<ENVIRONMENT>");
      s_return(sc, sc->T);
    } else if (!is_pair(sc->args)) {
      printatom(sc, sc->args, sc->print_flag);
      s_return(sc, sc->T);
    } else if (car(sc->args) == sc->QUOTE && ok_abbrev(cdr(sc->args))) {
      putstr(sc, "'");
      sc->args = cadr(sc->args);
      s_goto(sc, OP_P0LIST);
    } else if (car(sc->args) == sc->QQUOTE && ok_abbrev(cdr(sc->args))) {
      putstr(sc, "`");
      sc->args = cadr(sc->args);
      s_goto(sc, OP_P0LIST);
    } else if (car(sc->args) == sc->UNQUOTE && ok_abbrev(cdr(sc->args))) {
      putstr(sc, ",");
      sc->args = cadr(sc->args);
      s_goto(sc, OP_P0LIST);
    } else if (car(sc->args) == sc->UNQUOTESP && ok_abbrev(cdr(sc->args))) {
      putstr(sc, ",@");
      sc->args = cadr(sc->args);
      s_goto(sc, OP_P0LIST);
    } else {
      putstr(sc, "(");
      s_save(sc, OP_P1LIST, cdr(sc->args), sc->NIL);
      sc->args = car(sc->args);
      s_goto(sc, OP_P0LIST);
    }

  case OP_P1LIST:
    if (is_pair(sc->args)) {
      s_save(sc, OP_P1LIST, cdr(sc->args), sc->NIL);
      putstr(sc, " ");
      sc->args = car(sc->args);
      s_goto(sc, OP_P0LIST);
    } else if (is_vector(sc->args)) {
      s_save(sc, OP_P1LIST, sc->NIL, sc->NIL);
      putstr(sc, " . ");
      s_goto(sc, OP_P0LIST);
    } else {
      if (sc->args != sc->NIL) {
        putstr(sc, " . ");
        printatom(sc, sc->args, sc->print_flag);
      }
      putstr(sc, ")");
      s_return(sc, sc->T);
    }
  case OP_PVECFROM:{
      int i = ivalue_unchecked(cdr(sc->args));
      pointer vec = car(sc->args);
      int len = ivalue_unchecked(vec);
      if (i == len) {
        putstr(sc, ")");
        s_return(sc, sc->T);
      } else {
        pointer elem = vector_elem(vec, i);
        ivalue_unchecked(cdr(sc->args)) = i + 1;
        s_save(sc, OP_PVECFROM, sc->args, sc->NIL);
        sc->args = elem;
        if (i > 0)
          putstr(sc, " ");
        s_goto(sc, OP_P0LIST);
      }
    }

  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);

  }
  return sc->T;
}

static pointer opexe_6(scheme * sc, enum scheme_opcodes op) {
  pointer x, y;
  long v;

  switch (op) {
  case OP_LIST_LENGTH:         /* length *//* a.k */
    v = list_length(sc, car(sc->args));
    if (v < 0) {
      Error_1(sc, "length: not a list:", car(sc->args));
    }
    s_return(sc, mk_integer(sc, v));

  case OP_ASSQ:                /* assq *//* a.k */
    x = car(sc->args);
    for (y = cadr(sc->args); is_pair(y); y = cdr(y)) {
      if (!is_pair(car(y))) {
        Error_0(sc, "unable to handle non pair element");
      }
      if (x == caar(y))
        break;
    }
    if (is_pair(y)) {
      s_return(sc, car(y));
    } else {
      s_return(sc, sc->F);
    }


  case OP_GET_CLOSURE:         /* get-closure-code *//* a.k */
    sc->args = car(sc->args);
    if (sc->args == sc->NIL) {
      s_return(sc, sc->F);
    } else if (is_closure(sc->args)) {
      s_return(sc, cons(sc, sc->LAMBDA, closure_code(sc->value)));
    } else if (is_macro(sc->args)) {
      s_return(sc, cons(sc, sc->LAMBDA, closure_code(sc->value)));
    } else {
      s_return(sc, sc->F);
    }
  case OP_CLOSUREP:            /* closure? */
    /*
     * Note, macro object is also a closure.
     * Therefore, (closure? <#MACRO>) ==> #t
     */
    s_retbool(is_closure(car(sc->args)));
  case OP_MACROP:              /* macro? */
    s_retbool(is_macro(car(sc->args)));
  default:
    sprintf(sc->strbuff, "%d: illegal operator", sc->op);
    Error_0(sc, sc->strbuff);
  }
  return sc->T;                 /* NOTREACHED */
}

typedef pointer(*dispatch_func) (scheme *, enum scheme_opcodes);

typedef int (*test_predicate) (pointer);
static int is_any(pointer p) {
  return 1;
}

static int is_nonneg(pointer p) {
  return ivalue(p) >= 0 && is_integer(p);
}

/* Correspond carefully with following defines! */
static struct {
  test_predicate fct;
  const char *kind;
} tests[] = {
  {0, 0},                        /* unused */
  {is_any, 0},
  {is_string, "string"},
  {is_symbol, "symbol"},
  {is_port, "port"},
  {is_inport, "input port"},
  {is_outport, "output port"},
  {is_environment, "environment"},
  {is_pair, "pair"},
  {0, "pair or '()"},
  {is_character, "character"},
  {is_vector, "vector"},
  {is_number, "number"},
  {is_integer, "integer"},
  {is_nonneg, "non-negative integer"},
  {is_bvector, "bytevector"},
};

/* correspond with preceding struct "tests" */
#define TST_NONE 0
#define TST_ANY "\001"
#define TST_STRING "\002"
#define TST_SYMBOL "\003"
#define TST_PORT "\004"
#define TST_INPORT "\005"
#define TST_OUTPORT "\006"
#define TST_ENVIRONMENT "\007"
#define TST_PAIR "\010"
#define TST_LIST "\011"
#define TST_CHAR "\012"
#define TST_VECTOR "\013"
#define TST_NUMBER "\014"
#define TST_INTEGER "\015"
#define TST_NATURAL "\016"
#define TST_BVECTOR "\017"

typedef struct {
  dispatch_func func;
  char *name;
  int min_arity;
  int max_arity;
  char *arg_tests_encoding;
} op_code_info;

#define INF_ARG 0xffff

static op_code_info dispatch_table[] = {
#define _OP_DEF(A,B,C,D,E,OP) {A,B,C,D,E},
#include "scm_opdf.h"
  {0}
};

static const char *procname(pointer x) {
  int n = procnum(x);
  const char *name = dispatch_table[n].name;
  if (name == 0) {
    name = "ILLEGAL!";
  }
  return name;
}

/* kernel of this interpreter */
static void Eval_Cycle(scheme * sc, enum scheme_opcodes op) {
  sc->op = op;
  for (;;) {
    op_code_info *pcd = dispatch_table + sc->op;
    if (pcd->name != 0) {       /* if built-in function, check arguments */
      char msg[AUXBUFF_SIZE];
      int ok = 1;
      int n = list_length(sc, sc->args);

      /* Check number of arguments */
      if (n < pcd->min_arity) {
        ok = 0;
        snprintf(msg, AUXBUFF_SIZE, "%s: needs%s %d argument(s)",
            pcd->name,
            pcd->min_arity == pcd->max_arity ? "" : " at least",
            pcd->min_arity);
      }
      if (ok && n > pcd->max_arity) {
        ok = 0;
        snprintf(msg, AUXBUFF_SIZE, "%s: needs%s %d argument(s)",
            pcd->name,
            pcd->min_arity == pcd->max_arity ? "" : " at most",
            pcd->max_arity);
      }
      if (ok) {
        if (pcd->arg_tests_encoding != 0) {
          int i = 0;
          int j;
          const char *t = pcd->arg_tests_encoding;
          pointer arglist = sc->args;
          do {
            pointer arg = car(arglist);
            j = (int) t[0];
            if (j == TST_LIST[0]) {
              if (arg != sc->NIL && !is_pair(arg))
                break;
            } else {
              if (!tests[j].fct(arg))
                break;
            }

            if (t[1] != 0) {    /* last test is replicated as necessary */
              t++;
            }
            arglist = cdr(arglist);
            i++;
          } while (i < n);
          if (i < n) {
            ok = 0;
            snprintf(msg, AUXBUFF_SIZE, "%s: argument %d must be: %s",
                pcd->name, i + 1, tests[j].kind);
          }
        }
      }
      if (!ok) {
        if (_Error_1(sc, msg, 0) == sc->NIL) {
          return;
        }
        pcd = dispatch_table + sc->op;
      }
    }
    ok_to_freely_gc(sc);
    if (pcd->func(sc, (enum scheme_opcodes) sc->op) == sc->NIL) {
      return;
    }
    if (sc->no_memory) {
      fprintf(stderr, "No memory!\n");
      sc->retcode = 9;
      return;
    }
  }
}

/* ========== Initialization of internal keywords ========== */

static void assign_syntax(scheme * sc, char *name) {
  pointer x;

  x = oblist_add_by_name(sc, name);
  typeflag(x) |= T_SYNTAX;
}

static void assign_proc(scheme * sc, enum scheme_opcodes op, char *name) {
  pointer x, y;

  x = mk_symbol(sc, name);
  y = mk_proc(sc, op);
  new_slot_in_env(sc, x, y);
}

static pointer mk_proc(scheme * sc, enum scheme_opcodes op) {
  pointer y;

  y = get_cell(sc, sc->NIL, sc->NIL);
  typeflag(y) = (T_PROC | T_ATOM);
  ivalue_unchecked(y) = (long) op;
  set_num_integer(y);
  return y;
}

/* Hard-coded for the given keywords. Remember to rewrite if more are added! */
static int syntaxnum(pointer p) {
  const char *s = strvalue(car(p));
  switch (strlength(car(p))) {
  case 2:
    if (s[0] == 'i')
      return OP_IF0;            /* if */
    else
      return OP_OR0;            /* or */
  case 3:
    if (s[0] == 'a')
      return OP_AND0;           /* and */
    else
      return OP_LET0;           /* let */
  case 4:
    switch (s[3]) {
    case 'e':
      return OP_CASE0;          /* case */
    case 'd':
      return OP_COND0;          /* cond */
    case '*':
      return OP_LET0AST;        /* let* */
    default:
      return OP_SET0;           /* set! */
    }
  case 5:
    switch (s[2]) {
    case 'g':
      return OP_BEGIN;          /* begin */
    case 'l':
      return OP_DELAY;          /* delay */
    case 'c':
      return OP_MACRO0;         /* macro */
    default:
      return OP_QUOTE;          /* quote */
    }
  case 6:
    switch (s[2]) {
    case 'm':
      return OP_LAMBDA;         /* lambda */
    case 'f':
      return OP_DEF0;           /* define */
    default:
      return OP_LET0REC;        /* letrec */
    }
  default:
    return OP_C0STREAM;         /* cons-stream */
  }
}

/* initialization of TinyScheme */
#if USE_INTERFACE
INTERFACE static pointer s_cons(scheme * sc, pointer a, pointer b) {
  return cons(sc, a, b);
}
INTERFACE static pointer s_immutable_cons(scheme * sc, pointer a, pointer b) {
  return immutable_cons(sc, a, b);
}

static struct scheme_interface vtbl = {
  scheme_define,
  s_cons,
  s_immutable_cons,
  reserve_cells,
  mk_integer,
  mk_real,
  mk_symbol,
  gensym,
  mk_string,
  mk_counted_string,
  mk_character,
  mk_vector,
  mk_foreign_func,
  putstr,
  putcharacter,

  is_string,
  string_value,
  is_number,
  nvalue,
  ivalue,
  rvalue,
  is_integer,
  is_real,
  is_character,
  charvalue,
  is_list,
  is_vector,
  list_length,
  ivalue,
  fill_vector,
  vector_elem,
  set_vector_elem,
  is_port,
  is_pair,
  pair_car,
  pair_cdr,
  set_car,
  set_cdr,

  is_symbol,
  symname,

  is_syntax,
  is_proc,
  is_foreign,
  syntaxname,
  is_closure,
  is_macro,
  closure_code,
  closure_env,

  is_continuation,
  is_promise,
  is_environment,
  is_immutable,
  setimmutable,

  scheme_load_file,
  scheme_load_string
};
#endif

scheme *scheme_init_new() {
  scheme *sc = (scheme *) malloc(sizeof(scheme));
  if (!scheme_init(sc)) {
    free(sc);
    return 0;
  } else {
    return sc;
  }
}

scheme *scheme_init_new_custom_alloc(func_alloc malloc, func_dealloc free) {
  scheme *sc = (scheme *) malloc(sizeof(scheme));
  if (!scheme_init_custom_alloc(sc, malloc, free)) {
    free(sc);
    return 0;
  } else {
    return sc;
  }
}


int scheme_init(scheme * sc) {
  return scheme_init_custom_alloc(sc, malloc, free);
}

int scheme_init_custom_alloc(scheme * sc, func_alloc malloc,
    func_dealloc free) {
  int i, n = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
  pointer x;

  num_zero.is_fixnum = 1;
  num_zero.value.ivalue = 0;
  num_one.is_fixnum = 1;
  num_one.value.ivalue = 1;

#if USE_INTERFACE
  sc->vptr = &vtbl;
#endif
  sc->gensym_cnt = 0;
  sc->malloc = malloc;
  sc->free = free;
  sc->last_cell_seg = -1;
  sc->backchar = -1;
  sc->sink = &sc->_sink;
  sc->NIL = &sc->_NIL;
  sc->T = &sc->_HASHT;
  sc->F = &sc->_HASHF;
  sc->EOF_OBJ = &sc->_EOF_OBJ;
  sc->free_cell = &sc->_NIL;
  sc->fcells = 0;
  sc->no_memory = 0;
  sc->alloc_seg = sc->malloc(sizeof(*(sc->alloc_seg)) * cell_nsegment);
  sc->cell_seg = sc->malloc(sizeof(*(sc->cell_seg)) * cell_nsegment);
  sc->strbuff = sc->malloc(STRBUFF_INITIAL_SIZE);
  sc->strbuff_size = STRBUFF_INITIAL_SIZE;
  sc->inport = sc->NIL;
  sc->outport = sc->NIL;
  sc->save_inport = sc->NIL;
  sc->loadport = sc->NIL;
  sc->nesting = 0;
  sc->interactive_repl = 0;

  if (alloc_cellseg(sc, FIRST_CELLSEGS) != FIRST_CELLSEGS) {
    sc->no_memory = 1;
    return 0;
  }
  sc->gc_verbose = 0;
  dump_stack_initialize(sc);
  sc->code = sc->NIL;
  sc->tracing = 0;

  /* init sc->NIL */
  typeflag(sc->NIL) = (T_ATOM | MARK);
  car(sc->NIL) = cdr(sc->NIL) = sc->NIL;
  /* init T */
  typeflag(sc->T) = (T_ATOM | MARK);
  car(sc->T) = cdr(sc->T) = sc->T;
  /* init F */
  typeflag(sc->F) = (T_ATOM | MARK);
  car(sc->F) = cdr(sc->F) = sc->F;
  /* init sink */
  typeflag(sc->sink) = (T_PAIR | MARK);
  car(sc->sink) = sc->NIL;
  /* init c_nest */
  sc->c_nest = sc->NIL;

  sc->oblist = oblist_initial_value(sc);
  /* init global_env */
  new_frame_in_env(sc, sc->NIL);
  sc->global_env = sc->envir;
  /* init else */
  x = mk_symbol(sc, "else");
  new_slot_in_env(sc, x, sc->T);

  assign_syntax(sc, "lambda");
  assign_syntax(sc, "quote");
  assign_syntax(sc, "define");
  assign_syntax(sc, "if");
  assign_syntax(sc, "begin");
  assign_syntax(sc, "set!");
  assign_syntax(sc, "let");
  assign_syntax(sc, "let*");
  assign_syntax(sc, "letrec");
  assign_syntax(sc, "cond");
  assign_syntax(sc, "delay");
  assign_syntax(sc, "and");
  assign_syntax(sc, "or");
  assign_syntax(sc, "cons-stream");
  assign_syntax(sc, "macro");
  assign_syntax(sc, "case");

  for (i = 0; i < n; i++) {
    if (dispatch_table[i].name != 0) {
      assign_proc(sc, (enum scheme_opcodes) i, dispatch_table[i].name);
    }
  }

  /* initialization of global pointers to special symbols */
  sc->LAMBDA = mk_symbol(sc, "lambda");
  sc->QUOTE = mk_symbol(sc, "quote");
  sc->QQUOTE = mk_symbol(sc, "quasiquote");
  sc->UNQUOTE = mk_symbol(sc, "unquote");
  sc->UNQUOTESP = mk_symbol(sc, "unquote-splicing");
  sc->FEED_TO = mk_symbol(sc, "=>");
  sc->COLON_HOOK = mk_symbol(sc, "*colon-hook*");
  sc->ERROR_HOOK = mk_symbol(sc, "*error-hook*");
  sc->SHARP_HOOK = mk_symbol(sc, "*sharp-hook*");
  sc->COMPILE_HOOK = mk_symbol(sc, "*compile-hook*");

  return !sc->no_memory;
}

void scheme_set_input_port_file(scheme * sc, FILE * fin) {
  sc->inport = port_from_file(sc, fin, port_input);
}

void scheme_set_input_port_string(scheme * sc, char *start,
    char *past_the_end) {
  sc->inport = port_from_string(sc, start, past_the_end, port_input);
}

void scheme_set_output_port_file(scheme * sc, FILE * fout) {
  sc->outport = port_from_file(sc, fout, port_output);
}

void scheme_set_output_port_string(scheme * sc, char *start,
    char *past_the_end) {
  sc->outport = port_from_string(sc, start, past_the_end, port_output);
}

void scheme_set_external_data(scheme * sc, void *p) {
  sc->ext_data = p;
}

void scheme_deinit(scheme * sc) {
  int i;

#if SHOW_ERROR_LINE
  char *fname;
#endif

  sc->oblist = sc->NIL;
  sc->global_env = sc->NIL;
  dump_stack_free(sc);
  sc->envir = sc->NIL;
  sc->code = sc->NIL;
  sc->args = sc->NIL;
  sc->value = sc->NIL;
  if (is_port(sc->inport)) {
    typeflag(sc->inport) = T_ATOM;
  }
  sc->inport = sc->NIL;
  sc->outport = sc->NIL;
  if (is_port(sc->save_inport)) {
    typeflag(sc->save_inport) = T_ATOM;
  }
  sc->save_inport = sc->NIL;
  if (is_port(sc->loadport)) {
    typeflag(sc->loadport) = T_ATOM;
  }
  sc->loadport = sc->NIL;
  sc->free(sc->strbuff);
  sc->gc_verbose = 0;
  gc(sc, sc->NIL, sc->NIL);

  for (i = 0; i <= sc->last_cell_seg; i++) {
    sc->free(sc->alloc_seg[i]);
  }
  sc->free(sc->cell_seg);
  sc->free(sc->alloc_seg);

#if SHOW_ERROR_LINE
  for (i = 0; i <= sc->file_i; i++) {
    if (sc->load_stack[i].kind & port_file) {
      fname = sc->load_stack[i].rep.stdio.filename;
      if (fname)
        sc->free(fname);
    }
  }
#endif
}

void scheme_load_file(scheme * sc, FILE * fin) {
  scheme_load_named_file(sc, fin, 0);
}

void scheme_load_named_file(scheme * sc, FILE * fin, const char *filename) {
  if (fin == NULL) {
    fprintf(stderr, "File pointer can not be NULL when loading a file\n");
    return;
  }
  dump_stack_reset(sc);
  sc->envir = sc->global_env;
  sc->file_i = 0;
  sc->load_stack[0].kind = port_input | port_file;
  sc->load_stack[0].rep.stdio.file = fin;
  sc->loadport = mk_port(sc, sc->load_stack);
  sc->retcode = 0;
  if (fin == stdin && !str_eq(filename, "--")) {
    sc->interactive_repl = 1;
  }
#if SHOW_ERROR_LINE
  sc->load_stack[0].rep.stdio.curr_line = 0;
  if (fin != stdin && filename)
    sc->load_stack[0].rep.stdio.filename =
        store_string(sc, strlen(filename), filename);
  else
    sc->load_stack[0].rep.stdio.filename = NULL;
#endif

  sc->args = mk_integer(sc, sc->file_i);
  Eval_Cycle(sc, OP_T0LVL);
  typeflag(sc->loadport) = T_ATOM;
  if (sc->retcode == 0) {
    sc->retcode = sc->nesting != 0;
  }
}

void scheme_load_string(scheme * sc, const char *cmd) {
  dump_stack_reset(sc);
  sc->envir = sc->global_env;
  sc->file_i = 0;
  sc->load_stack[0].kind = port_input | port_string;
  sc->load_stack[0].rep.string.start = (char *) cmd;    /* This func respects const */
  sc->load_stack[0].rep.string.past_the_end = (char *) cmd + strlen(cmd);
  sc->load_stack[0].rep.string.curr = (char *) cmd;
  sc->loadport = mk_port(sc, sc->load_stack);
  sc->retcode = 0;
  sc->interactive_repl = 0;
  sc->args = mk_integer(sc, sc->file_i);
  Eval_Cycle(sc, OP_T0LVL);
  typeflag(sc->loadport) = T_ATOM;
  if (sc->retcode == 0) {
    sc->retcode = sc->nesting != 0;
  }
}

void scheme_define(scheme * sc, pointer envir, pointer symbol, pointer value) {
  pointer x;

  x = find_slot_in_env(sc, envir, symbol, 0);
  if (x != sc->NIL) {
    set_slot_in_env(x, value);
  } else {
    new_slot_spec_in_env(sc, envir, symbol, value);
  }
}

#if !STANDALONE
void scheme_register_foreign_func(scheme * sc, scheme_registerable * sr) {
  scheme_define(sc,
      sc->global_env, mk_symbol(sc, sr->name), mk_foreign_func(sc, sr->f));
}

void scheme_register_foreign_func_list(scheme * sc,
    scheme_registerable * list, int count) {
  int i;
  for (i = 0; i < count; i++) {
    scheme_register_foreign_func(sc, list + i);
  }
}

pointer scheme_apply0(scheme * sc, const char *procname) {
  return scheme_eval(sc, cons(sc, mk_symbol(sc, procname), sc->NIL));
}

void save_from_C_call(scheme * sc) {
  pointer saved_data = cons(sc,
      car(sc->sink),
      cons(sc,
          sc->envir,
          sc->dump));
  /* Push */
  sc->c_nest = cons(sc, saved_data, sc->c_nest);
  /* Truncate the dump stack so TS will return here when done, not
     directly resume pre-C-call operations. */
  dump_stack_reset(sc);
}
void restore_from_C_call(scheme * sc) {
  car(sc->sink) = caar(sc->c_nest);
  sc->envir = cadar(sc->c_nest);
  sc->dump = cdr(cdar(sc->c_nest));
  /* Pop */
  sc->c_nest = cdr(sc->c_nest);
}

/* "func" and "args" are assumed to be already eval'ed. */
pointer scheme_call(scheme * sc, pointer func, pointer args) {
  int old_repl = sc->interactive_repl;
  sc->interactive_repl = 0;
  save_from_C_call(sc);
  sc->envir = sc->global_env;
  sc->args = args;
  sc->code = func;
  sc->retcode = 0;
  Eval_Cycle(sc, OP_APPLY);
  sc->interactive_repl = old_repl;
  restore_from_C_call(sc);
  return sc->value;
}

pointer scheme_eval(scheme * sc, pointer obj) {
  int old_repl = sc->interactive_repl;
  sc->interactive_repl = 0;
  save_from_C_call(sc);
  sc->args = sc->NIL;
  sc->code = obj;
  sc->retcode = 0;
  Eval_Cycle(sc, OP_EVAL);
  sc->interactive_repl = old_repl;
  restore_from_C_call(sc);
  return sc->value;
}
#endif

char* get_version() {
  return VERSION;
}

/* ========== Main ========== */

#if STANDALONE

FILE *open_file(char *fname) {
    if (str_eq(fname, "-") || str_eq(fname, "--")) {
        return stdin;
    }
    return fopen(fname, "r");
}

void initFromEnv() {
    char* val = getenv("CELL_SEGSIZE");
    cell_segsize = val != NULL ? atoi(val) : CELL_SEGSIZE;
    val = getenv("CELL_NSEGMENT");
    cell_nsegment = val != NULL ? atoi(val) : CELL_NSEGMENT;
#ifdef EVAL_LIMIT
    val = getenv("EVAL_LIMIT");
    eval_limit = val != NULL ? atol(val) : EVAL_LIMIT;
#endif
}

void args_into_real_list(pointer args, char **buffer, int list_size, int step) {
    if(step == list_size) return;

    buffer[step] = string_value(car(args));
    args_into_real_list(cdr(args), buffer, list_size, step += 1);
}

int run_subprocess(const char *name, const char **args, int arg_count)
{
#ifdef _WIN32
	// https://docs.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output

	STARTUPINFOA siStartInfo;
	ZeroMemory(&siStartInfo, sizeof(siStartInfo));
	siStartInfo.cb = sizeof(STARTUPINFO);
	// NOTE: theoretically setting NULL to std handles should not be a problem
	// https://docs.microsoft.com/en-us/windows/console/getstdhandle?redirectedfrom=MSDN#attachdetach-behavior
	// TODO: check for errors in GetStdHandle
	siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	siStartInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	siStartInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	PROCESS_INFORMATION piProcInfo;
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    #define BUFFER_MAX (4096 * 4)
    char buffer[BUFFER_MAX] = {0};
    size_t cursor = 0;

	cursor += snprintf(buffer, BUFFER_MAX, "%s", name);
	cursor -= 1;

	for (uint32_t i = 0; i < arg_count; i++)
	{
		buffer[cursor++] = ' ';

		bool need_quoting = strpbrk(args[i], "\t\v ") != NULL || args[i][0] == 0;
		if (need_quoting) buffer[cursor++] = '"';
		for (int j = 0; '\0' != args[i][j]; j++)
		{
			switch (args[i][j])
			{
				default:
					break;
				case '\\':
					if (args[i][j + 1] == '"')
					{
						buffer[cursor++] = '\\';
					}

					break;
				case '"':
					buffer[cursor++] = '\\';
					break;
			}

			buffer[cursor++] = args[i][j];
		}

		if (need_quoting) buffer[cursor++] = '"';
	}

    buffer[cursor] = '\0';

	BOOL bSuccess = CreateProcessA(
		NULL,
		buffer,
		NULL,
		NULL,
		TRUE,
		0,
		NULL,
		NULL,
		&siStartInfo,
		&piProcInfo);

	if (!bSuccess)
	{
		fprintf(stderr, "Could not create child process: %lu", GetLastError());
		return -1;
	}

	CloseHandle(piProcInfo.hThread);

	DWORD result = WaitForSingleObject(
			piProcInfo.hProcess, // HANDLE hHandle,
			INFINITE // DWORD  dwMilliseconds
	);

	if (result == WAIT_FAILED)
	{
		fprintf(stderr, "Could not wait on child process: %lu", GetLastError());
		return -1;
	}

	DWORD exit_status;
	if (!GetExitCodeProcess(piProcInfo.hProcess, &exit_status))
	{
		fprintf(stderr, "Could not get process exit code: %lu", GetLastError());
		return -1;
	}

	CloseHandle(piProcInfo.hProcess);

	return exit_status;
#else
	pid_t cpid = fork();
	if (cpid < 0)
	{
		fprintf(stderr, "Could not fork child process %s: %s\n", name, strerror(errno));
		return -1;
	}

	if (cpid == 0)
	{
		if (execvp(name, (char *const *)args) < 0)
		{
			fprintf(stderr, "Could not exec child process %s: %s\n", name, strerror(errno));
			exit(1);
		}
		exit(0);
	}

	for (;;)
	{
		int wstatus = 0;
		if (waitpid(cpid, &wstatus, 0) < 0)
		{
			if (errno != EINTR)
			{
				fprintf(stderr, "Could not wait on %s (pid %d): %s\n", name, cpid, strerror(errno));
				return -1;
			}
			continue;
		}

		if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);

		if (WIFSIGNALED(wstatus))
		{
			fprintf(stderr, "Program interrupted by signal %d.\n", WTERMSIG(wstatus));
			return -1;
		}
	}

	return cpid;
#endif
}

pointer do_subprocess(scheme *sc, pointer args) {
    int number_of_arguments = list_length(sc, args);

    if (number_of_arguments < 1) {
        return sc->F;
    }

    /* extra slot for null terminator */
    char **arguments_buffer = alloca(sizeof(char*)*(number_of_arguments+1));

    args_into_real_list(args, arguments_buffer, number_of_arguments, 0);

    arguments_buffer[number_of_arguments] = NULL;

    for(int i = 0; i < number_of_arguments; i++)
        printf("%s ", arguments_buffer[i]);
    printf("\n");

    int result = run_subprocess(
        (const char *)arguments_buffer[0],
        (const char **)arguments_buffer,
        number_of_arguments
    );

    return mk_integer(sc, result);
}

int main(int argc, char **argv) {
  scheme sc;
  FILE *fin = NULL;
  char *file_name = InitFile;
  char *executable_name = argv[0];
  int retcode;
  int isfile = 1;

  initFromEnv();
  if (argc == 1) {
    printf("%s", get_version());
  }
  if (argc == 2 && str_eq(argv[1], "-?")) {
    printf("Usage: tinyscheme -?\n");
    printf("or:    tinyscheme [<file1> <file2> ...]\n");
    printf("followed by\n");
    printf("          -1 <file> [<arg1> <arg2> ...]\n");
    printf("          -c <Scheme commands> [<arg1> <arg2> ...]\n");
    printf("assuming that the executable is named tinyscheme.\n");
    printf("Use - as filename for stdin.\n");
    return 1;
  }
  if (!scheme_init(&sc)) {
    fprintf(stderr, "Could not initialize!\n");
    return 2;
  }
  scheme_set_input_port_file(&sc, stdin);
  scheme_set_output_port_file(&sc, stdout);

  scheme_define(&sc, sc.global_env, mk_symbol(&sc, "cmd"), mk_foreign_func(&sc, do_subprocess));

#if USE_DL
  scheme_define(&sc, sc.global_env, mk_symbol(&sc, "load-extension"),
      mk_foreign_func(&sc, scm_load_ext));
#endif
  argv++;
  if (access(file_name, 0) != 0) {
    char *p = getenv("TINYSCHEMEINIT");
    if (p != 0) {
      file_name = p;
    } else {
      strcpy(sc.strbuff, executable_name);
      p = strrchr(sc.strbuff, '/');
      if (p != 0) {
        strcpy(p + 1, file_name);
        file_name = sc.strbuff;
      }
    }
  }
  evalcnt = 0;
  do {
    if (str_eq(file_name, "-1") || str_eq(file_name, "-c")) {
      pointer args = sc.NIL;
      isfile = file_name[1] == '1';
      file_name = *argv++;
      if (isfile) {
        fin = open_file(file_name);
      }
      for (; *argv; argv++) {
        pointer value = mk_string(&sc, *argv);
        args = cons(&sc, value, args);
      }
      args = reverse_in_place(&sc, sc.NIL, args);
      scheme_define(&sc, sc.global_env, mk_symbol(&sc, "*args*"), args);

    } else {
      fin = open_file(file_name);
    }
    if (isfile && fin == 0) {
      fprintf(stderr, "Could not open file %s\n", file_name);
    } else {
      if (isfile) {
        scheme_load_named_file(&sc, fin, file_name);
      } else {
        scheme_load_string(&sc, file_name);
      }
      if (!isfile || fin != stdin) {
        if (sc.retcode != 0) {
          fprintf(stderr, "Errors encountered reading %s\n", file_name);
        }
        if (isfile) {
          fclose(fin);
        }
      }
    }
    file_name = *argv++;
  } while (file_name != 0);
  if (argc == 1) {
    scheme_load_named_file(&sc, stdin, "-");
  }
  retcode = sc.retcode;
  scheme_deinit(&sc);

  return retcode;
}

#endif
