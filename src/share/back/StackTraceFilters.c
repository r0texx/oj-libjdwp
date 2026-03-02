#include "StackTraceFilters.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "util.h"                    /* getClassname, jvmtiAllocate/Deallocate, gdata, JVMTI_FUNC_PTR */
#include "inStream.h"
#include "outStream.h"

/* ---------------- Pattern model ---------------- */

typedef enum {
    MK_EXACT = 0, /* "Foo" */
    MK_PREFIX,    /* "Foo*" */
    MK_SUFFIX,    /* "*Foo" */
    MK_CONTAINS   /* "*Foo" */
} match_kind_t;

typedef struct {
    match_kind_t kind;
    char* text;                            /* owned (jvmtiAllocate) */
    unsigned short len;            /* cached length */
} component_t;

typedef struct {
    component_t* klass;            /* required (non-NULL) */
    component_t* method;         /* optional; NULL => "any method" */
} frame_filter_entry_t;

typedef struct {
    int count;
    frame_filter_entry_t* entries; /* owned (jvmtiAllocate) */
} frame_filter_set_t;

typedef struct {
    int count;
    component_t** entries; /* owned (jvmtiAllocate) */
} source_name_filter_set_t;

/* Single snapshot; set by the client once during startup. */
static frame_filter_set_t* g_frame_filters = NULL;
static source_name_filter_set_t* g_source_filters = NULL;

/* ---------------- Small utils ---------------- */

static void free_component(component_t* c) {
    if (c == NULL) {
        return;
    }
    if (c->text != NULL) {
        jvmtiDeallocate((unsigned char*)c->text);
    }
    jvmtiDeallocate((unsigned char*)c);
}

static void free_frame_filter_set(frame_filter_set_t* s) {
    if (s == NULL) {
        return;
    }
    if (s->entries != NULL) {
        for (int i = 0; i < s->count; ++i) {
            free_component(s->entries[i].klass);
            free_component(s->entries[i].method);
        }
        jvmtiDeallocate((unsigned char*)s->entries);
    }
    jvmtiDeallocate((unsigned char*)s);
}

static void free_source_name_filter_set(source_name_filter_set_t* s) {
    if (s == NULL) {
        return;
    }
    if (s->entries != NULL) {
        for (int i = 0; i < s->count; ++i) {
            free_component(s->entries[i]);
        }
        jvmtiDeallocate((unsigned char*)s->entries);
    }
    jvmtiDeallocate((unsigned char*)s);
}

static jboolean dup_str_jvmti(const char* src, char** out, unsigned short* out_len) {
    size_t n = src ? strlen(src) : 0;
    if (n > 65535) {
        return JNI_FALSE;
    }
    char* p = (char*)jvmtiAllocate(n + 1);
    if (p == NULL) {
        return JNI_FALSE;
    }
    if (n) {
        memcpy(p, src, n);
    }
    p[n] = '\0';
    *out = p;
    if (out_len) {
        *out_len = (unsigned short)n;
    }
    return JNI_TRUE;
}

static component_t* new_component(const char* s) {
    if (s == NULL || s[0] == '\0') {
        return NULL;
    }

    size_t n   = strlen(s);
    int starts = (s[0] == '*');
    int ends   = (s[n - 1] == '*' && n > 1);

    component_t* c = (component_t*) jvmtiAllocate(sizeof(component_t));
    c->text = NULL;
    c->len = 0;

    if (starts && ends) {
        c->kind = MK_CONTAINS;
        char* tmp = NULL;
        unsigned short len = 0;
        if (!dup_str_jvmti(s + 1, &tmp, &len)) {
            free_component(c);
            return NULL;
        }
        tmp[len - 1] = '\0';    /* drop trailing '*' */
        c->text = tmp;
        c->len  = (unsigned short)(len - 1);
    } else if (starts) {
        c->kind = MK_SUFFIX;
        if (!dup_str_jvmti(s + 1, &c->text, &c->len)) {
            free_component(c);
            return NULL;
        }
    } else if (ends) {
        c->kind = MK_PREFIX;
        char* tmp = NULL;
        unsigned short len = 0;
        if (!dup_str_jvmti(s, &tmp, &len)) {
            free_component(c);
            return NULL;
        }
        tmp[len - 1] = '\0';    /* drop trailing '*' */
        c->text = tmp;
        c->len  = (unsigned short)(len - 1);
    } else {
        c->kind = MK_EXACT;
        if (!dup_str_jvmti(s, &c->text, &c->len)) {
            free_component(c);
            return NULL;
        }
    }
    return c;
}

