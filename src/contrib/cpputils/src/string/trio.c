/*************************************************************************
 *
 * $Id$
 *
 * Copyright (C) 1998, 2009 Bjorn Reese and Daniel Stenberg.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS AND
 * CONTRIBUTORS ACCEPT NO RESPONSIBILITY IN ANY CONCEIVABLE MANNER.
 *
 *************************************************************************
 *
 * A note to trio contributors:
 *
 * Avoid heap allocation at all costs to ensure that the trio functions
 * are async-safe. The exceptions are the printf/fprintf functions, which
 * uses fputc, and the asprintf functions and the <alloc> modifier, which
 * by design are required to allocate form the heap.
 *
 ************************************************************************/


/*
 * TODO:
 *  - Scan is probably too permissive about its modifiers.
 *  - C escapes in %#[] ?
 *  - Multibyte characters (done for format parsing, except scan groups)
 *  - Complex numbers? (C99 _Complex)
 *  - Boolean values? (C99 _Bool)
 *  - C99 NaN(n-char-sequence) missing. The n-char-sequence can be used
 *    to print the mantissa, e.g. NaN(0xc000000000000000)
 *  - Should we support the GNU %a alloc modifier? GNU has an ugly hack
 *    for %a, because C99 used %a for other purposes. If specified as
 *    %as or %a[ it is interpreted as the alloc modifier, otherwise as
 *    the C99 hex-float. This means that you cannot scan %as as a hex-float
 *    immediately followed by an 's'.
 *  - Scanning of collating symbols.
 */

/*************************************************************************
 * Trio include files
 */
#include "triodef.h"
#include "trio.h"
#include "triop.h"

#if defined(TRIO_EMBED_NAN)
# define TRIO_PUBLIC_NAN static
# if TRIO_FEATURE_FLOAT
#  define TRIO_FUNC_NAN
#  define TRIO_FUNC_NINF
#  define TRIO_FUNC_PINF
#  define TRIO_FUNC_FPCLASSIFY_AND_SIGNBIT
#  define TRIO_FUNC_ISINF
# endif
#endif
#include "trionan.h"

#if defined(TRIO_EMBED_STRING)
# define TRIO_PUBLIC_STRING static
# define TRIO_FUNC_LENGTH
# define TRIO_FUNC_LENGTH_MAX
# define TRIO_FUNC_TO_LONG
# if TRIO_FEATURE_LOCALE
#  define TRIO_FUNC_COPY_MAX
# endif
# if TRIO_FEATURE_DYNAMICSTRING
#  define TRIO_FUNC_XSTRING_DUPLICATE
# endif
# if TRIO_EXTENSION && TRIO_FEATURE_SCANF
#  define TRIO_FUNC_EQUAL_LOCALE
# endif
# if TRIO_FEATURE_ERRNO
#  define TRIO_FUNC_ERROR
# endif
# if TRIO_FEATURE_FLOAT && TRIO_FEATURE_SCANF
#  define TRIO_FUNC_TO_DOUBLE
# endif
# if TRIO_FEATURE_DYNAMICSTRING
#  define TRIO_FUNC_STRING_EXTRACT
# endif
# if TRIO_FEATURE_DYNAMICSTRING
#  define TRIO_FUNC_STRING_TERMINATE
# endif
# if TRIO_FEATURE_USER_DEFINED
#  define TRIO_FUNC_DUPLICATE
# endif
# if TRIO_FEATURE_DYNAMICSTRING
#  define TRIO_FUNC_STRING_DESTROY
# endif
# if TRIO_FEATURE_USER_DEFINED
#  define TRIO_FUNC_DESTROY
# endif
# if TRIO_FEATURE_USER_DEFINED || (TRIO_FEATURE_FLOAT && TRIO_FEATURE_SCANF)
#  define TRIO_FUNC_EQUAL
# endif
# if TRIO_FEATURE_USER_DEFINED || TRIO_FEATURE_SCANF
#  define TRIO_FUNC_EQUAL_CASE
# endif
# if (TRIO_EXTENSION && TRIO_FEATURE_SCANF)
#  define TRIO_FUNC_EQUAL_MAX
# endif
# if TRIO_FEATURE_SCANF
#  define TRIO_FUNC_TO_UPPER
# endif
# if TRIO_FEATURE_DYNAMICSTRING
#  define TRIO_FUNC_XSTRING_APPEND_CHAR
# endif
#endif
#include "triostr.h"

/**************************************************************************
 *
 * Definitions
 *
 *************************************************************************/

#include <limits.h>
#if TRIO_FEATURE_FLOAT
# include <math.h>
# include <float.h>
#endif

#if defined(__STDC_ISO_10646__) || defined(MB_LEN_MAX) || defined(USE_MULTIBYTE) || TRIO_FEATURE_WIDECHAR
# if !defined(TRIO_PLATFORM_WINCE)
#  define TRIO_COMPILER_SUPPORTS_MULTIBYTE
#  if !defined(MB_LEN_MAX)
#   define MB_LEN_MAX 6
#  endif
# endif
#endif

#if (defined(TRIO_COMPILER_VISUALC) && (TRIO_COMPILER_VISUALC >= 1100)) || defined(TRIO_COMPILER_BORLAND)
# define TRIO_COMPILER_SUPPORTS_VISUALC_INT
#endif

#if TRIO_FEATURE_FLOAT
# if defined(PREDEF_STANDARD_C99) \
  || defined(PREDEF_STANDARD_UNIX03)
#  if !defined(HAVE_FLOORL) && !defined(TRIO_NO_FLOORL)
#   define HAVE_FLOORL
#  endif
#  if !defined(HAVE_CEILL) && !defined(TRIO_NO_CEILL)
#   define HAVE_CEILL
#  endif
#  if !defined(HAVE_POWL) && !defined(TRIO_NO_POWL)
#   define HAVE_POWL
#  endif
#  if !defined(HAVE_FMODL) && !defined(TRIO_NO_FMODL)
#   define HAVE_FMODL
#  endif
#  if !defined(HAVE_LOG10L) && !defined(TRIO_NO_LOG10L)
#   define HAVE_LOG10L
#  endif
# endif
# if defined(TRIO_COMPILER_VISUALC)
#  if defined(floorl)
#   define HAVE_FLOORL
#  endif
#  if defined(ceill)
#   define HAVE_CEILL
#  endif
#  if defined(powl)
#   define HAVE_POWL
#  endif
#  if defined(fmodl)
#   define HAVE_FMODL
#  endif
#  if defined(log10l)
#   define HAVE_LOG10L
#  endif
# endif
#endif

/*************************************************************************
 * Generic definitions
 */

#if !(defined(DEBUG) || defined(NDEBUG))
# define NDEBUG
#endif

#include <assert.h>
#include <ctype.h>
#if defined(PREDEF_STANDARD_C99) && !defined(isascii)
# define isascii(x) ((x) & 0x7F)
#endif
#if defined(TRIO_COMPILER_ANCIENT)
# include <varargs.h>
#else
# include <stdarg.h>
#endif
#include <stddef.h>
#if defined(TRIO_PLATFORM_WINCE)
extern int errno;
#else
# include <errno.h>
#endif

#ifndef NULL
# define NULL 0
#endif
#define NIL ((char)0)
#ifndef FALSE
# define FALSE (1 == 0)
# define TRUE (! FALSE)
#endif
#define BOOLEAN_T int

/* mincore() can be used for debugging purposes */
#define VALID(x) (NULL != (x))

#if TRIO_FEATURE_ERRORCODE
  /*
   * Encode the error code and the position. This is decoded
   * with TRIO_ERROR_CODE and TRIO_ERROR_POSITION.
   */
# define TRIO_ERROR_RETURN(x,y) (- ((x) + ((y) << 8)))
#else
# define TRIO_ERROR_RETURN(x,y) (-1)
#endif

typedef unsigned long trio_flags_t;


/*************************************************************************
 * Platform specific definitions
 */
#if defined(TRIO_PLATFORM_UNIX)
# include <unistd.h>
# include <signal.h>
# include <locale.h>
# if !defined(TRIO_FEATURE_LOCALE)
#  define USE_LOCALE
# endif
#endif /* TRIO_PLATFORM_UNIX */
#if defined(TRIO_PLATFORM_VMS)
# include <unistd.h>
#endif
#if defined(TRIO_PLATFORM_WIN32)
# if defined(TRIO_PLATFORM_WINCE)
int read(int handle, char *buffer, unsigned int length);
int write(int handle, const char *buffer, unsigned int length);
# else
#  include <io.h>
#  define read _read
#  define write _write
# endif
#endif /* TRIO_PLATFORM_WIN32 */

#if TRIO_FEATURE_WIDECHAR
# if defined(PREDEF_STANDARD_C94)
#  include <wchar.h>
#  include <wctype.h>
typedef wchar_t trio_wchar_t;
typedef wint_t trio_wint_t;
# else
typedef char trio_wchar_t;
typedef int trio_wint_t;
#  define WCONST(x) L ## x
#  define WEOF EOF
#  define iswalnum(x) isalnum(x)
#  define iswalpha(x) isalpha(x)
#  define iswcntrl(x) iscntrl(x)
#  define iswdigit(x) isdigit(x)
#  define iswgraph(x) isgraph(x)
#  define iswlower(x) islower(x)
#  define iswprint(x) isprint(x)
#  define iswpunct(x) ispunct(x)
#  define iswspace(x) isspace(x)
#  define iswupper(x) isupper(x)
#  define iswxdigit(x) isxdigit(x)
# endif
#endif


/*************************************************************************
 * Compiler dependent definitions
 */

/* Support for long long */
#ifndef __cplusplus
# if !defined(USE_LONGLONG)
#  if defined(TRIO_COMPILER_GCC) && !defined(__STRICT_ANSI__)
#   define USE_LONGLONG
#  else
#   if defined(TRIO_COMPILER_SUNPRO)
#    define USE_LONGLONG
#   else
#    if defined(TRIO_COMPILER_MSVC) && (_MSC_VER >= 1400)
#     define USE_LONGLONG
#    else
#     if defined(_LONG_LONG) || defined(_LONGLONG)
#      define USE_LONGLONG
#     endif
#    endif
#   endif
#  endif
# endif
#endif

/* The extra long numbers */
#if defined(USE_LONGLONG)
typedef signed long long int trio_longlong_t;
typedef unsigned long long int trio_ulonglong_t;
#else
# if defined(TRIO_COMPILER_SUPPORTS_VISUALC_INT)
typedef signed __int64 trio_longlong_t;
typedef unsigned __int64 trio_ulonglong_t;
# else
typedef TRIO_SIGNED long int trio_longlong_t;
typedef unsigned long int trio_ulonglong_t;
# endif
#endif

/* Maximal and fixed integer types */
#if defined(PREDEF_STANDARD_C99)
# include <stdint.h>
typedef intmax_t trio_intmax_t;
typedef uintmax_t trio_uintmax_t;
typedef int8_t trio_int8_t;
typedef int16_t trio_int16_t;
typedef int32_t trio_int32_t;
typedef int64_t trio_int64_t;
#else
# if defined(PREDEF_STANDARD_UNIX98)
#  include <inttypes.h>
typedef intmax_t trio_intmax_t;
typedef uintmax_t trio_uintmax_t;
typedef int8_t trio_int8_t;
typedef int16_t trio_int16_t;
typedef int32_t trio_int32_t;
typedef int64_t trio_int64_t;
# else
#  if defined(TRIO_COMPILER_SUPPORTS_VISUALC_INT)
typedef trio_longlong_t trio_intmax_t;
typedef trio_ulonglong_t trio_uintmax_t;
typedef __int8 trio_int8_t;
typedef __int16 trio_int16_t;
typedef __int32 trio_int32_t;
typedef __int64 trio_int64_t;
#  else
typedef trio_longlong_t trio_intmax_t;
typedef trio_ulonglong_t trio_uintmax_t;
#   if defined(TRIO_INT8_T)
typedef TRIO_INT8_T trio_int8_t;
#   else
typedef TRIO_SIGNED char trio_int8_t;
#   endif
#   if defined(TRIO_INT16_T)
typedef TRIO_INT16_T trio_int16_t;
#   else
typedef TRIO_SIGNED short trio_int16_t;
#   endif
#   if defined(TRIO_INT32_T)
typedef TRIO_INT32_T trio_int32_t;
#   else
typedef TRIO_SIGNED int trio_int32_t;
#   endif
#   if defined(TRIO_INT64_T)
typedef TRIO_INT64_T trio_int64_t;
#   else
typedef trio_longlong_t trio_int64_t;
#   endif
#  endif
# endif
#endif

#if defined(HAVE_FLOORL)
# define trio_floor(x) floorl((x))
#else
# define trio_floor(x) floor((double)(x))
#endif

#if defined(HAVE_CEILL)
# define trio_ceil(x) ceill((x))
#else
# define trio_ceil(x) ceil((double)(x))
#endif

#if defined(HAVE_FMODL)
# define trio_fmod(x,y) fmodl((x),(y))
#else
# define trio_fmod(x,y) fmod((double)(x),(double)(y))
#endif

#if defined(HAVE_POWL)
# define trio_pow(x,y) powl((x),(y))
#else
# define trio_pow(x,y) pow((double)(x),(double)(y))
#endif

#if defined(HAVE_LOG10L)
# define trio_log10(x) log10l((x))
#else
# define trio_log10(x) log10((double)(x))
#endif

#if TRIO_FEATURE_FLOAT
# define TRIO_FABS(x) (((x) < 0.0) ? -(x) : (x))
#endif

/*************************************************************************
 * Internal Definitions
 */

#if TRIO_FEATURE_FLOAT

# if !defined(DECIMAL_DIG)
#  define DECIMAL_DIG DBL_DIG
# endif

/* Long double sizes */
# ifdef LDBL_DIG
#  define MAX_MANTISSA_DIGITS LDBL_DIG
#  define MAX_EXPONENT_DIGITS 4
#  define MAX_DOUBLE_DIGITS LDBL_MAX_10_EXP
# else
#  define MAX_MANTISSA_DIGITS DECIMAL_DIG
#  define MAX_EXPONENT_DIGITS 3
#  define MAX_DOUBLE_DIGITS DBL_MAX_10_EXP
# endif

# if defined(TRIO_COMPILER_ANCIENT) || !defined(LDBL_DIG)
#  undef LDBL_DIG
#  undef LDBL_MANT_DIG
#  undef LDBL_EPSILON
#  define LDBL_DIG DBL_DIG
#  define LDBL_MANT_DIG DBL_MANT_DIG
#  define LDBL_EPSILON DBL_EPSILON
# endif

#endif /* TRIO_FEATURE_FLOAT */

/* The maximal number of digits is for base 2 */
#define MAX_CHARS_IN(x) (sizeof(x) * CHAR_BIT)
/* The width of a pointer. The number of bits in a hex digit is 4 */
#define POINTER_WIDTH ((sizeof("0x") - 1) + sizeof(trio_pointer_t) * CHAR_BIT / 4)

#if TRIO_FEATURE_FLOAT
/* Infinite and Not-A-Number for floating-point */
# define INFINITE_LOWER "inf"
# define INFINITE_UPPER "INF"
# define LONG_INFINITE_LOWER "infinite"
# define LONG_INFINITE_UPPER "INFINITE"
# define NAN_LOWER "nan"
# define NAN_UPPER "NAN"
#endif

/* Various constants */
enum {
  TYPE_PRINT = 1,
#if TRIO_FEATURE_SCANF
  TYPE_SCAN  = 2,
#endif

  /* Flags. FLAGS_LAST must be less than ULONG_MAX */
  FLAGS_NEW                 = 0,
  FLAGS_STICKY              = 1,
  FLAGS_SPACE               = 2 * FLAGS_STICKY,
  FLAGS_SHOWSIGN            = 2 * FLAGS_SPACE,
  FLAGS_LEFTADJUST          = 2 * FLAGS_SHOWSIGN,
  FLAGS_ALTERNATIVE         = 2 * FLAGS_LEFTADJUST,
  FLAGS_SHORT               = 2 * FLAGS_ALTERNATIVE,
  FLAGS_SHORTSHORT          = 2 * FLAGS_SHORT,
  FLAGS_LONG                = 2 * FLAGS_SHORTSHORT,
  FLAGS_QUAD                = 2 * FLAGS_LONG,
  FLAGS_LONGDOUBLE          = 2 * FLAGS_QUAD,
  FLAGS_SIZE_T              = 2 * FLAGS_LONGDOUBLE,
  FLAGS_PTRDIFF_T           = 2 * FLAGS_SIZE_T,
  FLAGS_INTMAX_T            = 2 * FLAGS_PTRDIFF_T,
  FLAGS_NILPADDING          = 2 * FLAGS_INTMAX_T,
  FLAGS_UNSIGNED            = 2 * FLAGS_NILPADDING,
  FLAGS_UPPER               = 2 * FLAGS_UNSIGNED,
  FLAGS_WIDTH               = 2 * FLAGS_UPPER,
  FLAGS_WIDTH_PARAMETER     = 2 * FLAGS_WIDTH,
  FLAGS_PRECISION           = 2 * FLAGS_WIDTH_PARAMETER,
  FLAGS_PRECISION_PARAMETER = 2 * FLAGS_PRECISION,
  FLAGS_BASE                = 2 * FLAGS_PRECISION_PARAMETER,
  FLAGS_BASE_PARAMETER      = 2 * FLAGS_BASE,
  FLAGS_FLOAT_E             = 2 * FLAGS_BASE_PARAMETER,
  FLAGS_FLOAT_G             = 2 * FLAGS_FLOAT_E,
  FLAGS_QUOTE               = 2 * FLAGS_FLOAT_G,
  FLAGS_WIDECHAR            = 2 * FLAGS_QUOTE,
  FLAGS_IGNORE              = 2 * FLAGS_WIDECHAR,
  FLAGS_IGNORE_PARAMETER    = 2 * FLAGS_IGNORE,
  FLAGS_VARSIZE_PARAMETER   = 2 * FLAGS_IGNORE_PARAMETER,
  FLAGS_FIXED_SIZE          = 2 * FLAGS_VARSIZE_PARAMETER,
  FLAGS_LAST                = FLAGS_FIXED_SIZE,
  /* Reused flags */
  FLAGS_EXCLUDE             = FLAGS_SHORT,
  FLAGS_USER_DEFINED        = FLAGS_IGNORE,
  FLAGS_USER_DEFINED_PARAMETER = FLAGS_IGNORE_PARAMETER,
  FLAGS_ROUNDING            = FLAGS_INTMAX_T,
  /* Compounded flags */
  FLAGS_ALL_VARSIZES        = FLAGS_LONG | FLAGS_QUAD | FLAGS_INTMAX_T | FLAGS_PTRDIFF_T | FLAGS_SIZE_T,
  FLAGS_ALL_SIZES           = FLAGS_ALL_VARSIZES | FLAGS_SHORTSHORT | FLAGS_SHORT,

  NO_POSITION  = -1,
  NO_WIDTH     =  0,
  NO_PRECISION = -1,
  NO_SIZE      = -1,

  /* Do not change these */
  NO_BASE      = -1,
  MIN_BASE     =  2,
  MAX_BASE     = 36,
  BASE_BINARY  =  2,
  BASE_OCTAL   =  8,
  BASE_DECIMAL = 10,
  BASE_HEX     = 16,

  /* Maximal number of allowed parameters */
  MAX_PARAMETERS = 64,
  /* Maximal number of characters in class */
  MAX_CHARACTER_CLASS = UCHAR_MAX + 1,

#if TRIO_FEATURE_USER_DEFINED
  /* Maximal string lengths for user-defined specifiers */
  MAX_USER_NAME = 64,
  MAX_USER_DATA = 256,
#endif
  
  /* Maximal length of locale separator strings */
  MAX_LOCALE_SEPARATOR_LENGTH = MB_LEN_MAX,
  /* Maximal number of integers in grouping */
  MAX_LOCALE_GROUPS = 64
};

#define NO_GROUPING ((int)CHAR_MAX)

/* Fundamental formatting parameter types */
#define FORMAT_SENTINEL  -1	/* marks end of parameters array */
#define FORMAT_UNKNOWN   0
#define FORMAT_INT       1
#define FORMAT_DOUBLE    2
#define FORMAT_CHAR      3
#define FORMAT_STRING    4
#define FORMAT_POINTER   5
#define FORMAT_COUNT     6
#define FORMAT_PARAMETER 7
#define FORMAT_GROUP     8
#define FORMAT_ERRNO     9
#define FORMAT_USER_DEFINED 10

/* Character constants */
#define CHAR_IDENTIFIER '%'
#define CHAR_ALT_IDENTIFIER '$'
#define CHAR_BACKSLASH '\\'
#define CHAR_QUOTE '\"'
#define CHAR_ADJUST ' '

#if TRIO_EXTENSION
/* Character class expressions */
# define CLASS_ALNUM "[:alnum:]"
# define CLASS_ALPHA "[:alpha:]"
# define CLASS_BLANK "[:blank:]"
# define CLASS_CNTRL "[:cntrl:]"
# define CLASS_DIGIT "[:digit:]"
# define CLASS_GRAPH "[:graph:]"
# define CLASS_LOWER "[:lower:]"
# define CLASS_PRINT "[:print:]"
# define CLASS_PUNCT "[:punct:]"
# define CLASS_SPACE "[:space:]"
# define CLASS_UPPER "[:upper:]"
# define CLASS_XDIGIT "[:xdigit:]"
#endif

/*
 * SPECIFIERS:
 *
 *
 * a  Hex-float
 * A  Hex-float
 * c  Character
 * C  Widechar character (wint_t)
 * d  Decimal
 * e  Float
 * E  Float
 * F  Float
 * F  Float
 * g  Float
 * G  Float
 * i  Integer
 * m  Error message
 * n  Count
 * o  Octal
 * p  Pointer
 * s  String
 * S  Widechar string (wchar_t *)
 * u  Unsigned
 * x  Hex
 * X  Hex
 * [] Group
 * <> User-defined
 *
 * Reserved:
 *
 * D  Binary Coded Decimal %D(length,precision) (OS/390)
 */
#define SPECIFIER_CHAR 'c'
#define SPECIFIER_STRING 's'
#define SPECIFIER_DECIMAL 'd'
#define SPECIFIER_INTEGER 'i'
#define SPECIFIER_UNSIGNED 'u'
#define SPECIFIER_OCTAL 'o'
#define SPECIFIER_HEX 'x'
#define SPECIFIER_HEX_UPPER 'X'
#if TRIO_FEATURE_FLOAT
# define SPECIFIER_FLOAT_E 'e'
# define SPECIFIER_FLOAT_E_UPPER 'E'
# define SPECIFIER_FLOAT_F 'f'
# define SPECIFIER_FLOAT_F_UPPER 'F'
# define SPECIFIER_FLOAT_G 'g'
# define SPECIFIER_FLOAT_G_UPPER 'G'
#endif
#define SPECIFIER_POINTER 'p'
#if TRIO_FEATURE_SCANF
# define SPECIFIER_GROUP '['
# define SPECIFIER_UNGROUP ']'
#endif
#define SPECIFIER_COUNT 'n'
#if TRIO_UNIX98
# define SPECIFIER_CHAR_UPPER 'C'
# define SPECIFIER_STRING_UPPER 'S'
#endif
#define SPECIFIER_HEXFLOAT 'a'
#define SPECIFIER_HEXFLOAT_UPPER 'A'
#define SPECIFIER_ERRNO 'm'
#if TRIO_FEATURE_BINARY
# define SPECIFIER_BINARY 'b'
# define SPECIFIER_BINARY_UPPER 'B'
#endif
#if TRIO_FEATURE_USER_DEFINED
# define SPECIFIER_USER_DEFINED_BEGIN '<'
# define SPECIFIER_USER_DEFINED_END '>'
# define SPECIFIER_USER_DEFINED_SEPARATOR ':'
# define SPECIFIER_USER_DEFINED_EXTRA '|'
#endif

/*
 * QUALIFIERS:
 *
 *
 * Numbers = d,i,o,u,x,X
 * Float = a,A,e,E,f,F,g,G
 * String = s
 * Char = c
 *
 *
 * 9$ Position
 *      Use the 9th parameter. 9 can be any number between 1 and
 *      the maximal argument
 *
 * 9 Width
 *      Set width to 9. 9 can be any number, but must not be postfixed
 *      by '$'
 *
 * h  Short
 *    Numbers:
 *      (unsigned) short int
 *
 * hh Short short
 *    Numbers:
 *      (unsigned) char
 *
 * l  Long
 *    Numbers:
 *      (unsigned) long int
 *    String:
 *      as the S specifier
 *    Char:
 *      as the C specifier
 *
 * ll Long Long
 *    Numbers:
 *      (unsigned) long long int
 *
 * L  Long Double
 *    Float
 *      long double
 *
 * #  Alternative
 *    Float:
 *      Decimal-point is always present
 *    String:
 *      non-printable characters are handled as \number
 *
 *    Spacing
 *
 * +  Sign
 *
 * -  Alignment
 *
 * .  Precision
 *
 * *  Parameter
 *    print: use parameter
 *    scan: no parameter (ignore)
 *
 * q  Quad
 *
 * Z  size_t
 *
 * w  Widechar
 *
 * '  Thousands/quote
 *    Numbers:
 *      Integer part grouped in thousands
 *    Binary numbers:
 *      Number grouped in nibbles (4 bits)
 *    String:
 *      Quoted string
 *
 * j  intmax_t
 * t  prtdiff_t
 * z  size_t
 *
 * !  Sticky
 * @  Parameter (for both print and scan)
 *
 * I  n-bit Integer
 *    Numbers:
 *      The following options exists
 *        I8  = 8-bit integer
 *        I16 = 16-bit integer
 *        I32 = 32-bit integer
 *        I64 = 64-bit integer
 */
#define QUALIFIER_POSITION '$'
#define QUALIFIER_SHORT 'h'
#define QUALIFIER_LONG 'l'
#define QUALIFIER_LONG_UPPER 'L'
#define QUALIFIER_ALTERNATIVE '#'
#define QUALIFIER_SPACE ' '
#define QUALIFIER_PLUS '+'
#define QUALIFIER_MINUS '-'
#define QUALIFIER_DOT '.'
#define QUALIFIER_STAR '*'
#define QUALIFIER_CIRCUMFLEX '^' /* For scanlists */
#define QUALIFIER_SIZE_T 'z'
#define QUALIFIER_PTRDIFF_T 't'
#define QUALIFIER_INTMAX_T 'j'
#define QUALIFIER_QUAD 'q'
#define QUALIFIER_SIZE_T_UPPER 'Z'
#if TRIO_MISC
# define QUALIFIER_WIDECHAR 'w'
#endif
#define QUALIFIER_FIXED_SIZE 'I'
#define QUALIFIER_QUOTE '\''
#define QUALIFIER_STICKY '!'
#define QUALIFIER_VARSIZE '&' /* This should remain undocumented */
#define QUALIFIER_ROUNDING_UPPER 'R'
#if TRIO_EXTENSION
# define QUALIFIER_PARAM '@' /* Experimental */
# define QUALIFIER_COLON ':' /* For scanlists */
# define QUALIFIER_EQUAL '=' /* For scanlists */
#endif


/*************************************************************************
 *
 * Internal Structures
 *
 *************************************************************************/

/* Parameters */
typedef struct {
  /* An indication of which entry in the data union is used */
  int type;
  /* The flags */
  trio_flags_t flags;
  /* The width qualifier */
  int width;
  /* The precision qualifier */
  int precision;
  /* The base qualifier */
  int base;
  /* Base from specifier */
  int baseSpecifier;
  /* The size for the variable size qualifier */
  int varsize;
  /* Offset of the first character of the specifier */
  int beginOffset;
  /* Offset of the first character after the specifier */
  int endOffset;
  /* Position in the argument list that this parameter refers to */
  int position;
  /* The data from the argument list */
  union {
    char *string;
#if TRIO_FEATURE_WIDECHAR
    trio_wchar_t *wstring;
#endif
    trio_pointer_t pointer;
    union {
      trio_intmax_t as_signed;
      trio_uintmax_t as_unsigned;
    } number;
#if TRIO_FEATURE_FLOAT
    double doubleNumber;
    double *doublePointer;
    trio_long_double_t longdoubleNumber;
    trio_long_double_t *longdoublePointer;
#endif
    int errorNumber;
  } data;
#if TRIO_FEATURE_USER_DEFINED
  /* For the user-defined specifier */
  union {
    char namespace[MAX_USER_NAME];
    int handler;        /* if flags & FLAGS_USER_DEFINED_PARAMETER */
  } user_defined;
  char user_data[MAX_USER_DATA];
#endif
} trio_parameter_t;

/* Container for customized functions */
typedef struct {
  union {
    trio_outstream_t out;
    trio_instream_t in;
  } stream;
  trio_pointer_t closure;
} trio_custom_t;

/* General trio "class" */
typedef struct _trio_class_t {
  /*
   * The function to write characters to a stream.
   */
  void (*OutStream) TRIO_PROTO((struct _trio_class_t *, int));
  /*
   * The function to read characters from a stream.
   */
  void (*InStream) TRIO_PROTO((struct _trio_class_t *, int *));
  /*
   * The function to undo read characters from a stream.
   */
  void (*UndoStream) TRIO_PROTO((struct _trio_class_t *));
  /*
   * The current location in the stream.
   */
  trio_pointer_t location;
  /*
   * The character currently being processed.
   */
  int current;
  /*
   * The number of characters that would have been written/read
   * if there had been sufficient space.
   */
  int processed;
  union {
    /*
     * The number of characters that are actually written. Processed and
     * committed will only differ for the *nprintf functions.
     */
    int committed;
    /*
     * The number of look-ahead characters read.
     */
    int cached;
  } actually;
  /*
   * The upper limit of characters that may be written/read.
   */
  int max;
  /*
   * The last output error that was detected.
   */
  int error;
} trio_class_t;

/* References (for user-defined callbacks) */
typedef struct _trio_reference_t {
  trio_class_t *data;
  trio_parameter_t *parameter;
} trio_reference_t;

#if TRIO_FEATURE_USER_DEFINED
/* Registered entries (for user-defined callbacks) */
typedef struct _trio_userdef_t {
  struct _trio_userdef_t *next;
  trio_callback_t callback;
  char *name;
} trio_userdef_t;
#endif

/*************************************************************************
 *
 * Internal Variables
 *
 *************************************************************************/

////////////////////////////////////////////////////////////////////////////////
// NOTE: string: comment out unused variable as it produces warning on clang
////////////////////////////////////////////////////////////////////////////////

// static TRIO_CONST char rcsid[] = "@(#)$Id$";

#if TRIO_FEATURE_FLOAT
/*
 * Need this to workaround a parser bug in HP C/iX compiler that fails
 * to resolves macro definitions that includes type 'long double',
 * e.g: va_arg(arg_ptr, long double)
 */
# if defined(TRIO_PLATFORM_MPEIX)
static TRIO_CONST trio_long_double_t ___dummy_long_double = 0;
# endif
#endif

static TRIO_CONST char internalNullString[] = "(nil)";

#if defined(USE_LOCALE)
static struct lconv *internalLocaleValues = NULL;
#endif

/*
 * UNIX98 says "in a locale where the radix character is not defined,
 * the radix character defaults to a period (.)"
 */
#if TRIO_FEATURE_FLOAT || TRIO_FEATURE_LOCALE || defined(USE_LOCALE)
static int internalDecimalPointLength = 1;
static char internalDecimalPoint = '.';
static char internalDecimalPointString[MAX_LOCALE_SEPARATOR_LENGTH + 1] = ".";
#endif
#if TRIO_FEATURE_QUOTE || TRIO_FEATURE_LOCALE || TRIO_EXTENSION
static int internalThousandSeparatorLength = 1;
static char internalThousandSeparator[MAX_LOCALE_SEPARATOR_LENGTH + 1] = ",";
static char internalGrouping[MAX_LOCALE_GROUPS] = { (char)NO_GROUPING };
#endif

