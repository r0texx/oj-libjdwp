#include "RuleIndexCmd.h"

#include "util.h"
#include "inStream.h"
#include "outStream.h"

jboolean JDWP_VM_LoadRuleIndex(PacketInputStream* in, PacketOutputStream* out) {
    jint len = inStream_readInt(in);
    if (inStream_error(in)) {
        return JNI_TRUE;
    }
    if (len < 0) {
        outStream_setError(out, JDWP_ERROR(ILLEGAL_ARGUMENT));
        return JNI_TRUE;
    }

    jbyte* buf = (jbyte*)jvmtiAllocate(len > 0 ? len : 1);
    if (buf == NULL) {
        outStream_setError(out, JDWP_ERROR(OUT_OF_MEMORY));
        return JNI_TRUE;
    }
    (void)inStream_readBytes(in, len, buf);
    if (!inStream_error(in)) {
        JVMTI_FUNC_PTR(gdata->jvmti, RuleIndexLoad)(gdata->jvmti, (const unsigned char*)buf, len);
    }
    jvmtiDeallocate(buf);
    return JNI_TRUE;
}
