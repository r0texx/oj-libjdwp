#ifndef JDWP_THREAD_NAME_FILTERS_H_
#define JDWP_THREAD_NAME_FILTERS_H_

#include <jni.h>
#include "inStream.h"
#include "outStream.h"

jboolean JDWP_VM_SetThreadNameFilters(PacketInputStream* in, PacketOutputStream* out);
jboolean JDWP_ShouldSuppressByThreadName(JNIEnv* env, jthread thread);

#endif  // JDWP_THREAD_NAME_FILTERS_H_