static TRIO_CONST char internalDigitsLower[] = "0123456789abcdefghijklmnopqrstuvwxyz";
static TRIO_CONST char internalDigitsUpper[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
#if TRIO_FEATURE_SCANF
static BOOLEAN_T internalDigitsUnconverted = TRUE;
static int internalDigitArray[128];
# if TRIO_EXTENSION
static BOOLEAN_T internalCollationUnconverted = TRUE;
static char internalCollationArray[MAX_CHARACTER_CLASS][MAX_CHARACTER_CLASS];
# endif
#endif

#if TRIO_FEATURE_USER_DEFINED
static TRIO_VOLATILE trio_callback_t internalEnterCriticalRegion = NULL;
static TRIO_VOLATILE trio_callback_t internalLeaveCriticalRegion = NULL;
static trio_userdef_t *internalUserDef = NULL;
#endif


/*************************************************************************
 *
 * Internal Functions
 *
 ************************************************************************/

////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: Embedded trionan.c
////////////////////////////////////////////////////////////////////////////////////////////////////

#include <math.h>
#include <string.h>
#include <limits.h>
#if !defined(TRIO_PLATFORM_SYMBIAN)
# include <float.h>
#endif
#if defined(TRIO_PLATFORM_UNIX)
# include <signal.h>
#endif
#if defined(TRIO_COMPILER_DECC)
# include <fp_class.h>
#endif
#include <assert.h>

#if defined(TRIO_DOCUMENTATION)
# include "doc/doc_nan.h"
#endif
/** @addtogroup SpecialQuantities
    @{
*/

/*************************************************************************
 * Definitions
 */

#if !defined(TRIO_PUBLIC_NAN)
# define TRIO_PUBLIC_NAN TRIO_PUBLIC
#endif
#if !defined(TRIO_PRIVATE_NAN)
# define TRIO_PRIVATE_NAN TRIO_PRIVATE
#endif

#define TRIO_TRUE (1 == 1)
#define TRIO_FALSE (0 == 1)

/*
 * We must enable IEEE floating-point on Alpha
 */
#if defined(__alpha) && !defined(_IEEE_FP)
# if defined(TRIO_COMPILER_DECC)
#  if defined(TRIO_PLATFORM_VMS)
#   error "Must be compiled with option /IEEE_MODE=UNDERFLOW_TO_ZERO/FLOAT=IEEE"
#  else
#   if !defined(_CFE)
#    error "Must be compiled with option -ieee"
#   endif
#  endif
# else
#  if defined(TRIO_COMPILER_GCC)
#   error "Must be compiled with option -mieee"
#  endif
# endif
#endif /* __alpha && ! _IEEE_FP */

/*
 * In ANSI/IEEE 754-1985 64-bits double format numbers have the
 * following properties (amoungst others)
 *
 *   o FLT_RADIX == 2: binary encoding
 *   o DBL_MAX_EXP == 1024: 11 bits exponent, where one bit is used
 *     to indicate special numbers (e.g. NaN and Infinity), so the
 *     maximum exponent is 10 bits wide (2^10 == 1024).
 *   o DBL_MANT_DIG == 53: The mantissa is 52 bits wide, but because
 *     numbers are normalized the initial binary 1 is represented
 *     implicitly (the so-called "hidden bit"), which leaves us with
 *     the ability to represent 53 bits wide mantissa.
 */
#if defined(__STDC_IEC_559__)
# define TRIO_IEEE_754
#else
# if (FLT_RADIX - 0 == 2) && (DBL_MAX_EXP - 0 == 1024) && (DBL_MANT_DIG - 0 == 53)
#  define TRIO_IEEE_754
# endif
#endif

/*
 * Determine which fpclassify_and_sign() function to use.
 */
#if defined(TRIO_FUNC_FPCLASSIFY_AND_SIGNBIT)
# if defined(PREDEF_STANDARD_C99) && defined(fpclassify)
#  define TRIO_FUNC_C99_FPCLASSIFY_AND_SIGNBIT
# else
#  if defined(TRIO_COMPILER_DECC)
#   define TRIO_FUNC_DECC_FPCLASSIFY_AND_SIGNBIT
#  else
#   if defined(TRIO_COMPILER_VISUALC) || defined(TRIO_COMPILER_BORLAND)
#    define TRIO_FUNC_MS_FPCLASSIFY_AND_SIGNBIT
#   else
#    if defined(TRIO_COMPILER_HP) && defined(FP_PLUS_NORM)
#     define TRIO_FUNC_HP_FPCLASSIFY_AND_SIGNBIT
#    else
#     if defined(TRIO_COMPILER_XLC) && defined(FP_PLUS_NORM)
#      define TRIO_FUNC_XLC_FPCLASSIFY_AND_SIGNBIT
#     else
#      define TRIO_FUNC_INTERNAL_FPCLASSIFY_AND_SIGNBIT
#     endif
#    endif
#   endif
#  endif
# endif
#endif

/*
 * Determine how to generate negative zero.
 */
#if defined(TRIO_FUNC_NZERO)
# if defined(TRIO_IEEE_754)
#  define TRIO_NZERO_IEEE_754
# else
#  define TRIO_NZERO_FALLBACK
# endif
#endif

/*
 * Determine how to generate positive infinity.
 */
#if defined(TRIO_FUNC_PINF)
# if defined(INFINITY) && defined(__STDC_IEC_559__)
#  define TRIO_PINF_C99_MACRO
# else
#  if defined(TRIO_IEEE_754)
#   define TRIO_PINF_IEEE_754
#  else
#   define TRIO_PINF_FALLBACK
#  endif
# endif
#endif

/*
 * Determine how to generate NaN.
 */
#if defined(TRIO_FUNC_NAN)
# if defined(PREDEF_STANDARD_C99) && !defined(TRIO_COMPILER_DECC)
#  define TRIO_NAN_C99_FUNCTION
# else
#  if defined(NAN) && defined(__STDC_IEC_559__)
#   define TRIO_NAN_C99_MACRO
#  else
#   if defined(TRIO_IEEE_754)
#    define TRIO_NAN_IEEE_754
#   else
#    define TRIO_NAN_FALLBACK
#   endif
#  endif
# endif
#endif

/*
 * Resolve internal dependencies.
 */
#if defined(TRIO_FUNC_INTERNAL_FPCLASSIFY_AND_SIGNBIT)
# define TRIO_FUNC_INTERNAL_ISNAN
# define TRIO_FUNC_INTERNAL_ISINF
# if defined(TRIO_IEEE_754)
#  define TRIO_FUNC_INTERNAL_IS_SPECIAL_QUANTITY
#  define TRIO_FUNC_INTERNAL_IS_NEGATIVE
# endif
#endif

#if defined(TRIO_NZERO_IEEE_754) \
 || defined(TRIO_PINF_IEEE_754) \
 || defined(TRIO_NAN_IEEE_754)
# define TRIO_FUNC_INTERNAL_MAKE_DOUBLE
#endif

#if defined(TRIO_FUNC_INTERNAL_ISNAN)
# if defined(PREDEF_STANDARD_XPG3)
#  define TRIO_INTERNAL_ISNAN_XPG3
# else
#  if defined(TRIO_IEEE_754)
#   define TRIO_INTERNAL_ISNAN_IEEE_754
#  else
#   define TRIO_INTERNAL_ISNAN_FALLBACK
#  endif
# endif
#endif

#if defined(TRIO_FUNC_INTERNAL_ISINF)
# if defined(TRIO_IEEE_754)
#  define TRIO_INTERNAL_ISINF_IEEE_754
# else
#  define TRIO_INTERNAL_ISINF_FALLBACK
# endif
#endif

/*************************************************************************
 * Constants
 */

#if !defined(TRIO_EMBED_NAN)
static TRIO_CONST char rcsid[] = "@(#)$Id$";
#endif

#if defined(TRIO_FUNC_INTERNAL_MAKE_DOUBLE) \
 || defined(TRIO_FUNC_INTERNAL_IS_SPECIAL_QUANTITY) \
 || defined(TRIO_FUNC_INTERNAL_IS_NEGATIVE)
/*
 * Endian-agnostic indexing macro.
 *
 * The value of internalEndianMagic, when converted into a 64-bit
 * integer, becomes 0x0706050403020100 (we could have used a 64-bit
 * integer value instead of a double, but not all platforms supports
 * that type). The value is automatically encoded with the correct
 * endianess by the compiler, which means that we can support any
 * kind of endianess. The individual bytes are then used as an index
 * for the IEEE 754 bit-patterns and masks.
 */
#define TRIO_DOUBLE_INDEX(x) (((unsigned char *)&internalEndianMagic)[7-(x)])
static TRIO_CONST double internalEndianMagic = 7.949928895127363e-275;
#endif

#if defined(TRIO_FUNC_INTERNAL_IS_SPECIAL_QUANTITY)
/* Mask for the exponent */
static TRIO_CONST unsigned char ieee_754_exponent_mask[] = {
  0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Mask for the mantissa */
static TRIO_CONST unsigned char ieee_754_mantissa_mask[] = {
  0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
#endif

#if defined(TRIO_FUNC_INTERNAL_IS_NEGATIVE)
/* Mask for the sign bit */
static TRIO_CONST unsigned char ieee_754_sign_mask[] = {
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#endif

#if defined(TRIO_NZERO_IEEE_754)
/* Bit-pattern for negative zero */
static TRIO_CONST unsigned char ieee_754_negzero_array[] = {
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#endif

#if defined(TRIO_PINF_IEEE_754)
/* Bit-pattern for infinity */
static TRIO_CONST unsigned char ieee_754_infinity_array[] = {
  0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#endif

#if defined(TRIO_NAN_IEEE_754)
/* Bit-pattern for quiet NaN */
static TRIO_CONST unsigned char ieee_754_qnan_array[] = {
  0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#endif


/*************************************************************************
 * Internal functions
 */

/*
 *
 */
#if defined(TRIO_PLATFORM_UNIX) && defined(TRIO_INTERNAL_ISNAN_FALLBACK)

/* Assume that if SA_SIGINFO is defined, then sigaction() and
 * 'struct sigaction' are also properly defined on this platform.
 */
#ifndef TRIO_USE_SIGACTION
#  ifdef SA_SIGINFO
#    define TRIO_USE_SIGACTION 1
#  else
#    define TRIO_USE_SIGACTION 0
#  endif
#endif

#  if TRIO_USE_SIGACTION
typedef struct sigaction signal_handler_t;
#  else
typedef void (*signal_handler_t) TRIO_PROTO((int));
#  endif

/*
 * internal_ignore_signal_handler
 */

TRIO_PRIVATE_NAN signal_handler_t
internal_ignore_signal_handler
TRIO_ARGS1((signum),
           int signum)
{
#  if TRIO_USE_SIGACTION
  signal_handler_t old_handler, new_handler;
  memset(&new_handler, '\0', sizeof(new_handler));
  new_handler.sa_handler = SIG_IGN;
  new_handler.sa_flags = SA_RESTART;
  sigaction(signum, &new_handler, &old_handler);
  return old_handler;
#  else
  return signal(signum, SIG_IGN);
#  endif
}

/*
 * internal_restore_signal_handler
 */
TRIO_PRIVATE_NAN void
internal_restore_signal_handler
TRIO_ARGS2((signum, handler),
           int signum,
           signal_handler_t handler)
{
#  if TRIO_USE_SIGACTION
    sigaction(signum, &handler, NULL);
#  else
    signal(signum, handler);
#  endif
}

#endif

/*
 * internal_make_double
 */
#if defined(TRIO_FUNC_INTERNAL_MAKE_DOUBLE)

TRIO_PRIVATE_NAN double
internal_make_double
TRIO_ARGS1((values),
       TRIO_CONST unsigned char *values)
{
  TRIO_VOLATILE double result;
  int i;

  for (i = 0; i < (int)sizeof(double); i++) {
    ((TRIO_VOLATILE unsigned char *)&result)[TRIO_DOUBLE_INDEX(i)] = values[i];
  }
  return result;
}

#endif

/*
 * internal_is_special_quantity
 */
#if defined(TRIO_FUNC_INTERNAL_IS_SPECIAL_QUANTITY)

TRIO_PRIVATE_NAN int
internal_is_special_quantity
TRIO_ARGS2((number, has_mantissa),
       double number,
       int *has_mantissa)
{
  unsigned int i;
  unsigned char current;
  int is_special_quantity = TRIO_TRUE;

  *has_mantissa = 0;

  for (i = 0; i < (unsigned int)sizeof(double); i++) {
    current = ((unsigned char *)&number)[TRIO_DOUBLE_INDEX(i)];
    is_special_quantity
      &= ((current & ieee_754_exponent_mask[i]) == ieee_754_exponent_mask[i]);
    *has_mantissa |= (current & ieee_754_mantissa_mask[i]);
  }
  return is_special_quantity;
}

#endif

/*
 * internal_is_negative
 */
#if defined(TRIO_FUNC_INTERNAL_IS_NEGATIVE)

TRIO_PRIVATE_NAN int
internal_is_negative
TRIO_ARGS1((number),
       double number)
{
  unsigned int i;
  int is_negative = TRIO_FALSE;

  for (i = 0; i < (unsigned int)sizeof(double); i++) {
    is_negative |= (((unsigned char *)&number)[TRIO_DOUBLE_INDEX(i)]
            & ieee_754_sign_mask[i]);
  }
  return is_negative;
}

#endif

#if defined(TRIO_FUNC_C99_FPCLASSIFY_AND_SIGNBIT)

TRIO_PRIVATE_NAN TRIO_INLINE int
c99_fpclassify_and_signbit
TRIO_ARGS2((number, is_negative),
       double number,
       int *is_negative)
{
  *is_negative = signbit(number);
  switch (fpclassify(number)) {
  case FP_NAN:
    return TRIO_FP_NAN;
  case FP_INFINITE:
    return TRIO_FP_INFINITE;
  case FP_SUBNORMAL:
    return TRIO_FP_SUBNORMAL;
  case FP_ZERO:
    return TRIO_FP_ZERO;
  default:
    return TRIO_FP_NORMAL;
  }
}

#endif /* TRIO_FUNC_C99_FPCLASSIFY_AND_SIGNBIT */

#if defined(TRIO_FUNC_DECC_FPCLASSIFY_AND_SIGNBIT)

TRIO_PRIVATE_NAN TRIO_INLINE int
decc_fpclassify_and_signbit
TRIO_ARGS2((number, is_negative),
      double number,
      int *is_negative)
{
  switch (fp_class(number)) {
  case FP_QNAN:
  case FP_SNAN:
    *is_negative = TRIO_FALSE; /* NaN has no sign */
    return TRIO_FP_NAN;
  case FP_POS_INF:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_INFINITE;
  case FP_NEG_INF:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_INFINITE;
  case FP_POS_DENORM:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_SUBNORMAL;
  case FP_NEG_DENORM:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_SUBNORMAL;
  case FP_POS_ZERO:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_ZERO;
  case FP_NEG_ZERO:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_ZERO;
  case FP_POS_NORM:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_NORMAL;
  case FP_NEG_NORM:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_NORMAL;
  default:
    *is_negative = (number < 0.0);
    return TRIO_FP_NORMAL;
  }
}

#endif /* TRIO_FUNC_DECC_FPCLASSIFY_AND_SIGNBIT */

#if defined(TRIO_FUNC_MS_FPCLASSIFY_AND_SIGNBIT)

TRIO_PRIVATE_NAN int
ms_fpclassify_and_signbit
TRIO_ARGS2((number, is_negative),
      double number,
      int *is_negative)
{
  int result;
# if defined(TRIO_COMPILER_BORLAND)
  /*
   * The floating-point precision may be changed by the Borland _fpclass()
   * function, so we have to save and restore the floating-point control mask.
   */
  unsigned int mask;
  /* Remember the old mask */
  mask = _control87(0, 0);
# endif

  switch (_fpclass(number)) {
  case _FPCLASS_QNAN:
  case _FPCLASS_SNAN:
    *is_negative = TRIO_FALSE; /* NaN has no sign */
    result = TRIO_FP_NAN;
    break;
  case _FPCLASS_PINF:
    *is_negative = TRIO_FALSE;
    result = TRIO_FP_INFINITE;
    break;
  case _FPCLASS_NINF:
    *is_negative = TRIO_TRUE;
    result = TRIO_FP_INFINITE;
    break;
  case _FPCLASS_PD:
    *is_negative = TRIO_FALSE;
    result = TRIO_FP_SUBNORMAL;
    break;
  case _FPCLASS_ND:
    *is_negative = TRIO_TRUE;
    result = TRIO_FP_SUBNORMAL;
    break;
  case _FPCLASS_PZ:
    *is_negative = TRIO_FALSE;
    result = TRIO_FP_ZERO;
    break;
  case _FPCLASS_NZ:
    *is_negative = TRIO_TRUE;
    result = TRIO_FP_ZERO;
    break;
  case _FPCLASS_PN:
    *is_negative = TRIO_FALSE;
    result = TRIO_FP_NORMAL;
    break;
  case _FPCLASS_NN:
    *is_negative = TRIO_TRUE;
    result = TRIO_FP_NORMAL;
    break;
  default:
    *is_negative = (number < 0.0);
    result = TRIO_FP_NORMAL;
    break;
  }

# if defined(TRIO_COMPILER_BORLAND)
  /* Restore the old precision */
  (void)_control87(mask, MCW_PC);
# endif

  return result;
}

#endif /* TRIO_FUNC_MS_FPCLASSIFY_AND_SIGNBIT */

#if defined(TRIO_FUNC_HP_FPCLASSIFY_AND_SIGNBIT)

TRIO_PRIVATE_NAN TRIO_INLINE int
hp_fpclassify_and_signbit
TRIO_ARGS2((number, is_negative),
      double number,
      int *is_negative)
{
  /*
   * HP-UX 9.x and 10.x have an fpclassify() function, that is different
   * from the C99 fpclassify() macro supported on HP-UX 11.x.
   */
  switch (fpclassify(number)) {
  case FP_QNAN:
  case FP_SNAN:
    *is_negative = TRIO_FALSE; /* NaN has no sign */
    return TRIO_FP_NAN;
  case FP_PLUS_INF:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_INFINITE;
  case FP_MINUS_INF:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_INFINITE;
  case FP_PLUS_DENORM:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_SUBNORMAL;
  case FP_MINUS_DENORM:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_SUBNORMAL;
  case FP_PLUS_ZERO:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_ZERO;
  case FP_MINUS_ZERO:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_ZERO;
  case FP_PLUS_NORM:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_NORMAL;
  case FP_MINUS_NORM:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_NORMAL;
  default:
    *is_negative = (number < 0.0);
    return TRIO_FP_NORMAL;
  }
}

#endif /* TRIO_FUNC_HP_FPCLASSIFY_AND_SIGNBIT */

#if defined(TRIO_FUNC_XLC_FPCLASSIFY_AND_SIGNBIT)

TRIO_PRIVATE_NAN TRIO_INLINE int
xlc_fpclassify_and_signbit
TRIO_ARGS2((number, is_negative),
      double number,
      int *is_negative)
{
  /*
   * AIX has class() for C, and _class() for C++
   */
# if defined(__cplusplus)
#  define AIX_CLASS(n) _class(n)
# else
#  define AIX_CLASS(n) class(n)
# endif

  switch (AIX_CLASS(number)) {
  case FP_QNAN:
  case FP_SNAN:
    *is_negative = TRIO_FALSE; /* NaN has no sign */
    return TRIO_FP_NAN;
  case FP_PLUS_INF:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_INFINITE;
  case FP_MINUS_INF:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_INFINITE;
  case FP_PLUS_DENORM:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_SUBNORMAL;
  case FP_MINUS_DENORM:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_SUBNORMAL;
  case FP_PLUS_ZERO:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_ZERO;
  case FP_MINUS_ZERO:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_ZERO;
  case FP_PLUS_NORM:
    *is_negative = TRIO_FALSE;
    return TRIO_FP_NORMAL;
  case FP_MINUS_NORM:
    *is_negative = TRIO_TRUE;
    return TRIO_FP_NORMAL;
  default:
    *is_negative = (number < 0.0);
    return TRIO_FP_NORMAL;
  }
}

#endif /* TRIO_FUNC_XLC_FPCLASSIFY_AND_SIGNBIT */

#if defined(TRIO_FUNC_INTERNAL_ISNAN)

TRIO_PRIVATE_NAN TRIO_INLINE int
internal_isnan
TRIO_ARGS1((number),
       double number)
{
# if defined(TRIO_INTERNAL_ISNAN_XPG3) || defined(TRIO_PLATFORM_SYMBIAN)
  /*
   * XPG3 defines isnan() as a function.
   */
  return isnan(number);

# endif

# if defined(TRIO_INTERNAL_ISNAN_IEEE_754)

  /*
   * Examine IEEE 754 bit-pattern. A NaN must have a special exponent
   * pattern, and a non-empty mantissa.
   */
  int has_mantissa;
  int is_special_quantity;

  is_special_quantity = internal_is_special_quantity(number, &has_mantissa);

  return (is_special_quantity && has_mantissa);

# endif

# if defined(TRIO_INTERNAL_ISNAN_FALLBACK)

  /*
   * Fallback solution
   */
  int status;
  double integral, fraction;

#  if defined(TRIO_PLATFORM_UNIX)
  signal_handler_t sigfpe_handler = internal_ignore_signal_handler(SIGFPE);
#  endif

  status = (/*
         * NaN is the only number which does not compare to itself
         */
        ((TRIO_VOLATILE double)number != (TRIO_VOLATILE double)number) ||
        /*
         * Fallback solution if NaN compares to NaN
         */
        ((number != 0.0) &&
         (fraction = modf(number, &integral),
          integral == fraction)));

#  if defined(TRIO_PLATFORM_UNIX)
  internal_restore_signal_handler(SIGFPR, sigfpe_handler);
#  endif

  return status;

# endif
}

#endif /* TRIO_FUNC_INTERNAL_ISNAN */

#if defined(TRIO_FUNC_INTERNAL_ISINF)

TRIO_PRIVATE_NAN TRIO_INLINE int
internal_isinf
TRIO_ARGS1((number),
       double number)
{
# if defined(TRIO_PLATFORM_SYMBIAN)

  return isinf(number);

# endif

# if defined(TRIO_INTERNAL_ISINF_IEEE_754)
  /*
   * Examine IEEE 754 bit-pattern. Infinity must have a special exponent
   * pattern, and an empty mantissa.
   */
  int has_mantissa;
  int is_special_quantity;

  is_special_quantity = internal_is_special_quantity(number, &has_mantissa);

  return (is_special_quantity && !has_mantissa)
    ? ((number < 0.0) ? -1 : 1)
    : 0;

# endif

# if defined(TRIO_INTERNAL_ISINF_FALLBACK)

  /*
   * Fallback solution.
   */
  int status;

#  if defined(TRIO_PLATFORM_UNIX)
  signal_handler_t sigfpe_handler = internal_ignore_signal_handler(SIGFPE);
#  endif

  double infinity = trio_pinf();

  status = ((number == infinity)
        ? 1
        : ((number == -infinity) ? -1 : 0));

#  if defined(TRIO_PLATFORM_UNIX)
  internal_restore_signal_handler(SIGFPE, sigfpe_handler);
#  endif

  return status;

# endif
}

#endif /* TRIO_FUNC_INTERNAL_ISINF */

/*************************************************************************
 * Public functions
 */

#if defined(TRIO_FUNC_FPCLASSIFY_AND_SIGNBIT)

TRIO_PUBLIC_NAN int
trio_fpclassify_and_signbit
TRIO_ARGS2((number, is_negative),
       double number,
       int *is_negative)
{
  /* The TRIO_FUNC_xxx_FPCLASSIFY_AND_SIGNBIT macros are mutually exclusive */

#if defined(TRIO_FUNC_C99_FPCLASSIFY_AND_SIGNBIT)

  return c99_fpclassify_and_signbit(number, is_negative);

#endif

#if defined(TRIO_FUNC_DECC_FPCLASSIFY_AND_SIGNBIT)

  return decc_fpclassify_and_signbit(number, is_negative);

#endif

#if defined(TRIO_FUNC_MS_FPCLASSIFY_AND_SIGNBIT)

  return ms_fpclassify_and_signbit(number, is_negative);

#endif

#if defined(TRIO_FUNC_HP_FPCLASSIFY_AND_SIGNBIT)

  return hp_fpclassify_and_signbit(number, is_negative);

#endif

#if defined(TRIO_FUNC_XLC_FPCLASSIFY_AND_SIGNBIT)

  return xlc_fpclassify_and_signbit(number, is_negative);

#endif

#if defined(TRIO_FUNC_INTERNAL_FPCLASSIFY_AND_SIGNBIT)

  /*
   * Fallback solution.
   */
  int rc;

  if (number == 0.0) {
    /*
     * In IEEE 754 the sign of zero is ignored in comparisons, so we
     * have to handle this as a special case by examining the sign bit
     * directly.
     */
# if defined(TRIO_IEEE_754)
    *is_negative = internal_is_negative(number);
# else
    *is_negative = TRIO_FALSE; /* FIXME */
# endif
    return TRIO_FP_ZERO;
  }
  if (internal_isnan(number)) {
    *is_negative = TRIO_FALSE;
    return TRIO_FP_NAN;
  }
  rc = internal_isinf(number);
  if (rc != 0) {
    *is_negative = (rc == -1);
    return TRIO_FP_INFINITE;
  }
  if ((number > 0.0) && (number < DBL_MIN)) {
    *is_negative = TRIO_FALSE;
    return TRIO_FP_SUBNORMAL;
  }
  if ((number < 0.0) && (number > -DBL_MIN)) {
    *is_negative = TRIO_TRUE;
    return TRIO_FP_SUBNORMAL;
  }
  *is_negative = (number < 0.0);
  return TRIO_FP_NORMAL;

#endif
}

#endif

/**
   Check for NaN.

   @param number An arbitrary floating-point number.
   @return Boolean value indicating whether or not the number is a NaN.
*/
#if defined(TRIO_FUNC_ISNAN)

TRIO_PUBLIC_NAN int
trio_isnan
TRIO_ARGS1((number),
       double number)
{
  int dummy;

  return (trio_fpclassify_and_signbit(number, &dummy) == TRIO_FP_NAN);
}

#endif

/**
   Check for infinity.

   @param number An arbitrary floating-point number.
   @return 1 if positive infinity, -1 if negative infinity, 0 otherwise.
*/
#if defined(TRIO_FUNC_ISINF)

TRIO_PUBLIC_NAN int
trio_isinf
TRIO_ARGS1((number),
       double number)
{
  int is_negative;

  if (trio_fpclassify_and_signbit(number, &is_negative) == TRIO_FP_INFINITE)
    {
      return (is_negative) ? -1 : 1;
    }
  else
    {
      return 0;
    }
}

#endif

/**
   Check for finity.

   @param number An arbitrary floating-point number.
   @return Boolean value indicating whether or not the number is a finite.
*/
#if defined(TRIO_FUNC_ISFINITE)

TRIO_PUBLIC_NAN int
trio_isfinite
TRIO_ARGS1((number),
       double number)
{
  int dummy;

  switch (trio_fpclassify_and_signbit(number, &dummy))
    {
    case TRIO_FP_INFINITE:
    case TRIO_FP_NAN:
      return 0;
    default:
      return 1;
    }
}

#endif

/**
   Examine the sign of a number.

   @param number An arbitrary floating-point number.
   @return Boolean value indicating whether or not the number has the
   sign bit set (i.e. is negative).
*/
#if defined(TRIO_FUNC_SIGNBIT)

TRIO_PUBLIC_NAN int
trio_signbit
TRIO_ARGS1((number),
       double number)
{
  int is_negative;

  (void)trio_fpclassify_and_signbit(number, &is_negative);
  return is_negative;
}

#endif

/**
   Examine the class of a number.

   @param number An arbitrary floating-point number.
   @return Enumerable value indicating the class of @p number
*/
#if defined(TRIO_FUNC_FPCLASSIFY)

TRIO_PUBLIC_NAN int
trio_fpclassify
TRIO_ARGS1((number),
       double number)
{
  int dummy;

  return trio_fpclassify_and_signbit(number, &dummy);
}

#endif

/**
   Generate negative zero.

   @return Floating-point representation of negative zero.
*/
#if defined(TRIO_FUNC_NZERO)

TRIO_PUBLIC_NAN double
trio_nzero(TRIO_NOARGS)
{
# if defined(TRIO_NZERO_IEEE_754)

  return internal_make_double(ieee_754_negzero_array);

# endif

# if defined(TRIO_NZERO_FALLBACK)

  TRIO_VOLATILE double zero = 0.0;

  return -zero;

# endif
}

#endif

/**
   Generate positive infinity.

   @return Floating-point representation of positive infinity.
*/
#if defined(TRIO_FUNC_PINF)

TRIO_PUBLIC_NAN double
trio_pinf(TRIO_NOARGS)
{
  /* Cache the result */
  static double pinf_value = 0.0;

  if (pinf_value == 0.0) {

# if defined(TRIO_PINF_C99_MACRO)

    pinf_value = (double)INFINITY;

# endif

# if defined(TRIO_PINF_IEEE_754)

    pinf_value = internal_make_double(ieee_754_infinity_array);

# endif

# if defined(TRIO_PINF_FALLBACK)
    /*
     * If HUGE_VAL is different from DBL_MAX, then HUGE_VAL is used
     * as infinity. Otherwise we have to resort to an overflow
     * operation to generate infinity.
     */
#  if defined(TRIO_PLATFORM_UNIX)
    signal_handler_t sigfpe_handler = internal_ignore_signal_handler(SIGFPE);
#  endif

    pinf_value = HUGE_VAL;
    if (HUGE_VAL == DBL_MAX) {
      /* Force overflow */
      pinf_value += HUGE_VAL;
    }

#  if defined(TRIO_PLATFORM_UNIX)
    internal_restore_signal_handler(SIGFPE, sigfpe_handler);
#  endif

# endif
  }
  return pinf_value;
}

#endif

/**
   Generate negative infinity.

   @return Floating-point value of negative infinity.
*/
#if defined(TRIO_FUNC_NINF)

TRIO_PUBLIC_NAN double
trio_ninf(TRIO_NOARGS)
{
  static double ninf_value = 0.0;

  if (ninf_value == 0.0) {
    /*
     * Negative infinity is calculated by negating positive infinity,
     * which can be done because it is legal to do calculations on
     * infinity (for example,  1 / infinity == 0).
     */
    ninf_value = -trio_pinf();
  }
  return ninf_value;
}

#endif

/**
   Generate NaN.

   @return Floating-point representation of NaN.
*/
#if defined(TRIO_FUNC_NAN)

TRIO_PUBLIC_NAN double
trio_nan(TRIO_NOARGS)
{
  /* Cache the result */
  static double nan_value = 0.0;

  if (nan_value == 0.0) {

# if defined(TRIO_NAN_C99_FUNCTION) || defined(TRIO_PLATFORM_SYMBIAN)

    nan_value = nan("");

# endif

# if defined(TRIO_NAN_C99_MACRO)

    nan_value = (double)NAN;

# endif

# if defined(TRIO_NAN_IEEE_754)

    nan_value = internal_make_double(ieee_754_qnan_array);

# endif

# if defined(TRIO_NAN_FALLBACK)
    /*
     * There are several ways to generate NaN. The one used here is
     * to divide infinity by infinity. I would have preferred to add
     * negative infinity to positive infinity, but that yields wrong
     * result (infinity) on FreeBSD.
     *
     * This may fail if the hardware does not support NaN, or if
     * the Invalid Operation floating-point exception is unmasked.
     */
#  if defined(TRIO_PLATFORM_UNIX)
    signal_handle_t sigfpe_handler = internal_ignore_signal_handler(SIGFPE);
#  endif

    nan_value = trio_pinf() / trio_pinf();

#  if defined(TRIO_PLATFORM_UNIX)
    internal_restore_signal_handler(SIGFPE, sigfpe_handler);
#  endif

# endif
  }
  return nan_value;
}

#endif

/** @} SpecialQuantities */

/*************************************************************************
 * For test purposes.
 *
 * Add the following compiler option to include this test code.
 *
 *  Unix : -DSTANDALONE
 *  VMS  : /DEFINE=(STANDALONE)
 */
#if defined(STANDALONE)
# include <stdio.h>

static TRIO_CONST char *
getClassification
TRIO_ARGS1((type),
       int type)
{
  switch (type) {
  case TRIO_FP_INFINITE:
    return "FP_INFINITE";
  case TRIO_FP_NAN:
    return "FP_NAN";
  case TRIO_FP_NORMAL:
    return "FP_NORMAL";
  case TRIO_FP_SUBNORMAL:
    return "FP_SUBNORMAL";
  case TRIO_FP_ZERO:
    return "FP_ZERO";
  default:
    return "FP_UNKNOWN";
  }
}

static void
print_class
TRIO_ARGS2((prefix, number),
       TRIO_CONST char *prefix,
       double number)
{
  printf("%-6s: %s %-15s %g\n",
     prefix,
     trio_signbit(number) ? "-" : "+",
     getClassification(trio_fpclassify(number)),
     number);
}

int main(TRIO_NOARGS)
{
  double my_nan;
  double my_pinf;
  double my_ninf;
# if defined(TRIO_PLATFORM_UNIX)
  signal_handler_t signal_handler;
# endif

  my_nan = trio_nan();
  my_pinf = trio_pinf();
  my_ninf = trio_ninf();

  print_class("Nan", my_nan);
  print_class("PInf", my_pinf);
  print_class("NInf", my_ninf);
  print_class("PZero", 0.0);
  print_class("NZero", -0.0);
  print_class("PNorm", 1.0);
  print_class("NNorm", -1.0);
  print_class("PSub", 1.01e-307 - 1.00e-307);
  print_class("NSub", 1.00e-307 - 1.01e-307);

  printf("NaN : %4g 0x%02x%02x%02x%02x%02x%02x%02x%02x (%2d, %2d, %2d)\n",
     my_nan,
     ((unsigned char *)&my_nan)[0],
     ((unsigned char *)&my_nan)[1],
     ((unsigned char *)&my_nan)[2],
     ((unsigned char *)&my_nan)[3],
     ((unsigned char *)&my_nan)[4],
     ((unsigned char *)&my_nan)[5],
     ((unsigned char *)&my_nan)[6],
     ((unsigned char *)&my_nan)[7],
     trio_isnan(my_nan), trio_isinf(my_nan), trio_isfinite(my_nan));
  printf("PInf: %4g 0x%02x%02x%02x%02x%02x%02x%02x%02x (%2d, %2d, %2d)\n",
     my_pinf,
     ((unsigned char *)&my_pinf)[0],
     ((unsigned char *)&my_pinf)[1],
     ((unsigned char *)&my_pinf)[2],
     ((unsigned char *)&my_pinf)[3],
     ((unsigned char *)&my_pinf)[4],
     ((unsigned char *)&my_pinf)[5],
     ((unsigned char *)&my_pinf)[6],
     ((unsigned char *)&my_pinf)[7],
     trio_isnan(my_pinf), trio_isinf(my_pinf), trio_isfinite(my_pinf));
  printf("NInf: %4g 0x%02x%02x%02x%02x%02x%02x%02x%02x (%2d, %2d, %2d)\n",
     my_ninf,
     ((unsigned char *)&my_ninf)[0],
     ((unsigned char *)&my_ninf)[1],
     ((unsigned char *)&my_ninf)[2],
     ((unsigned char *)&my_ninf)[3],
     ((unsigned char *)&my_ninf)[4],
     ((unsigned char *)&my_ninf)[5],
     ((unsigned char *)&my_ninf)[6],
     ((unsigned char *)&my_ninf)[7],
     trio_isnan(my_ninf), trio_isinf(my_ninf), trio_isfinite(my_ninf));

# if defined(TRIO_PLATFORM_UNIX)
  signal_handler = internal_ignore_signal_handler(SIGFPE);
# endif

  my_pinf = DBL_MAX + DBL_MAX;
  my_ninf = -my_pinf;
  my_nan = my_pinf / my_pinf;

# if defined(TRIO_PLATFORM_UNIX)
  internal_restore_signal_handler(SIGFPE, signal_handler);
# endif

  printf("NaN : %4g 0x%02x%02x%02x%02x%02x%02x%02x%02x (%2d, %2d, %2d)\n",
     my_nan,
     ((unsigned char *)&my_nan)[0],
     ((unsigned char *)&my_nan)[1],
     ((unsigned char *)&my_nan)[2],
     ((unsigned char *)&my_nan)[3],
     ((unsigned char *)&my_nan)[4],
     ((unsigned char *)&my_nan)[5],
     ((unsigned char *)&my_nan)[6],
     ((unsigned char *)&my_nan)[7],
     trio_isnan(my_nan), trio_isinf(my_nan), trio_isfinite(my_nan));
  printf("PInf: %4g 0x%02x%02x%02x%02x%02x%02x%02x%02x (%2d, %2d, %2d)\n",
     my_pinf,
     ((unsigned char *)&my_pinf)[0],
     ((unsigned char *)&my_pinf)[1],
     ((unsigned char *)&my_pinf)[2],
     ((unsigned char *)&my_pinf)[3],
     ((unsigned char *)&my_pinf)[4],
     ((unsigned char *)&my_pinf)[5],
     ((unsigned char *)&my_pinf)[6],
     ((unsigned char *)&my_pinf)[7],
     trio_isnan(my_pinf), trio_isinf(my_pinf), trio_isfinite(my_pinf));
  printf("NInf: %4g 0x%02x%02x%02x%02x%02x%02x%02x%02x (%2d, %2d, %2d)\n",
     my_ninf,
     ((unsigned char *)&my_ninf)[0],
     ((unsigned char *)&my_ninf)[1],
     ((unsigned char *)&my_ninf)[2],
     ((unsigned char *)&my_ninf)[3],
     ((unsigned char *)&my_ninf)[4],
     ((unsigned char *)&my_ninf)[5],
     ((unsigned char *)&my_ninf)[6],
     ((unsigned char *)&my_ninf)[7],
     trio_isnan(my_ninf), trio_isinf(my_ninf), trio_isfinite(my_ninf));

  return 0;
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: Embedded trionan.c
////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(HAVE_CONFIG_H)
# include <config.h>
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "triodef.h"
#include "triostr.h"
#if defined(TRIO_FUNC_TO_LONG_DOUBLE)
# define USE_MATH
#endif
#if defined(USE_MATH)
# include <math.h>
#endif

/*************************************************************************
 * Definitions
 */

#if !defined(TRIO_PUBLIC_STRING)
# define TRIO_PUBLIC_STRING TRIO_PUBLIC
#endif
#if !defined(TRIO_PRIVATE_STRING)
# define TRIO_PRIVATE_STRING TRIO_PRIVATE
#endif

#if !defined(NULL)
# define NULL 0
#endif
#if !defined(NIL)
# define NIL ((char)0)
#endif
#if !defined(FALSE)
# define FALSE (1 == 0)
# define TRUE (! FALSE)
#endif
#if !defined(BOOLEAN_T)
# define BOOLEAN_T int
#endif

#if defined(USE_MATH)
# if defined(PREDEF_STANDARD_C99)
#  if defined(TRIO_COMPILER_DECC)
#   if (TRIO_COMPILER_DECC - 0 > 80000000)
/*
 * The OSF/1 runtime that comes with the DECC compiler does not support
 * hexfloats conversion.
 */
#    define USE_STRTOD
#    define USE_STRTOF
#   endif
#  else
#   define USE_STRTOD
#   define USE_STRTOF
#  endif
# else
#  if defined(TRIO_COMPILER_VISUALC)
#   define USE_STRTOD
#  endif
#endif
#endif

#if defined(TRIO_PLATFORM_UNIX)
# if defined(PREDEF_STANDARD_UNIX95)
#  define USE_STRCASECMP
#  define USE_STRNCASECMP
# endif
# if defined(TRIO_PLATFORM_SUNOS)
#  define USE_SYS_ERRLIST
# else
#  define USE_STRERROR
# endif
# if defined(TRIO_PLATFORM_QNX)
#  define strcasecmp(x,y) stricmp(x,y)
#  define strncasecmp(x,y,n) strnicmp(x,y,n)
# endif
#endif

#if defined(TRIO_PLATFORM_WIN32)
# define USE_STRCASECMP
# if defined(TRIO_PLATFORM_WINCE)
#  define strcasecmp(x,y) _stricmp(x,y)
# else
////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: strcasecmp is already defined in MinGW.
////////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef _MSC_VER
#  define strcasecmp(x,y) strcmpi(x,y)
#endif
# endif
#endif

#if !defined(HAVE_CONFIG_H)
# if !(defined(TRIO_PLATFORM_SUNOS))
#  define HAVE_TOLOWER
#  define HAVE_TOUPPER
# endif
#endif

#if defined(USE_MATH) && !defined(TRIO_NO_POWL)
# if !defined(HAVE_POWL)
#  if defined(PREDEF_STANDARD_C99) \
   || defined(PREDEF_STANDARD_UNIX03)
#   define HAVE_POWL
#  else
#   if defined(TRIO_COMPILER_VISUALC)
#    if defined(powl)
#     define HAVE_POWL
#    endif
#   endif
#  endif
# endif
#endif

#if defined(HAVE_POWL)
# define trio_powl(x,y) powl((x),(y))
#else
# define trio_powl(x,y) pow((double)(x),(double)(y))
#endif

#if defined(TRIO_FUNC_TO_UPPER) \
 || (defined(TRIO_FUNC_EQUAL) && !defined(USE_STRCASECMP)) \
 || (defined(TRIO_FUNC_EQUAL_MAX) && !defined(USE_STRNCASECMP)) \
 || defined(TRIO_FUNC_MATCH) \
 || defined(TRIO_FUNC_TO_LONG_DOUBLE) \
 || defined(TRIO_FUNC_UPPER)
# define TRIO_FUNC_INTERNAL_TO_UPPER
#endif

/*************************************************************************
 * Structures
 */

struct _trio_string_t
{
  char *content;
  size_t length;
  size_t allocated;
};

/*************************************************************************
 * Constants
 */

#if !defined(TRIO_EMBED_STRING)
static TRIO_CONST char rcsid[] = "@(#)$Id$";
#endif

/*************************************************************************
 * Static String Functions
 */

#if defined(TRIO_DOCUMENTATION)
# include "doc/doc_static.h"
#endif
/** @addtogroup StaticStrings
    @{
*/

/*
 * internal_duplicate_max
 */
#if defined(TRIO_FUNC_DUPLICATE) \
 || defined(TRIO_FUNC_DUPLICATE_MAX) \
 || defined(TRIO_FUNC_STRING_DUPLICATE) \
 || defined(TRIO_FUNC_XSTRING_DUPLICATE)

TRIO_PRIVATE_STRING char *
internal_duplicate_max
TRIO_ARGS2((source, size),
       TRIO_CONST char *source,
       size_t size)
{
  char *target;

  assert(source);

  /* Make room for string plus a terminating zero */
  size++;
  target = trio_create(size);
  if (target)
    {
      trio_copy_max(target, size, source);
    }
  return target;
}

#endif

/*
 * internal_string_alloc
 */
#if defined(TRIO_FUNC_STRING_CREATE) \
 || defined(TRIO_FUNC_STRING_DUPLICATE) \
 || defined(TRIO_FUNC_XSTRING_DUPLICATE)

TRIO_PRIVATE_STRING trio_string_t *
internal_string_alloc(TRIO_NOARGS)
{
  trio_string_t *self;

  self = (trio_string_t *)TRIO_MALLOC(sizeof(trio_string_t));
  if (self)
    {
      self->content = NULL;
      self->length = 0;
      self->allocated = 0;
    }
  return self;
}

#endif

/*
 * internal_string_grow
 *
 * The size of the string will be increased by 'delta' characters. If
 * 'delta' is zero, the size will be doubled.
 */
#if defined(TRIO_FUNC_STRING_CREATE) \
 || defined(TRIO_FUNC_STRING_APPEND) \
 || defined(TRIO_FUNC_XSTRING_APPEND) \
 || defined(TRIO_FUNC_XSTRING_APPEND_CHAR)

TRIO_PRIVATE_STRING BOOLEAN_T
internal_string_grow
TRIO_ARGS2((self, delta),
       trio_string_t *self,
       size_t delta)
{
  BOOLEAN_T status = FALSE;
  char *new_content;
  size_t new_size;

  new_size = (delta == 0)
    ? ( (self->allocated == 0) ? 1 : self->allocated * 2 )
    : self->allocated + delta;

  new_content = (char *)TRIO_REALLOC(self->content, new_size);
  if (new_content)
    {
      self->content = new_content;
      self->allocated = new_size;
      status = TRUE;
    }
  return status;
}

#endif

/*
 * internal_string_grow_to
 *
 * The size of the string will be increased to 'length' plus one characters.
 * If 'length' is less than the original size, the original size will be
 * used (that is, the size of the string is never decreased).
 */
#if defined(TRIO_FUNC_STRING_APPEND) \
 || defined(TRIO_FUNC_XSTRING_APPEND) \
 || defined(TRIO_FUNC_XSTRING_APPEND_MAX)

TRIO_PRIVATE_STRING BOOLEAN_T
internal_string_grow_to
TRIO_ARGS2((self, length),
       trio_string_t *self,
       size_t length)
{
  length++; /* Room for terminating zero */
  return (self->allocated < length)
    ? internal_string_grow(self, length - self->allocated)
    : TRUE;
}

#endif

#if defined(TRIO_FUNC_INTERNAL_TO_UPPER)

TRIO_PRIVATE_STRING int
internal_to_upper
TRIO_ARGS1((source),
       int source)
{
# if defined(HAVE_TOUPPER)

  return toupper(source);

# else

  /* Does not handle locales or non-contiguous alphabetic characters */
  return ((source >= (int)'a') && (source <= (int)'z'))
    ? source - 'a' + 'A'
    : source;

# endif
}

#endif


/**
   Create new string.

   @param size Size of new string.
   @return Pointer to string, or NULL if allocation failed.
*/
#if defined(TRIO_FUNC_CREATE)

TRIO_PUBLIC_STRING char *
trio_create
TRIO_ARGS1((size),
       size_t size)
{
  return (char *)TRIO_MALLOC(size);
}

#endif

/**
   Destroy string.

   @param string String to be freed.
*/
#if defined(TRIO_FUNC_DESTROY)

TRIO_PUBLIC_STRING void
trio_destroy
TRIO_ARGS1((string),
       char *string)
{
  if (string)
    {
      TRIO_FREE(string);
    }
}

#endif

/**
   Count the number of characters in a string.

   @param string String to measure.
   @return Number of characters in @p string.
*/
#if defined(TRIO_FUNC_LENGTH)

TRIO_PUBLIC_STRING size_t
trio_length
TRIO_ARGS1((string),
       TRIO_CONST char *string)
{
  return strlen(string);
}

#endif

/**
   Count at most @p max characters in a string.

   @param string String to measure.
   @param max Maximum number of characters to count.
   @return The maximum value of @p max and number of characters in @p string.
*/
#if defined(TRIO_FUNC_LENGTH_MAX)

TRIO_PUBLIC_STRING size_t
trio_length_max
TRIO_ARGS2((string, max),
       TRIO_CONST char *string,
       size_t max)
{
  size_t i;

  for (i = 0; i < max; ++i)
    {
      if (string[i] == 0)
    break;
    }
  return i;
}

#endif

/**
   Append @p source at the end of @p target.

   @param target Target string.
   @param source Source string.
   @return Boolean value indicating success or failure.

   @pre @p target must point to a memory chunk with sufficient room to
   contain the @p target string and @p source string.
   @pre No boundary checking is performed, so insufficient memory will
   result in a buffer overrun.
   @post @p target will be zero terminated.
*/
#if defined(TRIO_FUNC_APPEND)

TRIO_PUBLIC_STRING int
trio_append
TRIO_ARGS2((target, source),
       char *target,
       TRIO_CONST char *source)
{
  assert(target);
  assert(source);

  return (strcat(target, source) != NULL);
}

#endif

/**
   Append at most @p max characters from @p source to @p target.

   @param target Target string.
   @param max Maximum number of characters to append.
   @param source Source string.
   @return Boolean value indicating success or failure.

   @pre @p target must point to a memory chuck with sufficient room to
   contain the @p target string and the @p source string (at most @p max
   characters).
   @pre No boundary checking is performed, so insufficient memory will
   result in a buffer overrun.
   @post @p target will be zero terminated.
*/
#if defined(TRIO_FUNC_APPEND_MAX)

TRIO_PUBLIC_STRING int
trio_append_max
TRIO_ARGS3((target, max, source),
       char *target,
       size_t max,
       TRIO_CONST char *source)
{
  size_t length;

  assert(target);
  assert(source);

  length = trio_length(target);

  if (max > length)
    {
      strncat(target, source, max - length - 1);
    }
  return TRUE;
}

#endif

/**
   Determine if a string contains a substring.

   @param string String to be searched.
   @param substring String to be found.
   @return Boolean value indicating success or failure.
*/
#if defined(TRIO_FUNC_CONTAINS)

TRIO_PUBLIC_STRING int
trio_contains
TRIO_ARGS2((string, substring),
       TRIO_CONST char *string,
       TRIO_CONST char *substring)
{
  assert(string);
  assert(substring);

  return (0 != strstr(string, substring));
}

#endif

/**
   Copy @p source to @p target.

   @param target Target string.
   @param source Source string.
   @return Boolean value indicating success or failure.

   @pre @p target must point to a memory chunk with sufficient room to
   contain the @p source string.
   @pre No boundary checking is performed, so insufficient memory will
   result in a buffer overrun.
   @post @p target will be zero terminated.
*/
#if defined(TRIO_FUNC_COPY)

TRIO_PUBLIC_STRING int
trio_copy
TRIO_ARGS2((target, source),
       char *target,
       TRIO_CONST char *source)
{
  assert(target);
  assert(source);

  (void)strcpy(target, source);
  return TRUE;
}

#endif

/**
   Copy at most @p max - 1 characters from @p source to @p target.

   @param target Target string.
   @param max Maximum number of characters to append (one of which is
   a NUL terminator).  In other words @p source must point to at least
   @p max - 1 bytes, but @p target must point to at least @p max
   bytes.
   @param source Source string.
   @return Boolean value indicating success or failure.

   @pre @p target must point to a memory chunk with sufficient room to
   contain the @p source string and a NUL terminator (at most @p max
   bytes total).
   @pre No boundary checking is performed, so insufficient memory will
   result in a buffer overrun.
   @post @p target will be zero terminated.
*/
#if defined(TRIO_FUNC_COPY_MAX)

TRIO_PUBLIC_STRING int
trio_copy_max
TRIO_ARGS3((target, max, source),
       char *target,
       size_t max,
       TRIO_CONST char *source)
{
  assert(target);
  assert(source);
  assert(max > 0); /* Includes != 0 */

  (void)strncpy(target, source, max - 1);
  target[max - 1] = (char)0;
  return TRUE;
}

#endif

/**
   Duplicate @p source.

   @param source Source string.
   @return A copy of the @p source string.

   @post @p target will be zero terminated.
*/
#if defined(TRIO_FUNC_DUPLICATE)

TRIO_PUBLIC_STRING char *
trio_duplicate
TRIO_ARGS1((source),
       TRIO_CONST char *source)
{
  return internal_duplicate_max(source, trio_length(source));
}

#endif

/**
   Duplicate at most @p max characters of @p source.

   @param source Source string.
   @param max Maximum number of characters to duplicate.
   @return A copy of the @p source string.

   @post @p target will be zero terminated.
*/
#if defined(TRIO_FUNC_DUPLICATE_MAX)

TRIO_PUBLIC_STRING char *
trio_duplicate_max
TRIO_ARGS2((source, max),
       TRIO_CONST char *source,
       size_t max)
{
  size_t length;

  assert(source);
  assert(max > 0);

  length = trio_length(source);
  if (length > max)
    {
      length = max;
    }
  return internal_duplicate_max(source, length);
}

#endif

/**
   Compare if two strings are equal.

   @param first First string.
   @param second Second string.
   @return Boolean indicating whether the two strings are equal or not.

   Case-insensitive comparison.
*/
#if defined(TRIO_FUNC_EQUAL)

TRIO_PUBLIC_STRING int
trio_equal
TRIO_ARGS2((first, second),
       TRIO_CONST char *first,
       TRIO_CONST char *second)
{
  assert(first);
  assert(second);

  if ((first != NULL) && (second != NULL))
    {
# if defined(USE_STRCASECMP)
      return (0 == strcasecmp(first, second));
# else
      while ((*first != NIL) && (*second != NIL))
    {
      if (internal_to_upper(*first) != internal_to_upper(*second))
        {
          break;
        }
      first++;
      second++;
    }
      return ((*first == NIL) && (*second == NIL));
# endif
    }
  return FALSE;
}

#endif

/**
   Compare if two strings are equal.

   @param first First string.
   @param second Second string.
   @return Boolean indicating whether the two strings are equal or not.

   Case-sensitive comparison.
*/
#if defined(TRIO_FUNC_EQUAL_CASE)

TRIO_PUBLIC_STRING int
trio_equal_case
TRIO_ARGS2((first, second),
       TRIO_CONST char *first,
       TRIO_CONST char *second)
{
  assert(first);
  assert(second);

  if ((first != NULL) && (second != NULL))
    {
      return (0 == strcmp(first, second));
    }
  return FALSE;
}

#endif

/**
   Compare if two strings up until the first @p max characters are equal.

   @param first First string.
   @param max Maximum number of characters to compare.
   @param second Second string.
   @return Boolean indicating whether the two strings are equal or not.

   Case-sensitive comparison.
*/
#if defined(TRIO_FUNC_EQUAL_CASE_MAX)

TRIO_PUBLIC_STRING int
trio_equal_case_max
TRIO_ARGS3((first, max, second),
       TRIO_CONST char *first,
       size_t max,
       TRIO_CONST char *second)
{
  assert(first);
  assert(second);

  if ((first != NULL) && (second != NULL))
    {
      return (0 == strncmp(first, second, max));
    }
  return FALSE;
}

#endif

/**
   Compare if two strings are equal.

   @param first First string.
   @param second Second string.
   @return Boolean indicating whether the two strings are equal or not.

   Collating characters are considered equal.
*/
#if defined(TRIO_FUNC_EQUAL_LOCALE)

TRIO_PUBLIC_STRING int
trio_equal_locale
TRIO_ARGS2((first, second),
       TRIO_CONST char *first,
       TRIO_CONST char *second)
{
  assert(first);
  assert(second);

# if defined(LC_COLLATE)
  return (strcoll(first, second) == 0);
# else
  return trio_equal(first, second);
# endif
}

#endif

/**
   Compare if two strings up until the first @p max characters are equal.

   @param first First string.
   @param max Maximum number of characters to compare.
   @param second Second string.
   @return Boolean indicating whether the two strings are equal or not.

   Case-insensitive comparison.
*/
#if defined(TRIO_FUNC_EQUAL_MAX)

TRIO_PUBLIC_STRING int
trio_equal_max
TRIO_ARGS3((first, max, second),
       TRIO_CONST char *first,
       size_t max,
       TRIO_CONST char *second)
{
  assert(first);
  assert(second);

  if ((first != NULL) && (second != NULL))
    {
# if defined(USE_STRNCASECMP)
      return (0 == strncasecmp(first, second, max));
# else
      /* Not adequately tested yet */
      size_t cnt = 0;
      while ((*first != NIL) && (*second != NIL) && (cnt <= max))
    {
      if (internal_to_upper(*first) != internal_to_upper(*second))
        {
          break;
        }
      first++;
      second++;
      cnt++;
    }
      return ((cnt == max) || ((*first == NIL) && (*second == NIL)));
# endif
    }
  return FALSE;
}

#endif

/**
   Provide a textual description of an error code (errno).

   @param error_number Error number.
   @return Textual description of @p error_number.
*/
#if defined(TRIO_FUNC_ERROR)

TRIO_PUBLIC_STRING TRIO_CONST char *
trio_error
TRIO_ARGS1((error_number),
       int error_number)
{
////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: Silence unused parameter warning.
////////////////////////////////////////////////////////////////////////////////////////////////////
    (void)error_number;
////////////////////////////////////////////////////////////////////////////////////////////////////

# if defined(USE_STRERROR)

  return strerror(error_number);

# else
#  if defined(USE_SYS_ERRLIST)

  extern char *sys_errlist[];
  extern int sys_nerr;

  return ((error_number < 0) || (error_number >= sys_nerr))
    ? "unknown"
    : sys_errlist[error_number];

#  else

  return "unknown";

#  endif
# endif
}

#endif

/**
   Format the date/time according to @p format.

   @param target Target string.
   @param max Maximum number of characters to format.
   @param format Formatting string.
   @param datetime Date/time structure.
   @return Number of formatted characters.

   The formatting string accepts the same specifiers as the standard C
   function strftime.
*/
#if defined(TRIO_FUNC_FORMAT_DATE_MAX)

TRIO_PUBLIC_STRING size_t
trio_format_date_max
TRIO_ARGS4((target, max, format, datetime),
       char *target,
       size_t max,
       TRIO_CONST char *format,
       TRIO_CONST struct tm *datetime)
{
  assert(target);
  assert(format);
  assert(datetime);
  assert(max > 0);

  return strftime(target, max, format, datetime);
}

#endif

/**
   Calculate a hash value for a string.

   @param string String to be calculated on.
   @param type Hash function.
   @return Calculated hash value.

   @p type can be one of the following
   @li @c TRIO_HASH_PLAIN Plain hash function.
*/
#if defined(TRIO_FUNC_HASH)

TRIO_PUBLIC_STRING unsigned long
trio_hash
TRIO_ARGS2((string, type),
       TRIO_CONST char *string,
       int type)
{
  unsigned long value = 0L;
  char ch;

  assert(string);

  switch (type)
    {
    case TRIO_HASH_PLAIN:
      while ( (ch = *string++) != NIL )
    {
      value *= 31;
      value += (unsigned long)ch;
    }
      break;
    default:
      assert(FALSE);
      break;
    }
  return value;
}

#endif

/**
   Find first occurrence of a character in a string.

   @param string String to be searched.
   @param character Character to be found.
   @return A pointer to the found character, or NULL if character was not found.
 */
#if defined(TRIO_FUNC_INDEX)

TRIO_PUBLIC_STRING char *
trio_index
TRIO_ARGS2((string, character),
       TRIO_CONST char *string,
       int character)
{
  assert(string);

  return strchr(string, character);
}

#endif

/**
   Find last occurrence of a character in a string.

   @param string String to be searched.
   @param character Character to be found.
   @return A pointer to the found character, or NULL if character was not found.
 */
#if defined(TRIO_FUNC_INDEX_LAST)

TRIO_PUBLIC_STRING char *
trio_index_last
TRIO_ARGS2((string, character),
       TRIO_CONST char *string,
       int character)
{
  assert(string);

  return strchr(string, character);
}

#endif

/**
   Convert the alphabetic letters in the string to lower-case.

   @param target String to be converted.
   @return Number of processed characters (converted or not).
*/
#if defined(TRIO_FUNC_LOWER)

TRIO_PUBLIC_STRING int
trio_lower
TRIO_ARGS1((target),
       char *target)
{
  assert(target);

  return trio_span_function(target, target, trio_to_lower);
}

#endif

/**
   Compare two strings using wildcards.

   @param string String to be searched.
   @param pattern Pattern, including wildcards, to search for.
   @return Boolean value indicating success or failure.

   Case-insensitive comparison.

   The following wildcards can be used
   @li @c * Match any number of characters.
   @li @c ? Match a single character.
*/
#if defined(TRIO_FUNC_MATCH)

TRIO_PUBLIC_STRING int
trio_match
TRIO_ARGS2((string, pattern),
       TRIO_CONST char *string,
       TRIO_CONST char *pattern)
{
  assert(string);
  assert(pattern);

  for (; ('*' != *pattern); ++pattern, ++string)
    {
      if (NIL == *string)
    {
      return (NIL == *pattern);
    }
      if ((internal_to_upper((int)*string) != internal_to_upper((int)*pattern))
      && ('?' != *pattern))
    {
      return FALSE;
    }
    }
  /* two-line patch to prevent *too* much recursiveness: */
  while ('*' == pattern[1])
    pattern++;

  do
    {
      if ( trio_match(string, &pattern[1]) )
    {
      return TRUE;
    }
    }
  while (*string++);

  return FALSE;
}

#endif

/**
   Compare two strings using wildcards.

   @param string String to be searched.
   @param pattern Pattern, including wildcards, to search for.
   @return Boolean value indicating success or failure.

   Case-sensitive comparison.

   The following wildcards can be used
   @li @c * Match any number of characters.
   @li @c ? Match a single character.
*/
#if defined(TRIO_FUNC_MATCH_CASE)

TRIO_PUBLIC_STRING int
trio_match_case
TRIO_ARGS2((string, pattern),
       TRIO_CONST char *string,
       TRIO_CONST char *pattern)
{
  assert(string);
  assert(pattern);

  for (; ('*' != *pattern); ++pattern, ++string)
    {
      if (NIL == *string)
    {
      return (NIL == *pattern);
    }
      if ((*string != *pattern)
      && ('?' != *pattern))
    {
      return FALSE;
    }
    }
  /* two-line patch to prevent *too* much recursiveness: */
  while ('*' == pattern[1])
    pattern++;

  do
    {
      if ( trio_match_case(string, &pattern[1]) )
    {
      return TRUE;
    }
    }
  while (*string++);

  return FALSE;
}

#endif

/**
   Execute a function on each character in string.

   @param target Target string.
   @param source Source string.
   @param Function Function to be executed.
   @return Number of processed characters.
*/
#if defined(TRIO_FUNC_SPAN_FUNCTION)

TRIO_PUBLIC_STRING size_t
trio_span_function
TRIO_ARGS3((target, source, Function),
       char *target,
       TRIO_CONST char *source,
       int (*Function) TRIO_PROTO((int)))
{
  size_t count = 0;

  assert(target);
  assert(source);
  assert(Function);

  while (*source != NIL)
    {
      *target++ = Function(*source++);
      count++;
    }
  return count;
}

#endif

/**
   Search for a substring in a string.

   @param string String to be searched.
   @param substring String to be found.
   @return Pointer to first occurrence of @p substring in @p string, or NULL
   if no match was found.
*/
#if defined(TRIO_FUNC_SUBSTRING)

TRIO_PUBLIC_STRING char *
trio_substring
TRIO_ARGS2((string, substring),
       TRIO_CONST char *string,
       TRIO_CONST char *substring)
{
  assert(string);
  assert(substring);

  return strstr(string, substring);
}

#endif

/**
   Search for a substring in the first @p max characters of a string.

   @param string String to be searched.
   @param max Maximum characters to be searched.
   @param substring String to be found.
   @return Pointer to first occurrence of @p substring in @p string, or NULL
   if no match was found.
*/
#if defined(TRIO_FUNC_SUBSTRING_MAX)

TRIO_PUBLIC_STRING char *
trio_substring_max
TRIO_ARGS3((string, max, substring),
       TRIO_CONST char *string,
       size_t max,
       TRIO_CONST char *substring)
{
  size_t count;
  size_t size;
  char *result = NULL;

  assert(string);
  assert(substring);

  size = trio_length(substring);
  if (size <= max)
    {
      for (count = 0; count <= max - size; count++)
    {
      if (trio_equal_max(substring, size, &string[count]))
        {
          result = (char *)&string[count];
          break;
        }
    }
    }
  return result;
}

#endif

/**
   Tokenize string.

   @param string String to be tokenized.
   @param delimiters String containing list of delimiting characters.
   @return Start of new token.

   @warning @p string will be destroyed.
*/
#if defined(TRIO_FUNC_TOKENIZE)

TRIO_PUBLIC_STRING char *
trio_tokenize
TRIO_ARGS2((string, delimiters),
       char *string,
       TRIO_CONST char *delimiters)
{
  assert(delimiters);

  return strtok(string, delimiters);
}

#endif

/**
   Convert string to floating-point number.

   @param source String to be converted.
   @param endp Pointer to end of the converted string.
   @return A floating-point number.

   The following Extended Backus-Naur form is used
   @verbatim
   double        ::= [ <sign> ]
                     ( <number> |
                       <number> <decimal_point> <number> |
                       <decimal_point> <number> )
                     [ <exponential> [ <sign> ] <number> ]
   number        ::= 1*( <digit> )
   digit         ::= ( '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9' )
   exponential   ::= ( 'e' | 'E' )
   sign          ::= ( '-' | '+' )
   decimal_point ::= '.'
   @endverbatim
*/
#if defined(TRIO_FUNC_TO_LONG_DOUBLE)

/* FIXME: Add EBNF for hex-floats */
TRIO_PUBLIC_STRING trio_long_double_t
trio_to_long_double
TRIO_ARGS2((source, endp),
       TRIO_CONST char *source,
       char **endp)
{
# if defined(USE_STRTOLD)
  return strtold(source, endp);
# else
  int isNegative = FALSE;
  int isExponentNegative = FALSE;
  trio_long_double_t integer = 0.0;
  trio_long_double_t fraction = 0.0;
  unsigned long exponent = 0;
  trio_long_double_t base;
  trio_long_double_t fracdiv = 1.0;
  trio_long_double_t value = 0.0;

  /* First try hex-floats */
  if ((source[0] == '0') && ((source[1] == 'x') || (source[1] == 'X')))
    {
      base = 16.0;
      source += 2;
      while (isxdigit((int)*source))
    {
      integer *= base;
      integer += (isdigit((int)*source)
              ? (*source - '0')
              : 10 + (internal_to_upper((int)*source) - 'A'));
      source++;
    }
      if (*source == '.')
    {
      source++;
      while (isxdigit((int)*source))
        {
          fracdiv /= base;
          fraction += fracdiv * (isdigit((int)*source)
                     ? (*source - '0')
                     : 10 + (internal_to_upper((int)*source) - 'A'));
          source++;
        }
      if ((*source == 'p') || (*source == 'P'))
        {
          source++;
          if ((*source == '+') || (*source == '-'))
        {
          isExponentNegative = (*source == '-');
          source++;
        }
          while (isdigit((int)*source))
        {
          exponent *= 10;
          exponent += (*source - '0');
          source++;
        }
        }
    }
      /* For later use with exponent */
      base = 2.0;
    }
  else /* Then try normal decimal floats */
    {
      base = 10.0;
      isNegative = (*source == '-');
      /* Skip sign */
      if ((*source == '+') || (*source == '-'))
    source++;

      /* Integer part */
      while (isdigit((int)*source))
    {
      integer *= base;
      integer += (*source - '0');
      source++;
    }

      if (*source == '.')
    {
      source++; /* skip decimal point */
      while (isdigit((int)*source))
        {
          fracdiv /= base;
          fraction += (*source - '0') * fracdiv;
          source++;
        }
    }
      if ((*source == 'e')
      || (*source == 'E')
#  if TRIO_MICROSOFT
      || (*source == 'd')
      || (*source == 'D')
#  endif
      )
    {
      source++; /* Skip exponential indicator */
      isExponentNegative = (*source == '-');
      if ((*source == '+') || (*source == '-'))
        source++;
      while (isdigit((int)*source))
        {
          exponent *= (int)base;
          exponent += (*source - '0');
          source++;
        }
    }
    }

  value = integer + fraction;
  if (exponent != 0)
    {
      if (isExponentNegative)
    value /= trio_powl(base, (trio_long_double_t)exponent);
      else
    value *= trio_powl(base, (trio_long_double_t)exponent);
    }
  if (isNegative)
    value = -value;

  if (endp)
    *endp = (char *)source;
  return value;
# endif
}

#endif

/**
   Convert string to floating-point number.

   @param source String to be converted.
   @param endp Pointer to end of the converted string.
   @return A floating-point number.

   See @ref trio_to_long_double.
*/
#if defined(TRIO_FUNC_TO_DOUBLE)

TRIO_PUBLIC_STRING double
trio_to_double
TRIO_ARGS2((source, endp),
       TRIO_CONST char *source,
       char **endp)
{
#if defined(USE_STRTOD)
  return strtod(source, endp);
#else
  return (double)trio_to_long_double(source, endp);
#endif
}

#endif

/**
   Convert string to floating-point number.

   @param source String to be converted.
   @param endp Pointer to end of the converted string.
   @return A floating-point number.

   See @ref trio_to_long_double.
*/
#if defined(TRIO_FUNC_TO_FLOAT)

TRIO_PUBLIC_STRING float
trio_to_float
TRIO_ARGS2((source, endp),
       TRIO_CONST char *source,
       char **endp)
{
#  if defined(USE_STRTOF)
  return strtof(source, endp);
#  else
  return (float)trio_to_long_double(source, endp);
#  endif
}

#endif

/**
   Convert string to signed integer.

   @param string String to be converted.
   @param endp Pointer to end of converted string.
   @param base Radix number of number.
*/
#if defined(TRIO_FUNC_TO_LONG)

TRIO_PUBLIC_STRING long
trio_to_long
TRIO_ARGS3((string, endp, base),
       TRIO_CONST char *string,
       char **endp,
       int base)
{
  assert(string);
  assert((base >= 2) && (base <= 36));

  return strtol(string, endp, base);
}

#endif

/**
   Convert one alphabetic letter to lower-case.

   @param source The letter to be converted.
   @return The converted letter.
*/
#if defined(TRIO_FUNC_TO_LOWER)

TRIO_PUBLIC_STRING int
trio_to_lower
TRIO_ARGS1((source),
       int source)
{
# if defined(HAVE_TOLOWER)

  return tolower(source);

# else

  /* Does not handle locales or non-contiguous alphabetic characters */
  return ((source >= (int)'A') && (source <= (int)'Z'))
    ? source - 'A' + 'a'
    : source;

# endif
}

#endif

/**
   Convert string to unsigned integer.

   @param string String to be converted.
   @param endp Pointer to end of converted string.
   @param base Radix number of number.
*/
#if defined(TRIO_FUNC_TO_UNSIGNED_LONG)

TRIO_PUBLIC_STRING unsigned long
trio_to_unsigned_long
TRIO_ARGS3((string, endp, base),
       TRIO_CONST char *string,
       char **endp,
       int base)
{
  assert(string);
  assert((base >= 2) && (base <= 36));

  return strtoul(string, endp, base);
}

#endif

/**
   Convert one alphabetic letter to upper-case.

   @param source The letter to be converted.
   @return The converted letter.
*/
#if defined(TRIO_FUNC_TO_UPPER)

TRIO_PUBLIC_STRING int
trio_to_upper
TRIO_ARGS1((source),
       int source)
{
  return internal_to_upper(source);
}

#endif

/**
   Convert the alphabetic letters in the string to upper-case.

   @param target The string to be converted.
   @return The number of processed characters (converted or not).
*/
#if defined(TRIO_FUNC_UPPER)

TRIO_PUBLIC_STRING int
trio_upper
TRIO_ARGS1((target),
       char *target)
{
  assert(target);

  return trio_span_function(target, target, internal_to_upper);
}

#endif

/** @} End of StaticStrings */


/*************************************************************************
 * Dynamic String Functions
 */

#if defined(TRIO_DOCUMENTATION)
# include "doc/doc_dynamic.h"
#endif
/** @addtogroup DynamicStrings
    @{
*/

/**
   Create a new dynamic string.

   @param initial_size Initial size of the buffer.
   @return Newly allocated dynamic string, or NULL if memory allocation failed.
*/
#if defined(TRIO_FUNC_STRING_CREATE)

TRIO_PUBLIC_STRING trio_string_t *
trio_string_create
TRIO_ARGS1((initial_size),
       int initial_size)
{
  trio_string_t *self;

  self = internal_string_alloc();
  if (self)
    {
      if (internal_string_grow(self,
             (size_t)((initial_size > 0) ? initial_size : 1)))
    {
      self->content[0] = (char)0;
      self->allocated = initial_size;
    }
      else
    {
      trio_string_destroy(self);
      self = NULL;
    }
    }
  return self;
}

#endif

/**
   Deallocate the dynamic string and its contents.

   @param self Dynamic string
*/
#if defined(TRIO_FUNC_STRING_DESTROY)

TRIO_PUBLIC_STRING void
trio_string_destroy
TRIO_ARGS1((self),
       trio_string_t *self)
{
  assert(self);

  if (self)
    {
      trio_destroy(self->content);
      TRIO_FREE(self);
    }
}

#endif

/**
   Get a pointer to the content.

   @param self Dynamic string.
   @param offset Offset into content.
   @return Pointer to the content.

   @p Offset can be zero, positive, or negative. If @p offset is zero,
   then the start of the content will be returned. If @p offset is positive,
   then a pointer to @p offset number of characters from the beginning of the
   content is returned. If @p offset is negative, then a pointer to @p offset
   number of characters from the ending of the string, starting at the
   terminating zero, is returned.
*/
#if defined(TRIO_FUNC_STRING_GET)

TRIO_PUBLIC_STRING char *
trio_string_get
TRIO_ARGS2((self, offset),
       trio_string_t *self,
       int offset)
{
  char *result = NULL;

  assert(self);

  if (self->content != NULL)
    {
      if (self->length == 0)
    {
      (void)trio_string_length(self);
    }
      if (offset >= 0)
    {
      if (offset > (int)self->length)
        {
          offset = self->length;
        }
    }
      else
    {
      offset += self->length + 1;
      if (offset < 0)
        {
          offset = 0;
        }
    }
      result = &(self->content[offset]);
    }
  return result;
}

#endif

/**
   Extract the content.

   @param self Dynamic String
   @return Content of dynamic string.

   The content is removed from the dynamic string. This enables destruction
   of the dynamic string without deallocation of the content.
*/
#if defined(TRIO_FUNC_STRING_EXTRACT)

TRIO_PUBLIC_STRING char *
trio_string_extract
TRIO_ARGS1((self),
       trio_string_t *self)
{
  char *result;

  assert(self);

  result = self->content;
  /* FIXME: Allocate new empty buffer? */
  self->content = NULL;
  self->length = self->allocated = 0;
  return result;
}

#endif

/**
   Set the content of the dynamic string.

   @param self Dynamic String
   @param buffer The new content.

   Sets the content of the dynamic string to a copy @p buffer.
   An existing content will be deallocated first, if necessary.

   @remark
   This function will make a copy of @p buffer.
   You are responsible for deallocating @p buffer yourself.
*/
#if defined(TRIO_FUNC_XSTRING_SET)

TRIO_PUBLIC_STRING void
trio_xstring_set
TRIO_ARGS2((self, buffer),
       trio_string_t *self,
       char *buffer)
{
  assert(self);

  trio_destroy(self->content);
  self->content = trio_duplicate(buffer);
}

#endif

/*
 * trio_string_size
 */
#if defined(TRIO_FUNC_STRING_SIZE)

TRIO_PUBLIC_STRING int
trio_string_size
TRIO_ARGS1((self),
       trio_string_t *self)
{
  assert(self);

  return self->allocated;
}

#endif

/*
 * trio_string_terminate
 */
#if defined(TRIO_FUNC_STRING_TERMINATE)

TRIO_PUBLIC_STRING void
trio_string_terminate
TRIO_ARGS1((self),
       trio_string_t *self)
{
  trio_xstring_append_char(self, 0);
}

#endif

/**
   Append the second string to the first.

   @param self Dynamic string to be modified.
   @param other Dynamic string to copy from.
   @return Boolean value indicating success or failure.
*/
#if defined(TRIO_FUNC_STRING_APPEND)

TRIO_PUBLIC_STRING int
trio_string_append
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  size_t length;

  assert(self);
  assert(other);

  length = self->length + other->length;
  if (!internal_string_grow_to(self, length))
    goto error;
  trio_copy(&self->content[self->length], other->content);
  self->length = length;
  return TRUE;

 error:
  return FALSE;
}

#endif


/*
 * trio_xstring_append
 */
#if defined(TRIO_FUNC_XSTRING_APPEND)

TRIO_PUBLIC_STRING int
trio_xstring_append
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  size_t length;

  assert(self);
  assert(other);

  length = self->length + trio_length(other);
  if (!internal_string_grow_to(self, length))
    goto error;
  trio_copy(&self->content[self->length], other);
  self->length = length;
  return TRUE;

 error:
  return FALSE;
}

#endif

/*
 * trio_xstring_append_char
 */
#if defined(TRIO_FUNC_XSTRING_APPEND_CHAR)

TRIO_PUBLIC_STRING int
trio_xstring_append_char
TRIO_ARGS2((self, character),
       trio_string_t *self,
       char character)
{
  assert(self);

  if ((int)self->length >= trio_string_size(self))
    {
      if (!internal_string_grow(self, 0))
    goto error;
    }
  self->content[self->length] = character;
  self->length++;
  return TRUE;

 error:
  return FALSE;
}

#endif

/*
 * trio_xstring_append_max
 */
#if defined(TRIO_FUNC_XSTRING_APPEND_MAX)

TRIO_PUBLIC_STRING int
trio_xstring_append_max
TRIO_ARGS3((self, other, max),
       trio_string_t *self,
       TRIO_CONST char *other,
           size_t max)
{
  size_t length;

  assert(self);
  assert(other);

  length = self->length + trio_length_max(other, max);
  if (!internal_string_grow_to(self, length))
    goto error;

  /*
   * Pass max + 1 since trio_copy_max copies one character less than
   * this from the source to make room for a terminating zero.
   */
  trio_copy_max(&self->content[self->length], max + 1, other);
  self->length = length;
  return TRUE;

 error:
  return FALSE;
}

#endif

/**
   Search for the first occurrence of second parameter in the first.

   @param self Dynamic string to be modified.
   @param other Dynamic string to copy from.
   @return Boolean value indicating success or failure.
*/
#if defined(TRIO_FUNC_STRING_CONTAINS)

TRIO_PUBLIC_STRING int
trio_string_contains
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_contains(self->content, other->content);
}

#endif

/*
 * trio_xstring_contains
 */
#if defined(TRIO_FUNC_XSTRING_CONTAINS)

TRIO_PUBLIC_STRING int
trio_xstring_contains
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_contains(self->content, other);
}

#endif

/*
 * trio_string_copy
 */
#if defined(TRIO_FUNC_STRING_COPY)

TRIO_PUBLIC_STRING int
trio_string_copy
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  self->length = 0;
  return trio_string_append(self, other);
}

#endif


/*
 * trio_xstring_copy
 */
#if defined(TRIO_FUNC_XSTRING_COPY)

TRIO_PUBLIC_STRING int
trio_xstring_copy
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  self->length = 0;
  return trio_xstring_append(self, other);
}

#endif

/*
 * trio_string_duplicate
 */
#if defined(TRIO_FUNC_STRING_DUPLICATE)

TRIO_PUBLIC_STRING trio_string_t *
trio_string_duplicate
TRIO_ARGS1((other),
       trio_string_t *other)
{
  trio_string_t *self;

  assert(other);

  self = internal_string_alloc();
  if (self)
    {
      self->content = internal_duplicate_max(other->content, other->length);
      if (self->content)
    {
      self->length = other->length;
      self->allocated = self->length + 1;
    }
      else
    {
      self->length = self->allocated = 0;
    }
    }
  return self;
}

#endif

/*
 * trio_xstring_duplicate
 */
#if defined(TRIO_FUNC_XSTRING_DUPLICATE)

TRIO_PUBLIC_STRING trio_string_t *
trio_xstring_duplicate
TRIO_ARGS1((other),
       TRIO_CONST char *other)
{
  trio_string_t *self;

  assert(other);

  self = internal_string_alloc();
  if (self)
    {
      self->content = internal_duplicate_max(other, trio_length(other));
      if (self->content)
    {
      self->length = trio_length(self->content);
      self->allocated = self->length + 1;
    }
      else
    {
      self->length = self->allocated = 0;
    }
    }
  return self;
}

#endif

/*
 * trio_string_equal
 */
#if defined(TRIO_FUNC_STRING_EQUAL)

TRIO_PUBLIC_STRING int
trio_string_equal
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_equal(self->content, other->content);
}

#endif


/*
 * trio_xstring_equal
 */
#if defined(TRIO_FUNC_XSTRING_EQUAL)

TRIO_PUBLIC_STRING int
trio_xstring_equal
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_equal(self->content, other);
}