static inline jboolean matches_component(const char* name, const component_t* pat) {
    if (pat == NULL) {
        return JNI_TRUE;
    }
    if (name == NULL) {
        return JNI_FALSE;
    }

    size_t n = strlen(name);
    switch (pat->kind) {
        case MK_EXACT:
            return (n == pat->len && memcmp(name, pat->text, pat->len) == 0);
        case MK_PREFIX:
            return (n >= pat->len && memcmp(name, pat->text, pat->len) == 0);
        case MK_SUFFIX:
            return (n >= pat->len && memcmp(name + (n - pat->len), pat->text, pat->len) == 0);
        case MK_CONTAINS:
            return (n >= pat->len && strstr(name, pat->text) != NULL);
        default:
            return JNI_FALSE;
    }
}

/* ---------------- Event-time check (AFTER per-request filters) ---------------- */

jboolean JDWP_IsSuppressingSourceName(jclass clazz) {
    if (g_source_filters == NULL || g_source_filters->count == 0) {
        return JNI_FALSE;
    }

    char *sourceName = NULL;
    jvmtiError error = JVMTI_FUNC_PTR(gdata->jvmti,GetSourceFileName)
                        (gdata->jvmti, clazz, &sourceName);
    if (sourceName == NULL) {
        return JNI_FALSE;
    }

    // filters
    for (int filter_index = 0; filter_index < g_source_filters->count; ++filter_index) {
        const component_t* fe = g_source_filters->entries[filter_index];

        if (matches_component(sourceName, fe)) {
            jvmtiDeallocate(sourceName);

            return JNI_TRUE;
        }
    }

    jvmtiDeallocate(sourceName);

    return JNI_FALSE;
}

jboolean JDWP_IsSuppressingFrame(jclass clazz, jmethodID method) {
    // class name
    char* class_name = getClassname(clazz);

    // class name dotted
    char* class_name_dotted = strdup(class_name);
    int index = 0; // replce $ with .
    while ((class_name_dotted[index]) != '\0') {
        if (class_name_dotted[index] == '$') {
            class_name_dotted[index] = '.';
        }
        ++index;
    }

    // method name
    char* method_name = NULL;
    JVMTI_FUNC_PTR(gdata->jvmti, GetMethodName)
                (gdata->jvmti, method, &method_name, NULL, NULL);

    // filters
    for (int filter_index = 0; filter_index < g_frame_filters->count; ++filter_index) {
        const frame_filter_entry_t* fe = &g_frame_filters->entries[filter_index];

        jboolean matches = (matches_component(class_name, fe->klass) || matches_component(class_name_dotted, fe->klass)) && matches_component(method_name, fe->method);

        if (matches) {
            jvmtiDeallocate(class_name);
            jvmtiDeallocate(class_name_dotted);
            jvmtiDeallocate(method_name);

            return JNI_TRUE;
        }
    }

    jvmtiDeallocate(class_name);
    jvmtiDeallocate(class_name_dotted);
    jvmtiDeallocate(method_name);

    return JNI_FALSE;
}

static inline int GetFrameStartIndex(EventInfo* evinfo) {
    switch (evinfo->ei) {
        case EI_FIELD_ACCESS:
        case EI_FIELD_MODIFICATION:
            return 0;
        default:
            return 1;
    }
}

jboolean JDWP_ShouldSuppressForStack(JNIEnv* env, EventInfo* evinfo, jthread thread) {
    if (g_frame_filters == NULL || g_frame_filters->count == 0) {
        return JNI_FALSE;
    }

    jvmtiError error;
    jint frame_count;
    error = JVMTI_FUNC_PTR(gdata->jvmti,GetFrameCount)
                        (gdata->jvmti, thread, &frame_count);
    if (error != JVMTI_ERROR_NONE) {
        return JNI_FALSE;
    }

    for (int frame_index = GetFrameStartIndex(evinfo); frame_index < frame_count; ++frame_index) {
        jboolean result;

        WITH_LOCAL_REFS(env, 1) {

            jclass clazz;
            jmethodID method;
            jlocation location;

            error = JVMTI_FUNC_PTR(gdata->jvmti, GetFrameLocation)
                                (gdata->jvmti, thread, frame_index, &method, &location);
            if (error == JVMTI_ERROR_OPAQUE_FRAME) {
                clazz = NULL;
                error = JVMTI_ERROR_NONE;
            } else if (error == JVMTI_ERROR_NONE) {
                error = methodClass(method, &clazz);
            }

            if (error == JVMTI_ERROR_NONE && clazz != NULL && method != NULL) {
                result = JDWP_IsSuppressingSourceName(clazz) || JDWP_IsSuppressingFrame(clazz, method);
            } else {
                result = JNI_FALSE;
            }

        } END_WITH_LOCAL_REFS(env);

        if (result == JNI_TRUE) {
            return JNI_TRUE;
        } else if (error != JVMTI_ERROR_NONE) {
            break;
        }
    }

    return JNI_FALSE;
}

