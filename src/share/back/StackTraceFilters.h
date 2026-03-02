#ifndef JDWP_STACK_FILTERS_H_
#define JDWP_STACK_FILTERS_H_

#include <jni.h>
#include "inStream.h"
#include "outStream.h"

jboolean JDWP_VM_SetStackTraceFilters(PacketInputStream* in, PacketOutputStream* out);
jboolean JDWP_VM_SetSourceNameFilters(PacketInputStream* in, PacketOutputStream* out);

jboolean JDWP_ShouldSuppressForStack(JNIEnv* env, EventInfo* evinfo, jthread thread);

#endif  // JDWP_STACK_FILTERS_H_