#endif

/*
 * trio_string_equal_max
 */
#if defined(TRIO_FUNC_STRING_EQUAL_MAX)

TRIO_PUBLIC_STRING int
trio_string_equal_max
TRIO_ARGS3((self, max, other),
       trio_string_t *self,
       size_t max,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_equal_max(self->content, max, other->content);
}
#endif

/*
 * trio_xstring_equal_max
 */
#if defined(TRIO_FUNC_XSTRING_EQUAL_MAX)

TRIO_PUBLIC_STRING int
trio_xstring_equal_max
TRIO_ARGS3((self, max, other),
       trio_string_t *self,
       size_t max,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_equal_max(self->content, max, other);
}

#endif

/*
 * trio_string_equal_case
 */
#if defined(TRIO_FUNC_STRING_EQUAL_CASE)

TRIO_PUBLIC_STRING int
trio_string_equal_case
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_equal_case(self->content, other->content);
}

#endif

/*
 * trio_xstring_equal_case
 */
#if defined(TRIO_FUNC_XSTRING_EQUAL_CASE)

TRIO_PUBLIC_STRING int
trio_xstring_equal_case
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_equal_case(self->content, other);
}

#endif

/*
 * trio_string_equal_case_max
 */
#if defined(TRIO_FUNC_STRING_EQUAL_CASE_MAX)

