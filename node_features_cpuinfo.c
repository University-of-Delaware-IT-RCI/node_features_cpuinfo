/**
 * @file    node_features_cpuinfo.c
 * @brief   cpuinfo node_features plugin
 * @details A Slurm plugin that uses the contents of a node's
 *          /proc/cpuinfo to populate the node feature list
 *          with hardware-specific named features.
 *
 *          The features are named with a <TYPE>::<VALUE>
 *          format.  The currently-defined <TYPE> strings
 *          are:
 *
 *              VENDOR      CPU vendor string
 *              MODEL       succinct CPU model name
 *              CACHE       kilobytes of cache reported
 *              ISA         ISA extensions
 *
 *          There will typically be multiple ISA features
 *          present -- but not every ISA feature is noted.
 *          The plugin currently targets SSE and AVX
 *          so end users can constrain jobs to a maximum
 *          ISA complement.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>

#ifdef NODE_FEATURE_CPUINFO_TESTING

#include <stdarg.h>

#define xstrfmtcat(__p, __fmt, args...) _xstrfmtcat(&(__p), __fmt, ## args)
#define xfree(__p) _xfree((void **)&(__p))

void _xstrfmtcat(char **str, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
void _xfree(void **ptr);
        
void
_xstrfmtcat(
    char        **str,
    const char  *fmt,
    ...
)
{
    va_list     argv;
    char        *s = *str;
    int         s_len, new_len, need_nul;
    
    /* Current length of the string: */
    if ( s ) {
        s_len = 1 + (*s ? strlen(s) : 0);
        need_nul = 0;
    } else {
        s_len = 0;
        need_nul = 1;
    }
    
    /* Length of new string: */
    va_start(argv, fmt);
    new_len = vsnprintf(NULL, 0, fmt, argv);
    va_end(argv);
    
    if ( new_len > 0 ) {
        /* Resize: */
        s = (char*)realloc(s, s_len + new_len + need_nul);
        if ( ! s ) {
            perror("Memory allocation failure in _xstrfmtcat");
            exit(errno);
        }
        *str = s;
        va_start(argv, fmt);
        vsnprintf(s, s_len + new_len + need_nul, fmt, argv);
        va_end(argv);
    }
}

void _xfree(
    void    **ptr
)
{
    if ( *ptr ) {
        free(*ptr);
        *ptr = NULL;
    }
}

#endif

/**
 * @brief   Does a string start with the given prefix string
 * @details The @a string is checked to see if @a prefix is present
 *          at its start.
 * @param   string      the string to check
 * @param   prefix      the prefix to search for
 * @param   string_len  limit the search to this many characters in
 *                      @a string; pass a negative value if @a string
 *                      is NUL-terminated and its full length  is
 *                      permissible
 * @return  Boolean true if @a string starts with @a prefix
 */
static bool
str_startswith(
    const char  *string,
    const char  *prefix,
    int         string_len
)
{
    if ( string_len < 0 ) string_len = strlen(string);
    
    while ( string_len && *string && *prefix && (*string == *prefix) ) string++, prefix++, string_len--;
    return ( ! *prefix );
}

/**
 * @brief   Data structure for a double-buffered file reader that
 *          iterates line by line
 */
typedef struct line_reader {
    FILE        *stream;        /**< stdio stream to read */
    int         err_code;       /**< noted error code from an operation */
    
    char        *line_buffer;   /**< resizable buffer into which each line is copied */
    size_t      capacity;       /**< maximum byte capacity of the buffer */
    size_t      used;           /**< number of bytes in-use in the buffer */
    
    size_t      chunk_size;     /**< read the file this many bytes at a time */
    char        *buffer_ptr;    /**< current position in the chunk buffer */
    char        *buffer_end;    /**< pointer just beyond the last used byte of the chunk buffer */
    char        buffer[0];      /**< the chunk buffer (allocated as part of this structure */
} line_reader_t;

/**
 * @brief   Allocate and initialize a new line reader
 * @details The file given by @a filename is opened for reading and, if
 *          successful, a new line reader pseudo-object is created to
 *          handle i/o.
 * @param   filename    file to read
 * @param   chunk_size  read the file in chunks of this size; if less
 *                      than 128, defaults to 128
 * @return  Returns @a NULL on error, a new object otherwise
 */
static line_reader_t*
line_reader_create(
    const char      *filename,
    size_t          chunk_size
)
{
    FILE            *stream = NULL;
    size_t          line_reader_bytes;
    line_reader_t   *new_line_reader = NULL;
    
    if ( chunk_size < 128 ) chunk_size = 128;
    
    stream = fopen(filename, "r");
    if ( ! stream ) return NULL;
    
    line_reader_bytes = sizeof(line_reader_t) + chunk_size;
    new_line_reader = (line_reader_t*)malloc(line_reader_bytes);
    if ( new_line_reader ) {
        memset(new_line_reader, 0, sizeof(*new_line_reader));
        
        new_line_reader->stream = stream;
        
        new_line_reader->chunk_size = chunk_size;
        new_line_reader->buffer_ptr = &new_line_reader->buffer[0];
        new_line_reader->buffer_end = new_line_reader->buffer_ptr;
    } else {
        fclose(stream);
    }
    return new_line_reader;
}

/**
 * @brief   Destroy a line reader object
 * @param   this        a pointer to the pointer to the line reader;
 *                      @a *this is set to NULL after disposing of the
 *                      object
 */
static void
line_reader_free(
    line_reader_t   **this
)
{
    if ( this && *this ) {
        if ( (*this)->stream ) fclose((*this)->stream);
        if ( (*this)->line_buffer ) free((void*)(*this)->line_buffer);
        free((void*)(*this));
        *this = NULL;
    }
}

/**
 * @brief   Get the pointer to the line buffer
 * @details If the object has allocated a line buffer, returns the pointer
 *          to its current contents
 * @param   this        pointer to the line reader
 * @return  If no line buffer has been allocated returns @a NULL
 */
static const char*
line_reader_getline(
    line_reader_t   *this
)
{
    return (const char*)this->line_buffer;
}

