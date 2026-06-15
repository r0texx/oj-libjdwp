#ifndef JDWP_RULE_INDEX_CMD_H_
#define JDWP_RULE_INDEX_CMD_H_

#include <jni.h>
#include "inStream.h"
#include "outStream.h"

jboolean JDWP_VM_LoadRuleIndex(PacketInputStream* in, PacketOutputStream* out);

#endif