TRIO_PUBLIC_STRING int
trio_string_equal_case_max
TRIO_ARGS3((self, max, other),
       trio_string_t *self,
       size_t max,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_equal_case_max(self->content, max, other->content);
}

#endif

/*
 * trio_xstring_equal_case_max
 */
#if defined(TRIO_FUNC_XSTRING_EQUAL_CASE_MAX)

TRIO_PUBLIC_STRING int
trio_xstring_equal_case_max
TRIO_ARGS3((self, max, other),
       trio_string_t *self,
       size_t max,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_equal_case_max(self->content, max, other);
}

#endif

/*
 * trio_string_format_data_max
 */
#if defined(TRIO_FUNC_STRING_FORMAT_DATE_MAX)

TRIO_PUBLIC_STRING size_t
trio_string_format_date_max
TRIO_ARGS4((self, max, format, datetime),
       trio_string_t *self,
       size_t max,
       TRIO_CONST char *format,
       TRIO_CONST struct tm *datetime)
{
  assert(self);

  return trio_format_date_max(self->content, max, format, datetime);
}

#endif

/*
 * trio_string_index
 */
#if defined(TRIO_FUNC_STRING_INDEX)

TRIO_PUBLIC_STRING char *
trio_string_index
TRIO_ARGS2((self, character),
       trio_string_t *self,
       int character)
{
  assert(self);

  return trio_index(self->content, character);
}

#endif

/*
 * trio_string_index_last
 */
#if defined(TRIO_FUNC_STRING_INDEX_LAST)

TRIO_PUBLIC_STRING char *
trio_string_index_last
TRIO_ARGS2((self, character),
       trio_string_t *self,
       int character)
{
  assert(self);

  return trio_index_last(self->content, character);
}

#endif

/*
 * trio_string_length
 */
#if defined(TRIO_FUNC_STRING_LENGTH)

TRIO_PUBLIC_STRING int
trio_string_length
TRIO_ARGS1((self),
       trio_string_t *self)
{
  assert(self);

  if (self->length == 0)
    {
      self->length = trio_length(self->content);
    }
  return self->length;
}

#endif

/*
 * trio_string_lower
 */
#if defined(TRIO_FUNC_STRING_LOWER)

TRIO_PUBLIC_STRING int
trio_string_lower
TRIO_ARGS1((self),
       trio_string_t *self)
{
  assert(self);

  return trio_lower(self->content);
}

#endif

/*
 * trio_string_match
 */
#if defined(TRIO_FUNC_STRING_MATCH)

TRIO_PUBLIC_STRING int
trio_string_match
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_match(self->content, other->content);
}

#endif

/*
 * trio_xstring_match
 */
#if defined(TRIO_FUNC_XSTRING_MATCH)

TRIO_PUBLIC_STRING int
trio_xstring_match
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_match(self->content, other);
}

#endif

/*
 * trio_string_match_case
 */
#if defined(TRIO_FUNC_STRING_MATCH_CASE)

TRIO_PUBLIC_STRING int
trio_string_match_case
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_match_case(self->content, other->content);
}

#endif

/*
 * trio_xstring_match_case
 */
#if defined(TRIO_FUNC_XSTRING_MATCH_CASE)

TRIO_PUBLIC_STRING int
trio_xstring_match_case
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_match_case(self->content, other);
}

#endif

/*
 * trio_string_substring
 */
#if defined(TRIO_FUNC_STRING_SUBSTRING)

TRIO_PUBLIC_STRING char *
trio_string_substring
TRIO_ARGS2((self, other),
       trio_string_t *self,
       trio_string_t *other)
{
  assert(self);
  assert(other);

  return trio_substring(self->content, other->content);
}

#endif

/*
 * trio_xstring_substring
 */
#if defined(TRIO_FUNC_XSTRING_SUBSTRING)

TRIO_PUBLIC_STRING char *
trio_xstring_substring
TRIO_ARGS2((self, other),
       trio_string_t *self,
       TRIO_CONST char *other)
{
  assert(self);
  assert(other);

  return trio_substring(self->content, other);
}

#endif

/*
 * trio_string_upper
 */
#if defined(TRIO_FUNC_STRING_UPPER)

TRIO_PUBLIC_STRING int
trio_string_upper
TRIO_ARGS1((self),
       trio_string_t *self)
{
  assert(self);

  return trio_upper(self->content);
}

#endif

/*************************************************************************
 * TrioInitializeParameter
 *
 * Description:
 *  Initialize a trio_parameter_t struct.
 */
TRIO_PRIVATE void
TrioInitializeParameter
TRIO_ARGS1((parameter),
	   trio_parameter_t *parameter)
{
  parameter->type = FORMAT_UNKNOWN;
  parameter->flags = 0;
  parameter->width = 0;
  parameter->precision = 0;
  parameter->base = 0;
  parameter->baseSpecifier = 0;
  parameter->varsize = 0;
  parameter->beginOffset = 0;
  parameter->endOffset = 0;
  parameter->position = 0;
  parameter->data.pointer = 0;
#if TRIO_FEATURE_USER_DEFINED
  parameter->user_defined.handler = 0;
  parameter->user_data[0] = 0;
#endif
}

/*************************************************************************
 * TrioCopyParameter
 *
 * Description:
 *  Copies one trio_parameter_t struct to another.
 */
TRIO_PRIVATE void
TrioCopyParameter
TRIO_ARGS2((target, source),
	   trio_parameter_t *target,
	   TRIO_CONST trio_parameter_t *source)
{
#if TRIO_FEATURE_USER_DEFINED
  size_t i;
#endif

  target->type = source->type;
  target->flags = source->flags;
  target->width = source->width;
  target->precision = source->precision;
  target->base = source->base;
  target->baseSpecifier = source->baseSpecifier;
  target->varsize = source->varsize;
  target->beginOffset = source->beginOffset;
  target->endOffset = source->endOffset;
  target->position = source->position;
  target->data = source->data;

#if TRIO_FEATURE_USER_DEFINED
  target->user_defined = source->user_defined;

  for (i = 0U; i < sizeof(target->user_data); ++i)
    {
      if ((target->user_data[i] = source->user_data[i]) == NIL)
	break;
    }
#endif
}

/*************************************************************************
 * TrioIsQualifier
 *
 * Description:
 *  Remember to add all new qualifiers to this function.
 *  QUALIFIER_POSITION must not be added.
 */
TRIO_PRIVATE BOOLEAN_T
TrioIsQualifier
TRIO_ARGS1((character),
	   TRIO_CONST char character)
{
  /* QUALIFIER_POSITION is not included */
  switch (character)
    {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case QUALIFIER_PLUS:
    case QUALIFIER_MINUS:
    case QUALIFIER_SPACE:
    case QUALIFIER_DOT:
    case QUALIFIER_STAR:
    case QUALIFIER_ALTERNATIVE:
    case QUALIFIER_SHORT:
    case QUALIFIER_LONG:
    case QUALIFIER_CIRCUMFLEX:
    case QUALIFIER_LONG_UPPER:
    case QUALIFIER_SIZE_T:
    case QUALIFIER_PTRDIFF_T:
    case QUALIFIER_INTMAX_T:
    case QUALIFIER_QUAD:
    case QUALIFIER_SIZE_T_UPPER:
#if defined(QUALIFIER_WIDECHAR)
    case QUALIFIER_WIDECHAR:
#endif
    case QUALIFIER_QUOTE:
    case QUALIFIER_STICKY:
    case QUALIFIER_VARSIZE:
#if defined(QUALIFIER_PARAM)
    case QUALIFIER_PARAM:
#endif
    case QUALIFIER_FIXED_SIZE:
    case QUALIFIER_ROUNDING_UPPER:
      return TRUE;
    default:
      return FALSE;
    }
}

/*************************************************************************
 * TrioSetLocale
 */
#if defined(USE_LOCALE)
TRIO_PRIVATE void
TrioSetLocale(TRIO_NOARGS)
{
  internalLocaleValues = (struct lconv *)localeconv();
  if (internalLocaleValues)
    {
      if ((internalLocaleValues->decimal_point) &&
	  (internalLocaleValues->decimal_point[0] != NIL))
	{
	  internalDecimalPointLength = trio_length(internalLocaleValues->decimal_point);
	  if (internalDecimalPointLength == 1)
	    {
	      internalDecimalPoint = internalLocaleValues->decimal_point[0];
	    }
	  else
	    {
	      internalDecimalPoint = NIL;
	      trio_copy_max(internalDecimalPointString,
			    sizeof(internalDecimalPointString),
			    internalLocaleValues->decimal_point);
	    }
	}
# if TRIO_EXTENSION
      if ((internalLocaleValues->thousands_sep) &&
	  (internalLocaleValues->thousands_sep[0] != NIL))
	{
	  trio_copy_max(internalThousandSeparator,
			sizeof(internalThousandSeparator),
			internalLocaleValues->thousands_sep);
	  internalThousandSeparatorLength = trio_length(internalThousandSeparator);
	}
# endif
# if TRIO_EXTENSION
      if ((internalLocaleValues->grouping) &&
	  (internalLocaleValues->grouping[0] != NIL))
	{
	  trio_copy_max(internalGrouping,
			sizeof(internalGrouping),
			internalLocaleValues->grouping);
	}
# endif
    }
}
#endif /* defined(USE_LOCALE) */

#if TRIO_FEATURE_FLOAT && TRIO_FEATURE_QUOTE
TRIO_PRIVATE int
TrioCalcThousandSeparatorLength
TRIO_ARGS1((digits),
	   int digits)
{
  int count = 0;
  int step = NO_GROUPING;
  char *groupingPointer = internalGrouping;

  while (digits > 0)
    {
      if (*groupingPointer == CHAR_MAX)
	{
	  /* Disable grouping */
	  break; /* while */
	}
      else if (*groupingPointer == 0)
	{
	  /* Repeat last group */
	  if (step == NO_GROUPING)
	    {
	      /* Error in locale */
	      break; /* while */
	    }
	}
      else
	{
	  step = *groupingPointer++;
	}
      if (digits > step)
	count += internalThousandSeparatorLength;
      digits -= step;
    }
  return count;
}
#endif /* TRIO_FEATURE_FLOAT && TRIO_FEATURE_QUOTE */

#if TRIO_FEATURE_QUOTE
TRIO_PRIVATE BOOLEAN_T
TrioFollowedBySeparator
TRIO_ARGS1((position),
	   int position)
{
  int step = 0;
  char *groupingPointer = internalGrouping;

  position--;
  if (position == 0)
    return FALSE;
  while (position > 0)
    {
      if (*groupingPointer == CHAR_MAX)
	{
	  /* Disable grouping */
	  break; /* while */
	}
      else if (*groupingPointer != 0)
	{
	  step = *groupingPointer++;
	}
      if (step == 0)
	break;
      position -= step;
    }
  return (position == 0);
}
#endif /* TRIO_FEATURE_QUOTE */

/*************************************************************************
 * TrioGetPosition
 *
 * Get the %n$ position.
 */
TRIO_PRIVATE int
TrioGetPosition
TRIO_ARGS2((format, offsetPointer),
	   TRIO_CONST char *format,
	   int *offsetPointer)
{
#if TRIO_FEATURE_POSITIONAL
  char *tmpformat;
  int number = 0;
  int offset = *offsetPointer;

  number = (int)trio_to_long(&format[offset], &tmpformat, BASE_DECIMAL);
  offset = (int)(tmpformat - format);
  if ((number != 0) && (QUALIFIER_POSITION == format[offset++]))
    {
      *offsetPointer = offset;
      /*
       * number is decreased by 1, because n$ starts from 1, whereas
       * the array it is indexing starts from 0.
       */
      return number - 1;
    }
#endif
  return NO_POSITION;
}

/*************************************************************************
 * TrioFindNamespace
 *
 * Find registered user-defined specifier.
 * The prev argument is used for optimization only.
 */
#if TRIO_FEATURE_USER_DEFINED
TRIO_PRIVATE trio_userdef_t *
TrioFindNamespace
TRIO_ARGS2((name, prev),
	   TRIO_CONST char *name,
	   trio_userdef_t **prev)
{
  trio_userdef_t *def;
  
  if (internalEnterCriticalRegion)
    (void)internalEnterCriticalRegion(NULL);
  
  for (def = internalUserDef; def; def = def->next)
    {
      /* Case-sensitive string comparison */
      if (trio_equal_case(def->name, name))
	break;
      
      if (prev)
	*prev = def;
    }
  
  if (internalLeaveCriticalRegion)
    (void)internalLeaveCriticalRegion(NULL);
  
  return def;
}
#endif

/*************************************************************************
 * TrioPower
 *
 * Description:
 *  Calculate pow(base, exponent), where number and exponent are integers.
 */
#if TRIO_FEATURE_FLOAT
TRIO_PRIVATE trio_long_double_t
TrioPower
TRIO_ARGS2((number, exponent),
	   int number,
	   int exponent)
{
  trio_long_double_t result;

  if (number == 10)
    {
      switch (exponent)
	{
	  /* Speed up calculation of common cases */
	case 0:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E-1);
	  break;
	case 1:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+0);
	  break;
	case 2:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+1);
	  break;
	case 3:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+2);
	  break;
	case 4:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+3);
	  break;
	case 5:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+4);
	  break;
	case 6:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+5);
	  break;
	case 7:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+6);
	  break;
	case 8:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+7);
	  break;
	case 9:
	  result = (trio_long_double_t)number * TRIO_SUFFIX_LONG(1E+8);
	  break;
	default:
	  result = trio_pow((trio_long_double_t)number,
			    (trio_long_double_t)exponent);
	  break;
	}
    }
  else
    {
      return trio_pow((trio_long_double_t)number,
		      (trio_long_double_t)exponent);
    }
  return result;
}
#endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * TrioLogarithm
 */
#if TRIO_FEATURE_FLOAT
TRIO_PRIVATE trio_long_double_t
TrioLogarithm
TRIO_ARGS2((number, base),
	   trio_long_double_t number,
	   int base)
{
  trio_long_double_t result;

  if (number <= 0.0)
    {
      /* xlC crashes on log(0) */
      result = (number == 0.0) ? trio_ninf() : trio_nan();
    }
  else
    {
      if (base == 10)
	{
	  result = trio_log10(number);
	}
      else
	{
	  result = trio_log10(number) / trio_log10((double)base);
	}
    }
  return result;
}
#endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * TrioLogarithmBase
 */
#if TRIO_FEATURE_FLOAT
TRIO_PRIVATE double
TrioLogarithmBase
TRIO_ARGS1((base),
	   int base)
{
  switch (base)
    {
    case BASE_BINARY : return 1.0;
    case BASE_OCTAL  : return 3.0;
    case BASE_DECIMAL: return 3.321928094887362345;
    case BASE_HEX    : return 4.0;
    default          : return TrioLogarithm((double)base, 2);
    }
}
#endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * TrioParseQualifiers
 *
 * Description:
 *  Parse the qualifiers of a potential conversion specifier
 */
TRIO_PRIVATE int
TrioParseQualifiers
TRIO_ARGS4((type, format, offset, parameter),
	   int type,
	   TRIO_CONST char *format,
	   int offset,
	   trio_parameter_t *parameter)
{
  char ch;
  int dots = 0;  /* Count number of dots in modifier part */
  char *tmpformat;

  parameter->beginOffset = offset - 1;
  parameter->flags = FLAGS_NEW;
  parameter->position = TrioGetPosition(format, &offset);

  /* Default values */
  parameter->width = NO_WIDTH;
  parameter->precision = NO_PRECISION;
  parameter->base = NO_BASE;
  parameter->varsize = NO_SIZE;

  while (TrioIsQualifier(format[offset]))
    {
      ch = format[offset++];

      switch (ch)
        {
	case QUALIFIER_SPACE:
	  parameter->flags |= FLAGS_SPACE;
	  break;

	case QUALIFIER_PLUS:
	  parameter->flags |= FLAGS_SHOWSIGN;
	  break;

	case QUALIFIER_MINUS:
	  parameter->flags |= FLAGS_LEFTADJUST;
	  parameter->flags &= ~FLAGS_NILPADDING;
	  break;

	case QUALIFIER_ALTERNATIVE:
	  parameter->flags |= FLAGS_ALTERNATIVE;
	  break;

	case QUALIFIER_DOT:
	  if (dots == 0) /* Precision */
	    {
	      dots++;

	      /* Skip if no precision */
	      if (QUALIFIER_DOT == format[offset])
		break;

	      /* After the first dot we have the precision */
	      parameter->flags |= FLAGS_PRECISION;
	      if ((QUALIFIER_STAR == format[offset])
#if defined(QUALIFIER_PARAM)
		  || (QUALIFIER_PARAM == format[offset])
#endif
		  )
		{
		  offset++;
		  parameter->flags |= FLAGS_PRECISION_PARAMETER;
		  parameter->precision = TrioGetPosition(format, &offset);
		}
	      else
		{
		  parameter->precision = trio_to_long(&format[offset],
						      &tmpformat,
						      BASE_DECIMAL);
		  offset = (int)(tmpformat - format);
		}
	    }
	  else if (dots == 1) /* Base */
	    {
	      dots++;

	      /* After the second dot we have the base */
	      parameter->flags |= FLAGS_BASE;
	      if ((QUALIFIER_STAR == format[offset])
#if defined(QUALIFIER_PARAM)
		  || (QUALIFIER_PARAM == format[offset])
#endif
		  )
		{
		  offset++;
		  parameter->flags |= FLAGS_BASE_PARAMETER;
		  parameter->base = TrioGetPosition(format, &offset);
		}
	      else
		{
		  parameter->base = trio_to_long(&format[offset],
						 &tmpformat,
						 BASE_DECIMAL);
		  if (parameter->base > MAX_BASE)
		    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
		  offset = (int)(tmpformat - format);
		}
	    }
	  else
	    {
	      return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
	    }
	  break; /* QUALIFIER_DOT */

#if defined(QUALIFIER_PARAM)
	case QUALIFIER_PARAM:
	  parameter->type = TYPE_PRINT;
	  /* FALLTHROUGH */
#endif
	case QUALIFIER_STAR:
	  /* This has different meanings for print and scan */
	  if (TYPE_PRINT == type)
	    {
	      /* Read with from parameter */
	      int width = TrioGetPosition(format, &offset);
	      parameter->flags |= (FLAGS_WIDTH | FLAGS_WIDTH_PARAMETER);
	      if (NO_POSITION != width)
                parameter->width = width;
	      /* else keep parameter->width = NO_WIDTH which != NO_POSITION */
	    }
#if TRIO_FEATURE_SCANF
	  else
	    {
	      /* Scan, but do not store result */
	      parameter->flags |= FLAGS_IGNORE;
	    }
#endif
	  break; /* QUALIFIER_STAR */

	case '0':
	  if (! (parameter->flags & FLAGS_LEFTADJUST))
	    parameter->flags |= FLAGS_NILPADDING;
	  /* FALLTHROUGH */
	case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
	  parameter->flags |= FLAGS_WIDTH;
	  /*
	   * &format[offset - 1] is used to "rewind" the read
	   * character from format
	   */
	  parameter->width = trio_to_long(&format[offset - 1],
					  &tmpformat,
					  BASE_DECIMAL);
	  offset = (int)(tmpformat - format);
	  break;

	case QUALIFIER_SHORT:
	  if (parameter->flags & FLAGS_SHORTSHORT)
	    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
	  else if (parameter->flags & FLAGS_SHORT)
	    parameter->flags |= FLAGS_SHORTSHORT;
	  else
	    parameter->flags |= FLAGS_SHORT;
	  break;

	case QUALIFIER_LONG:
	  if (parameter->flags & FLAGS_QUAD)
	    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
	  else if (parameter->flags & FLAGS_LONG)
	    parameter->flags |= FLAGS_QUAD;
	  else
	    parameter->flags |= FLAGS_LONG;
	  break;

#if TRIO_FEATURE_LONGDOUBLE
	case QUALIFIER_LONG_UPPER:
	  parameter->flags |= FLAGS_LONGDOUBLE;
	  break;
#endif

#if TRIO_FEATURE_SIZE_T
	case QUALIFIER_SIZE_T:
	  parameter->flags |= FLAGS_SIZE_T;
	  /* Modify flags for later truncation of number */
	  if (sizeof(size_t) == sizeof(trio_ulonglong_t))
	    parameter->flags |= FLAGS_QUAD;
	  else if (sizeof(size_t) == sizeof(long))
	    parameter->flags |= FLAGS_LONG;
	  break;
#endif

#if TRIO_FEATURE_PTRDIFF_T
	case QUALIFIER_PTRDIFF_T:
	  parameter->flags |= FLAGS_PTRDIFF_T;
	  if (sizeof(ptrdiff_t) == sizeof(trio_ulonglong_t))
	    parameter->flags |= FLAGS_QUAD;
	  else if (sizeof(ptrdiff_t) == sizeof(long))
	    parameter->flags |= FLAGS_LONG;
	  break;
#endif

#if TRIO_FEATURE_INTMAX_T
	case QUALIFIER_INTMAX_T:
	  parameter->flags |= FLAGS_INTMAX_T;
	  if (sizeof(trio_intmax_t) == sizeof(trio_ulonglong_t))
	    parameter->flags |= FLAGS_QUAD;
	  else if (sizeof(trio_intmax_t) == sizeof(long))
	    parameter->flags |= FLAGS_LONG;
	  break;
#endif

#if TRIO_FEATURE_QUAD
	case QUALIFIER_QUAD:
	  parameter->flags |= FLAGS_QUAD;
	  break;
#endif

#if TRIO_FEATURE_FIXED_SIZE
	case QUALIFIER_FIXED_SIZE:
	  if (parameter->flags & FLAGS_FIXED_SIZE)
	    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);

	  if (parameter->flags & (FLAGS_ALL_SIZES |
				  FLAGS_LONGDOUBLE |
				  FLAGS_WIDECHAR |
				  FLAGS_VARSIZE_PARAMETER))
	    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);

	  if ((format[offset] == '6') &&
	      (format[offset + 1] == '4'))
	    {
	      parameter->varsize = sizeof(trio_int64_t);
	      offset += 2;
	    }
	  else if ((format[offset] == '3') &&
		   (format[offset + 1] == '2'))
	    {
	      parameter->varsize = sizeof(trio_int32_t);
	      offset += 2;
	    }
	  else if ((format[offset] == '1') &&
		   (format[offset + 1] == '6'))
	    {
	      parameter->varsize = sizeof(trio_int16_t);
	      offset += 2;
	    }
	  else if (format[offset] == '8')
	    {
	      parameter->varsize = sizeof(trio_int8_t);
	      offset++;
	    }
	  else
	    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);

	  parameter->flags |= FLAGS_FIXED_SIZE;
	  break;
#endif /* TRIO_FEATURE_FIXED_SIZE */

#if defined(QUALIFIER_WIDECHAR)
	case QUALIFIER_WIDECHAR:
	  parameter->flags |= FLAGS_WIDECHAR;
	  break;
#endif

#if TRIO_FEATURE_SIZE_T_UPPER
	case QUALIFIER_SIZE_T_UPPER:
	  break;
#endif

#if TRIO_FEATURE_QUOTE
	case QUALIFIER_QUOTE:
	  parameter->flags |= FLAGS_QUOTE;
	  break;
#endif

#if TRIO_FEATURE_STICKY
	case QUALIFIER_STICKY:
	  parameter->flags |= FLAGS_STICKY;
	  break;
#endif

#if TRIO_FEATURE_VARSIZE
	case QUALIFIER_VARSIZE:
	  parameter->flags |= FLAGS_VARSIZE_PARAMETER;
	  break;
#endif

#if TRIO_FEATURE_ROUNDING
	case QUALIFIER_ROUNDING_UPPER:
	  parameter->flags |= FLAGS_ROUNDING;
	  break;
#endif

	default:
	  /* Bail out completely to make the error more obvious */
	  return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
	}
    } /* while qualifier */

  parameter->endOffset = offset;

  return 0;
}

/*************************************************************************
 * TrioParseSpecifier
 *
 * Description:
 *  Parse the specifier part of a potential conversion specifier
 */
TRIO_PRIVATE int
TrioParseSpecifier
TRIO_ARGS4((type, format, offset, parameter),
	   int type,
	   TRIO_CONST char *format,
	   int offset,
	   trio_parameter_t *parameter)
{
////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: Silence unused parameter warning.
////////////////////////////////////////////////////////////////////////////////////////////////////
  (void)type;
////////////////////////////////////////////////////////////////////////////////////////////////////
  parameter->baseSpecifier = NO_BASE;

  switch (format[offset++])
    {
#if defined(SPECIFIER_CHAR_UPPER)
    case SPECIFIER_CHAR_UPPER:
      parameter->flags |= FLAGS_WIDECHAR;
      /* FALLTHROUGH */
#endif
    case SPECIFIER_CHAR:
      if (parameter->flags & FLAGS_LONG)
	parameter->flags |= FLAGS_WIDECHAR;
      else if (parameter->flags & FLAGS_SHORT)
	parameter->flags &= ~FLAGS_WIDECHAR;
      parameter->type = FORMAT_CHAR;
      break;

#if defined(SPECIFIER_STRING_UPPER)
    case SPECIFIER_STRING_UPPER:
      parameter->flags |= FLAGS_WIDECHAR;
      /* FALLTHROUGH */
#endif
    case SPECIFIER_STRING:
      if (parameter->flags & FLAGS_LONG)
	parameter->flags |= FLAGS_WIDECHAR;
      else if (parameter->flags & FLAGS_SHORT)
	parameter->flags &= ~FLAGS_WIDECHAR;
      parameter->type = FORMAT_STRING;
      break;

#if defined(SPECIFIER_GROUP)
    case SPECIFIER_GROUP:
      if (TYPE_SCAN == type)
	{
	  int depth = 1;
	  parameter->type = FORMAT_GROUP;
	  if (format[offset] == QUALIFIER_CIRCUMFLEX)
	    offset++;
	  if (format[offset] == SPECIFIER_UNGROUP)
	    offset++;
	  if (format[offset] == QUALIFIER_MINUS)
	    offset++;
	  /* Skip nested brackets */
	  while (format[offset] != NIL)
	    {
	      if (format[offset] == SPECIFIER_GROUP)
		{
		  depth++;
		}
	      else if (format[offset] == SPECIFIER_UNGROUP)
	      {
		if (--depth <= 0)
		  {
		    offset++;
		    break;
		  }
	      }
	      offset++;
	    }
	}
      break;
#endif /* defined(SPECIFIER_GROUP) */

    case SPECIFIER_INTEGER:
      parameter->type = FORMAT_INT;
      break;

    case SPECIFIER_UNSIGNED:
      parameter->flags |= FLAGS_UNSIGNED;
      parameter->type = FORMAT_INT;
      break;

    case SPECIFIER_DECIMAL:
      parameter->baseSpecifier = BASE_DECIMAL;
      parameter->type = FORMAT_INT;
      break;

    case SPECIFIER_OCTAL:
      parameter->flags |= FLAGS_UNSIGNED;
      parameter->baseSpecifier = BASE_OCTAL;
      parameter->type = FORMAT_INT;
      break;

#if TRIO_FEATURE_BINARY
    case SPECIFIER_BINARY_UPPER:
      parameter->flags |= FLAGS_UPPER;
      /* FALLTHROUGH */
    case SPECIFIER_BINARY:
      parameter->flags |= FLAGS_NILPADDING;
      parameter->baseSpecifier = BASE_BINARY;
      parameter->type = FORMAT_INT;
      break;
#endif

    case SPECIFIER_HEX_UPPER:
      parameter->flags |= FLAGS_UPPER;
      /* FALLTHROUGH */
    case SPECIFIER_HEX:
      parameter->flags |= FLAGS_UNSIGNED;
      parameter->baseSpecifier = BASE_HEX;
      parameter->type = FORMAT_INT;
      break;

#if defined(SPECIFIER_FLOAT_E)
# if defined(SPECIFIER_FLOAT_E_UPPER)
    case SPECIFIER_FLOAT_E_UPPER:
      parameter->flags |= FLAGS_UPPER;
      /* FALLTHROUGH */
# endif
    case SPECIFIER_FLOAT_E:
      parameter->flags |= FLAGS_FLOAT_E;
      parameter->type = FORMAT_DOUBLE;
      break;
#endif

#if defined(SPECIFIER_FLOAT_G)
# if defined(SPECIFIER_FLOAT_G_UPPER)
    case SPECIFIER_FLOAT_G_UPPER:
      parameter->flags |= FLAGS_UPPER;
      /* FALLTHROUGH */
# endif
    case SPECIFIER_FLOAT_G:
      parameter->flags |= FLAGS_FLOAT_G;
      parameter->type = FORMAT_DOUBLE;
      break;
#endif

#if defined(SPECIFIER_FLOAT_F)
# if defined(SPECIFIER_FLOAT_F_UPPER)
    case SPECIFIER_FLOAT_F_UPPER:
      parameter->flags |= FLAGS_UPPER;
      /* FALLTHROUGH */
# endif
    case SPECIFIER_FLOAT_F:
      parameter->type = FORMAT_DOUBLE;
      break;
#endif

#if defined(TRIO_COMPILER_VISUALC)
# pragma warning( push )
# pragma warning( disable : 4127 ) /* Conditional expression is constant */
#endif
    case SPECIFIER_POINTER:
      if (sizeof(trio_pointer_t) == sizeof(trio_ulonglong_t))
	parameter->flags |= FLAGS_QUAD;
      else if (sizeof(trio_pointer_t) == sizeof(long))
	parameter->flags |= FLAGS_LONG;
      parameter->type = FORMAT_POINTER;
      break;
#if defined(TRIO_COMPILER_VISUALC)
# pragma warning( pop )
#endif

    case SPECIFIER_COUNT:
      parameter->type = FORMAT_COUNT;
      break;

#if TRIO_FEATURE_HEXFLOAT
    case SPECIFIER_HEXFLOAT_UPPER:
      parameter->flags |= FLAGS_UPPER;
      /* FALLTHROUGH */
    case SPECIFIER_HEXFLOAT:
      parameter->baseSpecifier = BASE_HEX;
      parameter->type = FORMAT_DOUBLE;
      break;
#endif

#if TRIO_FEATURE_ERRNO
    case SPECIFIER_ERRNO:
      parameter->type = FORMAT_ERRNO;
      break;
#endif

#if TRIO_FEATURE_USER_DEFINED
    case SPECIFIER_USER_DEFINED_BEGIN:
      {
	unsigned int max;
	int without_namespace = TRUE;
	char* tmpformat = (char *)&format[offset];
	int ch;

	parameter->type = FORMAT_USER_DEFINED;
	parameter->user_defined.namespace[0] = NIL;

	while ((ch = format[offset]) != NIL)
	  {
	    offset++;
	    if ((ch == SPECIFIER_USER_DEFINED_END) || (ch == SPECIFIER_USER_DEFINED_EXTRA))
	      {
		if (without_namespace)
		  /* No namespace, handler will be passed as an argument */
		  parameter->flags |= FLAGS_USER_DEFINED_PARAMETER;

		/* Copy the user data */
		max = (unsigned int)(&format[offset] - tmpformat);
		if (max > MAX_USER_DATA)
		  max = MAX_USER_DATA;
		trio_copy_max(parameter->user_data, max, tmpformat);

		/* Skip extra data (which is only there to keep the compiler happy) */
		while ((ch != NIL) && (ch != SPECIFIER_USER_DEFINED_END))
		  ch = format[offset++];

		break; /* while */
	      }

	    if (ch == SPECIFIER_USER_DEFINED_SEPARATOR)
	      {
		without_namespace = FALSE;
		/* Copy the namespace for later looking-up */
		max = (int)(&format[offset] - tmpformat);
		if (max > MAX_USER_NAME)
		  max = MAX_USER_NAME;
		trio_copy_max(parameter->user_defined.namespace, max, tmpformat);
		tmpformat = (char *)&format[offset];
	      }
	  }

	if (ch != SPECIFIER_USER_DEFINED_END)
	  return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
      }
      break;
#endif /* TRIO_FEATURE_USER_DEFINED */

    default:
      /* Bail out completely to make the error more obvious */
      return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
  }

  parameter->endOffset = offset;

  return 0;
}