/**
 * @brief   Remove any leading and trailing whitespace characters from
 *          the line in the line buffer
 * @param   this        pointer to the line reader
 */
static void
line_reader_trim(
    line_reader_t   *this
)
{
    if ( this->line_buffer && this->used ) {
        char        *p = this->line_buffer + this->used,
                    *e = this->line_buffer;
        size_t      d;
        
        /* Strip trailing whitespace; p starts out pointing just past the NUL
           character: */
        p--;
        while ( (p > e) && isspace(*(p - 1)) ) {
            p--, this->used--;
            *p = '\0';
        }
        
        /* Check for leading whitespace: */
        p = this->line_buffer;
        e = p + this->used;
        while ( (p < e) && isspace(*p) ) p++;
        if ( (d = p - this->line_buffer) > 0 ) {
            memmove(this->line_buffer, p, this->used - d);
            this->used -= d;
        }
    }
}

/**
 * @brief   Move the next line from the file to the line buffer
 * @details The line buffer may be resized/moved during this procedure.
 *          Characters remaining in the chunk buffer are consumed and
 *          additional chunks are read from the file as necessary.  Once
 *          a newline or end-of-file is reached, the line buffer fill is
 *          complete.
 * @param   this        pointer to the line reader
 * @return  On any error, @a NULL is returned; if a line was successfully
 *          consumed, the line buffer pointer is returned
 */
static const char*
line_reader_nextline(
    line_reader_t   *this
)
{
    /* Reset the line buffer: */
    this->used = 0;
    
    while ( 1 ) {
        bool        is_done = false;
        
        /* Any data left in the buffer? */
        while ( ! is_done && (this->buffer_ptr < this->buffer_end) ) {
            char        c;
            
            if ( this->used == this->capacity ) {
                size_t      new_capacity = this->capacity ? (2 * this->capacity) : 128;
                char        *new_line_buffer = (char*)realloc(this->line_buffer, new_capacity);
                
                if ( ! new_line_buffer ) {
                    this->err_code = ENOMEM;
                    return NULL;
                }
                this->line_buffer = new_line_buffer;
                this->capacity = new_capacity;
            }
            c = *this->buffer_ptr++;
            this->line_buffer[this->used++] = c;
            if ( (c == '\0') || (c == '\n') ) is_done = true;
        }
        
        /* If we're not done, then we must be at the end of the chunk
           buffer; read another: */
        if ( this->buffer_ptr == this->buffer_end ) {
            size_t      bytes_read;
            
            this->buffer_ptr = &this->buffer[0];
            bytes_read = fread(this->buffer_ptr, 1, this->chunk_size, this->stream);
            if ( bytes_read <= 0 ) {
                this->err_code = ( ferror(this->stream) ? -1 : 0 );
                return NULL;
            }
            this->buffer_end = this->buffer_ptr + bytes_read;
        }
        else if ( is_done ) {
            if ( this->used == this->capacity ) {
                size_t      new_capacity = this->capacity ? (2 * this->capacity) : 128;
                char        *new_line_buffer = (char*)realloc(this->line_buffer, new_capacity);
                
                if ( ! new_line_buffer ) {
                    this->err_code = ENOMEM;
                    return NULL;
                }
                this->line_buffer = new_line_buffer;
                this->capacity = new_capacity;
            }
            this->line_buffer[this->used++] = '\0';
            return (const char*)this->line_buffer;
        }
    }
}

/**
 * @brief cpuinfo ISA flag bit indices
 */
typedef enum {
    cpuinfo_flags_sse           =  0,   /**< SSE */
    cpuinfo_flags_sse2          =  1,   /**< SSE2 */
    cpuinfo_flags_ssse3         =  2,   /**< SSSE3 */
    cpuinfo_flags_sse4_1        =  3,   /**< SSE4.1 */
    cpuinfo_flags_sse4_2        =  4,   /**< SSE4.2 */
    cpuinfo_flags_avx           =  5,   /**< AVX */
    cpuinfo_flags_avx2          =  6,   /**< AVX2 */
    cpuinfo_flags_avx512f       =  7,   /**< AVX512 Foundation */
    cpuinfo_flags_avx512dq      =  8,   /**< AVX512 Double and Quad words */
    cpuinfo_flags_avx512cd      =  9,   /**< AVX512 Conflict Detection */
    cpuinfo_flags_avx512bw      = 10,   /**< AVX512 Byte words  */
    cpuinfo_flags_avx512vl      = 11,   /**< AVX512 Vector Length */
    cpuinfo_flags_avx512_vnni   = 12,   /**< AVX512 Vector Neural Network Instructions */
    cpuinfo_flags_MAX,                  /**< Index just beyond the last defined bit */
    cpuinfo_flags_START         =  0    /**< Index of the first bit */
} cpuinfo_flags_t;

/**
 * @var     cpuinfo_flags_strings
 * @brief   feature strings found in cpuinfo
 * @details ordered to match the @a cpuinfo_flags_t bit enumeration
 */
static const char* cpuinfo_flags_strings[] = {
        "sse",
        "sse2",
        "ssse3",
        "sse4_1",
        "sse4_2",
        "avx",
        "avx2",
        "avx512f",
        "avx512dq",
        "avx512cd",
        "avx512bw",
        "avx512vl",
        "avx512_vnni",
        NULL
    };

/**
 * @brief   Processor features from cpuinfo
 * @details Fields in this data structure are filled-in by
 *          reading the contents of the /proc/cpuinfo file.
 */
typedef struct cpuinfo_features {
    const char          *vendor_id;         /**< E.g. GenuineIntel, AuthenticAMD */
    const char          *model_name;        /**< Succinct CPU model name */
    unsigned int        cache_kb;           /**< Kilobytes of on-die cache */
    unsigned int        flags;              /**< ISA flags (bitmap w.r.t. cpuinfo_flags_t) */
} cpuinfo_features_t;

