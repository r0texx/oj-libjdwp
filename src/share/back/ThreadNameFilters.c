#include "ThreadNameFilters.h"

#include <android/log.h>

#include <regex.h>
#include <string.h>
#include <stdlib.h>

#include "debugInit.h"
#include "bag.h"

typedef struct {
	regex_t* entries;	 /* array of compiled regex */
	int count;
} regex_set_t;

static regex_set_t* g_thread_name_filters = NULL;


/* -------- name-keyed cache using bag -------- */

typedef struct {
  unsigned  ck;     /* FNV-1a checksum of name */
  char*     name;   /* owned copy (jvmtiAllocate) */
  jboolean  blocked;
} NameCacheEntry;


static struct bag*   g_name_cache = NULL;   /* bag of NameCacheEntry* */

static jrawMonitorID g_cache_mon  = NULL;  /* libjdwp monitor */

#ifndef THREAD_CACHE_MAX
#define THREAD_CACHE_MAX 1024
#endif

/* -------- helpers -------- */
static void cache_init_once(void) {
    static int inited = 0;
    if (inited) {
    	return;
    }
    g_name_cache = bagCreateBag(sizeof(NameCacheEntry), /*initialAllocation=*/16);
    g_cache_mon  = debugMonitorCreate("ThreadNameFilters");
    inited = 1;
}

static inline unsigned fnv1a32(const char* s) {
	unsigned h = 2166136261u;
	for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
		h ^= *p;
		h *= 16777619u;
	}
	return h;
}

static void free_entry(NameCacheEntry* e) {
  if (e->name) {
  	jvmtiDeallocate((unsigned char*)e->name);
  }
  jvmtiDeallocate((unsigned char*)e);
}

static jboolean cache_find_cb(void* item, void* arg) {
    NameCacheEntry* e = (NameCacheEntry*)item;
    NameCacheEntry* k = (NameCacheEntry*)arg;   /* key: name (non-owned), ck set by caller */
    if (e->ck == k->ck && e->name != NULL && strcmp(e->name, k->name) == 0) {
        k->blocked = e->blocked;                /* return via key.blocked */
        return JNI_FALSE;                       /* abort enumeration: FOUND */
    }
    return JNI_TRUE;                            /* keep scanning */
}

/* Locked; returns JNI_TRUE if found and sets *out_blocked */
static jboolean cache_lookup_locked(const char* name, unsigned ck, jboolean* out_blocked) {
    NameCacheEntry key;
    key.name = (char*)name;   /* non-owned */
    key.ck   = ck;

    /* bagEnumerateOver returns JNI_FALSE if the callback aborted (found). */
    jboolean completed = bagEnumerateOver(g_name_cache, cache_find_cb, &key);
    if (completed == JNI_FALSE) {
        *out_blocked = key.blocked;
        return JNI_TRUE;      /* found */
    }
    return JNI_FALSE;         /* not found */
}

static void cache_insert_locked(const char* name, unsigned ck, jboolean blocked) {
    NameCacheEntry* slot = (NameCacheEntry*)bagAdd(g_name_cache); /* returns space for one item */
    size_t n = strlen(name);
    char* dup = (char*)jvmtiAllocate(n + 1);
    memcpy(dup, name, n + 1);
    slot->name    = dup;
    slot->ck      = ck;
    slot->blocked = blocked;
}

/* ---------- Query at event-time (AFTER per-request filters) ---------- */
jboolean JDWP_IsSuppressingThreadName(char* name) {
	const unsigned ck = fnv1a32(name);

	jboolean blocked;

	debugMonitorEnter(g_cache_mon);

	if (!cache_lookup_locked(name, ck, &blocked)) {
		blocked = JNI_FALSE;
		for (int i = 0; i < g_thread_name_filters->count; ++i) {
			if (regexec(&g_thread_name_filters->entries[i], name, 0, NULL, 0) == 0) {
				blocked = JNI_TRUE;
				break;
			}
		}
		cache_insert_locked(name, ck, blocked);
	}

	debugMonitorExit(g_cache_mon);

	return blocked;
}

jboolean JDWP_ShouldSuppressByThreadName(JNIEnv* env, jthread thread) {
	cache_init_once();

	if (!thread) {
		return JNI_FALSE;
	}
	if (g_thread_name_filters == NULL || g_thread_name_filters->count == 0) {
		return JNI_FALSE;
	}

    jboolean result;

    WITH_LOCAL_REFS(env, 1) {

        jvmtiThreadInfo info;
        jvmtiError error;

        (void)memset(&info, 0, sizeof(info));

        error = JVMTI_FUNC_PTR(gdata->jvmti,GetThreadInfo)
                                (gdata->jvmti, thread, &info);

        if (error == JVMTI_ERROR_NONE && info.name) {
        	result = JDWP_IsSuppressingThreadName(info.name);
        } else {
            result = false;
        }

        if (info.name != NULL) {
            jvmtiDeallocate(info.name);
        }

    } END_WITH_LOCAL_REFS(env);

    return result;
}


/* ---------- JDWP: VirtualMachine(24) ---------- */
/* Payload:
 *	 int32 count
 *	 count × UTF-8 regex strings (POSIX ERE)
 */
jboolean JDWP_VM_SetThreadNameFilters(PacketInputStream* in, PacketOutputStream* out) {
	cache_init_once();

	const jint n = inStream_readInt(in);
	if (inStream_error(in)) {
		return JNI_TRUE;
	}

	if (n == 0) {
		return JNI_TRUE;
	} else if (n < 0 || n > 4096) {
		outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
		return JNI_TRUE;
	}

    /* Build new immutable set */
	regex_set_t* filters = (regex_set_t*) jvmtiAllocate(sizeof(regex_set_t));
    if (filters == NULL) {
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
	filters->count = n;
	filters->entries = (regex_t*) jvmtiAllocate(sizeof(regex_t) * n);
    if (filters->entries == NULL) {
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
	memset(filters->entries, 0, sizeof(regex_t) * n);

	for (int i = 0; i < n; ++i) {
		char* raw = inStream_readString(in);
		if (inStream_error(in) || raw == NULL || raw[0] == '\0') {
			outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
			return JNI_TRUE;
		}

		int error = regcomp(&filters->entries[i], raw, REG_EXTENDED | REG_NOSUB);
		if (error != 0) {
			__android_log_print(ANDROID_LOG_WARN, "JDWP", "Bad thread filter, [%d] = '%s', regcomp() error code = %d", i, raw, error);
			outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
			return JNI_TRUE;
		} else {
			jvmtiDeallocate(raw);
		}
	}

	g_thread_name_filters = filters;

	return JNI_TRUE;
}