/*************************************************************************
 * TrioParse
 *
 * Description:
 *  Parse the format string
 */
TRIO_PRIVATE int
TrioParse
TRIO_ARGS6((type, format, parameters, arglist, argfunc, argarray),
	   int type,
	   TRIO_CONST char *format,
	   trio_parameter_t *parameters,
	   va_list arglist,
	   trio_argfunc_t argfunc,
	   trio_pointer_t *argarray)
{
  /* Count the number of times a parameter is referenced */
  unsigned short usedEntries[MAX_PARAMETERS];
  /* Parameter counters */
  int parameterPosition;
  int maxParam = -1;
  /* Utility variables */
  int offset;  /* Offset into formatting string */
  BOOLEAN_T positional;  /* Does the specifier have a positional? */
#if TRIO_FEATURE_STICKY
  BOOLEAN_T gotSticky = FALSE;  /* Are there any sticky modifiers at all? */
#endif
  /*
   * indices specifies the order in which the parameters must be
   * read from the va_args (this is necessary to handle positionals)
   */
  int indices[MAX_PARAMETERS];
  int pos = 0;
  /* Various variables */
#if defined(TRIO_COMPILER_SUPPORTS_MULTIBYTE)
  int charlen;
#endif
  int save_errno;
  int i = -1;
  int num;
  trio_parameter_t workParameter;
  int status;

  /* Both must be set or none must be set */
  assert(((argfunc == NULL) && (argarray == NULL)) ||
	 ((argfunc != NULL) && (argarray != NULL)));

  /*
   * The 'parameters' array is not initialized, but we need to
   * know which entries we have used.
   */
  memset(usedEntries, 0, sizeof(usedEntries));

  save_errno = errno;
  offset = 0;
  parameterPosition = 0;
#if defined(TRIO_COMPILER_SUPPORTS_MULTIBYTE)
  charlen = mblen(NULL, 0);
#endif
  
  while (format[offset])
    {
      TrioInitializeParameter(&workParameter);

#if defined(TRIO_COMPILER_SUPPORTS_MULTIBYTE)
      if (! isascii(format[offset]))
	{
	  /*
	   * Multibyte characters cannot be legal specifiers or
	   * modifiers, so we skip over them.
	   */
	  charlen = mblen(&format[offset], MB_LEN_MAX);
	  offset += (charlen > 0) ? charlen : 1;
	  continue; /* while */
	}
#endif /* TRIO_COMPILER_SUPPORTS_MULTIBYTE */

      switch(format[offset++]) {

      case CHAR_IDENTIFIER:
	{
	  if (CHAR_IDENTIFIER == format[offset])
	    {
	      /* skip double "%" */
	      offset++;
	      continue; /* while */
	    }

	  status = TrioParseQualifiers(type, format, offset, &workParameter);
	  if (status < 0)
	    return status; /* Return qualifier syntax error */

	  status = TrioParseSpecifier(type, format, workParameter.endOffset, &workParameter);
	  if (status < 0)
	    return status; /* Return specifier syntax error */
	}
	break;

#if TRIO_EXTENSION
      case CHAR_ALT_IDENTIFIER:
	{
	  status = TrioParseQualifiers(type, format, offset, &workParameter);
	  if (status < 0)
	    continue; /* False alert, not a user defined specifier */

	  status = TrioParseSpecifier(type, format, workParameter.endOffset, &workParameter);
	  if ((status < 0) || (FORMAT_USER_DEFINED != workParameter.type))
	    continue; /* False alert, not a user defined specifier */
	}
	break;
#endif

      default:
	continue; /* while */
      }

      /* now handle the parsed conversion specification */
      positional = (NO_POSITION != workParameter.position);

      /*
       * Parameters only need the type and value. The value is
       * read later.
       */
      if (workParameter.flags & FLAGS_WIDTH_PARAMETER)
        {
	  if (workParameter.width == NO_WIDTH)
	    {
	      workParameter.width = parameterPosition++;
	    }
	  else
	    {
	      if (! positional)
		  workParameter.position = workParameter.width + 1;
	    }

	  usedEntries[workParameter.width] += 1;
	  if (workParameter.width > maxParam)
	    maxParam = workParameter.width;
	  parameters[pos].type = FORMAT_PARAMETER;
	  parameters[pos].flags = 0;
	  indices[workParameter.width] = pos;
	  workParameter.width = pos++;
	}
      if (workParameter.flags & FLAGS_PRECISION_PARAMETER)
	{
	  if (workParameter.precision == NO_PRECISION)
	    {
	      workParameter.precision = parameterPosition++;
	    }
	  else
	    {
	      if (! positional)
		  workParameter.position = workParameter.precision + 1;
	    }

	  usedEntries[workParameter.precision] += 1;
	  if (workParameter.precision > maxParam)
	    maxParam = workParameter.precision;
	  parameters[pos].type = FORMAT_PARAMETER;
	  parameters[pos].flags = 0;
	  indices[workParameter.precision] = pos;
	  workParameter.precision = pos++;
	}
      if (workParameter.flags & FLAGS_BASE_PARAMETER)
	{
	  if (workParameter.base == NO_BASE)
	    {
	      workParameter.base = parameterPosition++;
	    }
	  else
	    {
	      if (! positional)
		  workParameter.position = workParameter.base + 1;
	    }

	  usedEntries[workParameter.base] += 1;
	  if (workParameter.base > maxParam)
	    maxParam = workParameter.base;
	  parameters[pos].type = FORMAT_PARAMETER;
	  parameters[pos].flags = 0;
	  indices[workParameter.base] = pos;
	  workParameter.base = pos++;
	}
#if TRIO_FEATURE_VARSIZE
      if (workParameter.flags & FLAGS_VARSIZE_PARAMETER)
	{
	  workParameter.varsize = parameterPosition++;

	  usedEntries[workParameter.varsize] += 1;
	  if (workParameter.varsize > maxParam)
	    maxParam = workParameter.varsize;
	  parameters[pos].type = FORMAT_PARAMETER;
	  parameters[pos].flags = 0;
	  indices[workParameter.varsize] = pos;
	  workParameter.varsize = pos++;
	}
#endif
#if TRIO_FEATURE_USER_DEFINED
      if (workParameter.flags & FLAGS_USER_DEFINED_PARAMETER)
	{
	  workParameter.user_defined.handler = parameterPosition++;

	  usedEntries[workParameter.user_defined.handler] += 1;
	  if (workParameter.user_defined.handler > maxParam)
	    maxParam = workParameter.user_defined.handler;
	  parameters[pos].type = FORMAT_PARAMETER;
	  parameters[pos].flags = FLAGS_USER_DEFINED;
	  indices[workParameter.user_defined.handler] = pos;
	  workParameter.user_defined.handler = pos++;
	}
#endif

      if (NO_POSITION == workParameter.position)
	{
	  workParameter.position = parameterPosition++;
	}

      if (workParameter.position > maxParam)
	maxParam = workParameter.position;

      if (workParameter.position >= MAX_PARAMETERS)
	{
	  /* Bail out completely to make the error more obvious */
	  return TRIO_ERROR_RETURN(TRIO_ETOOMANY, offset);
	}

      indices[workParameter.position] = pos;

      /*  Count the number of times this entry has been used */
      usedEntries[workParameter.position] += 1;

      /* Find last sticky parameters */
#if TRIO_FEATURE_STICKY
      if (workParameter.flags & FLAGS_STICKY)
	{
	  gotSticky = TRUE;
	}
      else if (gotSticky)
	{
	  for (i = pos - 1; i >= 0; i--)
	    {
	      if (parameters[i].type == FORMAT_PARAMETER)
		continue;
	      if ((parameters[i].flags & FLAGS_STICKY) &&
		  (parameters[i].type == workParameter.type))
		{
		  /* Do not overwrite current qualifiers */
		  workParameter.flags |= (parameters[i].flags & (unsigned long)~FLAGS_STICKY);
		  if (workParameter.width == NO_WIDTH)
		    workParameter.width = parameters[i].width;
		  if (workParameter.precision == NO_PRECISION)
		    workParameter.precision = parameters[i].precision;
		  if (workParameter.base == NO_BASE)
		    workParameter.base = parameters[i].base;
		  break;
		}
	    }
	}
#endif

      if (workParameter.base == NO_BASE)
	workParameter.base = BASE_DECIMAL;

      offset = workParameter.endOffset;

      TrioCopyParameter(&parameters[pos++], &workParameter);
    } /* while format characters left */

  parameters[pos].type = FORMAT_SENTINEL;  /* end parameter array with sentinel */
  parameters[pos].beginOffset = offset;

  for (num = 0; num <= maxParam; num++)
    {
      if (usedEntries[num] != 1)
	{
	  if (usedEntries[num] == 0) /* gap detected */
	    return TRIO_ERROR_RETURN(TRIO_EGAP, num);
	  else /* double references detected */
	    return TRIO_ERROR_RETURN(TRIO_EDBLREF, num);
	}
      
      i = indices[num];

      /*
       * FORMAT_PARAMETERS are only present if they must be read,
       * so it makes no sense to check the ignore flag (besides,
       * the flags variable is not set for that particular type)
       */
      if ((parameters[i].type != FORMAT_PARAMETER) &&
	  (parameters[i].flags & FLAGS_IGNORE))
	continue; /* for all arguments */

      /*
       * The stack arguments are read according to ANSI C89
       * default argument promotions:
       *
       *  char           = int
       *  short          = int
       *  unsigned char  = unsigned int
       *  unsigned short = unsigned int
       *  float          = double
       *
       * In addition to the ANSI C89 these types are read (the
       * default argument promotions of C99 has not been
       * considered yet)
       *
       *  long long
       *  long double
       *  size_t
       *  ptrdiff_t
       *  intmax_t
       */
      switch (parameters[i].type)
	{
	case FORMAT_GROUP:
	case FORMAT_STRING:
#if TRIO_FEATURE_WIDECHAR
	  if (parameters[i].flags & FLAGS_WIDECHAR)
	    {
	      parameters[i].data.wstring = (argfunc == NULL)
		? va_arg(arglist, trio_wchar_t *)
                : (trio_wchar_t *)(argfunc(argarray, num, TRIO_TYPE_PWCHAR));
	    }
	  else
#endif
	    {
	      parameters[i].data.string = (argfunc == NULL)
		? va_arg(arglist, char *)
                : (char *)(argfunc(argarray, num, TRIO_TYPE_PCHAR));
	    }
	  break;

#if TRIO_FEATURE_USER_DEFINED
	case FORMAT_USER_DEFINED:
#endif
	case FORMAT_POINTER:
	case FORMAT_COUNT:
	case FORMAT_UNKNOWN:
	  parameters[i].data.pointer = (argfunc == NULL)
	    ? va_arg(arglist, trio_pointer_t )
            : argfunc(argarray, num, TRIO_TYPE_POINTER);
	  break;

	case FORMAT_CHAR:
	case FORMAT_INT:
#if TRIO_FEATURE_SCANF
	  if (TYPE_SCAN == type)
	    {
              if (argfunc == NULL)
                parameters[i].data.pointer =
                  (trio_pointer_t)va_arg(arglist, trio_pointer_t);
              else
                {
                  if (parameters[i].type == FORMAT_CHAR)
                    parameters[i].data.pointer =
                      (trio_pointer_t)((char *)argfunc(argarray, num, TRIO_TYPE_CHAR));
                  else if (parameters[i].flags & FLAGS_SHORT)
                    parameters[i].data.pointer =
                      (trio_pointer_t)((short *)argfunc(argarray, num, TRIO_TYPE_SHORT));
                  else
                    parameters[i].data.pointer =
                      (trio_pointer_t)((int *)argfunc(argarray, num, TRIO_TYPE_INT));
                }
	    }
	  else
#endif /* TRIO_FEATURE_SCANF */
	    {
#if TRIO_FEATURE_VARSIZE || TRIO_FEATURE_FIXED_SIZE
	      if (parameters[i].flags
		  & (FLAGS_VARSIZE_PARAMETER | FLAGS_FIXED_SIZE))
		{
		  int varsize;
		  if (parameters[i].flags & FLAGS_VARSIZE_PARAMETER)
		    {
		      /*
		       * Variable sizes are mapped onto the fixed sizes, in
		       * accordance with integer promotion.
		       *
		       * Please note that this may not be portable, as we
		       * only guess the size, not the layout of the numbers.
		       * For example, if int is little-endian, and long is
		       * big-endian, then this will fail.
		       */
		      varsize = (int)parameters[parameters[i].varsize].data.number.as_unsigned;
		    }
		  else
		    {
		      /* Used for the I<bits> modifiers */
		      varsize = parameters[i].varsize;
		    }
		  parameters[i].flags &= ~FLAGS_ALL_VARSIZES;
		  
		  if (varsize <= (int)sizeof(int))
		    ;
		  else if (varsize <= (int)sizeof(long))
		    parameters[i].flags |= FLAGS_LONG;
#if TRIO_FEATURE_INTMAX_T
		  else if (varsize <= (int)sizeof(trio_longlong_t))
		    parameters[i].flags |= FLAGS_QUAD;
		  else
		    parameters[i].flags |= FLAGS_INTMAX_T;
#else
		  else
		    parameters[i].flags |= FLAGS_QUAD;
#endif
		}
#endif /* TRIO_FEATURE_VARSIZE */
#if TRIO_FEATURE_SIZE_T || TRIO_FEATURE_SIZE_T_UPPER
	      if (parameters[i].flags & FLAGS_SIZE_T)
		parameters[i].data.number.as_unsigned = (argfunc == NULL)
		  ? (trio_uintmax_t)va_arg(arglist, size_t)
                  : (trio_uintmax_t)(*((size_t *)argfunc(argarray, num, TRIO_TYPE_SIZE)));
	      else
#endif
#if TRIO_FEATURE_PTRDIFF_T
	      if (parameters[i].flags & FLAGS_PTRDIFF_T)
		parameters[i].data.number.as_unsigned = (argfunc == NULL)
		  ? (trio_uintmax_t)va_arg(arglist, ptrdiff_t)
                  : (trio_uintmax_t)(*((ptrdiff_t *)argfunc(argarray, num, TRIO_TYPE_PTRDIFF)));
	      else
#endif
#if TRIO_FEATURE_INTMAX_T
	      if (parameters[i].flags & FLAGS_INTMAX_T)
		parameters[i].data.number.as_unsigned = (argfunc == NULL)
		  ? (trio_uintmax_t)va_arg(arglist, trio_intmax_t)
                  : (trio_uintmax_t)(*((trio_intmax_t *)argfunc(argarray, num, TRIO_TYPE_UINTMAX)));
	      else
#endif
	      if (parameters[i].flags & FLAGS_QUAD)
		parameters[i].data.number.as_unsigned = (argfunc == NULL)
		  ? (trio_uintmax_t)va_arg(arglist, trio_ulonglong_t)
                  : (trio_uintmax_t)(*((trio_ulonglong_t *)argfunc(argarray, num, TRIO_TYPE_ULONGLONG)));
	      else if (parameters[i].flags & FLAGS_LONG)
		parameters[i].data.number.as_unsigned = (argfunc == NULL)
		  ? (trio_uintmax_t)va_arg(arglist, long)
                  : (trio_uintmax_t)(*((long *)argfunc(argarray, num, TRIO_TYPE_LONG)));
	      else
		{
		  if (argfunc == NULL)
		    parameters[i].data.number.as_unsigned = (trio_uintmax_t)va_arg(arglist, int);
		  else
		    {
		      if (parameters[i].type == FORMAT_CHAR)
			parameters[i].data.number.as_unsigned =
                          (trio_uintmax_t)(*((char *)argfunc(argarray, num, TRIO_TYPE_CHAR)));
		      else if (parameters[i].flags & FLAGS_SHORT)
			parameters[i].data.number.as_unsigned =
                          (trio_uintmax_t)(*((short *)argfunc(argarray, num, TRIO_TYPE_SHORT)));
		      else
			parameters[i].data.number.as_unsigned =
                          (trio_uintmax_t)(*((int *)argfunc(argarray, num, TRIO_TYPE_INT)));
		    }
		}
	    }
	  break;

	case FORMAT_PARAMETER:
	  /*
	   * The parameter for the user-defined specifier is a pointer,
	   * whereas the rest (width, precision, base) uses an integer.
	   */
	  if (parameters[i].flags & FLAGS_USER_DEFINED)
	    parameters[i].data.pointer = (argfunc == NULL)
	      ? va_arg(arglist, trio_pointer_t )
              : argfunc(argarray, num, TRIO_TYPE_POINTER);
	  else
	    parameters[i].data.number.as_unsigned = (argfunc == NULL)
	      ? (trio_uintmax_t)va_arg(arglist, int)
              : (trio_uintmax_t)(*((int *)argfunc(argarray, num, TRIO_TYPE_INT)));
	  break;

#if TRIO_FEATURE_FLOAT
	case FORMAT_DOUBLE:
# if TRIO_FEATURE_SCANF
	  if (TYPE_SCAN == type)
	    {
	      if (parameters[i].flags & FLAGS_LONGDOUBLE)
		parameters[i].data.longdoublePointer = (argfunc == NULL)
		  ? va_arg(arglist, trio_long_double_t *)
                  : (trio_long_double_t *)argfunc(argarray, num, TRIO_TYPE_LONGDOUBLE);
	      else
                {
		  if (parameters[i].flags & FLAGS_LONG)
		    parameters[i].data.doublePointer = (argfunc == NULL)
		      ? va_arg(arglist, double *)
                      : (double *)argfunc(argarray, num, TRIO_TYPE_DOUBLE);
		  else
		    parameters[i].data.doublePointer = (argfunc == NULL)
		      ? (double *)va_arg(arglist, float *)
                      : (double *)argfunc(argarray, num, TRIO_TYPE_DOUBLE);
                }
	    }
	  else
# endif /* TRIO_FEATURE_SCANF */
	    {
	      if (parameters[i].flags & FLAGS_LONGDOUBLE)
		parameters[i].data.longdoubleNumber = (argfunc == NULL)
		  ? va_arg(arglist, trio_long_double_t)
                  : (trio_long_double_t)(*((trio_long_double_t *)argfunc(argarray, num, TRIO_TYPE_LONGDOUBLE)));
	      else
		{
		  if (argfunc == NULL)
		    parameters[i].data.longdoubleNumber =
		      (trio_long_double_t)va_arg(arglist, double);
		  else
		    {
		      if (parameters[i].flags & FLAGS_SHORT)
			parameters[i].data.longdoubleNumber =
                          (trio_long_double_t)(*((float *)argfunc(argarray, num, TRIO_TYPE_FLOAT)));
		      else
			parameters[i].data.longdoubleNumber =
                          (trio_long_double_t)(*((double *)argfunc(argarray, num, TRIO_TYPE_DOUBLE)));
		    }
		}
	    }
	  break;
#endif /* TRIO_FEATURE_FLOAT */

#if TRIO_FEATURE_ERRNO
	case FORMAT_ERRNO:
	  parameters[i].data.errorNumber = save_errno;
	  break;
#endif

	default:
	  break;
	}
    } /* for all specifiers */
  return num;
}


/*************************************************************************
 *
 * FORMATTING
 *
 ************************************************************************/


/*************************************************************************
 * TrioWriteNumber
 *
 * Description:
 *  Output a number.
 *  The complexity of this function is a result of the complexity
 *  of the dependencies of the flags.
 */
TRIO_PRIVATE void
TrioWriteNumber
TRIO_ARGS6((self, number, flags, width, precision, base),
	   trio_class_t *self,
	   trio_uintmax_t number,
	   trio_flags_t flags,
	   int width,
	   int precision,
	   int base)
{
  BOOLEAN_T isNegative;
  BOOLEAN_T isNumberZero;
  BOOLEAN_T isPrecisionZero;
  BOOLEAN_T ignoreNumber;
  char buffer[MAX_CHARS_IN(trio_uintmax_t) * (1 + MAX_LOCALE_SEPARATOR_LENGTH) + 1];
  char *bufferend;
  char *pointer;
  TRIO_CONST char *digits;
  int i;
#if TRIO_FEATURE_QUOTE
  int length;
  char *p;
#endif
  int count;
  int digitOffset;

  assert(VALID(self));
  assert(VALID(self->OutStream));
  assert(((base >= MIN_BASE) && (base <= MAX_BASE)) || (base == NO_BASE));

  digits = (flags & FLAGS_UPPER) ? internalDigitsUpper : internalDigitsLower;
  if (base == NO_BASE)
    base = BASE_DECIMAL;

  isNumberZero = (number == 0);
  isPrecisionZero = (precision == 0);
  ignoreNumber = (isNumberZero
		  && isPrecisionZero
		  && !((flags & FLAGS_ALTERNATIVE) && (base == BASE_OCTAL)));

  if (flags & FLAGS_UNSIGNED)
    {
      isNegative = FALSE;
      flags &= ~FLAGS_SHOWSIGN;
    }
  else
    {
      isNegative = ((trio_intmax_t)number < 0);
      if (isNegative)
	number = -((trio_intmax_t)number);
    }

  if (flags & FLAGS_QUAD)
    number &= (trio_ulonglong_t)-1;
  else if (flags & FLAGS_LONG)
    number &= (unsigned long)-1;
  else
    number &= (unsigned int)-1;
  
  /* Build number */
  pointer = bufferend = &buffer[sizeof(buffer) - 1];
  *pointer-- = NIL;
  for (i = 1; i < (int)sizeof(buffer); i++)
    {
      digitOffset = number % base;
      *pointer-- = digits[digitOffset];
      number /= base;
      if (number == 0)
	break;

#if TRIO_FEATURE_QUOTE
      if ((flags & FLAGS_QUOTE) && TrioFollowedBySeparator(i + 1))
	{
	  /*
	   * We are building the number from the least significant
	   * to the most significant digit, so we have to copy the
	   * thousand separator backwards
	   */
	  length = internalThousandSeparatorLength;
	  if (((int)(pointer - buffer) - length) > 0)
	    {
	      p = &internalThousandSeparator[length - 1];
	      while (length-- > 0)
		*pointer-- = *p--;
	    }
	}
#endif
    }

  if (! ignoreNumber)
    {
      /* Adjust width */
      width -= (bufferend - pointer) - 1;
    }

  /* Adjust precision */
  if (NO_PRECISION != precision)
    {
      precision -= (bufferend - pointer) - 1;
      if (precision < 0)
	precision = 0;
      flags |= FLAGS_NILPADDING;
    }

  /* Calculate padding */
  count = (! ((flags & FLAGS_LEFTADJUST) || (precision == NO_PRECISION)))
    ? precision
    : 0;
  
  /* Adjust width further */
  if (isNegative || (flags & FLAGS_SHOWSIGN) || (flags & FLAGS_SPACE))
    width--;
  if ((flags & FLAGS_ALTERNATIVE) && !isNumberZero)
    {
      switch (base)
	{
	case BASE_BINARY:
	case BASE_HEX:
	  width -= 2;
	  break;
	case BASE_OCTAL:
	  if (!(flags & FLAGS_NILPADDING) || (count == 0))
	    width--;
	  break;
	default:
	  break;
	}
    }

  /* Output prefixes spaces if needed */
  if (! ((flags & FLAGS_LEFTADJUST) ||
	 ((flags & FLAGS_NILPADDING) && (precision == NO_PRECISION))))
    {
      while (width-- > count)
	self->OutStream(self, CHAR_ADJUST);
    }

  /* width has been adjusted for signs and alternatives */
  if (isNegative)
    self->OutStream(self, '-');
  else if (flags & FLAGS_SHOWSIGN)
    self->OutStream(self, '+');
  else if (flags & FLAGS_SPACE)
    self->OutStream(self, ' ');

  /* Prefix is not written when the value is zero */
  if ((flags & FLAGS_ALTERNATIVE) && !isNumberZero)
    {
      switch (base)
	{
	case BASE_BINARY:
	  self->OutStream(self, '0');
	  self->OutStream(self, (flags & FLAGS_UPPER) ? 'B' : 'b');
	  break;

	case BASE_OCTAL:
	  if (!(flags & FLAGS_NILPADDING) || (count == 0))
	    self->OutStream(self, '0');
	  break;

	case BASE_HEX:
	  self->OutStream(self, '0');
	  self->OutStream(self, (flags & FLAGS_UPPER) ? 'X' : 'x');
	  break;

	default:
	  break;
	} /* switch base */
    }

  /* Output prefixed zero padding if needed */
  if (flags & FLAGS_NILPADDING)
    {
      if (precision == NO_PRECISION)
	precision = width;
      while (precision-- > 0)
	{
	  self->OutStream(self, '0');
	  width--;
	}
    }

  if (! ignoreNumber)
    {
      /* Output the number itself */
      while (*(++pointer))
	{
	  self->OutStream(self, *pointer);
	}
    }

  /* Output trailing spaces if needed */
  if (flags & FLAGS_LEFTADJUST)
    {
      while (width-- > 0)
	self->OutStream(self, CHAR_ADJUST);
    }
}

/*************************************************************************
 * TrioWriteStringCharacter
 *
 * Description:
 *  Output a single character of a string
 */
TRIO_PRIVATE void
TrioWriteStringCharacter
TRIO_ARGS3((self, ch, flags),
	   trio_class_t *self,
	   int ch,
	   trio_flags_t flags)
{
  if (flags & FLAGS_ALTERNATIVE)
    {
      if (! isprint(ch))
	{
	  /*
	   * Non-printable characters are converted to C escapes or
	   * \number, if no C escape exists.
	   */
	  self->OutStream(self, CHAR_BACKSLASH);
	  switch (ch)
	    {
	    case '\007': self->OutStream(self, 'a'); break;
	    case '\b': self->OutStream(self, 'b'); break;
	    case '\f': self->OutStream(self, 'f'); break;
	    case '\n': self->OutStream(self, 'n'); break;
	    case '\r': self->OutStream(self, 'r'); break;
	    case '\t': self->OutStream(self, 't'); break;
	    case '\v': self->OutStream(self, 'v'); break;
	    case '\\': self->OutStream(self, '\\'); break;
	    default:
	      self->OutStream(self, 'x');
	      TrioWriteNumber(self, (trio_uintmax_t)ch,
			      FLAGS_UNSIGNED | FLAGS_NILPADDING,
			      2, 2, BASE_HEX);
	      break;
	    }
	}
      else if (ch == CHAR_BACKSLASH)
	{
	  self->OutStream(self, CHAR_BACKSLASH);
	  self->OutStream(self, CHAR_BACKSLASH);
	}
      else
	{
	  self->OutStream(self, ch);
	}
    }
  else
    {
      self->OutStream(self, ch);
    }
}

/*************************************************************************
 * TrioWriteString
 *
 * Description:
 *  Output a string
 */
TRIO_PRIVATE void
TrioWriteString
TRIO_ARGS5((self, string, flags, width, precision),
	   trio_class_t *self,
	   TRIO_CONST char *string,
	   trio_flags_t flags,
	   int width,
	   int precision)
{
  int length;
  int ch;

  assert(VALID(self));
  assert(VALID(self->OutStream));

  if (string == NULL)
    {
      string = internalNullString;
      length = sizeof(internalNullString) - 1;
#if TRIO_FEATURE_QUOTE
      /* Disable quoting for the null pointer */
      flags &= (~FLAGS_QUOTE);
#endif
      width = 0;
    }
  else
    {
      if (precision == 0)
	{
	  length = trio_length(string);
	}
      else
	{
	  length = trio_length_max(string, precision);
	}
    }
  if ((NO_PRECISION != precision) &&
      (precision < length))
    {
      length = precision;
    }
  width -= length;

#if TRIO_FEATURE_QUOTE
  if (flags & FLAGS_QUOTE)
    self->OutStream(self, CHAR_QUOTE);
#endif

  if (! (flags & FLAGS_LEFTADJUST))
    {
      while (width-- > 0)
	self->OutStream(self, CHAR_ADJUST);
    }

  while (length-- > 0)
    {
      /* The ctype parameters must be an unsigned char (or EOF) */
      ch = (int)((unsigned char)(*string++));
      TrioWriteStringCharacter(self, ch, flags);
    }

  if (flags & FLAGS_LEFTADJUST)
    {
      while (width-- > 0)
	self->OutStream(self, CHAR_ADJUST);
    }
#if TRIO_FEATURE_QUOTE
  if (flags & FLAGS_QUOTE)
    self->OutStream(self, CHAR_QUOTE);
#endif
}

/*************************************************************************
 * TrioWriteWideStringCharacter
 *
 * Description:
 *  Output a wide string as a multi-byte sequence
 */
#if TRIO_FEATURE_WIDECHAR
TRIO_PRIVATE int
TrioWriteWideStringCharacter
TRIO_ARGS4((self, wch, flags, width),
	   trio_class_t *self,
	   trio_wchar_t wch,
	   trio_flags_t flags,
	   int width)
{
  int size;
  int i;
  int ch;
  char *string;
  char buffer[MB_LEN_MAX + 1];

  if (width == NO_WIDTH)
    width = sizeof(buffer);
  
  size = wctomb(buffer, wch);
  if ((size <= 0) || (size > width) || (buffer[0] == NIL))
    return 0;

  string = buffer;
  i = size;
  while ((width >= i) && (width-- > 0) && (i-- > 0))
    {
      /* The ctype parameters must be an unsigned char (or EOF) */
      ch = (int)((unsigned char)(*string++));
      TrioWriteStringCharacter(self, ch, flags);
    }
  return size;
}
#endif /* TRIO_FEATURE_WIDECHAR */

/*************************************************************************
 * TrioWriteWideString
 *
 * Description:
 *  Output a wide character string as a multi-byte string
 */
#if TRIO_FEATURE_WIDECHAR
TRIO_PRIVATE void
TrioWriteWideString
TRIO_ARGS5((self, wstring, flags, width, precision),
	   trio_class_t *self,
	   TRIO_CONST trio_wchar_t *wstring,
	   trio_flags_t flags,
	   int width,
	   int precision)
{
  int length;
  int size;

  assert(VALID(self));
  assert(VALID(self->OutStream));

#if defined(TRIO_COMPILER_SUPPORTS_MULTIBYTE)
  /* Required by TrioWriteWideStringCharacter */
  (void)mblen(NULL, 0);
#endif
  
  if (wstring == NULL)
    {
      TrioWriteString(self, NULL, flags, width, precision);
      return;
    }
  
  if (NO_PRECISION == precision)
    {
      length = INT_MAX;
    }
  else
    {
      length = precision;
      width -= length;
    }

#if TRIO_FEATURE_QUOTE
  if (flags & FLAGS_QUOTE)
    self->OutStream(self, CHAR_QUOTE);
#endif

  if (! (flags & FLAGS_LEFTADJUST))
    {
      while (width-- > 0)
	self->OutStream(self, CHAR_ADJUST);
    }

  while (length > 0)
    {
      size = TrioWriteWideStringCharacter(self, *wstring++, flags, length);
      if (size == 0)
	break; /* while */
      length -= size;
    }

  if (flags & FLAGS_LEFTADJUST)
    {
      while (width-- > 0)
	self->OutStream(self, CHAR_ADJUST);
    }
#if TRIO_FEATURE_QUOTE
  if (flags & FLAGS_QUOTE)
    self->OutStream(self, CHAR_QUOTE);
#endif
}
#endif /* TRIO_FEATURE_WIDECHAR */

/*************************************************************************
 * TrioWriteDouble
 *
 * http://wwwold.dkuug.dk/JTC1/SC22/WG14/www/docs/dr_211.htm
 *
 * "5.2.4.2.2 paragraph #4
 *
 *  The accuracy [...] is implementation defined, as is the accuracy
 *  of the conversion between floating-point internal representations
 *  and string representations performed by the libray routine in
 *  <stdio.h>"
 */
/* FIXME: handle all instances of constant long-double number (L)
 *   and *l() math functions.
 */