/**
 * @brief   Determine is a Slurm feature string is one that this
 *          plugin added
 * @param   feature_str     the Slurm feature string
 * @param   feature_str_len the number of characters to which the
 *                          search should be limited; if <= 0 then
 *                          @a feature_str is assumed to be a
 *                          NUL-terminated C string and strlen() is
 *                          used to determine the length limit
 * @return  Boolean true if @feature_str has one of our prefixes
 *          on it
 */
static bool
cpuinfo_features_is_str_ours(
    const char  *feature_str,
    int         feature_str_len
)
{
    if ( feature_str_len <= 0 ) feature_str_len = strlen(feature_str);
    if ( str_startswith(feature_str, "VENDOR::", feature_str_len) ) return true;
    if ( str_startswith(feature_str, "MODEL::", feature_str_len) ) return true;
    if ( str_startswith(feature_str, "CACHE::", feature_str_len) ) return true;
    if ( str_startswith(feature_str, "ISA::", feature_str_len) ) return true;
#ifdef HAVE_PCI_DETECTION
    if ( str_startswith(feature_str, "PCI::", feature_str_len) ) return true;
#endif
    return false;
}

/**
 * @brief   Initialize a cpuinfo_features_t data structure
 * @param   cif     pointer to the cpuinfo_features_t
 * @return  Returns @a cif (for chaining operations)
 */
static cpuinfo_features_t*
cpuinfo_features_init(
    cpuinfo_features_t  *cif
)
{
    memset(cif, 0, sizeof(*cif));
    return cif;
}

/**
 * @brief   Reset a cpuinfo_features_t data structure
 * @details Disposes of any external memory associated with @a cif -- namely
 *          any strings -- then reinitializes all fields
 * @param   cif     pointer to the cpuinfo_features_t
 * @return  Returns @a cif (for chaining operations)
 */
static cpuinfo_features_t*
cpuinfo_features_reset(
    cpuinfo_features_t  *cif
)
{
    if ( cif->vendor_id ) free((void*)cif->vendor_id);
    if ( cif->model_name ) free((void*)cif->model_name);
    return cpuinfo_features_init(cif);
}

/**
 * @brief   Write a summary of the cpuinfo_features_t fields to stdout
 * @param   cif     pointer to the cpuinfo_features_t
 */
static void
cpuinfo_features_summarize(
    cpuinfo_features_t  *cif
)
{
    unsigned int        i = cpuinfo_flags_START, mask = 1;
    const char          *delim = "";
    
    if ( cif->vendor_id ) printf("%sVENDOR::%s", delim, cif->vendor_id), delim = ",";
    if ( cif->model_name ) printf("%sMODEL::%s", delim, cif->model_name), delim = ",";
    if ( cif->cache_kb ) printf("%sCACHE::%uKB", delim, cif->cache_kb), delim = ",";
    while ( i < cpuinfo_flags_MAX ) {
        if ( (cif->flags & mask) == mask ) printf("%sISA::%s", delim, cpuinfo_flags_strings[i]), delim = ",";
        i++, mask <<= 1;
    }
    printf("\n");
}

/**
 * @brief   Opaque pointer to a cpuinfo feature parser record
 */
typedef struct cpuinfo_feature_parser * cpuinfo_feature_parser_ref;

/**
 * @brief   Type of a callback function that parses a cpuinfo item
 * @details Items occur as a keyword and value constrained to a single
 *          line of text.
 * @param   parser_registry the registry struct for this feature
 * @param   cif             pointer to the cpuinfo_features data structure to
 *                          fill-in
 * @param   text            immutable C string containing the feature value
 *                          from the cpuinfo file
 * @return  Boolean false should be returned if an error is encountered,
 *          otherwise boolean true.
 */
typedef bool (*cpuinfo_feature_parse_cb)(cpuinfo_feature_parser_ref parser_registry, cpuinfo_features_t *cif, const char *text);

/**
 * @brief   Registration data structure for a parsing callback
 */
typedef struct cpuinfo_feature_parser {
    const char                  *feature_str;   /**< The identifier to match in the cpuinfo file */
    cpuinfo_feature_parse_cb    parse_cb;       /**< The parsing callback */
    size_t                      arg_offset;     /**< Optional unsigned integer associated with feature */
    void                        *arg_pointer;   /**< Optional pointer associated with feature */
} cpuinfo_feature_parser_t;

/**
 * @brief   Parser callback that copies the value string
 * @details The @a arg_offset in the @a parser_registry locates the char*
 *          field that will be set to a duplicated copy of @text
 * @param   parser_registry the registry struct for the feature
 * @param   cif             pointer to the cpuinfo_features data structure to
 *                          fill-in
 * @param   text            immutable C string containing the feature value
 *                          from the cpuinfo file
 */
static bool
cpuinfo_parse_strdup(
    cpuinfo_feature_parser_ref      parser_registry,
    cpuinfo_features_t              *cif,
    const char                      *text
)
{
    void                            *p = (void*)cif;
    char                            **s;
    
    p += parser_registry->arg_offset;
    s = (char**)p;
    if ( *s != NULL ) free((void*)*s);
    *s = strdup(text);
    return true;
}

/**
 * @brief   Parser callback that handles cache size
 * @param   parser_registry the registry struct for the feature
 * @param   cif             pointer to the cpuinfo_features data structure to
 *                          fill-in
 * @param   text            immutable C string containing the feature value
 *                          from the cpuinfo file
 */
static bool
cpuinfo_parse_cache_size(
    cpuinfo_feature_parser_ref      parser_registry,
    cpuinfo_features_t              *cif,
    const char                      *text
)
{
    char                            *endp = NULL;
    double                          numerical_val = strtod(text, &endp);
    
    if ( endp > text ) {
        while ( *endp && isspace(*endp) ) endp++;
        switch ( toupper(*endp) ) {
            case 'G':
                numerical_val *= 1024.0;
            case 'M':
                numerical_val *= 1024.0;
            case 'K':
                endp++;
                break;
            case 'B':
                numerical_val /= 1024.0;
                break;
        }
        switch ( toupper(*endp) ) {
            case 'B':
            case '\0':
                cif->cache_kb = (unsigned int)numerical_val;
                return true;
        }
    }
    return false;
}