/* ---------------- JDWP: VM(23) setter (set-once, no locks) ----------------
 * Payload:
 *     int32 count
 *     count × string filter, where filter := classPattern[ ':' methodPattern ]
 * Pattern rule: at most one '*' at beginning OR end; bare "*" is invalid.
 */
jboolean JDWP_VM_SetStackTraceFilters(PacketInputStream* in, PacketOutputStream* out) {
    jint n = inStream_readInt(in);
    if (inStream_error(in)) {
        return JNI_TRUE;
    }
    if (n == 0) {
        return JNI_TRUE;
    }
    if (n < 0 || n > 2000) {
        outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
        return JNI_TRUE;
    }

    /* Build new immutable set */
    frame_filter_set_t* filters = (frame_filter_set_t*) jvmtiAllocate(sizeof(frame_filter_set_t));
    if (filters == NULL) {
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
    filters->count = n;
    filters->entries = (frame_filter_entry_t*) jvmtiAllocate(sizeof(frame_filter_entry_t) * n);
    if (filters->entries == NULL) {
        free_frame_filter_set(filters);
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
    memset(filters->entries, 0, sizeof(frame_filter_entry_t) * n);

    for (int i = 0; i < n; ++i) {
        char* raw = inStream_readString(in);
        if (inStream_error(in) || raw == NULL || raw[0] == '\0') {
            if (raw != NULL) {
                jvmtiDeallocate((unsigned char*)raw);
            }
            free_frame_filter_set(filters);
            outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
            return JNI_TRUE;
        }

        char* colon = strchr(raw, ':');
        if (colon != NULL) {
            *colon = '\0';
            const char* klass = raw;
            const char* meth  = colon + 1;

            filters->entries[i].klass = new_component(klass);
            if (filters->entries[i].klass == NULL) {
                jvmtiDeallocate((unsigned char*)raw);
                free_frame_filter_set(filters);
                outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
                return JNI_TRUE;
            }

            filters->entries[i].method = new_component(meth);
            if (filters->entries[i].method == NULL) {
                jvmtiDeallocate((unsigned char*)raw);
                free_frame_filter_set(filters);
                outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
                return JNI_TRUE;
            }

        } else {
            /* class-only */
            filters->entries[i].klass = new_component(raw);
            if (filters->entries[i].klass == NULL) {
                jvmtiDeallocate((unsigned char*)raw);
                free_frame_filter_set(filters);
                outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
                return JNI_TRUE;
            }
            filters->entries[i].method = NULL;        /* any method */
        }

        jvmtiDeallocate((unsigned char*)raw);
    }

    /* Set once; if already set, just replace (no locks needed if done at startup) */
    if (g_frame_filters != NULL) {
        free_frame_filter_set(g_frame_filters);
    }
    g_frame_filters = filters;

    return JNI_TRUE;    /* empty success reply */
}

jboolean JDWP_VM_SetSourceNameFilters(PacketInputStream* in, PacketOutputStream* out) {
    jint n = inStream_readInt(in);
    if (inStream_error(in)) {
        return JNI_TRUE;
    }
    if (n == 0) {
        return JNI_TRUE;
    }
    if (n < 0 || n > 2000) {
        outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
        return JNI_TRUE;
    }

    /* Build new immutable set */
    source_name_filter_set_t* filters = (source_name_filter_set_t*) jvmtiAllocate(sizeof(source_name_filter_set_t));
    if (filters == NULL) {
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
    filters->count = n;
    filters->entries = (component_t**) jvmtiAllocate(sizeof(component_t*) * n);
    if (filters->entries == NULL) {
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
    memset(filters->entries, 0, sizeof(component_t*) * n);

    for (int i = 0; i < n; ++i) {
        char* raw = inStream_readString(in);
        if (inStream_error(in) || raw == NULL || raw[0] == '\0') {
            if (raw != NULL) {
                jvmtiDeallocate((unsigned char*)raw);
            }
            outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
            return JNI_TRUE;
        }

        filters->entries[i] = new_component(raw);
        if (filters->entries[i] == NULL) {
            jvmtiDeallocate((unsigned char*)raw);
            free_source_name_filter_set(filters);
            outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
            return JNI_TRUE;
        }

        jvmtiDeallocate((unsigned char*)raw);
    }

    /* Set once; if already set, just replace (no locks needed if done at startup) */
    if (g_source_filters != NULL) {
        free_source_name_filter_set(g_source_filters);
    }
    g_source_filters = filters;

    return JNI_TRUE;    /* empty success reply */
}