#if TRIO_FEATURE_FLOAT
TRIO_PRIVATE void
TrioWriteDouble
TRIO_ARGS6((self, number, flags, width, precision, base),
	   trio_class_t *self,
	   trio_long_double_t number,
	   trio_flags_t flags,
	   int width,
	   int precision,
	   int base)
{
  trio_long_double_t integerNumber;
  trio_long_double_t fractionNumber;
  trio_long_double_t workNumber;
  int integerDigits;
  int fractionDigits;
  int exponentDigits;
  int workDigits;
  int baseDigits;
  int integerThreshold;
  int fractionThreshold;
  int expectedWidth;
  int exponent = 0;
  unsigned int uExponent = 0;
  int exponentBase;
  trio_long_double_t dblBase;
  trio_long_double_t dblFractionBase;
  trio_long_double_t integerAdjust;
  trio_long_double_t fractionAdjust;
  trio_long_double_t workFractionNumber;
  trio_long_double_t workFractionAdjust;
  int fractionDigitsInspect;
  BOOLEAN_T isNegative;
  BOOLEAN_T isExponentNegative = FALSE;
  BOOLEAN_T requireTwoDigitExponent;
  BOOLEAN_T isHex;
  TRIO_CONST char *digits;
# if TRIO_FEATURE_QUOTE
  char *groupingPointer;
# endif
  int i;
  int offset;
  BOOLEAN_T hasOnlyZeroes;
  int leadingFractionZeroes = -1;
  register int trailingZeroes;
  BOOLEAN_T keepTrailingZeroes;
  BOOLEAN_T keepDecimalPoint;
  trio_long_double_t epsilon;
  trio_long_double_t epsilonCorrection;
  BOOLEAN_T adjustNumber = FALSE;
  
  assert(VALID(self));
  assert(VALID(self->OutStream));
  assert(((base >= MIN_BASE) && (base <= MAX_BASE)) || (base == NO_BASE));

  /* Determine sign and look for special quantities */
  switch (trio_fpclassify_and_signbit(number, &isNegative))
    {
    case TRIO_FP_NAN:
      TrioWriteString(self,
		      (flags & FLAGS_UPPER)
		      ? NAN_UPPER
		      : NAN_LOWER,
		      flags, width, precision);
      return;
      
    case TRIO_FP_INFINITE:
      if (isNegative)
	{
	  /* Negative infinity */
	  TrioWriteString(self,
			  (flags & FLAGS_UPPER)
			  ? "-" INFINITE_UPPER
			  : "-" INFINITE_LOWER,
			  flags, width, precision);
	  return;
	}
      else
	{
	  /* Positive infinity */
	  TrioWriteString(self,
			  (flags & FLAGS_UPPER)
			  ? INFINITE_UPPER
			  : INFINITE_LOWER,
			  flags, width, precision);
	  return;
	}

    default:
      /* Finitude */
      break;
    }
  
  /* Normal numbers */
  if (flags & FLAGS_LONGDOUBLE)
    {
      baseDigits = (base == 10)
	? LDBL_DIG
	: (int)trio_floor(LDBL_MANT_DIG / TrioLogarithmBase(base));
      epsilon = LDBL_EPSILON;
    }
  else if (flags & FLAGS_SHORT)
    {
      baseDigits = (base == BASE_DECIMAL)
	? FLT_DIG
	: (int)trio_floor(FLT_MANT_DIG / TrioLogarithmBase(base));
      epsilon = FLT_EPSILON;
    }
  else
    {
      baseDigits = (base == BASE_DECIMAL)
	? DBL_DIG
	: (int)trio_floor(DBL_MANT_DIG / TrioLogarithmBase(base));
      epsilon = DBL_EPSILON;
    }

  digits = (flags & FLAGS_UPPER) ? internalDigitsUpper : internalDigitsLower;
  isHex = (base == BASE_HEX);
  if (base == NO_BASE)
    base = BASE_DECIMAL;
  dblBase = (trio_long_double_t)base;
  /*
   * Some log10() functions can "err by almost 3 ulps" according to
   * http://www.cs.berkeley.edu/~wkahan/LOG10HAF.TXT
   */
  epsilonCorrection = 3 * epsilon;
  keepTrailingZeroes = !( (flags & FLAGS_ROUNDING) ||
			  ( (flags & FLAGS_FLOAT_G) &&
			    !(flags & FLAGS_ALTERNATIVE) ) );

# if TRIO_FEATURE_ROUNDING
  if (flags & FLAGS_ROUNDING)
    {
      precision = baseDigits;
    }
# endif

  if (precision == NO_PRECISION)
    {
      if (isHex)
	{
	  keepTrailingZeroes = FALSE;
	  precision = FLT_MANT_DIG;
	}
      else
	{
	  precision = FLT_DIG;
	}
    }
  
  if (isNegative)
    {
      number = -number;
    }

  if (isHex)
    {
      flags |= FLAGS_FLOAT_E;
    }

 reprocess:

  if (flags & FLAGS_FLOAT_G)
    {
      if (precision == 0)
	precision = 1;

      if ( (number < TRIO_SUFFIX_LONG(1.0E-4)) ||
	   (number >= TrioPower(base, (trio_long_double_t)precision)) )
	{
	  /* Use scientific notation */
	  flags |= FLAGS_FLOAT_E;
	}
      else if (number < 1.0)
	{
	  /*
	   * Use normal notation. If the integer part of the number is
	   * zero, then adjust the precision to include leading fractional
	   * zeros.
	   */
	  workNumber = TrioLogarithm(number, base);
	  workNumber = TRIO_FABS(workNumber);
	  if (workNumber - trio_floor(workNumber) < epsilon)
	    workNumber--;
	  leadingFractionZeroes = (int)trio_floor(workNumber);
	}
    }

  if (flags & FLAGS_FLOAT_E)
    {
      /* Scale the number */
      workNumber = TrioLogarithm(number, base);
      if (trio_isinf(workNumber) == -1)
	{
	  exponent = 0;
	  /* Undo setting */
	  if (flags & FLAGS_FLOAT_G)
	    flags &= ~FLAGS_FLOAT_E;
	}
      else
	{
	  exponent = (int)trio_floor(workNumber + epsilonCorrection);
	  workNumber = number;
	  /*
           * We want to apply A / 10^B but the equivalent A * 10^-B gives better
           * accuracy on platforms with true long double support.
	   */
#if defined(TRIO_DOUBLE_DOUBLE)
	  workNumber /= TrioPower(dblBase, (trio_long_double_t)exponent);
#else
	  workNumber *= TrioPower(dblBase, (trio_long_double_t)-exponent);
#endif
	  if (trio_isinf(workNumber)) {
	    /*
	     * Scaling is done it two steps to avoid problems with subnormal
	     * numbers.
	     */
	    workNumber /= TrioPower(dblBase, (trio_long_double_t)(exponent / 2));
	    workNumber /= TrioPower(dblBase, (trio_long_double_t)(exponent - (exponent / 2)));
	  }
	  number = workNumber;
	  isExponentNegative = (exponent < 0);
	  uExponent = (isExponentNegative) ? -exponent : exponent;
	  if (isHex)
	    uExponent *= 4; /* log16(2) */
#if TRIO_FEATURE_QUOTE
	  /* No thousand separators */
	  flags &= ~FLAGS_QUOTE;
#endif
	}
    }

  integerNumber = trio_floor(number);
  fractionNumber = number - integerNumber;

  /*
   * Truncated number.
   *
   * Precision is number of significant digits for FLOAT_G and number of
   * fractional digits for others.
   */
  integerDigits = 1;
  if (integerNumber > epsilon)
    {
      integerDigits += (int)(TrioLogarithm(integerNumber, base) + epsilonCorrection);
    }

  fractionDigits = precision;
  if (flags & FLAGS_FLOAT_G)
    {
      if (leadingFractionZeroes > 0)
	{
	  fractionDigits += leadingFractionZeroes;
	}
      if ((integerNumber > epsilon) || (number <= epsilon))
	{
	  fractionDigits -= integerDigits;
	}
    }

  dblFractionBase = TrioPower(base, fractionDigits);

  if (integerNumber < 1.0)
    {
      workNumber = number * dblFractionBase + TRIO_SUFFIX_LONG(0.5);
      if (trio_floor(number * dblFractionBase) != trio_floor(workNumber))
	{
	  adjustNumber = TRUE;
	  /* Remove a leading fraction zero if fraction is rounded up */
	  if ((int)(TrioLogarithm(number * dblFractionBase, base) + epsilonCorrection) !=
              (int)(TrioLogarithm(workNumber, base) + epsilonCorrection))
	    {
	      --leadingFractionZeroes;
	    }
	}
      workNumber /= dblFractionBase;
    }
  else
    {
      workNumber = number + TRIO_SUFFIX_LONG(0.5) / dblFractionBase;
      adjustNumber = (trio_floor(number) != trio_floor(workNumber));
    }
  if (adjustNumber)
    {
      if ((flags & FLAGS_FLOAT_G) && !(flags & FLAGS_FLOAT_E))
	{
	  /* The adjustment may require a change to scientific notation */
	  if ( (workNumber < TRIO_SUFFIX_LONG(1.0E-4)) ||
	       (workNumber >= TrioPower(base, (trio_long_double_t)precision)) )
	    {
	      /* Use scientific notation */
	      flags |= FLAGS_FLOAT_E;
	      goto reprocess;
	    }
	}
      
      if (flags & FLAGS_FLOAT_E)
	{
	  workDigits = 1 + (TrioLogarithm(trio_floor(workNumber), base) + epsilonCorrection);
	  if (integerDigits == workDigits)
	    {
	      /* Adjust if the same number of digits are used */
	      number += TRIO_SUFFIX_LONG(0.5) / dblFractionBase;
	      integerNumber = trio_floor(number);
	      fractionNumber = number - integerNumber;
	    }
	  else
	    {
	      /* Adjust if number was rounded up one digit (ie. 0.99 to 1.00) */
	      exponent++;
	      isExponentNegative = (exponent < 0);
	      uExponent = (isExponentNegative) ? -exponent : exponent;
	      if (isHex)
		uExponent *= 4; /* log16(2) */
	      workNumber = (number + TRIO_SUFFIX_LONG(0.5) / dblFractionBase) / dblBase;
	      integerNumber = trio_floor(workNumber);
	      fractionNumber = workNumber - integerNumber;
	    }
	}
      else
	{
	  if (workNumber > 1.0)
	    {
	      /* Adjust if number was rounded up one digit (ie. 99 to 100) */
	      integerNumber = trio_floor(workNumber);
	      fractionNumber = 0.0;
	      integerDigits = (integerNumber > epsilon)
		? 1 + (int)(TrioLogarithm(integerNumber, base) + epsilonCorrection)
		: 1;
	      if (flags & FLAGS_FLOAT_G)
		{
		  if (flags & FLAGS_ALTERNATIVE)
		    {
		      fractionDigits = precision;
		      if ((integerNumber > epsilon) || (number <= epsilon))
			{
			  fractionDigits -= integerDigits;
			}
		    }
		  else
		    {
		      fractionDigits = 0;
		    }
		}
	    }
	  else
	    {
	      integerNumber = trio_floor(workNumber);
	      fractionNumber = workNumber - integerNumber;
	      if (flags & FLAGS_FLOAT_G)
		{
		  if (flags & FLAGS_ALTERNATIVE)
		    {
		      fractionDigits = precision;
		      if (leadingFractionZeroes > 0)
			{
			  fractionDigits += leadingFractionZeroes;
			}
		      if ((integerNumber > epsilon) || (number <= epsilon))
			{
			  fractionDigits -= integerDigits;
			}
		    }
		}
	    }
	}
    }

  /* Estimate accuracy */
  integerAdjust = fractionAdjust = TRIO_SUFFIX_LONG(0.5);
# if TRIO_FEATURE_ROUNDING
  if (flags & FLAGS_ROUNDING)
    {
      if (integerDigits > baseDigits)
	{
	  integerThreshold = baseDigits;
	  fractionDigits = 0;
	  dblFractionBase = 1.0;
	  fractionThreshold = 0;
	  precision = 0; /* Disable decimal-point */
	  integerAdjust = TrioPower(base, integerDigits - integerThreshold - 1);
	  fractionAdjust = 0.0;
	}
      else
	{
	  integerThreshold = integerDigits;
	  fractionThreshold = fractionDigits - integerThreshold;
	  fractionAdjust = 1.0;
	}
    }
  else
# endif
    {
      integerThreshold = INT_MAX;
      fractionThreshold = INT_MAX;
    }
  
  /*
   * Calculate expected width.
   *  sign + integer part + thousands separators + decimal point
   *  + fraction + exponent
   */
  fractionAdjust /= dblFractionBase;
  hasOnlyZeroes = (trio_floor((fractionNumber + fractionAdjust) *
			       dblFractionBase) < epsilon);
  keepDecimalPoint = ( (flags & FLAGS_ALTERNATIVE) ||
		       !((precision == 0) ||
			 (!keepTrailingZeroes && hasOnlyZeroes)) );

  expectedWidth = integerDigits + fractionDigits;

  if (!keepTrailingZeroes)
    {
      trailingZeroes = 0;
      workFractionNumber = fractionNumber;
      workFractionAdjust = fractionAdjust;
      fractionDigitsInspect = fractionDigits;

      if (integerDigits > integerThreshold)
	{
	  fractionDigitsInspect = 0;
	}
      else if (fractionThreshold  <= fractionDigits)
	{
	  fractionDigitsInspect = fractionThreshold + 1;
	}

      trailingZeroes = fractionDigits - fractionDigitsInspect;
      for (i = 0; i < fractionDigitsInspect; i++)
	{
	  workFractionNumber *= dblBase;
	  workFractionAdjust *= dblBase;
	  workNumber = trio_floor(workFractionNumber + workFractionAdjust);
	  workFractionNumber -= workNumber;
	  offset = (int)trio_fmod(workNumber, dblBase);
	  if (offset == 0)
	    {
	      trailingZeroes++;
	    }
	  else
	    {
	      trailingZeroes = 0;
	    }
	}
      expectedWidth -= trailingZeroes;
    }
  
  if (keepDecimalPoint)
    {
      expectedWidth += internalDecimalPointLength;
    }
  
#if TRIO_FEATURE_QUOTE
  if (flags & FLAGS_QUOTE)
    {
      expectedWidth += TrioCalcThousandSeparatorLength(integerDigits);
    }
#endif
  
  if (isNegative || (flags & FLAGS_SHOWSIGN) || (flags & FLAGS_SPACE))
    {
      expectedWidth += sizeof("-") - 1;
    }
  
  exponentDigits = 0;
  if (flags & FLAGS_FLOAT_E)
    {
      exponentDigits = (uExponent == 0)
	? 1
	: (int)trio_ceil(TrioLogarithm((double)(uExponent + 1),
				       (isHex) ? 10 : base));
    }
  requireTwoDigitExponent = ((base == BASE_DECIMAL) && (exponentDigits == 1));
  if (exponentDigits > 0)
    {
      expectedWidth += exponentDigits;
      expectedWidth += (requireTwoDigitExponent
			? sizeof("E+0") - 1
			: sizeof("E+") - 1);
    }
  
  if (isHex)
    {
      expectedWidth += sizeof("0X") - 1;
    }
  
  /* Output prefixing */
  if (flags & FLAGS_NILPADDING)
    {
      /* Leading zeros must be after sign */
      if (isNegative)
	self->OutStream(self, '-');
      else if (flags & FLAGS_SHOWSIGN)
	self->OutStream(self, '+');
      else if (flags & FLAGS_SPACE)
	self->OutStream(self, ' ');
      if (isHex)
	{
	  self->OutStream(self, '0');
	  self->OutStream(self, (flags & FLAGS_UPPER) ? 'X' : 'x');
	}
      if (!(flags & FLAGS_LEFTADJUST))
	{
	  for (i = expectedWidth; i < width; i++)
	    {
	      self->OutStream(self, '0');
	    }
	}
    }
  else
    {
      /* Leading spaces must be before sign */
      if (!(flags & FLAGS_LEFTADJUST))
	{
	  for (i = expectedWidth; i < width; i++)
	    {
	      self->OutStream(self, CHAR_ADJUST);
	    }
	}
      if (isNegative)
	self->OutStream(self, '-');
      else if (flags & FLAGS_SHOWSIGN)
	self->OutStream(self, '+');
      else if (flags & FLAGS_SPACE)
	self->OutStream(self, ' ');
      if (isHex)
	{
	  self->OutStream(self, '0');
	  self->OutStream(self, (flags & FLAGS_UPPER) ? 'X' : 'x');
	}
    }
  
  /* Output the integer part and thousand separators */
  for (i = 0; i < integerDigits; i++)
    {
      workNumber = trio_floor(((integerNumber + integerAdjust)
			       / TrioPower(base, integerDigits - i - 1)));
      if (i > integerThreshold)
	{
	  /* Beyond accuracy */
	  self->OutStream(self, digits[0]);
	}
      else
	{
	  self->OutStream(self, digits[(int)trio_fmod(workNumber, dblBase)]);
	}

#if TRIO_FEATURE_QUOTE
      if (((flags & (FLAGS_FLOAT_E | FLAGS_QUOTE)) == FLAGS_QUOTE)
	  && TrioFollowedBySeparator(integerDigits - i))
	{
	  for (groupingPointer = internalThousandSeparator;
	       *groupingPointer != NIL;
	       groupingPointer++)
	    {
	      self->OutStream(self, *groupingPointer);
	    }
	}
#endif
    }
  
  /* Insert decimal point and build the fraction part */
  trailingZeroes = 0;

  if (keepDecimalPoint)
    {
      if (internalDecimalPoint)
	{
	  self->OutStream(self, internalDecimalPoint);
	}
      else
	{
	  for (i = 0; i < internalDecimalPointLength; i++)
	    {
	      self->OutStream(self, internalDecimalPointString[i]);
	    }
	}
    }

  for (i = 0; i < fractionDigits; i++)
    {
      if ((integerDigits > integerThreshold) || (i > fractionThreshold))
	{
	  /* Beyond accuracy */
	  trailingZeroes++;
	}
      else
	{
	  fractionNumber *= dblBase;
	  fractionAdjust *= dblBase;
	  workNumber = trio_floor(fractionNumber + fractionAdjust);
	  if (workNumber > fractionNumber)
	    {
	      /* fractionNumber should never become negative */
	      fractionNumber = 0.0;
	      fractionAdjust = 0.0;
	    }
	  else
	    {
	      fractionNumber -= workNumber;
	    }
	  offset = (int)trio_fmod(workNumber, dblBase);
	  if (offset == 0)
	    {
	      trailingZeroes++;
	    }
	  else
	    {
	      while (trailingZeroes > 0)
		{
		  /* Not trailing zeroes after all */
		  self->OutStream(self, digits[0]);
		  trailingZeroes--;
		}
	      self->OutStream(self, digits[offset]);
	    }
	}
    }
  
  if (keepTrailingZeroes)
    {
      while (trailingZeroes > 0)
	{
	  self->OutStream(self, digits[0]);
	  trailingZeroes--;
	}
    }
  
  /* Output exponent */
  if (exponentDigits > 0)
    {
      self->OutStream(self,
		      isHex
		      ? ((flags & FLAGS_UPPER) ? 'P' : 'p')
		      : ((flags & FLAGS_UPPER) ? 'E' : 'e'));
      self->OutStream(self, (isExponentNegative) ? '-' : '+');

      /* The exponent must contain at least two digits */
      if (requireTwoDigitExponent)
        self->OutStream(self, '0');

      if (isHex)
	base = 10;
      exponentBase = (int)TrioPower(base, exponentDigits - 1);
      for (i = 0; i < exponentDigits; i++)
	{
	  self->OutStream(self, digits[(uExponent / exponentBase) % base]);
	  exponentBase /= base;
	}
    }
  /* Output trailing spaces */
  if (flags & FLAGS_LEFTADJUST)
    {
      for (i = expectedWidth; i < width; i++)
	{
	  self->OutStream(self, CHAR_ADJUST);
	}
    }
}
#endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * TrioFormatProcess
 *
 * Description:
 *  This is the main engine for formatting output
 */
TRIO_PRIVATE int
TrioFormatProcess
TRIO_ARGS3((data, format, parameters),
	   trio_class_t *data,
	   TRIO_CONST char *format,
	   trio_parameter_t *parameters)
{
  int i;
#if TRIO_FEATURE_ERRNO
  TRIO_CONST char *string;
#endif
  trio_pointer_t pointer;
  trio_flags_t flags;
  int width;
  int precision;
  int base;
  int offset;
  
  offset = 0;
  i = 0;

  for (;;)
    {
      /* Skip the parameter entries */
      while (parameters[i].type == FORMAT_PARAMETER)
	i++;

      /* Copy non conversion-specifier part of format string */
      while (offset < parameters[i].beginOffset)
        {
	  if (CHAR_IDENTIFIER == format[offset] && CHAR_IDENTIFIER == format[offset + 1])
	    {
	      data->OutStream(data, CHAR_IDENTIFIER);
	      offset += 2;
	    }
	  else
	    {
	      data->OutStream(data, format[offset++]);
	    }
	}

      /* Abort if we reached end of format string */
      if (parameters[i].type == FORMAT_SENTINEL)
	break;

      /* Ouput parameter */
      flags = parameters[i].flags;

      /* Find width */
      width = parameters[i].width;
      if (flags & FLAGS_WIDTH_PARAMETER)
	{
	  /* Get width from parameter list */
	  width = (int)parameters[width].data.number.as_signed;
	  if (width < 0)
	    {
	      /*
	       * A negative width is the same as the - flag and
	       * a positive width.
	       */
	      flags |= FLAGS_LEFTADJUST;
	      flags &= ~FLAGS_NILPADDING;
	      width = -width;
	    }
	}

      /* Find precision */
      if (flags & FLAGS_PRECISION)
	{
	  precision = parameters[i].precision;
	  if (flags & FLAGS_PRECISION_PARAMETER)
	    {
	      /* Get precision from parameter list */
	      precision = (int)parameters[precision].data.number.as_signed;
	      if (precision < 0)
		{
		  /*
		   * A negative precision is the same as no
		   * precision
		   */
		  precision = NO_PRECISION;
		}
	    }
	}
      else
	{
	  precision = NO_PRECISION;
	}

      /* Find base */
      if (NO_BASE != parameters[i].baseSpecifier)
	{
	  /* Base from specifier has priority */
	  base = parameters[i].baseSpecifier;
	}
      else if (flags & FLAGS_BASE_PARAMETER)
	{
	  /* Get base from parameter list */
	  base = parameters[i].base;
	  base = (int)parameters[base].data.number.as_signed;
	}
      else
	{
	  /* Use base from format string */
	  base = parameters[i].base;
	}

      switch (parameters[i].type)
        {
	case FORMAT_CHAR:
#if TRIO_FEATURE_QUOTE
	  if (flags & FLAGS_QUOTE)
	    data->OutStream(data, CHAR_QUOTE);
#endif
	  if (! (flags & FLAGS_LEFTADJUST))
	    {
	      while (--width > 0)
		data->OutStream(data, CHAR_ADJUST);
	    }
#if TRIO_FEATURE_WIDECHAR
	  if (flags & FLAGS_WIDECHAR)
	    {
	      TrioWriteWideStringCharacter(data,
					   (trio_wchar_t)parameters[i].data.number.as_signed,
					   flags,
					   NO_WIDTH);
	    }
	  else
#endif
	  {
	    TrioWriteStringCharacter(data,
				     (int)parameters[i].data.number.as_signed,
				     flags);
	  }

	  if (flags & FLAGS_LEFTADJUST)
	    {
	      while(--width > 0)
		data->OutStream(data, CHAR_ADJUST);
	    }
#if TRIO_FEATURE_QUOTE
	  if (flags & FLAGS_QUOTE)
	    data->OutStream(data, CHAR_QUOTE);
#endif

	  break; /* FORMAT_CHAR */

	case FORMAT_INT:
	  TrioWriteNumber(data,
			  parameters[i].data.number.as_unsigned,
			  flags,
			  width,
			  precision,
			  base);

	  break; /* FORMAT_INT */

#if TRIO_FEATURE_FLOAT
	case FORMAT_DOUBLE:
	  TrioWriteDouble(data,
			  parameters[i].data.longdoubleNumber,
			  flags,
			  width,
			  precision,
			  base);
	  break; /* FORMAT_DOUBLE */
#endif

	case FORMAT_STRING:
#if TRIO_FEATURE_WIDECHAR
	  if (flags & FLAGS_WIDECHAR)
	    {
	      TrioWriteWideString(data,
				  parameters[i].data.wstring,
				  flags,
				  width,
				  precision);
	    }
	  else
#endif
	    {
	      TrioWriteString(data,
			      parameters[i].data.string,
			      flags,
			      width,
			      precision);
	    }
	  break; /* FORMAT_STRING */

	case FORMAT_POINTER:
	  {
	    trio_reference_t reference;

	    reference.data = data;
	    reference.parameter = &parameters[i];
	    trio_print_pointer(&reference, parameters[i].data.pointer);
	  }
	  break; /* FORMAT_POINTER */

	case FORMAT_COUNT:
	  pointer = parameters[i].data.pointer;
	  if (NULL != pointer)
	    {
	      /*
	       * C99 paragraph 7.19.6.1.8 says "the number of
	       * characters written to the output stream so far by
	       * this call", which is data->actually.committed
	       */
#if TRIO_FEATURE_SIZE_T || TRIO_FEATURE_SIZE_T_UPPER
	      if (flags & FLAGS_SIZE_T)
		*(size_t *)pointer = (size_t)data->actually.committed;
	      else
#endif
#if TRIO_FEATURE_PTRDIFF_T
	      if (flags & FLAGS_PTRDIFF_T)
		*(ptrdiff_t *)pointer = (ptrdiff_t)data->actually.committed;
	      else
#endif
#if TRIO_FEATURE_INTMAX_T
	      if (flags & FLAGS_INTMAX_T)
		*(trio_intmax_t *)pointer = (trio_intmax_t)data->actually.committed;
	      else
#endif
	      if (flags & FLAGS_QUAD)
		{
		  *(trio_ulonglong_t *)pointer = (trio_ulonglong_t)data->actually.committed;
		}
	      else if (flags & FLAGS_LONG)
		{
		  *(long int *)pointer = (long int)data->actually.committed;
		}
	      else if (flags & FLAGS_SHORT)
		{
		  *(short int *)pointer = (short int)data->actually.committed;
		}
	      else
		{
		  *(int *)pointer = (int)data->actually.committed;
		}
	    }
	  break; /* FORMAT_COUNT */

	case FORMAT_PARAMETER:
	  break; /* FORMAT_PARAMETER */

#if TRIO_FEATURE_ERRNO
	case FORMAT_ERRNO:
	  string = trio_error(parameters[i].data.errorNumber);
	  if (string)
	    {
	      TrioWriteString(data,
			      string,
			      flags,
			      width,
			      precision);
	    }
	  else
	  {
	    data->OutStream(data, '#');
	    TrioWriteNumber(data,
			    (trio_uintmax_t)parameters[i].data.errorNumber,
			    flags,
			    width,
			    precision,
			    BASE_DECIMAL);
	  }
	  break; /* FORMAT_ERRNO */
#endif /* TRIO_FEATURE_ERRNO */

#if TRIO_FEATURE_USER_DEFINED
	case FORMAT_USER_DEFINED:
	  {
	    trio_reference_t reference;
	    trio_userdef_t *def = NULL;

	    if (parameters[i].flags & FLAGS_USER_DEFINED_PARAMETER)
	      {
		/* Use handle */
		if ((i > 0) ||
		    (parameters[i - 1].type == FORMAT_PARAMETER))
		  def = (trio_userdef_t *)parameters[i - 1].data.pointer;
	      }
	    else
	      {
		/* Look up namespace */
		def = TrioFindNamespace(parameters[i].user_defined.namespace, NULL);
	      }
	    if (def)
	      {
		reference.data = data;
		reference.parameter = &parameters[i];
		def->callback(&reference);
	      }
	  }
	  break;
#endif /* TRIO_FEATURE_USER_DEFINED */

	default:
	  break;
	} /* switch parameter type */

      /* Prepare for next */
      offset = parameters[i].endOffset;
      i++;
    }

  return data->processed;
}

/*************************************************************************
 * TrioFormatRef
 */
#if TRIO_EXTENSION
TRIO_PRIVATE int
TrioFormatRef
TRIO_ARGS5((reference, format, arglist, argfunc, argarray),
	   trio_reference_t *reference,
	   TRIO_CONST char *format,
	   va_list arglist,
	   trio_argfunc_t argfunc,
	   trio_pointer_t *argarray)
{
  int status;
  trio_parameter_t parameters[MAX_PARAMETERS];

  status = TrioParse(TYPE_PRINT, format, parameters, arglist, argfunc, argarray);
  if (status < 0)
    return status;

  status = TrioFormatProcess(reference->data, format, parameters);
  if (reference->data->error != 0)
    {
      status = reference->data->error;
    }
  return status;
}
#endif /* TRIO_EXTENSION */

/*************************************************************************
 * TrioFormat
 */
TRIO_PRIVATE int
TrioFormat
TRIO_ARGS7((destination, destinationSize, OutStream, format, arglist, argfunc, argarray),
	   trio_pointer_t destination,
	   size_t destinationSize,
	   void (*OutStream) TRIO_PROTO((trio_class_t *, int)),
	   TRIO_CONST char *format,
	   va_list arglist,
	   trio_argfunc_t argfunc,
	   trio_pointer_t *argarray)
{
  int status;
  trio_class_t data;
  trio_parameter_t parameters[MAX_PARAMETERS];

  assert(VALID(OutStream));
  assert(VALID(format));

  memset(&data, 0, sizeof(data));
  data.OutStream = OutStream;
  data.location = destination;
  data.max = destinationSize;
  data.error = 0;

#if defined(USE_LOCALE)
  if (NULL == internalLocaleValues)
    {
      TrioSetLocale();
    }
#endif

  status = TrioParse(TYPE_PRINT, format, parameters, arglist, argfunc, argarray);
  if (status < 0)
    return status;

  status = TrioFormatProcess(&data, format, parameters);
  if (data.error != 0)
    {
      status = data.error;
    }
  return status;
}

/*************************************************************************
 * TrioOutStreamFile
 */
#if TRIO_FEATURE_FILE || TRIO_FEATURE_STDIO
TRIO_PRIVATE void
TrioOutStreamFile
TRIO_ARGS2((self, output),
	   trio_class_t *self,
	   int output)
{
  FILE *file;

  assert(VALID(self));
  assert(VALID(self->location));

  file = (FILE *)self->location;
  self->processed++;
  if (fputc(output, file) == EOF)
    {
      self->error = TRIO_ERROR_RETURN(TRIO_EOF, 0);
    }
  else
    {
      self->actually.committed++;
    }
}
#endif /* TRIO_FEATURE_FILE || TRIO_FEATURE_STDIO */

/*************************************************************************
 * TrioOutStreamFileDescriptor
 */
#if TRIO_FEATURE_FD
TRIO_PRIVATE void
TrioOutStreamFileDescriptor
TRIO_ARGS2((self, output),
	   trio_class_t *self,
	   int output)
{
  int fd;
  char ch;

  assert(VALID(self));

  fd = *((int *)self->location);
  ch = (char)output;
  self->processed++;
  if (write(fd, &ch, sizeof(char)) == -1)
    {
      self->error = TRIO_ERROR_RETURN(TRIO_ERRNO, 0);
    }
  else
    {
      self->actually.committed++;
    }
}
#endif /* TRIO_FEATURE_FD */

/*************************************************************************
 * TrioOutStreamCustom
 */
#if TRIO_FEATURE_CLOSURE
TRIO_PRIVATE void
TrioOutStreamCustom
TRIO_ARGS2((self, output),
	   trio_class_t *self,
	   int output)
{
  int status;
  trio_custom_t *data;

  assert(VALID(self));
  assert(VALID(self->location));

  data = (trio_custom_t *)self->location;
  if (data->stream.out)
    {
      status = (data->stream.out)(data->closure, output);
      if (status >= 0)
	{
	  self->actually.committed++;
	}
      else
	{
	  if (self->error == 0)
	    {
	      self->error = TRIO_ERROR_RETURN(TRIO_ECUSTOM, -status);
	    }
	}
    }
  self->processed++;
}
#endif /* TRIO_FEATURE_CLOSURE */

/*************************************************************************
 * TrioOutStreamString
 */
TRIO_PRIVATE void
TrioOutStreamString
TRIO_ARGS2((self, output),
	   trio_class_t *self,
	   int output)
{
  char **buffer;

  assert(VALID(self));
  assert(VALID(self->location));

  buffer = (char **)self->location;
  **buffer = (char)output;
  (*buffer)++;
  self->processed++;
  self->actually.committed++;
}

/*************************************************************************
 * TrioOutStreamStringMax
 */
TRIO_PRIVATE void
TrioOutStreamStringMax
TRIO_ARGS2((self, output),
	   trio_class_t *self,
	   int output)
{
  char **buffer;

  assert(VALID(self));
  assert(VALID(self->location));
  
  buffer = (char **)self->location;

  if (self->processed < self->max)
    {
      **buffer = (char)output;
      (*buffer)++;
      self->actually.committed++;
    }
  self->processed++;
}

/*************************************************************************
 * TrioOutStreamStringDynamic
 */
#if TRIO_FEATURE_DYNAMICSTRING
TRIO_PRIVATE void
TrioOutStreamStringDynamic
TRIO_ARGS2((self, output),
	   trio_class_t *self,
	   int output)
{
  assert(VALID(self));
  assert(VALID(self->location));

  if (self->error == 0)
    {
      trio_xstring_append_char((trio_string_t *)self->location,
			       (char)output);
      self->actually.committed++;
    }
  /* The processed variable must always be increased */
  self->processed++;
}
#endif /* TRIO_FEATURE_DYNAMICSTRING */

/*************************************************************************
 * TrioArrayGetter
 */
TRIO_PRIVATE
trio_pointer_t TrioArrayGetter(trio_pointer_t context, int index, int type)
{
////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: Silence unused parameter warning.
////////////////////////////////////////////////////////////////////////////////////////////////////
  (void)type;
////////////////////////////////////////////////////////////////////////////////////////////////////
  /* Utility function for the printfv family */
  trio_pointer_t *argarray = (trio_pointer_t *)context;
  return argarray[index];
}

/*************************************************************************
 *
 * Formatted printing functions
 *
 ************************************************************************/

/** @addtogroup Printf
    @{
*/

/*************************************************************************
 * printf
 */

/**
   Print to standard output stream.

   @param format Formatting string.
   @param ... Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_STDIO
TRIO_PUBLIC int
trio_printf
TRIO_VARGS2((format, va_alist),
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioFormat(stdout, 0, TrioOutStreamFile, format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_STDIO */

/**
   Print to standard output stream.

   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_STDIO
TRIO_PUBLIC int
trio_vprintf
TRIO_ARGS2((format, args),
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(format));

  return TrioFormat(stdout, 0, TrioOutStreamFile, format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_STDIO */

/**
   Print to standard output stream.

   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_STDIO
TRIO_PUBLIC int
trio_printfv
TRIO_ARGS2((format, args),
	   TRIO_CONST char *format,
	   trio_pointer_t * args)
{
  static va_list unused;
  
  assert(VALID(format));

  return TrioFormat(stdout, 0, TrioOutStreamFile, format,
		    unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_STDIO */

/*************************************************************************
 * fprintf
 */

/**
   Print to file.

   @param file File pointer.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_FILE
TRIO_PUBLIC int
trio_fprintf
TRIO_VARGS3((file, format, va_alist),
	    FILE *file,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(file));
  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioFormat(file, 0, TrioOutStreamFile, format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_FILE */

/**
   Print to file.

   @param file File pointer.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_FILE
TRIO_PUBLIC int
trio_vfprintf
TRIO_ARGS3((file, format, args),
	   FILE *file,
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(file));
  assert(VALID(format));
  
  return TrioFormat(file, 0, TrioOutStreamFile, format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_FILE */

/**
   Print to file.

   @param file File pointer.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_FILE
TRIO_PUBLIC int
trio_fprintfv
TRIO_ARGS3((file, format, args),
	   FILE *file,
	   TRIO_CONST char *format,
	   trio_pointer_t * args)
{
  static va_list unused;

  assert(VALID(file));
  assert(VALID(format));

  return TrioFormat(file, 0, TrioOutStreamFile, format,
		    unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_FILE */

/*************************************************************************
 * dprintf
 */

/**
   Print to file descriptor.

   @param fd File descriptor.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_FD
TRIO_PUBLIC int
trio_dprintf
TRIO_VARGS3((fd, format, va_alist),
	    int fd,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioFormat(&fd, 0, TrioOutStreamFileDescriptor, format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_FD */

/**
   Print to file descriptor.

   @param fd File descriptor.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_FD
TRIO_PUBLIC int
trio_vdprintf
TRIO_ARGS3((fd, format, args),
	   int fd,
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(format));
  
  return TrioFormat(&fd, 0, TrioOutStreamFileDescriptor, format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_FD */

/**
   Print to file descriptor.

   @param fd File descriptor.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_FD
TRIO_PUBLIC int
trio_dprintfv
TRIO_ARGS3((fd, format, args),
	   int fd,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  
  assert(VALID(format));
  
  return TrioFormat(&fd, 0, TrioOutStreamFileDescriptor, format,
		    unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_FD */

/*************************************************************************
 * cprintf
 */