/**
 * @brief   Parser callback that handles processor model name
 * @details The model name field tends to be extremely verbose.  This function
 *          was developed on a cluster with Intel and AMD processors and assumes
 *          the most-important aspect of the processor model will match the
 *          regex:
 *
 *              (Gold |EPYC )?[A-Z0-9][A-Z-]*[0-9][A-Z0-9-]*( v[0-9]+)?
 * @param   parser_registry the registry struct for the feature
 * @param   cif             pointer to the cpuinfo_features data structure to
 *                          fill-in
 * @param   text            immutable C string containing the feature value
 *                          from the cpuinfo file
 */
static bool
cpuinfo_parse_model_name(
    cpuinfo_feature_parser_ref      parser_registry,
    cpuinfo_features_t              *cif,
    const char                      *text
)
{
    const char                      *prefix, *s, *e;
    
    /* First check for "Gold" or "EPYC" leadins: */
    if ( (s = strstr(text, "Gold ")) != NULL ) {
        prefix = s;
        s += 5;
    }
    else if ( (s = strstr(text, "EPYC ")) != NULL ) {
        prefix = s;
        s += 5;
    }
    else {
        prefix = NULL;
        s = text;
    }
    
    if ( prefix ) {
        if ( isalnum(*s) ) {
            /* Skip past the first alnum character: */
            e = s;
            e++;
            /* Zero or more alpha and dash characters: */
            while ( *e && (isalpha(*e) || (*e == '-')) ) e++;
            /* A single digit character: */
            if ( isdigit(*e) ) {
                char                *p;
                size_t              d;
                
                /* Skip past the digit: */
                e++;
                /* Zero or more alnum or dash characters: */
                while ( *e && (isalnum(*e) || (*e == '-')) ) e++;
                
                /* Characters [s,e) are the model: */
                if ( cif->model_name ) free((void*)cif->model_name);
                d = e - prefix;
                p = (char*)malloc(d + 1);
                memcpy(p, prefix, d);
                p[d] = '\0';
                cif->model_name = (const char*)p;
                e = p + d;
                while ( p < e ) {
                    if ( *p == ' ' ) *p = '_';
                    p++;
                }
                return true;
            }
        }
        /* Fall through to the generic check */
        s = text;
    }
    
    while ( *s ) {
        while ( ! isalnum(*s) ) s++;
        if ( *s ) {
            /* Skip past the first alnum character: */
            e = s;
            e++;
            /* Zero or more alpha and dash characters: */
            while ( *e && (isalpha(*e) || (*e == '-')) ) e++;
            /* A single digit character: */
            if ( isdigit(*e) ) {
                char                *p;
                size_t              d;
                
                /* Skip past the digit: */
                e++;
                /* Zero or more alnum or dash characters: */
                while ( *e && (isalnum(*e) || (*e == '-')) ) e++;
                
                /* Optional " v\d+": */
                if ( (*e == ' ') && (*(e+1) == 'v') && isdigit(*(e+2)) ) {
                    e += 2;
                    while ( isdigit(*e) ) e++;
                }
                
                /* Characters [s,e) are the model: */
                if ( cif->model_name ) free((void*)cif->model_name);
                d = e - s;
                p = (char*)malloc(d + 1);
                memcpy(p, s, d);
                p[d] = '\0';
                cif->model_name = (const char*)p;
                e = p + d;
                while ( p < e ) {
                    if ( *p == ' ' ) *p = '_';
                    p++;
                }
                return true;
            }
        }
        s++;
    }
    return false;
}

/**
 * @brief   Does one string occur within the other with only adjoining whitespace
 * @details The @a haystack is searched for an occurrence of @a needle and is trailed
 *          only by whitespace or the NUL terminator.
 * @param   haystack    the C string to search
 * @param   needle      the C string to find in the @a haystack
 * @return  Returns boolean true if the @a needle was found, false otherwise
 */
static bool
__contains_str(
    const char  *haystack,
    const char  *needle,
    const char  *delimiter
)
{
    size_t      needle_len = strlen(needle);
    const char  *p = haystack;
    
    if ( ! haystack || ! *haystack ) return false;
    
    if ( ! delimiter || ! *delimiter ) delimiter = " \t";
    
    while ( *p ) {
        char    *found = strstr(p, needle);
        char    *next_char = found + needle_len;
        
        if ( ! found ) break;
        if ( (*next_char == '\0') || (strchr(delimiter, *next_char) != NULL) ) return true;
        p = next_char;
    }
    return false;
}

/**
 * @brief   Parser callback that handles processor ISA flags
 * @param   parser_registry the registry struct for the feature
 * @param   cif             pointer to the cpuinfo_features data structure to
 *                          fill-in
 * @param   text            immutable C string containing the feature value
 *                          from the cpuinfo file
 */ 
static bool
cpuinfo_parse_flags(
    cpuinfo_feature_parser_ref      parser_registry,
    cpuinfo_features_t              *cif,
    const char                      *text
)
{
    unsigned int        i = cpuinfo_flags_START, mask = 1;
    
    cif->flags = 0;
    while ( i < cpuinfo_flags_MAX ) {
        if ( __contains_str(text, cpuinfo_flags_strings[i], NULL) ) cif->flags |= mask;
        i++, mask <<= 1;
    }
    return true;
}

/**
 * @var     cpuinfo_parsers
 * @brief   The list of feature parsers
 * @details The list of feature parser callbacks and their string identifier.
 *          A struct with both fields' being NULL/0 acts as the list
 *          terminator.
 */
static cpuinfo_feature_parser_t cpuinfo_feature_parsers[] = {
        { "cache size", cpuinfo_parse_cache_size, 0, NULL },
        { "flags", cpuinfo_parse_flags, 0, NULL },
        { "model name", cpuinfo_parse_model_name, 0, NULL },
        { "vendor_id", cpuinfo_parse_strdup, offsetof(cpuinfo_features_t, vendor_id), NULL },
        { NULL, NULL, 0, NULL }
    };