#if TRIO_FEATURE_CLOSURE
TRIO_PUBLIC int
trio_cprintf
TRIO_VARGS4((stream, closure, format, va_alist),
	    trio_outstream_t stream,
	    trio_pointer_t closure,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;
  trio_custom_t data;

  assert(VALID(stream));
  assert(VALID(format));

  TRIO_VA_START(args, format);
  data.stream.out = stream;
  data.closure = closure;
  status = TrioFormat(&data, 0, TrioOutStreamCustom, format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_CLOSURE */

#if TRIO_FEATURE_CLOSURE
TRIO_PUBLIC int
trio_vcprintf
TRIO_ARGS4((stream, closure, format, args),
	   trio_outstream_t stream,
	   trio_pointer_t closure,
	   TRIO_CONST char *format,
	   va_list args)
{
  trio_custom_t data;

  assert(VALID(stream));
  assert(VALID(format));

  data.stream.out = stream;
  data.closure = closure;
  return TrioFormat(&data, 0, TrioOutStreamCustom, format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_CLOSURE */

#if TRIO_FEATURE_CLOSURE
TRIO_PUBLIC int
trio_cprintfv
TRIO_ARGS4((stream, closure, format, args),
	   trio_outstream_t stream,
	   trio_pointer_t closure,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  trio_custom_t data;

  assert(VALID(stream));
  assert(VALID(format));

  data.stream.out = stream;
  data.closure = closure;
  return TrioFormat(&data, 0, TrioOutStreamCustom, format,
		    unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_CLOSURE */

#if TRIO_FEATURE_CLOSURE && TRIO_FEATURE_ARGFUNC
TRIO_PUBLIC int
trio_cprintff
TRIO_ARGS5((stream, closure, format, argfunc, context),
	   trio_outstream_t stream,
	   trio_pointer_t closure,
	   TRIO_CONST char *format,
	   trio_argfunc_t argfunc,
	   trio_pointer_t context)
{
  static va_list unused;
  trio_custom_t data;

  assert(VALID(stream));
  assert(VALID(format));
  assert(VALID(argfunc));

  data.stream.out = stream;
  data.closure = closure;
  return TrioFormat(&data, 0, TrioOutStreamCustom, format,
                    unused, argfunc, (trio_pointer_t *)context);
}
#endif /* TRIO_FEATURE_CLOSURE && TRIO_FEATURE_ARGFUNC */

/*************************************************************************
 * sprintf
 */

/**
   Print to string.

   @param buffer Output string.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of printed characters.
 */
TRIO_PUBLIC int
trio_sprintf
TRIO_VARGS3((buffer, format, va_alist),
	    char *buffer,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(buffer));
  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioFormat(&buffer, 0, TrioOutStreamString, format, args, NULL, NULL);
  *buffer = NIL; /* Terminate with NIL character */
  TRIO_VA_END(args);
  return status;
}

/**
   Print to string.

   @param buffer Output string.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
TRIO_PUBLIC int
trio_vsprintf
TRIO_ARGS3((buffer, format, args),
	   char *buffer,
	   TRIO_CONST char *format,
	   va_list args)
{
  int status;

  assert(VALID(buffer));
  assert(VALID(format));

  status = TrioFormat(&buffer, 0, TrioOutStreamString, format, args, NULL, NULL);
  *buffer = NIL;
  return status;
}

/**
   Print to string.

   @param buffer Output string.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
TRIO_PUBLIC int
trio_sprintfv
TRIO_ARGS3((buffer, format, args),
	   char *buffer,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  int status;
  
  assert(VALID(buffer));
  assert(VALID(format));

  status = TrioFormat(&buffer, 0, TrioOutStreamString, format,
		      unused, TrioArrayGetter, args);
  *buffer = NIL;
  return status;
}

/*************************************************************************
 * snprintf
 */

/**
   Print at most @p max characters to string.

   @param buffer Output string.
   @param max Maximum number of characters to print.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of printed characters.
 */
TRIO_PUBLIC int
trio_snprintf
TRIO_VARGS4((buffer, max, format, va_alist),
	    char *buffer,
	    size_t max,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(buffer) || (max == 0));
  assert(VALID(format));

  TRIO_VA_START(args, format);
  status = TrioFormat(&buffer, max > 0 ? max - 1 : 0,
		      TrioOutStreamStringMax, format, args, NULL, NULL);
  if (max > 0)
    *buffer = NIL;
  TRIO_VA_END(args);
  return status;
}

/**
   Print at most @p max characters to string.

   @param buffer Output string.
   @param max Maximum number of characters to print.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
TRIO_PUBLIC int
trio_vsnprintf
TRIO_ARGS4((buffer, max, format, args),
	   char *buffer,
	   size_t max,
	   TRIO_CONST char *format,
	   va_list args)
{
  int status;

  assert(VALID(buffer) || (max == 0));
  assert(VALID(format));

  status = TrioFormat(&buffer, max > 0 ? max - 1 : 0,
		      TrioOutStreamStringMax, format, args, NULL, NULL);
  if (max > 0)
    *buffer = NIL;
  return status;
}

/**
   Print at most @p max characters to string.

   @param buffer Output string.
   @param max Maximum number of characters to print.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
TRIO_PUBLIC int
trio_snprintfv
TRIO_ARGS4((buffer, max, format, args),
	   char *buffer,
	   size_t max,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  int status;

  assert(VALID(buffer) || (max == 0));
  assert(VALID(format));

  status = TrioFormat(&buffer, max > 0 ? max - 1 : 0,
		      TrioOutStreamStringMax, format,
		      unused, TrioArrayGetter, args);
  if (max > 0)
    *buffer = NIL;
  return status;
}

/*************************************************************************
 * snprintfcat
 * Appends the new string to the buffer string overwriting the '\0'
 * character at the end of buffer.
 */
#if TRIO_EXTENSION
TRIO_PUBLIC int
trio_snprintfcat
TRIO_VARGS4((buffer, max, format, va_alist),
	    char *buffer,
	    size_t max,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;
  size_t buf_len;

  TRIO_VA_START(args, format);

  assert(VALID(buffer));
  assert(VALID(format));

  buf_len = trio_length(buffer);
  buffer = &buffer[buf_len];

  status = TrioFormat(&buffer, max - 1 - buf_len,
		      TrioOutStreamStringMax, format, args, NULL, NULL);
  TRIO_VA_END(args);
  *buffer = NIL;
  return status;
}
#endif

#if TRIO_EXTENSION
TRIO_PUBLIC int
trio_vsnprintfcat
TRIO_ARGS4((buffer, max, format, args),
	   char *buffer,
	   size_t max,
	   TRIO_CONST char *format,
	   va_list args)
{
  int status;
  size_t buf_len;
  
  assert(VALID(buffer));
  assert(VALID(format));

  buf_len = trio_length(buffer);
  buffer = &buffer[buf_len];
  status = TrioFormat(&buffer, max - 1 - buf_len,
		      TrioOutStreamStringMax, format, args, NULL, NULL);
  *buffer = NIL;
  return status;
}
#endif

/*************************************************************************
 * trio_aprintf
 */

#if TRIO_DEPRECATED && TRIO_FEATURE_DYNAMICSTRING
TRIO_PUBLIC char *
trio_aprintf
TRIO_VARGS2((format, va_alist),
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  va_list args;
  trio_string_t *info;
  char *result = NULL;

  assert(VALID(format));
  
  info = trio_xstring_duplicate("");
  if (info)
    {
      TRIO_VA_START(args, format);
      (void)TrioFormat(info, 0, TrioOutStreamStringDynamic,
		       format, args, NULL, NULL);
      TRIO_VA_END(args);

      trio_string_terminate(info);
      result = trio_string_extract(info);
      trio_string_destroy(info);
    }
  return result;
}
#endif /* TRIO_DEPRECATED && TRIO_FEATURE_DYNAMICSTRING */

#if TRIO_DEPRECATED && TRIO_FEATURE_DYNAMICSTRING
TRIO_PUBLIC char *
trio_vaprintf
TRIO_ARGS2((format, args),
	   TRIO_CONST char *format,
	   va_list args)
{
  trio_string_t *info;
  char *result = NULL;
  
  assert(VALID(format));
  
  info = trio_xstring_duplicate("");
  if (info)
    {
      (void)TrioFormat(info, 0, TrioOutStreamStringDynamic,
		       format, args, NULL, NULL);
      trio_string_terminate(info);
      result = trio_string_extract(info);
      trio_string_destroy(info);
    }
  return result;
}
#endif /* TRIO_DEPRECATED && TRIO_FEATURE_DYNAMICSTRING */

/**
   Allocate and print to string.
   The memory allocated and returned by @p result must be freed by the
   calling application.

   @param result Output string.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_DYNAMICSTRING
TRIO_PUBLIC int
trio_asprintf
TRIO_VARGS3((result, format, va_alist),
	    char **result,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  va_list args;
  int status;
  trio_string_t *info;

  assert(VALID(format));

  *result = NULL;
  
  info = trio_xstring_duplicate("");
  if (info == NULL)
    {
      status = TRIO_ERROR_RETURN(TRIO_ENOMEM, 0);
    }
  else
    {
      TRIO_VA_START(args, format);
      status = TrioFormat(info, 0, TrioOutStreamStringDynamic,
			  format, args, NULL, NULL);
      TRIO_VA_END(args);
      if (status >= 0)
	{
	  trio_string_terminate(info);
	  *result = trio_string_extract(info);
	}
      trio_string_destroy(info);
    }
  return status;
}
#endif /* TRIO_FEATURE_DYNAMICSTRING */

/**
   Allocate and print to string.
   The memory allocated and returned by @p result must be freed by the
   calling application.

   @param result Output string.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_DYNAMICSTRING
TRIO_PUBLIC int
trio_vasprintf
TRIO_ARGS3((result, format, args),
	   char **result,
	   TRIO_CONST char *format,
	   va_list args)
{
  int status;
  trio_string_t *info;
  
  assert(VALID(format));

  *result = NULL;
  
  info = trio_xstring_duplicate("");
  if (info == NULL)
    {
      status = TRIO_ERROR_RETURN(TRIO_ENOMEM, 0);
    }
  else
    {
      status = TrioFormat(info, 0, TrioOutStreamStringDynamic,
			  format, args, NULL, NULL);
      if (status >= 0)
	{
	  trio_string_terminate(info);
	  *result = trio_string_extract(info);
	}
      trio_string_destroy(info);
    }
  return status;
}
#endif /* TRIO_FEATURE_DYNAMICSTRING */

/**
   Allocate and print to string.
   The memory allocated and returned by @p result must be freed by the
   calling application.

   @param result Output string.
   @param format Formatting string.
   @param args Arguments.
   @return Number of printed characters.
 */
#if TRIO_FEATURE_DYNAMICSTRING
TRIO_PUBLIC int
trio_asprintfv
TRIO_ARGS3((result, format, args),
           char **result,
           TRIO_CONST char *format,
           trio_pointer_t * args)
{
  static va_list unused;
  int status;
  trio_string_t *info;
  
  assert(VALID(format));

  *result = NULL;

  info = trio_xstring_duplicate("");
  if (info == NULL)
    {
      status = TRIO_ERROR_RETURN(TRIO_ENOMEM, 0);
    }
  else
    {
      status = TrioFormat(info, 0, TrioOutStreamStringDynamic, format,
                          unused, TrioArrayGetter, args);
      if (status >= 0)
        {
          trio_string_terminate(info);
          *result = trio_string_extract(info);
        }
      trio_string_destroy(info);
    }
  return status;
}
#endif /* TRIO_FEATURE_DYNAMICSTRING */

#if defined(TRIO_DOCUMENTATION)
# include "doc/doc_printf.h"
#endif

/** @} End of Printf documentation module */

/*************************************************************************
 *
 * CALLBACK
 *
 ************************************************************************/

#if defined(TRIO_DOCUMENTATION)
# include "doc/doc_register.h"
#endif
/**
   @addtogroup UserDefined
   @{
*/

#if TRIO_FEATURE_USER_DEFINED

/*************************************************************************
 * trio_register
 */

/**
   Register new user-defined specifier.

   @param callback
   @param name
   @return Handle.
 */
TRIO_PUBLIC trio_pointer_t 
trio_register
TRIO_ARGS2((callback, name),
	   trio_callback_t callback,
	   TRIO_CONST char *name)
{
  trio_userdef_t *def;
  trio_userdef_t *prev = NULL;

  if (callback == NULL)
    return NULL;

  if (name)
    {
      /* Handle built-in namespaces */
      if (name[0] == ':')
	{
	  if (trio_equal(name, ":enter"))
	    {
	      internalEnterCriticalRegion = callback;
	    }
	  else if (trio_equal(name, ":leave"))
	    {
	      internalLeaveCriticalRegion = callback;
	    }
	  return NULL;
	}
      
      /* Bail out if namespace is too long */
      if (trio_length(name) >= MAX_USER_NAME)
	return NULL;
      
      /* Bail out if namespace already is registered */
      def = TrioFindNamespace(name, &prev);
      if (def)
	return NULL;
    }
  
  def = (trio_userdef_t *)TRIO_MALLOC(sizeof(trio_userdef_t));
  if (def)
    {
      if (internalEnterCriticalRegion)
	(void)internalEnterCriticalRegion(NULL);
      
      if (name)
	{
	  /* Link into internal list */
	  if (prev == NULL)
	    internalUserDef = def;
	  else
	    prev->next = def;
	}
      /* Initialize */
      def->callback = callback;
      def->name = (name == NULL)
	? NULL
	: trio_duplicate(name);
      def->next = NULL;

      if (internalLeaveCriticalRegion)
	(void)internalLeaveCriticalRegion(NULL);
    }
  return (trio_pointer_t)def;
}

/**
   Unregister an existing user-defined specifier.

   @param handle
 */
TRIO_PUBLIC
void
trio_unregister
TRIO_ARGS1((handle),
	   trio_pointer_t handle)
{
  trio_userdef_t *self = (trio_userdef_t *)handle;
  trio_userdef_t *def;
  trio_userdef_t *prev = NULL;

  assert(VALID(self));

  if (self->name)
    {
      def = TrioFindNamespace(self->name, &prev);
      if (def)
	{
	  if (internalEnterCriticalRegion)
	    (void)internalEnterCriticalRegion(NULL);
	  
	  if (prev == NULL)
	    internalUserDef = internalUserDef->next;
	  else
	    prev->next = def->next;
	  
	  if (internalLeaveCriticalRegion)
	    (void)internalLeaveCriticalRegion(NULL);
	}
      trio_destroy(self->name);
    }
  TRIO_FREE(self);
}

/*************************************************************************
 * trio_get_format
 */
TRIO_PUBLIC
TRIO_CONST char *
trio_get_format
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
#if TRIO_FEATURE_USER_DEFINED
  assert(((trio_reference_t *)ref)->parameter->type == FORMAT_USER_DEFINED);
#endif
  
  return (((trio_reference_t *)ref)->parameter->user_data);
}

/*************************************************************************
 * trio_get_argument
 */
TRIO_PUBLIC
trio_pointer_t
trio_get_argument
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
#if TRIO_FEATURE_USER_DEFINED
  assert(((trio_reference_t *)ref)->parameter->type == FORMAT_USER_DEFINED);
#endif

  return ((trio_reference_t *)ref)->parameter->data.pointer;
}

/*************************************************************************
 * trio_get_width / trio_set_width
 */
TRIO_PUBLIC
int
trio_get_width
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return ((trio_reference_t *)ref)->parameter->width;
}

TRIO_PUBLIC
void
trio_set_width
TRIO_ARGS2((ref, width),
	   trio_pointer_t ref,
	   int width)
{
  ((trio_reference_t *)ref)->parameter->width = width;
}

/*************************************************************************
 * trio_get_precision / trio_set_precision
 */
TRIO_PUBLIC
int
trio_get_precision
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->precision);
}

TRIO_PUBLIC
void
trio_set_precision
TRIO_ARGS2((ref, precision),
	   trio_pointer_t ref,
	   int precision)
{
  ((trio_reference_t *)ref)->parameter->precision = precision;
}

/*************************************************************************
 * trio_get_base / trio_set_base
 */
TRIO_PUBLIC
int
trio_get_base
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->base);
}

TRIO_PUBLIC
void
trio_set_base
TRIO_ARGS2((ref, base),
	   trio_pointer_t ref,
	   int base)
{
  ((trio_reference_t *)ref)->parameter->base = base;
}

/*************************************************************************
 * trio_get_long / trio_set_long
 */
TRIO_PUBLIC
int
trio_get_long
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_LONG)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_long
TRIO_ARGS2((ref, is_long),
	   trio_pointer_t ref,
	   int is_long)
{
  if (is_long)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_LONG;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_LONG;
}

/*************************************************************************
 * trio_get_longlong / trio_set_longlong
 */
TRIO_PUBLIC
int
trio_get_longlong
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_QUAD)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_longlong
TRIO_ARGS2((ref, is_longlong),
	   trio_pointer_t ref,
	   int is_longlong)
{
  if (is_longlong)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_QUAD;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_QUAD;
}

/*************************************************************************
 * trio_get_longdouble / trio_set_longdouble
 */
# if TRIO_FEATURE_FLOAT
TRIO_PUBLIC
int
trio_get_longdouble
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_LONGDOUBLE)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_longdouble
TRIO_ARGS2((ref, is_longdouble),
	   trio_pointer_t ref,
	   int is_longdouble)
{
  if (is_longdouble)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_LONGDOUBLE;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_LONGDOUBLE;
}
# endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * trio_get_short / trio_set_short
 */
TRIO_PUBLIC
int
trio_get_short
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_SHORT)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_short
TRIO_ARGS2((ref, is_short),
	   trio_pointer_t ref,
	   int is_short)
{
  if (is_short)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_SHORT;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_SHORT;
}

/*************************************************************************
 * trio_get_shortshort / trio_set_shortshort
 */
TRIO_PUBLIC
int
trio_get_shortshort
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_SHORTSHORT)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_shortshort
TRIO_ARGS2((ref, is_shortshort),
	   trio_pointer_t ref,
	   int is_shortshort)
{
  if (is_shortshort)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_SHORTSHORT;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_SHORTSHORT;
}

/*************************************************************************
 * trio_get_alternative / trio_set_alternative
 */
TRIO_PUBLIC
int
trio_get_alternative
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_ALTERNATIVE)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_alternative
TRIO_ARGS2((ref, is_alternative),
	   trio_pointer_t ref,
	   int is_alternative)
{
  if (is_alternative)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_ALTERNATIVE;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_ALTERNATIVE;
}

/*************************************************************************
 * trio_get_alignment / trio_set_alignment
 */
TRIO_PUBLIC
int
trio_get_alignment
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_LEFTADJUST)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_alignment
TRIO_ARGS2((ref, is_leftaligned),
	   trio_pointer_t ref,
	   int is_leftaligned)
{
  if (is_leftaligned)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_LEFTADJUST;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_LEFTADJUST;
}

/*************************************************************************
 * trio_get_spacing /trio_set_spacing
 */
TRIO_PUBLIC
int
trio_get_spacing
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_SPACE)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_spacing
TRIO_ARGS2((ref, is_space),
	   trio_pointer_t ref,
	   int is_space)
{
  if (is_space)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_SPACE;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_SPACE;
}

/*************************************************************************
 * trio_get_sign / trio_set_sign
 */
TRIO_PUBLIC
int
trio_get_sign
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_SHOWSIGN)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_sign
TRIO_ARGS2((ref, is_sign),
	   trio_pointer_t ref,
	   int is_sign)
{
  if (is_sign)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_SHOWSIGN;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_SHOWSIGN;
}

/*************************************************************************
 * trio_get_padding / trio_set_padding
 */
TRIO_PUBLIC
int
trio_get_padding
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_NILPADDING)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_padding
TRIO_ARGS2((ref, is_padding),
	   trio_pointer_t ref,
	   int is_padding)
{
  if (is_padding)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_NILPADDING;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_NILPADDING;
}

/*************************************************************************
 * trio_get_quote / trio_set_quote
 */
# if TRIO_FEATURE_QUOTE
TRIO_PUBLIC
int
trio_get_quote
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_QUOTE)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_quote
TRIO_ARGS2((ref, is_quote),
	   trio_pointer_t ref,
	   int is_quote)
{
  if (is_quote)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_QUOTE;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_QUOTE;
}
#endif /* TRIO_FEATURE_QUOTE */

/*************************************************************************
 * trio_get_upper / trio_set_upper
 */
TRIO_PUBLIC
int
trio_get_upper
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_UPPER)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_upper
TRIO_ARGS2((ref, is_upper),
	   trio_pointer_t ref,
	   int is_upper)
{
  if (is_upper)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_UPPER;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_UPPER;
}

/*************************************************************************
 * trio_get_largest / trio_set_largest
 */
#if TRIO_FEATURE_INTMAX_T
TRIO_PUBLIC
int
trio_get_largest
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_INTMAX_T)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_largest
TRIO_ARGS2((ref, is_largest),
	   trio_pointer_t ref,
	   int is_largest)
{
  if (is_largest)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_INTMAX_T;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_INTMAX_T;
}
#endif /* TRIO_FEATURE_INTMAX_T */

/*************************************************************************
 * trio_get_ptrdiff / trio_set_ptrdiff
 */
#if TRIO_FEATURE_PTRDIFF_T
TRIO_PUBLIC
int
trio_get_ptrdiff
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_PTRDIFF_T)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_ptrdiff
TRIO_ARGS2((ref, is_ptrdiff),
	   trio_pointer_t ref,
	   int is_ptrdiff)
{
  if (is_ptrdiff)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_PTRDIFF_T;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_PTRDIFF_T;
}
#endif /* TRIO_FEATURE_PTRDIFF_T */

/*************************************************************************
 * trio_get_size / trio_set_size
 */
#if TRIO_FEATURE_SIZE_T
TRIO_PUBLIC
int
trio_get_size
TRIO_ARGS1((ref),
	   trio_pointer_t ref)
{
  return (((trio_reference_t *)ref)->parameter->flags & FLAGS_SIZE_T)
    ? TRUE
    : FALSE;
}

TRIO_PUBLIC
void
trio_set_size
TRIO_ARGS2((ref, is_size),
	   trio_pointer_t ref,
	   int is_size)
{
  if (is_size)
    ((trio_reference_t *)ref)->parameter->flags |= FLAGS_SIZE_T;
  else
    ((trio_reference_t *)ref)->parameter->flags &= ~FLAGS_SIZE_T;
}
#endif /* TRIO_FEATURE_SIZE_T */

/*************************************************************************
 * trio_print_int
 */
TRIO_PUBLIC
void
trio_print_int
TRIO_ARGS2((ref, number),
	   trio_pointer_t ref,
	   int number)
{
  trio_reference_t *self = (trio_reference_t *)ref;

  TrioWriteNumber(self->data,
		  (trio_uintmax_t)number,
		  self->parameter->flags,
		  self->parameter->width,
		  self->parameter->precision,
		  self->parameter->base);
}

/*************************************************************************
 * trio_print_uint
 */
TRIO_PUBLIC
void
trio_print_uint
TRIO_ARGS2((ref, number),
	   trio_pointer_t ref,
	   unsigned int number)
{
  trio_reference_t *self = (trio_reference_t *)ref;

  TrioWriteNumber(self->data,
		  (trio_uintmax_t)number,
		  self->parameter->flags | FLAGS_UNSIGNED,
		  self->parameter->width,
		  self->parameter->precision,
		  self->parameter->base);
}

/*************************************************************************
 * trio_print_double
 */
#if TRIO_FEATURE_FLOAT
TRIO_PUBLIC
void
trio_print_double
TRIO_ARGS2((ref, number),
	   trio_pointer_t ref,
	   double number)
{
  trio_reference_t *self = (trio_reference_t *)ref;

  TrioWriteDouble(self->data,
		  number,
		  self->parameter->flags,
		  self->parameter->width,
		  self->parameter->precision,
		  self->parameter->base);
}
#endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * trio_print_string
 */
TRIO_PUBLIC
void
trio_print_string
TRIO_ARGS2((ref, string),
	   trio_pointer_t ref,
	   TRIO_CONST char *string)
{
  trio_reference_t *self = (trio_reference_t *)ref;

  TrioWriteString(self->data,
		  string,
		  self->parameter->flags,
		  self->parameter->width,
		  self->parameter->precision);
}

/*************************************************************************
 * trio_print_ref
 */
TRIO_PUBLIC
int
trio_print_ref
TRIO_VARGS3((ref, format, va_alist),
	    trio_pointer_t ref,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list arglist;

  assert(VALID(format));
  
  TRIO_VA_START(arglist, format);
  status = TrioFormatRef((trio_reference_t *)ref, format, arglist, NULL, NULL);
  TRIO_VA_END(arglist);
  return status;
}

/*************************************************************************
 * trio_vprint_ref
 */
TRIO_PUBLIC
int
trio_vprint_ref
TRIO_ARGS3((ref, format, arglist),
	   trio_pointer_t ref,
	   TRIO_CONST char *format,
	   va_list arglist)
{
  assert(VALID(format));
  
  return TrioFormatRef((trio_reference_t *)ref, format, arglist, NULL, NULL);
}

/*************************************************************************
 * trio_printv_ref
 */
TRIO_PUBLIC
int
trio_printv_ref
TRIO_ARGS3((ref, format, argarray),
	   trio_pointer_t ref,
	   TRIO_CONST char *format,
	   trio_pointer_t *argarray)
{
  static va_list unused;
  
  assert(VALID(format));
  
  return TrioFormatRef((trio_reference_t *)ref, format,
		       unused, TrioArrayGetter, argarray);
}

#endif

/*************************************************************************
 * trio_print_pointer
 */
TRIO_PUBLIC
void
trio_print_pointer
TRIO_ARGS2((ref, pointer),
	   trio_pointer_t ref,
	   trio_pointer_t pointer)
{
  trio_reference_t *self = (trio_reference_t *)ref;
  trio_flags_t flags;
  trio_uintmax_t number;

  if (NULL == pointer)
    {
      TRIO_CONST char *string = internalNullString;
      while (*string)
	self->data->OutStream(self->data, *string++);
    }
  else
    {
      /*
       * The subtraction of the null pointer is a workaround
       * to avoid a compiler warning. The performance overhead
       * is negligible (and likely to be removed by an
       * optimizing compiler). The (char *) casting is done
       * to please ANSI C++.
       */
      number = (trio_uintmax_t)((char *)pointer - (char *)0);
      /* Shrink to size of pointer */
      number &= (trio_uintmax_t)-1;
      flags = self->parameter->flags;
      flags |= (FLAGS_UNSIGNED | FLAGS_ALTERNATIVE |
	        FLAGS_NILPADDING);
      TrioWriteNumber(self->data,
		      number,
		      flags,
		      POINTER_WIDTH,
		      NO_PRECISION,
		      BASE_HEX);
    }
}

/** @} End of UserDefined documentation module */

/*************************************************************************
 *
 * LOCALES
 *
 ************************************************************************/

/*************************************************************************
 * trio_locale_set_decimal_point
 *
 * Decimal point can only be one character. The input argument is a
 * string to enable multibyte characters. At most MB_LEN_MAX characters
 * will be used.
 */
#if TRIO_FEATURE_LOCALE
TRIO_PUBLIC void
trio_locale_set_decimal_point
TRIO_ARGS1((decimalPoint),
	   char *decimalPoint)
{
#if defined(USE_LOCALE)
  if (NULL == internalLocaleValues)
    {
      TrioSetLocale();
    }
#endif
  internalDecimalPointLength = trio_length(decimalPoint);
  if (internalDecimalPointLength == 1)
    {
      internalDecimalPoint = *decimalPoint;
    }
  else
    {
      internalDecimalPoint = NIL;
      trio_copy_max(internalDecimalPointString,
		    sizeof(internalDecimalPointString),
		    decimalPoint);
    }
}
#endif

/*************************************************************************
 * trio_locale_set_thousand_separator
 *
 * See trio_locale_set_decimal_point
 */
#if TRIO_FEATURE_LOCALE || TRIO_EXTENSION
TRIO_PUBLIC void
trio_locale_set_thousand_separator
TRIO_ARGS1((thousandSeparator),
	   char *thousandSeparator)
{
# if defined(USE_LOCALE)
  if (NULL == internalLocaleValues)
    {
      TrioSetLocale();
    }
# endif
  trio_copy_max(internalThousandSeparator,
		sizeof(internalThousandSeparator),
		thousandSeparator);
  internalThousandSeparatorLength = trio_length(internalThousandSeparator);
}
#endif

/*************************************************************************
 * trio_locale_set_grouping
 *
 * Array of bytes. Reversed order.
 *
 *  CHAR_MAX : No further grouping
 *  0        : Repeat last group for the remaining digits (not necessary
 *             as C strings are zero-terminated)
 *  n        : Set current group to n
 *
 * Same order as the grouping attribute in LC_NUMERIC.
 */
#if TRIO_FEATURE_LOCALE || TRIO_EXTENSION
TRIO_PUBLIC void
trio_locale_set_grouping
TRIO_ARGS1((grouping),
	   char *grouping)
{
# if defined(USE_LOCALE)
  if (NULL == internalLocaleValues)
    {
      TrioSetLocale();
    }
# endif
  trio_copy_max(internalGrouping,
		sizeof(internalGrouping),
		grouping);
}
#endif


/*************************************************************************
 *
 * SCANNING
 *
 ************************************************************************/

#if TRIO_FEATURE_SCANF

/*************************************************************************
 * TrioSkipWhitespaces
 */
TRIO_PRIVATE int
TrioSkipWhitespaces
TRIO_ARGS1((self),
	   trio_class_t *self)
{
  int ch;

  ch = self->current;
  while (isspace(ch))
    {
      self->InStream(self, &ch);
    }
  return ch;
}

/*************************************************************************
 * TrioGetCollation
 */
#if TRIO_EXTENSION
TRIO_PRIVATE void
TrioGetCollation(TRIO_NOARGS)
{
  int i;
  int j;
  int k;
  char first[2];
  char second[2];

  /* This is computationally expensive */
  first[1] = NIL;
  second[1] = NIL;
  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
    {
      k = 0;
      first[0] = (char)i;
      for (j = 0; j < MAX_CHARACTER_CLASS; j++)
	{
	  second[0] = (char)j;
	  if (trio_equal_locale(first, second))
	    internalCollationArray[i][k++] = (char)j;
	}
      internalCollationArray[i][k] = NIL;
    }
}
#endif

/*************************************************************************
 * TrioGetCharacterClass
 *
 * FIXME:
 *  multibyte
 */