/**
 * @brief   Lookup the parser registration for a named feature
 * @param   feature_str_ptr Character buffer containing the feature
 *                          name
 * @param   feature_str_len Number of characters in the buffer
                            @a feature_str_ptr
 * @return  @a NULL if no such feature is registered, otherwise a
 *          pointer to the feature's registration record.
 */
static cpuinfo_feature_parser_t*
cpuinfo_feature_parsers_lookup(
    const char  *feature_str_ptr,
    size_t      feature_str_len
)
{
    cpuinfo_feature_parser_t    *parsers = cpuinfo_feature_parsers;
    while ( parsers->feature_str ) {
        int                     parser_feature_len = strlen(parsers->feature_str);
        
        if ( (parser_feature_len == feature_str_len) && (strncasecmp(parsers->feature_str, feature_str_ptr, feature_str_len) == 0) ) {
            return parsers;
        }
        parsers++;
    }
    return NULL;
}

/**
 * @brief   Parse a line of text read from the /proc/cpuinfo file
 * @param   cif     pointer to the cpuinfo_features data structure to
 *                  fill-in
 * @param   text    immutable C string containing a line from the file
 * @return  Boolean true is returned if the line was parsed successfully,
 *          otherwise boolean false.
 */
static bool
cpuinfo_parse_line(
    cpuinfo_features_t          *cif,
    const char                  *line
)
{
    cpuinfo_feature_parser_t    *parser = NULL;
    const char                  *feature_start, *feature_end;
    
    /* Drop any leading whitespace: */
    while ( *line && isspace(*line) ) line++;
    if ( ! *line ) return false;
    
    /* Skip ahead to the colon: */
    feature_start = line;
    while ( *line && *line != ':' ) line++;
    if ( *line != ':' ) return false;
    /* Now backtrack from the colon, past any whitespace to
       the first non-whitespace character: */
    feature_end = line;
    while ( (feature_end > feature_start) && isspace(*(feature_end - 1)) ) feature_end--;
    /* At this point the feature name lies from [feature_start, feature_end) */
    parser = cpuinfo_feature_parsers_lookup(feature_start, feature_end - feature_start);
    if ( ! parser ) return false;
    
    /* Pickup from where we left off with line pointing to the colon and skip past
       any whitespace: */
    line++;
    while ( *line && isspace(*line) ) line++;
    
    /* Present this to the parser: */
    return parser->parse_cb(parser, cif, line);
}

/**
 * @brief   Parse a file containing cpuinfo
 * @param   cif         pointer to the cpuinfo_features data structure to
 *                      fill-in
 * @param   filename    the file to read
 * @return  Boolean true is returned if the file was parsed successfully,
 *          otherwise boolean false.
 */
static bool
cpuinfo_parse_file(
    cpuinfo_features_t  *cif,
    const char          *filename
)
{
    line_reader_t       *line_reader = line_reader_create(filename, 0);
    
    if ( line_reader ) {
        const char      *line;
        
        while ( (line = line_reader_nextline(line_reader)) ) {
            line_reader_trim(line_reader);
            if ( *line == '\0' ) break;
            cpuinfo_parse_line(cif, line);
        }
        line_reader_free(&line_reader);
        return true;
    }
    return false;
}


#ifdef HAVE_PCI_DETECTION

#include <pciaccess.h>

/**
 * @brief   Map a feature name to a PCI device id
 */
typedef struct pci_device_feature {
    uint32_t                device_id;      /**< 16-bit PCI device id */
    const char              *feature_name;  /**< Slurm feature name */
} pci_device_feature_t;

/**
 * @brief   Map a list of device-feature matches to a PCI vendor id
 */
typedef struct pci_vendor_devices {
    uint32_t                vendor_id;          /**< 16-bit PCI vendor id */
    pci_device_feature_t    device_features[];  /**< array of device-to-feature mappings */
} pci_vendor_devices_t;

/**
 * @brief   Pointer to a vendor device list
 */
typedef pci_vendor_devices_t * pci_vendor_devices_ptr; 

/**
 * @brief   Summarize a vendor device list to stdout
 * @details Writes a summary of the vendors and each device associated
 *          with that vendor from the @a vendor_devices list
 * @param   vendor_devices  NUL-terminated list of vendor device pointers
 */
static void
pci_vendor_devices_summary(
    pci_vendor_devices_ptr  *vendor_devices
)
{
    while ( vendor_devices && *vendor_devices ) {
        pci_vendor_devices_t    *dev = *vendor_devices;
        pci_device_feature_t    *features = &dev->device_features[0];
        
        printf("0x%04X\n", dev->vendor_id);
        while ( features->device_id ) {
            printf("0x%04X 0x%04X %s\n", dev->vendor_id, features->device_id, features->feature_name);
            features++;
        }
        
        vendor_devices++;
    }
}

/**
 * @brief   Iterate the PCI buses and compile features associated with
 *          found devices
 * @details Find all devices matching the PCI @a device_class (as masked by
 *          @a device_class_mask) and if any appear in the @a vendor_devices
 *          list add their feature names to @out_features.
 * @param   vendor_devices      NUL-terminated list of vendor device pointers
 * @param   device_class        24-bit device class value
 * @param   device_class_mask   Mask indicating bits to compare in the
 *                              @a device_class
 * @param   out_features        Pointer to the C string pointer to contain
 *                              the features
 * @return  On any error, boolean false is returned.  Otherwise, boolean true
 *          is returned.
 */