TRIO_PRIVATE int
TrioGetCharacterClass
TRIO_ARGS4((format, offsetPointer, flagsPointer, characterclass),
	   TRIO_CONST char *format,
	   int *offsetPointer,
	   trio_flags_t *flagsPointer,
	   int *characterclass)
{
  int offset = *offsetPointer;
  int i;
  char ch;
  char range_begin;
  char range_end;

  *flagsPointer &= ~FLAGS_EXCLUDE;

  if (format[offset] == QUALIFIER_CIRCUMFLEX)
    {
      *flagsPointer |= FLAGS_EXCLUDE;
      offset++;
    }
  /*
   * If the ungroup character is at the beginning of the scanlist,
   * it will be part of the class, and a second ungroup character
   * must follow to end the group.
   */
  if (format[offset] == SPECIFIER_UNGROUP)
    {
      characterclass[(int)SPECIFIER_UNGROUP]++;
      offset++;
    }
  /*
   * Minus is used to specify ranges. To include minus in the class,
   * it must be at the beginning of the list
   */
  if (format[offset] == QUALIFIER_MINUS)
    {
      characterclass[(int)QUALIFIER_MINUS]++;
      offset++;
    }
  /* Collect characters */
  for (ch = format[offset];
       (ch != SPECIFIER_UNGROUP) && (ch != NIL);
       ch = format[++offset])
    {
      switch (ch)
	{
	case QUALIFIER_MINUS: /* Scanlist ranges */
	  
	  /*
	   * Both C99 and UNIX98 describes ranges as implementation-
	   * defined.
	   *
	   * We support the following behaviour (although this may
	   * change as we become wiser)
	   * - only increasing ranges, ie. [a-b] but not [b-a]
	   * - transitive ranges, ie. [a-b-c] == [a-c]
	   * - trailing minus, ie. [a-] is interpreted as an 'a'
	   *   and a '-'
	   * - duplicates (although we can easily convert these
	   *   into errors)
	   */
	  range_begin = format[offset - 1];
	  range_end = format[++offset];
	  if (range_end == SPECIFIER_UNGROUP)
	    {
	      /* Trailing minus is included */
	      characterclass[(int)ch]++;
	      ch = range_end;
	      break; /* for */
	    }
	  if (range_end == NIL)
	    return TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
	  if (range_begin > range_end)
	    return TRIO_ERROR_RETURN(TRIO_ERANGE, offset);
	    
	  for (i = (int)range_begin; i <= (int)range_end; i++)
	    characterclass[i]++;
	    
	  ch = range_end;
	  break;
	  
#if TRIO_EXTENSION

	case SPECIFIER_GROUP:
	  
	  switch (format[offset + 1])
	    {
	    case QUALIFIER_DOT: /* Collating symbol */
	      /*
	       * FIXME: This will be easier to implement when multibyte
	       * characters have been implemented. Until now, we ignore
	       * this feature.
	       */
	      for (i = offset + 2; ; i++)
		{
		  if (format[i] == NIL)
		    /* Error in syntax */
		    return -1;
		  else if (format[i] == QUALIFIER_DOT)
		    break; /* for */
		}
	      if (format[++i] != SPECIFIER_UNGROUP)
		return -1;
	      
	      offset = i;
	      break;
	  
	    case QUALIFIER_EQUAL: /* Equivalence class expressions */
	      {
		unsigned int j;
		unsigned int k;
	    
		if (internalCollationUnconverted)
		  {
		    /* Lazy evaluation of collation array */
		    TrioGetCollation();
		    internalCollationUnconverted = FALSE;
		  }
		for (i = offset + 2; ; i++)
		  {
		    if (format[i] == NIL)
		      /* Error in syntax */
		      return -1;
		    else if (format[i] == QUALIFIER_EQUAL)
		      break; /* for */
		    else
		      {
			/* Mark any equivalent character */
			k = (unsigned int)format[i];
			for (j = 0; internalCollationArray[k][j] != NIL; j++)
			  characterclass[(int)internalCollationArray[k][j]]++;
		      }
		  }
		if (format[++i] != SPECIFIER_UNGROUP)
		  return -1;
		
		offset = i;
	      }
	      break;
	  
	    case QUALIFIER_COLON: /* Character class expressions */
	  
	      if (trio_equal_max(CLASS_ALNUM, sizeof(CLASS_ALNUM) - 1,
				 &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isalnum(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_ALNUM) - 1;
		}
	      else if (trio_equal_max(CLASS_ALPHA, sizeof(CLASS_ALPHA) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isalpha(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_ALPHA) - 1;
		}
	      else if (trio_equal_max(CLASS_CNTRL, sizeof(CLASS_CNTRL) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (iscntrl(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_CNTRL) - 1;
		}
	      else if (trio_equal_max(CLASS_DIGIT, sizeof(CLASS_DIGIT) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isdigit(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_DIGIT) - 1;
		}
	      else if (trio_equal_max(CLASS_GRAPH, sizeof(CLASS_GRAPH) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isgraph(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_GRAPH) - 1;
		}
	      else if (trio_equal_max(CLASS_LOWER, sizeof(CLASS_LOWER) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (islower(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_LOWER) - 1;
		}
	      else if (trio_equal_max(CLASS_PRINT, sizeof(CLASS_PRINT) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isprint(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_PRINT) - 1;
		}
	      else if (trio_equal_max(CLASS_PUNCT, sizeof(CLASS_PUNCT) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (ispunct(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_PUNCT) - 1;
		}
	      else if (trio_equal_max(CLASS_SPACE, sizeof(CLASS_SPACE) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isspace(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_SPACE) - 1;
		}
	      else if (trio_equal_max(CLASS_UPPER, sizeof(CLASS_UPPER) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isupper(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_UPPER) - 1;
		}
	      else if (trio_equal_max(CLASS_XDIGIT, sizeof(CLASS_XDIGIT) - 1,
				      &format[offset]))
		{
		  for (i = 0; i < MAX_CHARACTER_CLASS; i++)
		    if (isxdigit(i))
		      characterclass[i]++;
		  offset += sizeof(CLASS_XDIGIT) - 1;
		}
	      else
		{
		  characterclass[(int)ch]++;
		}
	      break;

	    default:
	      characterclass[(int)ch]++;
	      break;
	    }
	  break;
	  
#endif /* TRIO_EXTENSION */
	  
	default:
	  characterclass[(int)ch]++;
	  break;
	}
    }
  return 0;
}

/*************************************************************************
 * TrioReadNumber
 *
 * We implement our own number conversion in preference of strtol and
 * strtoul, because we must handle 'long long' and thousand separators.
 */
TRIO_PRIVATE BOOLEAN_T
TrioReadNumber
TRIO_ARGS5((self, target, flags, width, base),
	   trio_class_t *self,
	   trio_uintmax_t *target,
	   trio_flags_t flags,
	   int width,
	   int base)
{
  trio_uintmax_t number = 0;
  int digit;
  int count;
  BOOLEAN_T isNegative = FALSE;
  BOOLEAN_T gotNumber = FALSE;
  int j;

  assert(VALID(self));
  assert(VALID(self->InStream));
  assert((base >= MIN_BASE && base <= MAX_BASE) || (base == NO_BASE));

  if (internalDigitsUnconverted)
    {
      /* Lazy evaluation of digits array */
      memset(internalDigitArray, -1, sizeof(internalDigitArray));
      for (j = 0; j < (int)sizeof(internalDigitsLower) - 1; j++)
	{
	  internalDigitArray[(int)internalDigitsLower[j]] = j;
	  internalDigitArray[(int)internalDigitsUpper[j]] = j;
	}
      internalDigitsUnconverted = FALSE;
    }
  
  TrioSkipWhitespaces(self);
  
  /* Leading sign */
  if (self->current == '+')
    {
      self->InStream(self, NULL);
    }
  else if (self->current == '-')
    {
      self->InStream(self, NULL);
      isNegative = TRUE;
    }
  
  count = self->processed;
  
  if (flags & FLAGS_ALTERNATIVE)
    {
      switch (base)
	{
	case NO_BASE:
	case BASE_OCTAL:
	case BASE_HEX:
	case BASE_BINARY:
	  if (self->current == '0')
	    {
	      self->InStream(self, NULL);
	      if (self->current)
		{
		  if ((base == BASE_HEX) &&
		      (trio_to_upper(self->current) == 'X'))
		    {
		      self->InStream(self, NULL);
		    }
		  else if ((base == BASE_BINARY) &&
			   (trio_to_upper(self->current) == 'B'))
		    {
		      self->InStream(self, NULL);
		    }
		}
	    }
	  else
	    return FALSE;
	  break;
	default:
	  break;
	}
    }

  while (((width == NO_WIDTH) || (self->processed - count < width)) &&
	 (! ((self->current == EOF) || isspace(self->current))))
    {
      if (isascii(self->current))
	{
	  digit = internalDigitArray[self->current];
	  /* Abort if digit is not allowed in the specified base */
	  if ((digit == -1) || (digit >= base))
	    break;
	}
#if TRIO_FEATURE_QUOTE
      else if (flags & FLAGS_QUOTE)
	{
	  /* Compare with thousands separator */
	  for (j = 0; internalThousandSeparator[j] && self->current; j++)
	    {
	      if (internalThousandSeparator[j] != self->current)
		break;

	      self->InStream(self, NULL);
	    }
	  if (internalThousandSeparator[j])
	    break; /* Mismatch */
	  else
	    continue; /* Match */
	}
#endif
      else
	break;
            
      number *= base;
      number += digit;
      gotNumber = TRUE; /* we need at least one digit */

      self->InStream(self, NULL);
    }

  /* Was anything read at all? */
  if (!gotNumber)
    return FALSE;
  
  if (target)
    *target = (isNegative) ? (trio_uintmax_t)(-((trio_intmax_t)number)) : number;
  return TRUE;
}

/*************************************************************************
 * TrioReadChar
 */
TRIO_PRIVATE int
TrioReadChar
TRIO_ARGS4((self, target, flags, width),
	   trio_class_t *self,
	   char *target,
	   trio_flags_t flags,
	   int width)
{
  int i;
  char ch;
  trio_uintmax_t number;
  
  assert(VALID(self));
  assert(VALID(self->InStream));

  for (i = 0;
       (self->current != EOF) && (i < width);
       i++)
    {
      ch = (char)self->current;
      self->InStream(self, NULL);
      if ((flags & FLAGS_ALTERNATIVE) && (ch == CHAR_BACKSLASH))
	{
	  switch (self->current)
	    {
	    case '\\': ch = '\\'; break;
	    case 'a': ch = '\007'; break;
	    case 'b': ch = '\b'; break;
	    case 'f': ch = '\f'; break;
	    case 'n': ch = '\n'; break;
	    case 'r': ch = '\r'; break;
	    case 't': ch = '\t'; break;
	    case 'v': ch = '\v'; break;
	    default:
	      if (isdigit(self->current))
		{
		  /* Read octal number */
		  if (!TrioReadNumber(self, &number, 0, 3, BASE_OCTAL))
		    return 0;
		  ch = (char)number;
		}
	      else if (trio_to_upper(self->current) == 'X')
		{
		  /* Read hexadecimal number */
		  self->InStream(self, NULL);
		  if (!TrioReadNumber(self, &number, 0, 2, BASE_HEX))
		    return 0;
		  ch = (char)number;
		}
	      else
		{
		  ch = (char)self->current;
		}
	      break;
	    }
	}
      
      if (target)
	target[i] = ch;
    }
  return i + 1;
}

/*************************************************************************
 * TrioReadString
 */
TRIO_PRIVATE BOOLEAN_T
TrioReadString
TRIO_ARGS4((self, target, flags, width),
	   trio_class_t *self,
	   char *target,
	   trio_flags_t flags,
	   int width)
{
  int i;
  
  assert(VALID(self));
  assert(VALID(self->InStream));

  TrioSkipWhitespaces(self);
    
  /*
   * Continue until end of string is reached, a whitespace is encountered,
   * or width is exceeded
   */
  for (i = 0;
       ((width == NO_WIDTH) || (i < width)) &&
       (! ((self->current == EOF) || isspace(self->current)));
       i++)
    {
      if (TrioReadChar(self, (target ? &target[i] : 0), flags, 1) == 0)
	break; /* for */
    }
  if (target)
    target[i] = NIL;
  return TRUE;
}

/*************************************************************************
 * TrioReadWideChar
 */
#if TRIO_FEATURE_WIDECHAR
TRIO_PRIVATE int
TrioReadWideChar
TRIO_ARGS4((self, target, flags, width),
	   trio_class_t *self,
	   trio_wchar_t *target,
	   trio_flags_t flags,
	   int width)
{
  int i;
  int j;
  int size;
  int amount = 0;
  trio_wchar_t wch;
  char buffer[MB_LEN_MAX + 1];
  
  assert(VALID(self));
  assert(VALID(self->InStream));

  for (i = 0;
       (self->current != EOF) && (i < width);
       i++)
    {
      if (isascii(self->current))
	{
	  if (TrioReadChar(self, buffer, flags, 1) == 0)
	    return 0;
	  buffer[1] = NIL;
	}
      else
	{
	  /*
	   * Collect a multibyte character, by enlarging buffer until
	   * it contains a fully legal multibyte character, or the
	   * buffer is full.
	   */
	  j = 0;
	  do
	    {
	      buffer[j++] = (char)self->current;
	      buffer[j] = NIL;
	      self->InStream(self, NULL);
	    }
	  while ((j < (int)sizeof(buffer)) && (mblen(buffer, (size_t)j) != j));
	}
      if (target)
	{
	  size = mbtowc(&wch, buffer, sizeof(buffer));
	  if (size > 0)
	    target[i] = wch;
	}
      amount += size;
      self->InStream(self, NULL);
    }
  return amount;
}
#endif /* TRIO_FEATURE_WIDECHAR */

/*************************************************************************
 * TrioReadWideString
 */
#if TRIO_FEATURE_WIDECHAR
TRIO_PRIVATE BOOLEAN_T
TrioReadWideString
TRIO_ARGS4((self, target, flags, width),
	   trio_class_t *self,
	   trio_wchar_t *target,
	   trio_flags_t flags,
	   int width)
{
  int i;
  int size;
  
  assert(VALID(self));
  assert(VALID(self->InStream));

  TrioSkipWhitespaces(self);

#if defined(TRIO_COMPILER_SUPPORTS_MULTIBYTE)
  /* Required by TrioReadWideChar */
  (void)mblen(NULL, 0);
#endif
  
  /*
   * Continue until end of string is reached, a whitespace is encountered,
   * or width is exceeded
   */
  for (i = 0;
       ((width == NO_WIDTH) || (i < width)) &&
       (! ((self->current == EOF) || isspace(self->current)));
       )
    {
      size = TrioReadWideChar(self, &target[i], flags, 1);
      if (size == 0)
	break; /* for */

      i += size;
    }
  if (target)
    target[i] = WCONST('\0');
  return TRUE;
}
#endif /* TRIO_FEATURE_WIDECHAR */

/*************************************************************************
 * TrioReadGroup
 *
 * Reads non-empty character groups.
 *
 * FIXME: characterclass does not work with multibyte characters
 */
TRIO_PRIVATE BOOLEAN_T
TrioReadGroup
TRIO_ARGS5((self, target, characterclass, flags, width),
	   trio_class_t *self,
	   char *target,
	   int *characterclass,
	   trio_flags_t flags,
	   int width)
{
  int ch;
  int i;
  
  assert(VALID(self));
  assert(VALID(self->InStream));

  ch = self->current;
  for (i = 0;
       ((width == NO_WIDTH) || (i < width)) &&
       (! ((ch == EOF) ||
	   (((flags & FLAGS_EXCLUDE) != 0) ^ (characterclass[ch] == 0))));
       i++)
    {
      if (target)
	target[i] = (char)ch;
      self->InStream(self, &ch);
    }

  if (i == 0)
    return FALSE;

  /* Terminate the string if input saved */
  if (target)
    target[i] = NIL;
  return TRUE;
}

/*************************************************************************
 * TrioReadDouble
 *
 * FIXME:
 *  add long double
 *  handle base
 */
#if TRIO_FEATURE_FLOAT
TRIO_PRIVATE BOOLEAN_T
TrioReadDouble
TRIO_ARGS4((self, target, flags, width),
	   trio_class_t *self,
	   trio_pointer_t target,
	   trio_flags_t flags,
	   int width)
{
  int ch;
  char doubleString[512];
  int offset = 0;
  int start;
# if TRIO_FEATURE_QUOTE
  int j;
# endif
  BOOLEAN_T isHex = FALSE;
  trio_long_double_t infinity;

  doubleString[0] = 0;
  
  if ((width == NO_WIDTH) || (width > (int)sizeof(doubleString) - 1))
    width = sizeof(doubleString) - 1;
  
  TrioSkipWhitespaces(self);
  
  /*
   * Read entire double number from stream. trio_to_double requires
   * a string as input, but InStream can be anything, so we have to
   * collect all characters.
   */
  ch = self->current;
  if ((ch == '+') || (ch == '-'))
    {
      doubleString[offset++] = (char)ch;
      self->InStream(self, &ch);
      width--;
    }

  start = offset;
  switch (ch)
    {
    case 'n':
    case 'N':
      /* Not-a-number */
      if (offset != 0)
	break;
      /* FALLTHROUGH */
    case 'i':
    case 'I':
      /* Infinity */
      while (isalpha(ch) && (offset - start < width))
	{
	  doubleString[offset++] = (char)ch;
	  self->InStream(self, &ch);
	}
      doubleString[offset] = NIL;

      /* Case insensitive string comparison */
      if (trio_equal(&doubleString[start], INFINITE_UPPER) ||
	  trio_equal(&doubleString[start], LONG_INFINITE_UPPER))
	{
	  infinity = ((start == 1) && (doubleString[0] == '-'))
	    ? trio_ninf()
	    : trio_pinf();
	  if (flags & FLAGS_LONGDOUBLE)
	    {
	      *((trio_long_double_t *)target) = infinity;
	    }
	  else if (flags & FLAGS_LONG)
	    {
	      *((double *)target) = infinity;
	    }
	  else
	    {
	      *((float *)target) = infinity;
	    }
	  return TRUE;
	}
      if (trio_equal(doubleString, NAN_UPPER))
	{
	  /* NaN must not have a preceeding + nor - */
	  if (flags & FLAGS_LONGDOUBLE)
	    {
	      *((trio_long_double_t *)target) = trio_nan();
	    }
	  else if (flags & FLAGS_LONG)
	    {
	      *((double *)target) = trio_nan();
	    }
	  else
	    {
	      *((float *)target) = trio_nan();
	    }
	  return TRUE;
	}
      return FALSE;

    case '0':
      doubleString[offset++] = (char)ch;
      self->InStream(self, &ch);
      if (trio_to_upper(ch) == 'X')
	{
	  isHex = TRUE;
	  doubleString[offset++] = (char)ch;
	  self->InStream(self, &ch);
	}
      break;
      
    default:
      break;
    }
  
  while ((ch != EOF) && (offset - start < width))
    {
      /* Integer part */
      if (isHex ? isxdigit(ch) : isdigit(ch))
	{
	  doubleString[offset++] = (char)ch;
	  self->InStream(self, &ch);
	}
# if TRIO_FEATURE_QUOTE
      else if (flags & FLAGS_QUOTE)
	{
	  /* Compare with thousands separator */
	  for (j = 0; internalThousandSeparator[j] && self->current; j++)
	    {
	      if (internalThousandSeparator[j] != self->current)
		break;

	      self->InStream(self, &ch);
	    }
	  if (internalThousandSeparator[j])
	    break; /* Mismatch */
	  else
	    continue; /* Match */
	}
# endif
      else
	break; /* while */
    }
  if (ch == '.')
    {
      /* Decimal part */
      doubleString[offset++] = (char)ch;
      self->InStream(self, &ch);
      while ((isHex ? isxdigit(ch) : isdigit(ch)) &&
	     (offset - start < width))
	{
	  doubleString[offset++] = (char)ch;
	  self->InStream(self, &ch);
	}
    }
  if (isHex ? (trio_to_upper(ch) == 'P') : (trio_to_upper(ch) == 'E'))
    {
      /* Exponent */
      doubleString[offset++] = (char)ch;
      self->InStream(self, &ch);
      if ((ch == '+') || (ch == '-'))
	{
	  doubleString[offset++] = (char)ch;
	  self->InStream(self, &ch);
	}
      while (isdigit(ch) && (offset - start < width))
	{
	  doubleString[offset++] = (char)ch;
	  self->InStream(self, &ch);
	}
    }

  if ((offset == start) || (*doubleString == NIL))
    return FALSE;

  doubleString[offset] = 0;
  
  if (flags & FLAGS_LONGDOUBLE)
    {
      *((trio_long_double_t *)target) = trio_to_long_double(doubleString, NULL);
    }
  else if (flags & FLAGS_LONG)
    {
      *((double *)target) = trio_to_double(doubleString, NULL);
    }
  else
    {
      *((float *)target) = trio_to_float(doubleString, NULL);
    }
  return TRUE;
}
#endif /* TRIO_FEATURE_FLOAT */

/*************************************************************************
 * TrioReadPointer
 */
TRIO_PRIVATE BOOLEAN_T
TrioReadPointer
TRIO_ARGS3((self, target, flags),
	   trio_class_t *self,
	   trio_pointer_t *target,
	   trio_flags_t flags)
{
  trio_uintmax_t number;
  char buffer[sizeof(internalNullString)];

  flags |= (FLAGS_UNSIGNED | FLAGS_ALTERNATIVE | FLAGS_NILPADDING);
  
  if (TrioReadNumber(self,
		     &number,
		     flags,
		     POINTER_WIDTH,
		     BASE_HEX))
    {
      if (target)
	{
#if defined(TRIO_COMPILER_GCC) || defined(TRIO_COMPILER_MIPSPRO)
	  /*
	   * The strange assignment of number is a workaround for a compiler
	   * warning
	   */
	  *target = &((char *)0)[number];
#else
	  *target = (trio_pointer_t)number;
#endif
	}
      return TRUE;
    }
  else if (TrioReadString(self,
			  (flags & FLAGS_IGNORE)
			  ? NULL
			  : buffer,
			  0,
			  sizeof(internalNullString) - 1))
    {
      if (trio_equal_case(buffer, internalNullString))
	{
	  if (target)
	    *target = NULL;
	  return TRUE;
	}
    }
  return FALSE;
}

/*************************************************************************
 * TrioScanProcess
 */
TRIO_PRIVATE int
TrioScanProcess
TRIO_ARGS3((data, format, parameters),
	   trio_class_t *data,
	   TRIO_CONST char *format,
	   trio_parameter_t *parameters)
{
  int status;
  int assignment;
  int ch;
  int offset; /* Offset of format string */
  int i; /* Offset of current parameter */
  trio_flags_t flags;
  int width;
  int base;
  trio_pointer_t pointer;

  /* Return on empty format string */
  if (format[0] == NIL)
    return 0;

  status = 0;
  assignment = 0;
  i = 0;
  offset = 0;
  data->InStream(data, &ch);

  for (;;)
    {
      /* Skip the parameter entries */
      while (parameters[i].type == FORMAT_PARAMETER)
	{
	  assert(i <= MAX_PARAMETERS);
	  i++;
	}

      /* Compare non conversion-specifier part of format string */
      while (offset < parameters[i].beginOffset)
	{
	  if ((CHAR_IDENTIFIER == format[offset]) &&
	      (CHAR_IDENTIFIER == format[offset + 1]))
	    {
	      /* Two % in format matches one % in input stream */
	      if (CHAR_IDENTIFIER == ch)
		{
		  data->InStream(data, &ch);
		  offset += 2;
		  continue; /* while format chars left */
		}
	      else
		{
		  status = TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
		  goto end;
		}
	    }
	  else /* Not an % identifier */
	    {
	      if (isspace((int)format[offset]))
		{
		  /* Whitespaces may match any amount of whitespaces */
		  ch = TrioSkipWhitespaces(data);
		}
	      else if (ch == format[offset])
		{
		  data->InStream(data, &ch);
		}
	      else
		{
		  status = assignment;
		  goto end;
		}

	      offset++;
	    }
	}

      if (parameters[i].type == FORMAT_SENTINEL)
	break;

      if ((EOF == ch) && (parameters[i].type != FORMAT_COUNT))
	{
	  status = (assignment > 0) ? assignment : EOF;
	  goto end;
	}

      flags = parameters[i].flags;

      /* Find width */
      width = parameters[i].width;
      if (flags & FLAGS_WIDTH_PARAMETER)
	{
	  /* Get width from parameter list */
	  width = (int)parameters[width].data.number.as_signed;
	}

      /* Find base */
      if (NO_BASE != parameters[i].baseSpecifier)
	{
	  /* Base from specifier has priority */
	  base = parameters[i].baseSpecifier;
	}
      else if (flags & FLAGS_BASE_PARAMETER)
	{
	  /* Get base from parameter list */
	  base = parameters[i].base;
	  base = (int)parameters[base].data.number.as_signed;
	}
      else
	{
	  /* Use base from format string */
	  base = parameters[i].base;
	}

      switch (parameters[i].type)
	{
	case FORMAT_INT:
	  {
	    trio_uintmax_t number;

	    if (0 == base)
	      base = BASE_DECIMAL;

	    if (!TrioReadNumber(data,
				&number,
				flags,
				width,
				base))
	      {
		status = assignment;
		goto end;
	      }

	    if (!(flags & FLAGS_IGNORE))
	      {
		assignment++;

		pointer = parameters[i].data.pointer;
#if TRIO_FEATURE_SIZE_T || TRIO_FEATURE_SIZE_T_UPPER
		if (flags & FLAGS_SIZE_T)
		  *(size_t *)pointer = (size_t)number;
		else
#endif
#if TRIO_FEATURE_PTRDIFF_T
		if (flags & FLAGS_PTRDIFF_T)
		  *(ptrdiff_t *)pointer = (ptrdiff_t)number;
		else
#endif
#if TRIO_FEATURE_INTMAX_T
		if (flags & FLAGS_INTMAX_T)
		  *(trio_intmax_t *)pointer = (trio_intmax_t)number;
		else
#endif
		if (flags & FLAGS_QUAD)
		  *(trio_ulonglong_t *)pointer = (trio_ulonglong_t)number;
		else if (flags & FLAGS_LONG)
		  *(long int *)pointer = (long int)number;
		else if (flags & FLAGS_SHORT)
		  *(short int *)pointer = (short int)number;
		else
		  *(int *)pointer = (int)number;
	      }
	  }
	  break; /* FORMAT_INT */

	case FORMAT_STRING:
#if TRIO_FEATURE_WIDECHAR
	  if (flags & FLAGS_WIDECHAR)
	    {
	      if (!TrioReadWideString(data,
				      (flags & FLAGS_IGNORE)
				      ? NULL
				      : parameters[i].data.wstring,
				      flags,
				      width))
		{
		  status = assignment;
		  goto end;
		}
	    }
	  else
#endif
	    {
	      if (!TrioReadString(data,
				  (flags & FLAGS_IGNORE)
				  ? NULL
				  : parameters[i].data.string,
				  flags,
				  width))
		{
		  status = assignment;
		  goto end;
		}
	    }
	  if (!(flags & FLAGS_IGNORE))
	    assignment++;
	  break; /* FORMAT_STRING */

#if TRIO_FEATURE_FLOAT
	case FORMAT_DOUBLE:
	  {
	    if (flags & FLAGS_IGNORE)
	      {
		pointer = NULL;
	      }
	    else
	      {
		pointer = (flags & FLAGS_LONGDOUBLE)
		  ? (trio_pointer_t)parameters[i].data.longdoublePointer
		  : (trio_pointer_t)parameters[i].data.doublePointer;
	      }
	    if (!TrioReadDouble(data, pointer, flags, width))
	      {
		status = assignment;
		goto end;
	      }
	    if (!(flags & FLAGS_IGNORE))
	      {
		assignment++;
	      }
	    break; /* FORMAT_DOUBLE */
	  }
#endif

	case FORMAT_GROUP:
	  {
	    int characterclass[MAX_CHARACTER_CLASS + 1];

	    /* Skip over modifiers */
	    while (format[offset] != SPECIFIER_GROUP)
	      {
		offset++;
	      }
	    /* Skip over group specifier */
	    offset++;

	    memset(characterclass, 0, sizeof(characterclass));
	    status = TrioGetCharacterClass(format,
					   &offset,
					   &flags,
					   characterclass);
	    if (status < 0)
	      goto end;

	    if (!TrioReadGroup(data,
			       (flags & FLAGS_IGNORE)
			       ? NULL
			       : parameters[i].data.string,
			       characterclass,
			       flags,
			       parameters[i].width))
	      {
		status = assignment;
		goto end;
	      }
	    if (!(flags & FLAGS_IGNORE))
	      assignment++;
	  }
	  break; /* FORMAT_GROUP */

	case FORMAT_COUNT:
	  pointer = parameters[i].data.pointer;
	  if (NULL != pointer)
	    {
	      int count = data->processed;
	      if (ch != EOF)
		count--; /* a character is read, but is not consumed yet */
#if TRIO_FEATURE_SIZE_T || TRIO_FEATURE_SIZE_T_UPPER
	      if (flags & FLAGS_SIZE_T)
		*(size_t *)pointer = (size_t)count;
	      else
#endif
#if TRIO_FEATURE_PTRDIFF_T
	      if (flags & FLAGS_PTRDIFF_T)
		*(ptrdiff_t *)pointer = (ptrdiff_t)count;
	      else
#endif
#if TRIO_FEATURE_INTMAX_T
	      if (flags & FLAGS_INTMAX_T)
		*(trio_intmax_t *)pointer = (trio_intmax_t)count;
	      else
#endif
	      if (flags & FLAGS_QUAD)
		{
		  *(trio_ulonglong_t *)pointer = (trio_ulonglong_t)count;
		}
	      else if (flags & FLAGS_LONG)
		{
		  *(long int *)pointer = (long int)count;
		}
	      else if (flags & FLAGS_SHORT)
		{
		  *(short int *)pointer = (short int)count;
		}
	      else
		{
		  *(int *)pointer = (int)count;
		}
	    }
	  break; /* FORMAT_COUNT */

	case FORMAT_CHAR:
#if TRIO_FEATURE_WIDECHAR
	  if (flags & FLAGS_WIDECHAR)
	    {
	      if (TrioReadWideChar(data,
				   (flags & FLAGS_IGNORE)
				   ? NULL
				   : parameters[i].data.wstring,
				   flags,
				   (width == NO_WIDTH) ? 1 : width) == 0)
		{
		  status = assignment;
		  goto end;
		}
	    }
	  else
#endif
	    {
	      if (TrioReadChar(data,
			       (flags & FLAGS_IGNORE)
			       ? NULL
			       : parameters[i].data.string,
			       flags,
			       (width == NO_WIDTH) ? 1 : width) == 0)
		{
		  status = assignment;
		  goto end;
		}
	    }
	  if (!(flags & FLAGS_IGNORE))
	    assignment++;
	  break; /* FORMAT_CHAR */

	case FORMAT_POINTER:
	  if (!TrioReadPointer(data,
			       (flags & FLAGS_IGNORE)
			       ? NULL
			       : (trio_pointer_t *)parameters[i].data.pointer,
			       flags))
	    {
	      status = assignment;
	      goto end;
	    }
	  if (!(flags & FLAGS_IGNORE))
	    assignment++;
	  break; /* FORMAT_POINTER */

	case FORMAT_PARAMETER:
	  break; /* FORMAT_PARAMETER */

	default:
	  status = TRIO_ERROR_RETURN(TRIO_EINVAL, offset);
	  goto end;
	}

      ch = data->current;
      offset = parameters[i].endOffset;
      i++;
    }

  status = assignment;
 end:
  if (data->UndoStream)
    data->UndoStream(data);
  return status;
}

/*************************************************************************
 * TrioScan
 */
TRIO_PRIVATE int
TrioScan
TRIO_ARGS8((source, sourceSize, InStream, UndoStream, format, arglist, argfunc, argarray),
	   trio_pointer_t source,
	   size_t sourceSize,
	   void (*InStream) TRIO_PROTO((trio_class_t *, int *)),
	   void (*UndoStream) TRIO_PROTO((trio_class_t *)),
	   TRIO_CONST char *format,
	   va_list arglist,
	   trio_argfunc_t argfunc,
	   trio_pointer_t *argarray)
{
  int status;
  trio_parameter_t parameters[MAX_PARAMETERS];
  trio_class_t data;

  assert(VALID(InStream));
  assert(VALID(format));

  memset(&data, 0, sizeof(data));
  data.InStream = InStream;
  data.UndoStream = UndoStream;
  data.location = (trio_pointer_t)source;
  data.max = sourceSize;
  data.error = 0;

#if defined(USE_LOCALE)
  if (NULL == internalLocaleValues)
    {
      TrioSetLocale();
    }
#endif

  status = TrioParse(TYPE_SCAN, format, parameters, arglist, argfunc, argarray);
  if (status < 0)
    return status;

  status = TrioScanProcess(&data, format, parameters);
  if (data.error != 0)
    {
      status = data.error;
    }
  return status;
}

/*************************************************************************
 * TrioInStreamFile
 */
#if TRIO_FEATURE_FILE || TRIO_FEATURE_STDIO
TRIO_PRIVATE void
TrioInStreamFile
TRIO_ARGS2((self, intPointer),
	   trio_class_t *self,
	   int *intPointer)
{
  FILE *file = (FILE *)self->location;

  assert(VALID(self));
  assert(VALID(file));

  self->actually.cached = 0;

  /* The initial value of self->current is zero */
  if (self->current == EOF)
    {
      self->error = (ferror(file))
	? TRIO_ERROR_RETURN(TRIO_ERRNO, 0)
	: TRIO_ERROR_RETURN(TRIO_EOF, 0);
    }
  else
    {
      self->processed++;
      self->actually.cached++;
    }

  self->current = fgetc(file);

  if (VALID(intPointer))
    {
      *intPointer = self->current;
    }
}
#endif /* TRIO_FEATURE_FILE || TRIO_FEATURE_STDIO */

/*************************************************************************
 * TrioUndoStreamFile
 */
#if TRIO_FEATURE_FILE || TRIO_FEATURE_STDIO
TRIO_PRIVATE void
TrioUndoStreamFile
TRIO_ARGS1((self),
	   trio_class_t *self)
{
  FILE *file = (FILE *)self->location;

  assert(VALID(self));
  assert(VALID(file));

  if (self->actually.cached > 0)
    {
      assert(self->actually.cached == 1);

      self->current = ungetc(self->current, file);
      self->actually.cached = 0;
    }
}
#endif /* TRIO_FEATURE_FILE || TRIO_FEATURE_STDIO */

/*************************************************************************
 * TrioInStreamFileDescriptor
 */
#if TRIO_FEATURE_FD
TRIO_PRIVATE void
TrioInStreamFileDescriptor
TRIO_ARGS2((self, intPointer),
	   trio_class_t *self,
	   int *intPointer)
{
  int fd = *((int *)self->location);
  int size;
  unsigned char input;

  assert(VALID(self));

  self->actually.cached = 0;

  size = read(fd, &input, sizeof(char));
  if (size == -1)
    {
      self->error = TRIO_ERROR_RETURN(TRIO_ERRNO, 0);
      self->current = EOF;
    }
  else
    {
      self->current = (size == 0) ? EOF : input;
    }
  if (self->current != EOF)
    {
      self->actually.cached++;
      self->processed++;
    }

  if (VALID(intPointer))
    {
      *intPointer = self->current;
    }
}
#endif /* TRIO_FEATURE_FD */

/*************************************************************************
 * TrioInStreamCustom
 */
#if TRIO_FEATURE_CLOSURE
TRIO_PRIVATE void
TrioInStreamCustom
TRIO_ARGS2((self, intPointer),
	   trio_class_t *self,
	   int *intPointer)
{
  trio_custom_t *data;

  assert(VALID(self));
  assert(VALID(self->location));

  self->actually.cached = 0;

  data = (trio_custom_t *)self->location;

  self->current = (data->stream.in == NULL)
    ? NIL
    : (data->stream.in)(data->closure);

  if (self->current == NIL)
    {
      self->current = EOF;
    }
  else
    {
      self->processed++;
      self->actually.cached++;
    }

  if (VALID(intPointer))
    {
      *intPointer = self->current;
    }
}
#endif /* TRIO_FEATURE_CLOSURE */

/*************************************************************************
 * TrioInStreamString
 */
TRIO_PRIVATE void
TrioInStreamString
TRIO_ARGS2((self, intPointer),
	   trio_class_t *self,
	   int *intPointer)
{
  unsigned char **buffer;

  assert(VALID(self));
  assert(VALID(self->location));

  self->actually.cached = 0;

  buffer = (unsigned char **)self->location;
  self->current = (*buffer)[0];
  if (self->current == NIL)
    {
      self->current = EOF;
    }
  else
    {
      (*buffer)++;
      self->processed++;
      self->actually.cached++;
    }

  if (VALID(intPointer))
    {
      *intPointer = self->current;
    }
}

/*************************************************************************
 *
 * Formatted scanning functions
 *
 ************************************************************************/

#if defined(TRIO_DOCUMENTATION)
# include "doc/doc_scanf.h"
#endif
/** @addtogroup Scanf
    @{
*/

/*************************************************************************
 * scanf
 */

/**
   Scan characters from standard input stream.

   @param format Formatting string.
   @param ... Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_STDIO
TRIO_PUBLIC int
trio_scanf
TRIO_VARGS2((format, va_alist),
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioScan((trio_pointer_t)stdin, 0,
		    TrioInStreamFile,
		    TrioUndoStreamFile,
		    format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_STDIO */

/**
   Scan characters from standard input stream.

   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_STDIO
TRIO_PUBLIC int
trio_vscanf
TRIO_ARGS2((format, args),
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)stdin, 0,
		  TrioInStreamFile,
		  TrioUndoStreamFile,
		  format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_STDIO */

/**
   Scan characters from standard input stream.

   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_STDIO
TRIO_PUBLIC int
trio_scanfv
TRIO_ARGS2((format, args),
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)stdin, 0,
		  TrioInStreamFile,
		  TrioUndoStreamFile,
		  format,
		  unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_STDIO */

/*************************************************************************
 * fscanf
 */

/**
   Scan characters from file.

   @param file File pointer.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_FILE
TRIO_PUBLIC int
trio_fscanf
TRIO_VARGS3((file, format, va_alist),
	    FILE *file,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(file));
  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioScan((trio_pointer_t)file, 0,
		    TrioInStreamFile,
		    TrioUndoStreamFile,
		    format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_FILE */

/**
   Scan characters from file.

   @param file File pointer.
   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_FILE
TRIO_PUBLIC int
trio_vfscanf
TRIO_ARGS3((file, format, args),
	   FILE *file,
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(file));
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)file, 0,
		  TrioInStreamFile,
		  TrioUndoStreamFile,
		  format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_FILE */

/**
   Scan characters from file.

   @param file File pointer.
   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_FILE
TRIO_PUBLIC int
trio_fscanfv
TRIO_ARGS3((file, format, args),
	   FILE *file,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  
  assert(VALID(file));
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)file, 0,
		  TrioInStreamFile,
		  TrioUndoStreamFile,
		  format,
		  unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_FILE */

/*************************************************************************
 * dscanf
 */

/**
   Scan characters from file descriptor.

   @param fd File descriptor.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_FD
TRIO_PUBLIC int
trio_dscanf
TRIO_VARGS3((fd, format, va_alist),
	    int fd,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioScan((trio_pointer_t)&fd, 0,
		    TrioInStreamFileDescriptor,
		    NULL,
		    format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_FD */

/**
   Scan characters from file descriptor.

   @param fd File descriptor.
   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_FD
TRIO_PUBLIC int
trio_vdscanf
TRIO_ARGS3((fd, format, args),
	   int fd,
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)&fd, 0,
		  TrioInStreamFileDescriptor,
		  NULL,
		  format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_FD */

/**
   Scan characters from file descriptor.

   @param fd File descriptor.
   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
#if TRIO_FEATURE_FD
TRIO_PUBLIC int
trio_dscanfv
TRIO_ARGS3((fd, format, args),
	   int fd,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)&fd, 0,
		  TrioInStreamFileDescriptor,
		  NULL,
		  format,
		  unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_FD */

/*************************************************************************
 * cscanf
 */
#if TRIO_FEATURE_CLOSURE
TRIO_PUBLIC int
trio_cscanf
TRIO_VARGS4((stream, closure, format, va_alist),
	    trio_instream_t stream,
	    trio_pointer_t closure,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;
  trio_custom_t data;

  assert(VALID(stream));
  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  data.stream.in = stream;
  data.closure = closure;
  status = TrioScan(&data, 0, TrioInStreamCustom, NULL, format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}
#endif /* TRIO_FEATURE_CLOSURE */

#if TRIO_FEATURE_CLOSURE
TRIO_PUBLIC int
trio_vcscanf
TRIO_ARGS4((stream, closure, format, args),
	   trio_instream_t stream,
	   trio_pointer_t closure,
	   TRIO_CONST char *format,
	   va_list args)
{
  trio_custom_t data;
  
  assert(VALID(stream));
  assert(VALID(format));

  data.stream.in = stream;
  data.closure = closure;
  return TrioScan(&data, 0, TrioInStreamCustom, NULL, format, args, NULL, NULL);
}
#endif /* TRIO_FEATURE_CLOSURE */

#if TRIO_FEATURE_CLOSURE
TRIO_PUBLIC int
trio_cscanfv
TRIO_ARGS4((stream, closure, format, args),
	   trio_instream_t stream,
	   trio_pointer_t closure,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  trio_custom_t data;
  
  assert(VALID(stream));
  assert(VALID(format));

  data.stream.in = stream;
  data.closure = closure;
  return TrioScan(&data, 0, TrioInStreamCustom, NULL, format,
		  unused, TrioArrayGetter, args);
}
#endif /* TRIO_FEATURE_CLOSURE */

#if TRIO_FEATURE_CLOSURE && TRIO_FEATURE_ARGFUNC
TRIO_PUBLIC int
trio_cscanff
TRIO_ARGS5((stream, closure, format, argfunc, context),
           trio_instream_t stream,
           trio_pointer_t closure,
           TRIO_CONST char *format,
           trio_argfunc_t argfunc,
           trio_pointer_t context)
{
  static va_list unused;
  trio_custom_t data;

  assert(VALID(stream));
  assert(VALID(format));
  assert(VALID(argfunc));

  data.stream.in = stream;
  data.closure = closure;
  return TrioScan(&data, 0, TrioInStreamCustom, NULL, format,
                  unused, argfunc, (trio_pointer_t *)context);
}
#endif /* TRIO_FEATURE_CLOSURE && TRIO_FEATURE_ARGFUNC */

/*************************************************************************
 * sscanf
 */

/**
   Scan characters from string.

   @param buffer Input string.
   @param format Formatting string.
   @param ... Arguments.
   @return Number of scanned characters.
 */
TRIO_PUBLIC int
trio_sscanf
TRIO_VARGS3((buffer, format, va_alist),
	    TRIO_CONST char *buffer,
	    TRIO_CONST char *format,
	    TRIO_VA_DECL)
{
  int status;
  va_list args;

  assert(VALID(buffer));
  assert(VALID(format));
  
  TRIO_VA_START(args, format);
  status = TrioScan((trio_pointer_t)&buffer, 0,
		    TrioInStreamString,
		    NULL,
		    format, args, NULL, NULL);
  TRIO_VA_END(args);
  return status;
}

/**
   Scan characters from string.

   @param buffer Input string.
   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
TRIO_PUBLIC int
trio_vsscanf
TRIO_ARGS3((buffer, format, args),
	   TRIO_CONST char *buffer,
	   TRIO_CONST char *format,
	   va_list args)
{
  assert(VALID(buffer));
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)&buffer, 0,
		  TrioInStreamString,
		  NULL,
		  format, args, NULL, NULL);
}

/**
   Scan characters from string.

   @param buffer Input string.
   @param format Formatting string.
   @param args Arguments.
   @return Number of scanned characters.
 */
TRIO_PUBLIC int
trio_sscanfv
TRIO_ARGS3((buffer, format, args),
	   TRIO_CONST char *buffer,
	   TRIO_CONST char *format,
	   trio_pointer_t *args)
{
  static va_list unused;
  
  assert(VALID(buffer));
  assert(VALID(format));
  
  return TrioScan((trio_pointer_t)&buffer, 0,
		  TrioInStreamString,
		  NULL,
		  format,
		  unused, TrioArrayGetter, args);
}

#endif /* TRIO_FEATURE_SCANF */

/** @} End of Scanf documentation module */

/*************************************************************************
 * trio_strerror
 */
TRIO_PUBLIC TRIO_CONST char *
trio_strerror
TRIO_ARGS1((errorcode),
	   int errorcode)
{
////////////////////////////////////////////////////////////////////////////////////////////////////
// NOTE: string: Silence unused parameter warning.
////////////////////////////////////////////////////////////////////////////////////////////////////
  (void)errorcode;
////////////////////////////////////////////////////////////////////////////////////////////////////
#if TRIO_FEATURE_STRERR
  /* Textual versions of the error codes */
  switch (TRIO_ERROR_CODE(errorcode))
    {
    case TRIO_EOF:
      return "End of file";
    case TRIO_EINVAL:
      return "Invalid argument";
    case TRIO_ETOOMANY:
      return "Too many arguments";
    case TRIO_EDBLREF:
      return "Double reference";
    case TRIO_EGAP:
      return "Reference gap";
    case TRIO_ENOMEM:
      return "Out of memory";
    case TRIO_ERANGE:
      return "Invalid range";
    case TRIO_ECUSTOM:
      return "Custom error";
    default:
      return "Unknown";
    }
#else
  return "Unknown";
#endif
}