static bool
pci_device_lookup(
    pci_vendor_devices_ptr  *vendor_devices,
    uint32_t                device_class,
    uint32_t                device_class_mask,
    char*                   *out_features
)
{
    static bool                 is_pci_inited = false;
    
    struct pci_device_iterator  *iter;
    struct pci_id_match         match;
    struct pci_device           *device;
    
    char                        *feature_list = NULL;
    const char                  *delim = (out_features && *out_features) ? "," : "";
    
    if ( ! out_features ) return false;
    
    if ( ! is_pci_inited ) {
        int                     rc = pci_system_init();
        if ( rc != 0 ) {
            // Handle error
            fprintf(stderr, "ERROR: unable to init PCI access (%d)\n", rc);
            return false;
        }
        is_pci_inited = true;
    }
    //
    // Iterate over display devices (class id 0x03):
    //
    match.vendor_id = PCI_MATCH_ANY;
    match.device_id = PCI_MATCH_ANY;
    match.subvendor_id = PCI_MATCH_ANY;
    match.subdevice_id = PCI_MATCH_ANY;
    match.device_class = device_class;
    match.device_class_mask = device_class_mask;
    match.match_data = 0;
    iter = pci_id_match_iterator_create(&match);
    while ((device = pci_device_next(iter)) != NULL) {
        pci_vendor_devices_ptr  *by_vendor = vendor_devices;
        bool                    is_found = false;
        
        while ( ! is_found && *by_vendor ) {
            if ( (*by_vendor)->vendor_id == device->vendor_id ) {
                pci_device_feature_t    *features = &(*by_vendor)->device_features[0];
                
                while ( features->device_id ) {
                    if ( features->device_id == device->device_id ) {
                        if ( ! feature_list || (strstr(feature_list, features->feature_name) == NULL) ) {
                            xstrfmtcat(feature_list, "%s%s", delim, features->feature_name);
                            delim = ",";
                            is_found = true;
                            break;
                        }
                    }
                    features++;
                }
            }
            by_vendor++;
        }
    }
    pci_iterator_destroy(iter);
    if ( out_features ) {
        *out_features = feature_list;
    } else {
        xfree(feature_list);
    }
    return true;
}

/**
 * @var     nvidia_gpu_devices
 * @brief   The list of NVIDIA GPU devices that exist in this cluster
 */
static pci_vendor_devices_t     nvidia_gpu_devices = {
            .vendor_id = 0x10de, .device_features = {
            /* P100 PCI, 12GB  */ { .device_id = 0x15f7, .feature_name = "PCI::GPU::P100" },
            /* V100 SXM2, 32GB */ { .device_id = 0x1db5, .feature_name = "PCI::GPU::V100" },
            /* V100 PCI, 32GB  */ { .device_id = 0x1db6, .feature_name = "PCI::GPU::V100" },
            /* T4              */ { .device_id = 0x1eb8, .feature_name = "PCI::GPU::T4"   },
            /* A100 PCI, 80GB  */ { .device_id = 0x20b5, .feature_name = "PCI::GPU::A100" },
            /* A40             */ { .device_id = 0x2235, .feature_name = "PCI::GPU::A40"  },
                                  { .device_id = 0x0000, .feature_name = NULL             }
    } };
    
/**
 * @var     amd_gpu_devices
 * @brief   The list of AMD GPU devices that exist in this cluster
 */
static pci_vendor_devices_t     amd_gpu_devices = {
            .vendor_id = 0x1002, .device_features = {
            /* Mi50     */ { .device_id = 0x66a1, .feature_name = "PCI::GPU::MI50"  },
            /* Mi100    */ { .device_id = 0x738c, .feature_name = "PCI::GPU::MI100" },
                           { .device_id = 0x0000, .feature_name = NULL              }
    } };
    
/**
 * @var     pci_known_devices
 * @brief   The list of PCI vendors (and their devices) that exist in this cluster
 */
static pci_vendor_devices_ptr   pci_known_devices[] = {
                                    &nvidia_gpu_devices,
                                    &amd_gpu_devices,
                                    NULL
                                };

/**
 * @var     pci_known_device_class
 * @brief   The PCI device class we're interested in iterating
 */
static const uint32_t           pci_known_device_class = 0x030000;

/**
 * @var     pci_known_device_class_mask
 * @brief   The bitmask for PCI device class components we're
 *          interested in iterating
 */
static const uint32_t           pci_known_device_class_mask = 0xFF0000;


#endif


#ifdef NODE_FEATURE_CPUINFO_TESTING

/*
 * Main program for testing the cpuinfo-scanning code
 */
int
main(
    int         argc,
    const char* argv[]
)
{
    cpuinfo_features_t  cif;
    int                 argi = 1;
    char                *pci_features = NULL;

#ifdef HAVE_PCI_DETECTION
    pci_device_lookup(pci_known_devices, pci_known_device_class, pci_known_device_class_mask, &pci_features);
#endif

    while ( argi < argc ) {
        cpuinfo_features_init(&cif);
        cpuinfo_parse_file(&cif, argv[argi]);
        printf("%s:    %s%s", argv[argi], pci_features?pci_features:"", pci_features?",":"");
        cpuinfo_features_summarize(&cif);
        cpuinfo_features_reset(&cif);
        argi++;
    }
    return 0;
}

#else

#include "slurm/slurm.h"

#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/fd.h"
#include "src/common/gres.h"
#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/node_conf.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/node_scheduler.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"
#include "src/slurmd/slurmd/req.h"

const char plugin_name[]        = "node_features cpuinfo plugin";
const char plugin_type[]        = "node_features/cpuinfo";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/* Configuration lock: */
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Configuration parameters: */
static bool is_node_features_inited = false;
static cpuinfo_features_t node_features;


/**
 * @brief   Load plugin
 * @return  SLURM_SUCCESS if successful, an error code otherwise
 */
extern int
init(void)
{
    debug("init");
    return SLURM_SUCCESS;
}

/**
 * @brief   Unload plugin
 * @details Release any memory allocated by this plugin
 * @return SLURM_SUCCESS if successful, an error code otherwise
 */
extern int
fini(void)
{
    debug("fini");
    if ( is_node_features_inited ) {
        cpuinfo_features_reset(&node_features);
        is_node_features_inited = false;
    }
	return SLURM_SUCCESS;
}

/**
 * @brief   Reload configuration
 * @return  SLURM_SUCCESS if successful, an error code otherwise
 */ 
extern int
node_features_p_reconfig(void)
{
    debug("node_features_p_reconfig");
	slurm_mutex_lock(&config_mutex);
    if ( is_node_features_inited ) {
        cpuinfo_features_reset(&node_features);
        is_node_features_inited = false;
    }
	slurm_mutex_unlock(&config_mutex);
	return SLURM_SUCCESS;
}

/**
 * @brief   Update active and available features on specified nodes
 * @details This function executes in the slurmctld context, NOT in slurmd
 *          on the nodes themselves
 * @param   node_list   list of node names or @a NULL if all nodes are
 *                      targetted
 * @return  SLURM_SUCCESS if successful, SLURM_ERROR otherwise
 */
extern int
node_features_p_get_node(
    char    *node_list
)
{
    debug("node_features_p_get_node: node_list = %s", node_list ? node_list : "(null)");
	return SLURM_SUCCESS;
}

/**
 * @brief   Get this node's current and available features
 * @param   avail_modes     pointer to a string pointer containing available
 *                          features (this plugin should append to it)
 * @param   current_mode    pointer to a string pointer containing active
 *                          features (this plugin should append to it)
 */
void
node_features_p_node_state(
    char    **avail_modes,
    char    **current_mode
)
{
    if ( ! avail_modes || ! current_mode ) return;
    
    debug("node_features_p_node_state: avail_modes = %s", *avail_modes ? *avail_modes : "(null)");
    debug("node_features_p_node_state: current_mode = %s", *current_mode ? *current_mode : "(null)");
    
	slurm_mutex_lock(&config_mutex);
    if ( ! is_node_features_inited ) {
        cpuinfo_features_init(&node_features);
        is_node_features_inited = cpuinfo_parse_file(&node_features, "/proc/cpuinfo");
    }
    if ( is_node_features_inited ) {
        unsigned int        i = cpuinfo_flags_START, mask = 1;
        char                *add_features = NULL;
        const char          *delim = "";
        
#ifdef HAVE_PCI_DETECTION
        if ( pci_device_lookup(pci_known_devices, pci_known_device_class, pci_known_device_class_mask, &add_features) ) {
            if ( add_features && *add_features ) delim = ",";
        }
#endif
        if ( node_features.vendor_id ) xstrfmtcat(add_features, "%sVENDOR::%s", delim, node_features.vendor_id), delim = ",";
        if ( node_features.model_name ) xstrfmtcat(add_features, "%sMODEL::%s", delim, node_features.model_name), delim = ",";
        if ( node_features.model_name ) xstrfmtcat(add_features, "%sCACHE::%uKB", delim, node_features.cache_kb), delim = ",";
        while ( i < cpuinfo_flags_MAX ) {
            if ( (node_features.flags & mask) == mask ) xstrfmtcat(add_features, "%sISA::%s", delim, cpuinfo_flags_strings[i]), delim = ",";
            i++, mask <<= 1;
        }
        if ( add_features && *add_features ) {
            if ( *avail_modes ) {
                xstrfmtcat(*avail_modes, ",%s", add_features);
            } else {
                *avail_modes = xstrdup(add_features);
            }
            if ( *current_mode ) {
                xstrfmtcat(*current_mode, ",%s", add_features);
            } else {
                *current_mode = xstrdup(add_features);
            }
            xfree(add_features);
        }
    }
	slurm_mutex_unlock(&config_mutex);
}

/**
 * @brief   Is a job's feature specification valid?
 * @details This is NOT a check of whether or not the features are acceptable
 *          on the slurmd node itself, just a semantic check
 * @param   job_features    the list of requested features
 * @return  SLURM_SUCCESS if the list is acceptable, an error code otherwise
 */
extern int node_features_p_job_valid(char *job_features)
{
    debug("node_features_p_job_valid");
    return SLURM_SUCCESS;
}

/**
 * @brief   Translate a job's feature request to the node features needed at boot time
 * @param   job_features    ampersand-separated list of features
 * @return  NULL if no features selected, otherwise a string of features associated
 *          with and needed by this plugin (allocated with a Slurm xmalloc etc.)
 */
extern char*
node_features_p_job_xlate(
    char    *job_features
)
{
    char    *out_features = NULL;
    
    debug("node_features_p_job_xlate: job_features = %s", job_features ? job_features : "(null)");
	if ( job_features && *job_features ) {
        char    *copy = xstrdup(job_features),
                *tokarg1 = copy, *tokptr, *save_ptr = NULL,
                *delim = "";
                
        while ( (tokptr = strtok_r(tokarg1, "&", &save_ptr)) ) {
            /* Reset tokarg1 to continue in the current string on subsequent iterations: */
            tokarg1 = NULL;
            
            if ( cpuinfo_features_is_str_ours(tokptr, -1) ) xstrfmtcat(out_features, "%s%s", delim, tokptr), delim = ",";
        }
        if ( ! out_features ) {
            out_features = copy;
        } else {
            xfree(copy);
        }
    }
	return out_features;
}

/**
 * @brief   Update node's active configuration based upon features in job
 *          constraints.
 * @details Executed by the slurmd daemon.
 * @param   active_features     the list of features required by a job
 * @return  SLURM_SUCCESS or an error code
 */
extern int node_features_p_node_set(char *active_features)
{
    debug("node_features_p_node_set: active_features = %s", active_features ? active_features : "(null)");
    return SLURM_SUCCESS;
}

/**
 * @brief   Does this plugin require PowerSave mode for booting nodes?
 * @return  Returns boolean true if PowerSave mode is needed, false
 *          otherwise
 */
extern bool
node_features_p_node_power(void)
{
	return false;
}

/**
 * @brief   Respond to an alteration of active features on a set of nodes
 * @details Note the active features associated with a set of nodes have been updated.
 *          This can be used to alter GRES values, for example.
 * @param   active_features     new list of active features
 * @param   node_bitmap         nodes on which @a active_features is now present
 * @return  SLURM_SUCCESS or an error code
 */
extern int
node_features_p_node_update(
    char        *active_features,
	bitstr_t    *node_bitmap
)
{
    debug("node_features_p_node_update: active_features = %s", active_features ? active_features : "(null)");
    return SLURM_SUCCESS;
}

/**
 * @brief   Is an update to node state valid?
 * @details Check that the specified node update request is valid with respect
 *          to features changes.
 * @param   node_ptr             pointer to the pertinent node_record struct
 * @param   update_node_msg pointer to update request
 * @return  Returns boolean true if the update is permissible, false otherwise
 */
extern bool
node_features_p_node_update_valid(
    void                *node_ptr,
	update_node_msg_t   *update_node_msg
)
{
    debug("node_features_p_node_update_valid: node_names=%s, features=%s, features_act=%s",
                update_node_msg->node_names ? update_node_msg->node_names : "(null)",
                update_node_msg->features ? update_node_msg->features : "(null)",
                update_node_msg->features_act ? update_node_msg->features_act : "(null)");
    return false;
}

/**
 * @brief   Is a feature name associated with this plugin?
 * @param   feature     a feature name string
 * @result  Returns boolean true if @feature is owned by us
 */
extern bool
node_features_p_changible_feature(
    char    *feature
)
{
    debug("node_features_p_changible_feature: feature = %s", feature ? feature : "(null)");
	return cpuinfo_features_is_str_ours(feature, -1);
}

/**
 * @brief   Filter a feature list to replace our old values with new ones
 * @details Translate a node's feature specification by replacing any features associated
 *	        with this plugin in the original value with the new values, preserving any
 *          features that are not associated with this plugin
 * @param   new_features    comma-separated list of new feature strings
 * @param   orig_features   comma-separated list of existing feature strings
 * @param   avail_features  comma-separated list of available feature strings
 * @return  A string containing the new set of our features merged with the original
 *          list (allocated with a Slurm xmalloc etc.)
 */
extern char*
node_features_p_node_xlate(
    char        *new_features,
    char        *orig_features,
	char        *avail_features
)
{
    char        *out_features = NULL;
    
    debug("node_features_p_node_xlate: new_features = %s", new_features ? new_features : "(null)");
    debug("node_features_p_node_xlate: orig_features = %s", orig_features ? orig_features : "(null)");
    debug("node_features_p_node_xlate: avail_features = %s", avail_features ? avail_features : "(null)");
    
    /* Short-circuit, no union necessary: */
    if ( ! new_features || ! *new_features ) {
        out_features = xstrdup(orig_features);
    }
    else if ( ! orig_features || ! *orig_features ) {
        out_features = xstrdup(new_features);
    }
    else {
        char    *tokptr, *saveptr = NULL, *delim = "";
        char    *orig_copy, *tokarg1;
        
        /* Produce    new_features U (orig_features - our_features): */
        out_features = xstrdup(new_features);
        if ( *out_features ) delim = ",";
        
        orig_copy = xstrdup(orig_features);
        tokarg1 = orig_copy;
        while ( (tokptr = strtok_r(tokarg1, ",", &saveptr)) ) {
            /* Reset tokarg1 to continue in the current string on subsequent iterations: */
            tokarg1 = NULL;
            
            /* Only add those which don't belong to us: */
            if ( ! cpuinfo_features_is_str_ours(tokptr, -1) ) {
                /* Ensure it's not already in the list: */
                if ( ! __contains_str(out_features, tokptr, ",") ) {
                    xstrfmtcat(out_features, "%s%s", delim, tokptr);
                    delim = ",";
                }
            }
        }
        xfree(orig_copy);
    }
    return out_features;
}

/**
 * @brief   Rearrange the order of feature strings in a new feature list
 * @details This is an opportunity after all plugins have constructed the new
 *          feature list to alter the ordering of strings within it.
 * @param   new_features    the list of feature strings
 * @return  A string containing the (possibly) reordered set of features
 *          (allocated with a Slurm xmalloc etc.)
 */
extern char*
node_features_p_node_xlate2(
    char    *new_features
)
{
    debug("node_features_p_node_xlate2: new_features = %s", new_features ? new_features : "(null)");
    /* We don't do any reordering: */
    return xstrdup(new_features);
}

/**
 * @brief   Perform set up for step launch
 * @param   mem_sort    trigger sort of memory pages (KNL zonesort)
 * @param   numa_bitmap NUMA nodes allocated to this job
 */
extern void
node_features_p_step_config(
    bool        mem_sort,
    bitstr_t    *numa_bitmap
)
{
    /* Unused by this plugin */
}

/**
 * @brief   Determine if the specified user can modify the currently available node
 *          features
 * @param   uid     the uid to test
 * @return  Boolean true if the user is allowed to reconfigure the node
 */
extern bool
node_features_p_user_update(
    uid_t       uid
)
{
    debug("node_features_p_user_update = %d", (int)uid);
    /* No reconfiguration is ever necessary */
    return false;
}

/**
 * @brief   Return estimated reboot time, in seconds
 */
extern uint32_t
node_features_p_boot_time(void)
{
    debug("node_features_p_boot_time");
	return 0;
}

#if SLURM_VERSION_NUMBER > SLURM_VERSION_NUM(18,0,0)
/*
 * Newer API members
 */

/**
 * @brief   Spelling-corrected variant of node_features_p_changible_feature
 */
extern bool
node_features_p_changeable_feature(
    char    *input
)
{
    return node_features_p_changible_feature(input);
}

/**
 * @brief   Construct a node bitmap indicating on which nodes this
 *          plugin functions
 * @return  A Slurm bitstr with applicable node indices bits set
 *          (caller is reponsible for xfree-ing it)
 */
extern bitstr_t*
node_features_p_get_node_bitmap(void)
{
	bitstr_t *bitmap;
	bitmap = bit_alloc(node_record_count);
	bit_set_all(bitmap);
	return bitmap;
}

extern int
node_features_p_overlap(
    bitstr_t    *active_bitmap
)
{
    /* Executed on slurmctld and not used by this plugin */
    return bit_set_count(active_bitmap);
}

extern uint32_t
node_features_p_reboot_weight(void)
{
        return 0;
}

/* Get node features plugin configuration */
extern void
node_features_p_get_config(
    config_plugin_params_t  *p
)
{
}

#endif

#endif
